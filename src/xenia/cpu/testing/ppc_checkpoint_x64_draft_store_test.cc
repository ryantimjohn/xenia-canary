/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/backend/x64/x64_tracers.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/compiler/compiler_passes.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DECLARE_bool(emit_source_annotations);
DECLARE_bool(instrument_call_times);

namespace xe::cpu::ppc::test {
namespace {

using namespace backend::x64;
using namespace hir;
constexpr uint32_t kStorePC = 0x80001000;
constexpr uint32_t kAfterStorePC = kStorePC + 4;
constexpr size_t kDestination = offsetof(PPCContext, r) + 3 * sizeof(uint64_t);
constexpr uint64_t kValue = 0x12345678;
constexpr size_t kMaxDraftBytes = 4096;
constexpr size_t kMaxStoreBytes = 16;

class FixtureHIR final : public HIRBuilder {
 public:
  FixtureHIR() : previous_(GetCurrent()) { MakeCurrent(); }
  ~FixtureHIR() override {
    if (GetCurrent() == this) {
      RemoveCurrent();
      if (previous_) {
        previous_->MakeCurrent();
      }
    }
  }

 private:
  HIRBuilder* const previous_;
};

struct OpaqueDraft {
  std::vector<uint8_t> bytes;
  std::vector<SourceMapEntry> source_map;
  size_t stack_size = 0;
};

// The protected seam emits scratch bytes without the public function setup or
// cleanup. AutoGrow label relocations remain unresolved: no ready(), placement,
// registration, function pointer, or invocation is provided by this fixture.
class DraftEmitter final : public X64Emitter {
 public:
  using X64Emitter::X64Emitter;
  ~DraftEmitter() override { Discard(); }

  bool EmitOpaque(HIRBuilder& builder, OpaqueDraft& draft) {
    draft = {};
    if (processor()->compilation_options().profile !=
            CompilationProfile::kOrdinary ||
        ContainsCheckpointHIR(builder) ||
        HIRBuilder::GetCurrent() != &builder) {
      return false;
    }
    REQUIRE(getSize() == 0);
    REQUIRE(epilog_label_ == nullptr);
    REQUIRE(tail_code_.empty());
    REQUIRE(label_cache_.empty());
    REQUIRE(guest_module_ == nullptr);
    REQUIRE(current_guest_function_ == 0);
    REQUIRE(debug_info_ == nullptr);
    REQUIRE(debug_info_flags_ == 0);
    REQUIRE(trace_data_ == nullptr);
    backend::EmitFunctionInfo info{};
    if (!X64Emitter::Emit(&builder, info) || !getSize() ||
        getSize() > kMaxDraftBytes) {
      Discard();
      return false;
    }
    // Copy opaque storage only; no scratch address escapes this method.
    const uint8_t* scratch = getCode();
    draft.bytes.assign(scratch, scratch + getSize());
    source_map_arena_.CloneContents(&draft.source_map);
    draft.stack_size = info.stack_size;
    Discard();
    return true;
  }

  void CheckDiscarded() {
    CHECK(getSize() == 0);
    CHECK(epilog_label_ == nullptr);
    CHECK(tail_code_.empty());
    CHECK(label_cache_.empty());
    CHECK(current_instr_ == nullptr);
    CHECK(stack_size_ == 0);
    CHECK_FALSE(synchronize_stack_on_next_instruction_);
    CHECK(mxcsr_mode_ == MXCSRMode::Unknown);
    std::vector<SourceMapEntry> retained;
    source_map_arena_.CloneContents(&retained);
    CHECK(retained.empty());
  }

