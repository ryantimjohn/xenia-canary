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

}  // namespace
}  // namespace xe::cpu::ppc::test

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