 private:
  void Discard() {
    // Clear Xbyak's references before destroying labels or captured callbacks.
    reset();
    tail_code_.clear();
    for (auto* label : label_cache_) {
      delete label;
    }
    label_cache_.clear();
    epilog_label_ = nullptr;
    source_map_arena_.Reset();
    guest_module_ = nullptr;
    current_guest_function_ = 0;
    current_instr_ = nullptr;
    debug_info_ = nullptr;
    debug_info_flags_ = 0;
    trace_data_ = nullptr;
    stack_size_ = 0;
    synchronize_stack_on_next_instruction_ = false;
    ForgetMxcsrMode();
  }
};

struct Decoder {
  csh handle = 0;
  cs_insn* instruction = nullptr;
  Decoder() {
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK &&
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK) {
      instruction = cs_malloc(handle);
    }
  }
  ~Decoder() {
    if (instruction) {
      cs_free(instruction, 1);
    }
    if (handle) {
      cs_close(&handle);
    }
  }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

void CheckConstantStore(const OpaqueDraft& draft) {
  REQUIRE(draft.source_map.size() == 2);
  REQUIRE(draft.source_map[0].guest_address == kStorePC);
  REQUIRE(draft.source_map[1].guest_address == kAfterStorePC);
  const size_t begin = draft.source_map[0].code_offset;
  const size_t end = draft.source_map[1].code_offset;
  REQUIRE(begin < end);
  REQUIRE(end <= draft.bytes.size());
  REQUIRE(end - begin <= kMaxStoreBytes);
  Decoder decoder;
  REQUIRE(decoder.instruction);
  const uint8_t* cursor = draft.bytes.data() + begin;
  size_t remaining = end - begin;
  uint64_t address = begin;  // Numeric draft offset, never a native address.
  REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                         decoder.instruction));
  // Exactly one MOV consumes the interval. This rejects extra memory effects,
  // branches, calls, and helpers rather than treating undecoded bytes as safe.
  REQUIRE(remaining == 0);
  REQUIRE(decoder.instruction->id == X86_INS_MOV);
  REQUIRE(decoder.instruction->detail);
  const auto& x = decoder.instruction->detail->x86;
  REQUIRE(x.op_count == 2);
  const auto& destination = x.operands[0];
  REQUIRE(destination.type == X86_OP_MEM);
  CHECK(destination.size == sizeof(uint64_t));
  CHECK(destination.access == CS_AC_WRITE);
  // RSI is the actual backend's GetContextReg contract, not a guest ABI.
  CHECK(destination.mem.base == X86_REG_RSI);
  CHECK(destination.mem.index == X86_REG_INVALID);
  CHECK(destination.mem.segment == X86_REG_INVALID);
  CHECK(destination.mem.disp == kDestination);
  REQUIRE(x.operands[1].type == X86_OP_IMM);
  CHECK(static_cast<uint64_t>(x.operands[1].imm) == kValue);
  cs_regs reads{}, writes{};
  uint8_t read_count = 0, write_count = 0;
  REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                         &read_count, writes, &write_count) == CS_ERR_OK);
  REQUIRE(read_count == 1);
  CHECK(reads[0] == X86_REG_RSI);
  CHECK(write_count == 0);
}

TEST_CASE("Actual x64 draft contains one canonical constant context store",
          "[cpu][x64-draft-store]") {
  // Prerequisites are observed, never changed to manufacture the interval.
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  Memory memory;
  REQUIRE(memory.Initialize());
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::make_unique<X64Backend>()));
  REQUIRE(processor.compilation_options().profile ==
          CompilationProfile::kOrdinary);
  FixtureHIR builder;
  builder.SourceOffset(kStorePC);
  builder.StoreContext(kDestination, builder.LoadConstantUint64(kValue));
  builder.SourceOffset(kAfterStorePC);
  builder.Return();
  REQUIRE(builder.Finalize());
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      processor.backend()->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  XbyakAllocator allocator;
  DraftEmitter emitter(static_cast<X64Backend*>(processor.backend()),
                       &allocator);
  OpaqueDraft draft;
  REQUIRE(emitter.EmitOpaque(builder, draft));
  emitter.CheckDiscarded();
  CheckConstantStore(draft);
  // Only the source-delimited, relocation-free store is inspected. Prolog,
  // epilog, helper tails and unresolved branches carry no execution claim;
  // neither complete bank capture nor checkpoint production is established.
}

constexpr uint32_t kSpillPC = 0x80002000;
constexpr size_t kSliceGPRCount = 8;
constexpr std::array<size_t, kSliceGPRCount> kCaptureOrder = {0, 1, 2, 4,
                                                              5, 6, 7, 3};

size_t GPROffset(size_t reg) {
  return offsetof(PPCContext, r) + reg * sizeof(uint64_t);
}

Value* CheckOldR3Spill(HIRBuilder& builder, Value* old_r3) {
  REQUIRE(old_r3->type == INT64_TYPE);
  REQUIRE_FALSE(old_r3->IsConstant());
  REQUIRE(old_r3->def);
  REQUIRE(old_r3->def->opcode->num == OPCODE_LOAD_CONTEXT);
  REQUIRE(old_r3->def->dest == old_r3);
  REQUIRE(old_r3->def->src1.offset == GPROffset(3));
  auto slot = old_r3->GetLocalSlot();
  REQUIRE(slot);
  REQUIRE(slot->type == INT64_TYPE);
  REQUIRE_FALSE(slot->IsConstant());
  REQUIRE(builder.locals().size() == 1);
  REQUIRE(builder.locals()[0] == slot);
  auto block = builder.first_block();
  REQUIRE(block);
  REQUIRE(block->instr_tail);
  REQUIRE(block->instr_tail->opcode->num == OPCODE_RETURN);
  REQUIRE(block->instr_tail->prev);
  REQUIRE(block->instr_tail->prev->opcode->num == OPCODE_SOURCE_OFFSET);
  REQUIRE(block->instr_tail->prev->src1.offset == kSpillPC + 12);
  // Return ends the body before RA. SpillOneRegister appends then relocates its
  // local instructions, leaving one empty terminal block; finalization keeps
  // it.
  auto scaffold = block->next;
  REQUIRE(scaffold);
  REQUIRE(scaffold == builder.last_block());
  REQUIRE(scaffold->prev == block);
  REQUIRE(scaffold->next == nullptr);
  REQUIRE(scaffold->instr_head == nullptr);
  REQUIRE(scaffold->instr_tail == nullptr);
  REQUIRE(scaffold->label_head == nullptr);
  REQUIRE(scaffold->label_tail == nullptr);
  REQUIRE(scaffold->incoming_edge_head == nullptr);
  REQUIRE(scaffold->outgoing_edge_head == nullptr);
  REQUIRE(scaffold->incoming_values == nullptr);
  REQUIRE(old_r3->def->block == block);
  Instr* spill = nullptr;
  Instr* reload = nullptr;
  Instr* overwrite = nullptr;
  Instr* restore = nullptr;
  bool definition_seen = false;
  std::array<size_t, 5> positions{};
  size_t position = 0;
  for (auto i = block->instr_head; i; i = i->next, ++position) {
    if (i == old_r3->def) {
      definition_seen = true;
      positions[0] = position;
    } else if (i->opcode->num == OPCODE_STORE_LOCAL) {
      REQUIRE(spill == nullptr);
      REQUIRE(i->src1.value == slot);
      REQUIRE(i->src2.value == old_r3);
      spill = i;
      positions[1] = position;
    } else if (i->opcode->num == OPCODE_LOAD_LOCAL) {
      REQUIRE(reload == nullptr);
      REQUIRE(i->src1.value == slot);
      REQUIRE(i->dest);
      REQUIRE(i->dest->type == INT64_TYPE);
      REQUIRE_FALSE(i->dest->IsConstant());
      REQUIRE(i->dest->def == i);
      REQUIRE(i->dest->GetLocalSlot() == slot);
      REQUIRE(i->dest->reg.set == old_r3->reg.set);
      REQUIRE(i->dest->reg.index >= 0);
      REQUIRE(static_cast<uint32_t>(i->dest->reg.index) <
              i->dest->reg.set->count);
      reload = i;
      positions[3] = position;
    } else if (i->opcode->num == OPCODE_STORE_CONTEXT &&
               i->src1.offset == GPROffset(3)) {
      if (i->src2.value->IsConstant()) {
        REQUIRE(overwrite == nullptr);
        REQUIRE(i->src2.value->type == INT64_TYPE);
        REQUIRE(i->src2.value->constant.u64 == kValue);
        overwrite = i;
        positions[2] = position;
      } else {
        REQUIRE(restore == nullptr);
        restore = i;
        positions[4] = position;
      }
    }
  }
  REQUIRE(definition_seen);
  REQUIRE(spill);
  REQUIRE(overwrite);
  REQUIRE(reload);
  REQUIRE(restore);
  REQUIRE(restore->src2.value == reload->dest);
  for (size_t i = 1; i < positions.size(); ++i) {
    REQUIRE(positions[i - 1] < positions[i]);
  }
  return slot;
}

size_t AllocatorGPRIndex(unsigned int reg) {
  // The backend's seven allocatable GPRs, not guessed SSA assignments.
  constexpr std::array<unsigned int, X64Emitter::GPR_COUNT> registers = {
      X86_REG_RBX, X86_REG_R10, X86_REG_R11, X86_REG_R12,
      X86_REG_R13, X86_REG_R14, X86_REG_R15};
  auto found = std::find(registers.begin(), registers.end(), reg);
  REQUIRE(found != registers.end());
  return static_cast<size_t>(found - registers.begin());
}

void CheckMovAccess(Decoder& decoder, const cs_x86& x, unsigned int base) {
  const auto& destination = x.operands[0];
  const auto& source = x.operands[1];
  const bool load = destination.type == X86_OP_REG;
  cs_regs reads{}, writes{};
  uint8_t read_count = 0, write_count = 0;
  REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                         &read_count, writes, &write_count) == CS_ERR_OK);
  REQUIRE(read_count == ((!load && source.type == X86_OP_REG) ? 2 : 1));
  REQUIRE(std::find(reads, reads + read_count, base) != reads + read_count);
  if (!load && source.type == X86_OP_REG) {
    REQUIRE(std::find(reads, reads + read_count, source.reg) !=
            reads + read_count);
  }
  REQUIRE(write_count == (load ? 1 : 0));
  if (load) {
    REQUIRE(writes[0] == destination.reg);
  }
}

void CheckSpillCapture(const OpaqueDraft& draft, uint32_t local_offset) {
  REQUIRE(draft.source_map.size() == 4);
  for (size_t i = 0; i < draft.source_map.size(); ++i) {
    REQUIRE(draft.source_map[i].guest_address == kSpillPC + i * 4);
    REQUIRE(draft.source_map[i].code_offset <= draft.bytes.size());
    if (i) {
      REQUIRE(draft.source_map[i - 1].code_offset <
              draft.source_map[i].code_offset);
    }
  }
  REQUIRE(draft.source_map.back().code_offset -
              draft.source_map.front().code_offset <=
          256);
  REQUIRE(local_offset >= StackLayout::GUEST_STACK_SIZE);
  REQUIRE(local_offset % sizeof(uint64_t) == 0);
  REQUIRE(draft.stack_size >= sizeof(uint64_t));
  REQUIRE(local_offset <= draft.stack_size - sizeof(uint64_t));
  Decoder decoder;
  REQUIRE(decoder.instruction);
  std::array<int, X64Emitter::GPR_COUNT> register_tokens;
  register_tokens.fill(-1);  // Reject reads without decoded provenance.
  int local_token = -1;
  size_t loads = 0, stores = 0, spills = 0, reloads = 0, overwrites = 0;
  // Each phase is decoded separately so no instruction may straddle a source
  // boundary. Tokens follow actual native operands, including register reuse.
  for (size_t phase = 0; phase < 3; ++phase) {
    const size_t begin = draft.source_map[phase].code_offset;
    const size_t end = draft.source_map[phase + 1].code_offset;
    const uint8_t* cursor = draft.bytes.data() + begin;
    size_t remaining = end - begin;
    uint64_t address = begin;  // Numeric draft offset, never a native address.
    while (remaining) {
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      REQUIRE(decoder.instruction->id == X86_INS_MOV);
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.op_count == 2);
      REQUIRE(x.addr_size == sizeof(uint64_t));
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      const auto& destination = x.operands[0];
      const auto& source = x.operands[1];
      const bool load = destination.type == X86_OP_REG;
      REQUIRE(destination.size == sizeof(uint64_t));
      REQUIRE(destination.access == CS_AC_WRITE);
      if (load) {
        REQUIRE(source.type == X86_OP_MEM);
      } else {
        REQUIRE(destination.type == X86_OP_MEM);
        REQUIRE((source.type == X86_OP_REG || source.type == X86_OP_IMM));
      }
      const auto& memory = load ? source : destination;
      REQUIRE(memory.size == sizeof(uint64_t));
      REQUIRE(memory.mem.index == X86_REG_INVALID);
      REQUIRE(memory.mem.segment == X86_REG_INVALID);
      const auto base = memory.mem.base;
      REQUIRE((base == X86_REG_RSI || base == X86_REG_RSP));
      CheckMovAccess(decoder, x, base);
      if (load) {
        REQUIRE(source.access == CS_AC_READ);
        const size_t reg = AllocatorGPRIndex(destination.reg);
        if (base == X86_REG_RSI) {
          REQUIRE(phase == 0);
          REQUIRE(loads < kSliceGPRCount);
          REQUIRE(memory.mem.disp == static_cast<int64_t>(GPROffset(loads)));
          register_tokens[reg] = static_cast<int>(loads++);
        } else {
          REQUIRE(phase == 2);
          REQUIRE(memory.mem.disp == local_offset);
          REQUIRE(local_token == 3);
          REQUIRE(reloads++ == 0);
          register_tokens[reg] = local_token;
        }
      } else if (source.type == X86_OP_IMM) {
        REQUIRE(phase == 1);
        REQUIRE(base == X86_REG_RSI);
        REQUIRE(memory.mem.disp == GPROffset(3));
        REQUIRE(static_cast<uint64_t>(source.imm) == kValue);
        REQUIRE(overwrites++ == 0);
      } else {
        REQUIRE(source.size == sizeof(uint64_t));
        REQUIRE(source.access == CS_AC_READ);
        const int token = register_tokens[AllocatorGPRIndex(source.reg)];
        REQUIRE(token >= 0);
        if (base == X86_REG_RSP) {
          REQUIRE(phase == 0);
          REQUIRE(memory.mem.disp == local_offset);
          REQUIRE(token == 3);
          REQUIRE(spills++ == 0);
          local_token = token;
        } else {
          REQUIRE(phase == 2);
          REQUIRE(stores < kCaptureOrder.size());
          const auto expected = kCaptureOrder[stores++];
          REQUIRE(memory.mem.disp == static_cast<int64_t>(GPROffset(expected)));
          REQUIRE(token == static_cast<int>(expected));
          if (expected == 3) {
            REQUIRE(reloads == 1);
          }
        }
      }
    }
    REQUIRE(address == end);
    if (phase == 0) {
      REQUIRE(loads == kSliceGPRCount);
      REQUIRE(spills == 1);
    } else if (phase == 1) {
      REQUIRE(overwrites == 1);
    }
  }
  REQUIRE(stores == kSliceGPRCount);
  REQUIRE(reloads == 1);
}

TEST_CASE("Actual x64 draft preserves an old GPR through spill and overwrite",
          "[cpu][x64-draft-store]") {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  Memory memory;
  REQUIRE(memory.Initialize());
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::make_unique<X64Backend>()));
  REQUIRE(processor.compilation_options().profile ==
          CompilationProfile::kOrdinary);
  REQUIRE(processor.backend()->machine_info()->register_sets[0].count <
          kSliceGPRCount);
  FixtureHIR builder;
  builder.SourceOffset(kSpillPC);
  std::array<Value*, kSliceGPRCount> old_gprs{};
  for (size_t i = 0; i < old_gprs.size(); ++i) {
    old_gprs[i] = builder.LoadContext(GPROffset(i), INT64_TYPE);
  }
  builder.SourceOffset(kSpillPC + 4);
  builder.StoreContext(GPROffset(3), builder.LoadConstantUint64(kValue));
  builder.SourceOffset(kSpillPC + 8);
  for (auto reg : kCaptureOrder) {
    builder.StoreContext(GPROffset(reg), old_gprs[reg]);
  }
  builder.SourceOffset(kSpillPC + 12);
  builder.Return();
  REQUIRE(builder.Finalize());
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&processor);
  // Explicit test recipe: promotion and dead-store removal would change the
  // ordered capture shape. This is not the ordinary translator's pass recipe.
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      processor.backend()->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  for (size_t i = 0; i < old_gprs.size(); ++i) {
    REQUIRE_FALSE(old_gprs[i]->IsConstant());
    REQUIRE(old_gprs[i]->def);
    REQUIRE(old_gprs[i]->def->opcode->num == OPCODE_LOAD_CONTEXT);
    REQUIRE(old_gprs[i]->def->dest == old_gprs[i]);
    REQUIRE(old_gprs[i]->def->src1.offset == GPROffset(i));
    REQUIRE(old_gprs[i]->reg.set ==
            &processor.backend()->machine_info()->register_sets[0]);
    REQUIRE(old_gprs[i]->reg.index >= 0);
    REQUIRE(static_cast<uint32_t>(old_gprs[i]->reg.index) <
            old_gprs[i]->reg.set->count);
  }
  auto old_r3_slot = CheckOldR3Spill(builder, old_gprs[3]);
  XbyakAllocator allocator;
  DraftEmitter emitter(static_cast<X64Backend*>(processor.backend()),
                       &allocator);
  OpaqueDraft draft;
  REQUIRE(emitter.EmitOpaque(builder, draft));
  emitter.CheckDiscarded();
  // Emit assigns numeric local offsets; no native stack address is observed.
  REQUIRE(old_r3_slot->IsConstant());
  REQUIRE(old_r3_slot->type == INT32_TYPE);
  CheckSpillCapture(draft, old_r3_slot->constant.u32);
  // Only decoded MOV operands establish this eight-GPR slice and old-r3
  // provenance. Whole-function relocations remain unresolved, with no ready,
  // placement, registration or invocation. EA arithmetic, complete bank
  // capture, checkpoint publication and production fault recovery are unproved.
}

constexpr uint32_t kScalarEAPC = 0x80003000;

size_t ScalarGPRIndex(const cs_x86_op& operand) {
  REQUIRE(operand.type == X86_OP_REG);
  if (operand.size == sizeof(uint64_t)) {
    return AllocatorGPRIndex(operand.reg);
  }
  REQUIRE(operand.size == sizeof(uint32_t));
  constexpr std::array<unsigned int, X64Emitter::GPR_COUNT> aliases = {
      X86_REG_EBX,  X86_REG_R10D, X86_REG_R11D, X86_REG_R12D,
      X86_REG_R13D, X86_REG_R14D, X86_REG_R15D};
  auto found = std::find(aliases.begin(), aliases.end(), operand.reg);
  REQUIRE(found != aliases.end());
  return static_cast<size_t>(found - aliases.begin());
}

void CheckScalarEA(const OpaqueDraft& draft,
                   const std::array<size_t, 4>& assignments) {
  REQUIRE(draft.source_map.size() == 4);
  for (size_t i = 0; i < draft.source_map.size(); ++i) {
    REQUIRE(draft.source_map[i].guest_address == kScalarEAPC + i * 4);
    REQUIRE(draft.source_map[i].code_offset <= draft.bytes.size());
    if (i) {
      REQUIRE(draft.source_map[i - 1].code_offset <
              draft.source_map[i].code_offset);
    }
  }
  REQUIRE(draft.source_map.back().code_offset -
              draft.source_map.front().code_offset <=
          128);
  // Symbolic expressions, not sample values: Sum is old_r3 - 4 modulo 2^64;
  // Low32 and EA both have zero upper bits after a native 32-bit MOV write.
  // Separate tokens require the actual truncation and zero-extension stages.
  enum class Token { Unknown, OldR3, MinusFour, Sum, Low32, EA };
  std::array<Token, X64Emitter::GPR_COUNT> tokens{};
  size_t loads = 0, constants = 0, adds = 0, truncates = 0, extends = 0;
  size_t overwrites = 0, stores = 0;
  Decoder decoder;
  REQUIRE(decoder.instruction);
  for (size_t phase = 0; phase < 3; ++phase) {
    const size_t begin = draft.source_map[phase].code_offset;
    const size_t end = draft.source_map[phase + 1].code_offset;
    const uint8_t* cursor = draft.bytes.data() + begin;
    size_t remaining = end - begin;
    uint64_t address = begin;  // Numeric offset; no native address is exposed.
    while (remaining) {
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      const bool add = decoder.instruction->id == X86_INS_ADD;
      REQUIRE((add || decoder.instruction->id == X86_INS_MOV));
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.op_count == 2);
      REQUIRE(x.addr_size == sizeof(uint64_t));
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      constexpr uint64_t kAddFlags =
          X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_CF | X86_EFLAGS_MODIFY_OF |
          X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF;
      REQUIRE(x.eflags == (add ? kAddFlags : 0));
      const auto& destination = x.operands[0];
      const auto& source = x.operands[1];
      REQUIRE(destination.access ==
              (add ? (CS_AC_READ | CS_AC_WRITE) : CS_AC_WRITE));
      REQUIRE((source.type == X86_OP_IMM || source.access == CS_AC_READ));
      // Derive every allowed native register effect from these operands.
      // Exact raw aliases distinguish 32-bit reads/writes from 64-bit ones.
      std::vector<unsigned int> expected_reads, expected_writes;
      auto read = [&](unsigned int reg) {
        if (std::find(expected_reads.begin(), expected_reads.end(), reg) ==
            expected_reads.end()) {
          expected_reads.push_back(reg);
        }
      };
      if (destination.type == X86_OP_MEM || source.type == X86_OP_MEM) {
        REQUIRE_FALSE(add);
        const auto& memory =
            destination.type == X86_OP_MEM ? destination : source;
        REQUIRE(memory.size == sizeof(uint64_t));
        REQUIRE(memory.mem.base == X86_REG_RSI);
        REQUIRE(memory.mem.index == X86_REG_INVALID);
        REQUIRE(memory.mem.segment == X86_REG_INVALID);
        read(X86_REG_RSI);
      }
      if (source.type == X86_OP_REG) {
        read(source.reg);
      }
      if (destination.type == X86_OP_REG) {
        if (add) {
          read(destination.reg);
        }
        expected_writes.push_back(destination.reg);
      }
      if (add) {
        expected_writes.push_back(X86_REG_EFLAGS);
      }
      cs_regs reads{}, writes{};
      uint8_t read_count = 0, write_count = 0;
      REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                             &read_count, writes, &write_count) == CS_ERR_OK);
      std::sort(reads, reads + read_count);
      std::sort(writes, writes + write_count);
      std::sort(expected_reads.begin(), expected_reads.end());
      std::sort(expected_writes.begin(), expected_writes.end());
      REQUIRE(read_count == expected_reads.size());
      REQUIRE(write_count == expected_writes.size());
      REQUIRE(std::equal(expected_reads.begin(), expected_reads.end(), reads));
      REQUIRE(
          std::equal(expected_writes.begin(), expected_writes.end(), writes));
      if (add) {
        REQUIRE(phase == 0);
        REQUIRE(destination.size == sizeof(uint64_t));
        const auto dest = ScalarGPRIndex(destination);
        REQUIRE(dest == assignments[1]);
        const auto left = tokens[dest];
        Token right = Token::Unknown;
        if (source.type == X86_OP_IMM) {
          REQUIRE(source.imm == -4);
          REQUIRE(constants == 0);
          right = Token::MinusFour;
        } else {
          REQUIRE(source.size == sizeof(uint64_t));
          right = tokens[ScalarGPRIndex(source)];
          REQUIRE(constants == 1);
        }
        REQUIRE(((left == Token::OldR3 && right == Token::MinusFour) ||
                 (left == Token::MinusFour && right == Token::OldR3)));
        REQUIRE(adds++ == 0);
        tokens[dest] = Token::Sum;
      } else if (destination.type == X86_OP_MEM) {
        REQUIRE(destination.mem.disp ==
                static_cast<int64_t>(GPROffset(phase == 1 ? 3 : 8)));
        REQUIRE(destination.size == sizeof(uint64_t));
        if (phase == 1) {
          REQUIRE(source.type == X86_OP_IMM);
          REQUIRE(static_cast<uint64_t>(source.imm) == kValue);
          REQUIRE(overwrites++ == 0);
        } else {
          REQUIRE(phase == 2);
          REQUIRE(source.size == sizeof(uint64_t));
          REQUIRE(tokens[ScalarGPRIndex(source)] == Token::EA);
          REQUIRE(extends == 1);
          REQUIRE(stores++ == 0);
        }
      } else {
        const auto dest = ScalarGPRIndex(destination);
        if (source.type == X86_OP_MEM) {
          REQUIRE(phase == 0);
          REQUIRE(destination.size == sizeof(uint64_t));
          REQUIRE(source.mem.disp == GPROffset(3));
          REQUIRE(dest == assignments[0]);
          REQUIRE(loads++ == 0);
          tokens[dest] = Token::OldR3;
        } else if (source.type == X86_OP_IMM) {
          // EmitCommutativeBinaryOp may materialize -4 when ADD's allocated
          // destination differs from its old-r3 source. No other constant fits.
          REQUIRE(phase == 0);
          REQUIRE(destination.size == sizeof(uint64_t));
          REQUIRE(source.imm == -4);
          REQUIRE(dest == assignments[1]);
          REQUIRE(constants++ == 0);
          REQUIRE(adds == 0);
          tokens[dest] = Token::MinusFour;
        } else {
          REQUIRE(destination.size == sizeof(uint32_t));
          REQUIRE(source.size == sizeof(uint32_t));
          const auto token = tokens[ScalarGPRIndex(source)];
          // Read the source token before updating an aliased destination.
          // A 32-bit write clears all upper 32 bits, including self-MOV.
          if (phase == 0) {
            REQUIRE(token == Token::Sum);
            REQUIRE(dest == assignments[2]);
            REQUIRE(truncates++ == 0);
            tokens[dest] = Token::Low32;
          } else {
            REQUIRE(phase == 2);
            REQUIRE(overwrites == 1);
            REQUIRE(token == Token::Low32);
            REQUIRE(dest == assignments[3]);
            REQUIRE(extends++ == 0);
            tokens[dest] = Token::EA;
          }
        }
      }
    }
    REQUIRE(address == end);
    if (phase == 0) {
      REQUIRE(loads == 1);
      REQUIRE(adds == 1);
      REQUIRE(truncates == 1);
    } else if (phase == 1) {
      REQUIRE(overwrites == 1);
    }
  }
  REQUIRE(extends == 1);
  REQUIRE(stores == 1);
}

TEST_CASE("Actual x64 draft preserves scalar EA across an input overwrite",
          "[cpu][x64-draft-store]") {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  Memory memory;
  REQUIRE(memory.Initialize());
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::make_unique<X64Backend>()));
  REQUIRE(processor.compilation_options().profile ==
          CompilationProfile::kOrdinary);
  FixtureHIR builder;
  builder.SourceOffset(kScalarEAPC);
  auto old_r3 = builder.LoadContext(GPROffset(3), INT64_TYPE);
  auto displacement = builder.LoadConstantInt64(-4);
  auto sum = builder.Add(old_r3, displacement);
  auto ea = builder.Truncate(sum, INT32_TYPE);
  builder.SourceOffset(kScalarEAPC + 4);
  builder.StoreContext(GPROffset(3), builder.LoadConstantUint64(kValue));
  builder.SourceOffset(kScalarEAPC + 8);
  auto canonical_ea = builder.ZeroExtend(ea, INT64_TYPE);
  builder.StoreContext(GPROffset(8), canonical_ea);
  builder.SourceOffset(kScalarEAPC + 12);
  builder.Return();
  REQUIRE(builder.Finalize());
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&processor);
  // The explicit compile-only recipe retains ordered context effects, using
  // the ordinary backend's real allocator, not the full translator recipe.
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      processor.backend()->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  REQUIRE(builder.locals().empty());
  auto body = builder.first_block();
  REQUIRE(body);
  REQUIRE(body == builder.last_block());
  REQUIRE(body->next == nullptr);
  std::array<Value*, 4> chain = {old_r3, sum, ea, canonical_ea};
  constexpr std::array<TypeName, 4> types = {INT64_TYPE, INT64_TYPE, INT32_TYPE,
                                             INT64_TYPE};
  constexpr std::array<uint32_t, 4> opcodes = {
      OPCODE_LOAD_CONTEXT, OPCODE_ADD, OPCODE_TRUNCATE, OPCODE_ZERO_EXTEND};
  std::array<size_t, 4> assignments{};
  for (size_t i = 0; i < chain.size(); ++i) {
    auto value = chain[i];
    REQUIRE(value->type == types[i]);
    REQUIRE_FALSE(value->IsConstant());
    REQUIRE(value->def);
    REQUIRE(value->def->block == body);
    REQUIRE(value->def->dest == value);
    REQUIRE(value->def->opcode->num == opcodes[i]);
    REQUIRE(value->def->flags == 0);
    REQUIRE(value->reg.set ==
            &processor.backend()->machine_info()->register_sets[0]);
    REQUIRE(value->reg.index >= 0);
    REQUIRE(static_cast<uint32_t>(value->reg.index) < value->reg.set->count);
    assignments[i] = static_cast<size_t>(value->reg.index);
  }
  REQUIRE(old_r3->def->src1.offset == GPROffset(3));
  REQUIRE(sum->def->src1.value == old_r3);
  REQUIRE(sum->def->src2.value == displacement);
  REQUIRE(displacement->type == INT64_TYPE);
  REQUIRE(displacement->IsConstant());
  REQUIRE(displacement->constant.i64 == -4);
  REQUIRE(ea->def->src1.value == sum);
  REQUIRE(canonical_ea->def->src1.value == ea);
  constexpr std::array<uint32_t, 11> body_opcodes = {
      OPCODE_SOURCE_OFFSET, OPCODE_LOAD_CONTEXT,  OPCODE_ADD,
      OPCODE_TRUNCATE,      OPCODE_SOURCE_OFFSET, OPCODE_STORE_CONTEXT,
      OPCODE_SOURCE_OFFSET, OPCODE_ZERO_EXTEND,   OPCODE_STORE_CONTEXT,
      OPCODE_SOURCE_OFFSET, OPCODE_RETURN};
  size_t position = 0, markers = 0;
  for (auto instr = body->instr_head; instr; instr = instr->next, ++position) {
    REQUIRE(position < body_opcodes.size());
    REQUIRE(instr->opcode->num == body_opcodes[position]);
    if (instr->opcode->num == OPCODE_SOURCE_OFFSET) {
      REQUIRE(instr->src1.offset == kScalarEAPC + markers++ * 4);
    } else if (position == 5) {
      REQUIRE(instr->src1.offset == GPROffset(3));
      REQUIRE(instr->src2.value->type == INT64_TYPE);
      REQUIRE(instr->src2.value->IsConstant());
      REQUIRE(instr->src2.value->constant.u64 == kValue);
    } else if (position == 8) {
      REQUIRE(instr->src1.offset == GPROffset(8));
      REQUIRE(instr->src2.value == canonical_ea);
    }
  }
  REQUIRE(position == body_opcodes.size());
  REQUIRE(markers == 4);
  XbyakAllocator allocator;
  DraftEmitter emitter(static_cast<X64Backend*>(processor.backend()),
                       &allocator);
  OpaqueDraft draft;
  REQUIRE(emitter.EmitOpaque(builder, draft));
  emitter.CheckDiscarded();
  CheckScalarEA(draft, assignments);
  // Only uint64(uint32(old_r3 - 4)) across an r3 overwrite is established by
  // the bounded native dataflow oracle. No register-pressure, guest-memory,
  // fault or production checkpoint claim follows. Whole-function AutoGrow
  // relocations remain unresolved; bytes are never published or invoked.
}

}  // namespace
}  // namespace xe::cpu::ppc::test

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
