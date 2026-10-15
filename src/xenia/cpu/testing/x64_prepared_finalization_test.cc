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
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/backend/x64/x64_tracers.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/compiler/compiler_passes.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DECLARE_bool(emit_source_annotations);
DECLARE_bool(instrument_call_times);
DECLARE_uint32(align_all_basic_blocks);

namespace xe::cpu::testing {
namespace {

using namespace backend::x64;
using PreparedGuestCode = X64CodeCache::PreparedGuestCode;
constexpr uint32_t kUnusedGuestSlot = 0x80004000;
constexpr size_t kRecipeSize = 17;

void CheckCommitted(uint64_t address, size_t size, DWORD protection) {
  REQUIRE(size > 0);
  MEMORY_BASIC_INFORMATION region{};
  REQUIRE(VirtualQuery(reinterpret_cast<const void*>(address), &region,
                       sizeof(region)) == sizeof(region));
  REQUIRE(region.State == MEM_COMMIT);
  REQUIRE(region.Protect == protection);
  const auto base = reinterpret_cast<uint64_t>(region.BaseAddress);
  REQUIRE(address >= base);
  const auto offset = address - base;
  REQUIRE(offset <= region.RegionSize);
  REQUIRE(size <= region.RegionSize - offset);
}

struct Fixture {
  Memory memory;
  Processor processor{&memory, nullptr};
  X64Backend* backend = nullptr;
  X64CodeCache* cache = nullptr;
  uint32_t initial_slot = 0;

  Fixture() {
    REQUIRE(memory.Initialize());
    REQUIRE(processor.Setup(std::make_unique<X64Backend>()));
    REQUIRE(processor.compilation_options().profile ==
            CompilationProfile::kOrdinary);
    backend = static_cast<X64Backend*>(processor.backend());
    cache = backend->code_cache();
    REQUIRE(cache);
    REQUIRE(cache->has_indirection_table());
    cache->CommitExecutableRange(kUnusedGuestSlot, kUnusedGuestSlot + 0x1000);
    // This legacy void API initializes slots internally. Observe the resulting
    // committed storage before this fixture reads any slot itself.
    CheckCommitted(kUnusedGuestSlot, sizeof(uint32_t), PAGE_READWRITE);
    initial_slot = *slot();
  }

  const uint32_t* slot() const {
    return reinterpret_cast<const uint32_t*>(uintptr_t{kUnusedGuestSlot});
  }

  uint64_t synthetic_target() const {
    // Numeric address within rel32 range, never dereferenced or invoked and
    // never resolved as a guest function. No external function is called.
    return cache->execute_base_address() + cache->total_size() - 16;
  }

  void CheckUnpublished(uint64_t pc, size_t size) {
    REQUIRE(size > 0);
    for (auto probe : {pc, pc + size - 1}) {
      CHECK(cache->LookupFunction(probe) == nullptr);
      CHECK(cache->LookupUnwindInfo(probe) == nullptr);
      DWORD64 image_base = 0;
      CHECK(RtlLookupFunctionEntry(probe, &image_base, nullptr) == nullptr);
    }
    CHECK(*slot() == initial_slot);
  }

  void CheckAborted(uint64_t pc, size_t size) {
    CheckUnpublished(pc, size);
    CheckCommitted(pc, size, PAGE_EXECUTE_READWRITE);
    // Cache lifetime remains live; only a read-only numeric observation is
    // retained, never the callback's write pointer or its scoped view.
    const auto bytes = reinterpret_cast<const uint8_t*>(pc);
    CHECK(std::all_of(bytes, bytes + size,
                      [](uint8_t byte) { return byte == 0xCC; }));
  }
};

// A fixed synthetic recipe uses the real X64Emitter's inherited Xbyak
// generator and relocation state. It does not translate HIR, call Emplace,
// return an entry, publish metadata or execute any emitted instruction.
class FinalizationEmitter final : public X64Emitter {
 public:
  using X64Emitter::X64Emitter;
  ~FinalizationEmitter() override { reset(); }

  std::vector<uint8_t> Build(uint64_t external, bool undefined = false) {
    REQUIRE(getSize() == 0);
    REQUIRE_FALSE(hasUndefinedLabel());
    REQUIRE_FALSE(isCalledCalcJmpAddress());
    REQUIRE_FALSE(allocator_->useProtect());
    ready_attempts_ = 0;
    L(back_);
    jmp(forward_, T_NEAR);  // Pending internal LasIs relocation.
    call(reinterpret_cast<const void*>(external));  // Pending external Labs.
    jmp(back_, T_NEAR);  // Already-defined backward relative reference.
    if (!undefined) {
      L(forward_);
    }
    nop();
    ret();
    REQUIRE(getSize() == kRecipeSize);
    REQUIRE(hasUndefinedLabel() == undefined);
    scratch_ = top_;
    scratch_bytes_.assign(top_, top_ + getSize());
    REQUIRE(std::all_of(scratch_bytes_.begin() + 1, scratch_bytes_.begin() + 5,
                        [](uint8_t byte) { return byte == 0; }));
    REQUIRE(std::all_of(scratch_bytes_.begin() + 6, scratch_bytes_.begin() + 10,
                        [](uint8_t byte) { return byte == 0; }));
    return scratch_bytes_;  // Opaque byte ownership, never a callable pointer.
  }

  void Finalize(PreparedGuestCode& prepared, uint64_t execute_pc) {
    REQUIRE(prepared.state() == PreparedGuestCode::State::kPrepared);
    REQUIRE(prepared.code_size() == getSize());
    REQUIRE(prepared.code_size() == kRecipeSize);
    REQUIRE(top_ == scratch_);
    REQUIRE_FALSE(isCalledCalcJmpAddress());
    REQUIRE(ready_attempts_ == 0);
    REQUIRE_FALSE(allocator_->useProtect());
    // Xbyak uses top_ for both writing and relocation arithmetic. This test
    // explicitly rejects split mappings instead of assuming a shared base.
    REQUIRE(reinterpret_cast<uint64_t>(prepared.write_data()) == execute_pc);
    CheckCommitted(execute_pc, prepared.allocation_size(),
                   PAGE_EXECUTE_READWRITE);
    REQUIRE(std::memcmp(prepared.write_data(), scratch_bytes_.data(),
                        scratch_bytes_.size()) == 0);
    ++ready_attempts_;
    {
      RestoreTop restore(top_);
      top_ = prepared.write_data();
      ready();  // Exactly one attempt; useProtect=false means no permission
                // API.
      REQUIRE(isCalledCalcJmpAddress());
    }  // Restore scratch before any reset, destructor or callback exit.
    CheckScratch(true);
  }

  void CheckScratch(bool finalized) {
    REQUIRE(top_ == scratch_);
    REQUIRE(getSize() == scratch_bytes_.size());
    REQUIRE(std::memcmp(top_, scratch_bytes_.data(), getSize()) == 0);
    REQUIRE(isCalledCalcJmpAddress() == finalized);
    REQUIRE(ready_attempts_ == 1);
  }

  void Discard() {
    REQUIRE(top_ == scratch_);
    reset();  // Drops relocation state and clears defined labels.
    // Xbyak does not track an undefined label in its defined-label pointer
    // list. Clear that retained object explicitly after dropping its record.
    back_.clear();
    forward_.clear();
    scratch_bytes_.clear();
    scratch_ = nullptr;
    REQUIRE(getSize() == 0);
    REQUIRE_FALSE(hasUndefinedLabel());
    REQUIRE_FALSE(isCalledCalcJmpAddress());
    REQUIRE(back_.getId() == 0);
    REQUIRE(forward_.getId() == 0);
  }

 private:
  struct RestoreTop {
    explicit RestoreTop(uint8_t*& top) : top(top), original(top) {}
    ~RestoreTop() { top = original; }
    uint8_t*& top;
    uint8_t* const original;
    RestoreTop(const RestoreTop&) = delete;
    RestoreTop& operator=(const RestoreTop&) = delete;
  };
  Xbyak::Label back_, forward_;
  uint8_t* scratch_ = nullptr;
  std::vector<uint8_t> scratch_bytes_;
  size_t ready_attempts_ = 0;
};

void CheckResolved(const PreparedGuestCode& prepared, uint64_t pc,
                   uint64_t external, const std::vector<uint8_t>& scratch) {
  const auto bytes = prepared.write_data();
  REQUIRE(prepared.code_size() == kRecipeSize);
  // Only the two pending rel32 fields may change. The backward displacement,
  // opcodes and terminal bytes must survive copying and finalization exactly.
  for (size_t i = 0; i < kRecipeSize; ++i) {
    if ((i >= 1 && i < 5) || (i >= 6 && i < 10)) {
      continue;
    }
    REQUIRE(bytes[i] == scratch[i]);
  }
  REQUIRE(bytes[0] == 0xE9);
  REQUIRE(bytes[5] == 0xE8);
  REQUIRE(bytes[10] == 0xE9);
  REQUIRE(bytes[15] == 0x90);
  REQUIRE(bytes[16] == 0xC3);
  int32_t forward = 0, call = 0, backward = 0;
  std::memcpy(&forward, bytes + 1, sizeof(forward));
  std::memcpy(&call, bytes + 6, sizeof(call));
  std::memcpy(&backward, bytes + 11, sizeof(backward));
  REQUIRE(forward == 10);
  REQUIRE(static_cast<int64_t>(pc + 10) + call ==
          static_cast<int64_t>(external));
  REQUIRE(backward == -15);
  // Padding and private unwind storage receive no relocation writes.
  REQUIRE(std::all_of(bytes + prepared.code_size(),
                      bytes + prepared.allocation_size(),
                      [](uint8_t byte) { return byte == 0xCC; }));
  csh decoder = 0;
  REQUIRE(cs_open(CS_ARCH_X86, CS_MODE_64, &decoder) == CS_ERR_OK);
  struct CloseDecoder {
    csh& handle;
    ~CloseDecoder() { cs_close(&handle); }
  } close{decoder};
  REQUIRE(cs_option(decoder, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK);
  cs_insn* decoded = cs_malloc(decoder);
  REQUIRE(decoded);
  struct FreeInstruction {
    cs_insn* instruction;
    ~FreeInstruction() { cs_free(instruction, 1); }
  } free{decoded};
  constexpr std::array<unsigned int, 5> ids = {
      X86_INS_JMP, X86_INS_CALL, X86_INS_JMP, X86_INS_NOP, X86_INS_RET};
  constexpr std::array<size_t, 5> sizes = {5, 5, 5, 1, 1};
  const std::array<uint64_t, 3> targets = {pc + 15, external, pc};
  const uint8_t* cursor = bytes;
  size_t remaining = prepared.code_size();
  uint64_t address = pc;  // Numeric decoding base only, never an entry.
  for (size_t i = 0; i < ids.size(); ++i) {
    const auto start = address;
    REQUIRE(cs_disasm_iter(decoder, &cursor, &remaining, &address, decoded));
    REQUIRE(decoded->id == ids[i]);
    REQUIRE(decoded->address == start);
    REQUIRE(decoded->size == sizes[i]);
    REQUIRE(decoded->detail);
    const auto& x = decoded->detail->x86;
    REQUIRE(x.eflags == 0);
    REQUIRE(x.addr_size == sizeof(uint64_t));
    for (auto prefix : x.prefix) {
      REQUIRE(prefix == 0);
    }
    REQUIRE(x.op_count == (i < targets.size() ? 1 : 0));
    if (i < targets.size()) {
      REQUIRE(x.operands[0].type == X86_OP_IMM);
      REQUIRE(static_cast<uint64_t>(x.operands[0].imm) == targets[i]);
    }
  }
  REQUIRE(remaining == 0);
  REQUIRE(cursor == bytes + prepared.code_size());
  REQUIRE(address == pc + prepared.code_size());
}

backend::EmitFunctionInfo CodeInfo(size_t size) {
  backend::EmitFunctionInfo info{};
  info.code_size.body = size;
  info.code_size.total = size;
  return info;
}

TEST_CASE("Unpublished x64 destination resolves retained relocation state",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  Fixture fixture;
  XbyakAllocator allocator;
  REQUIRE_FALSE(allocator.useProtect());
  FinalizationEmitter emitter(fixture.backend, &allocator);
  const auto target = fixture.synthetic_target();
  const auto scratch = emitter.Build(target);
  uint64_t pc = 0;
  size_t span = 0, callbacks = 0;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      scratch.data(), CodeInfo(scratch.size()),
      [&](PreparedGuestCode& prepared) {
        ++callbacks;
        pc = fixture.cache->execute_base_address() + prepared.execute_offset();
        span = prepared.allocation_size();
        fixture.CheckUnpublished(pc, span);
        emitter.Finalize(prepared, pc);
        CheckResolved(prepared, pc, target, scratch);
        fixture.CheckUnpublished(pc, span);
      }));
  REQUIRE(callbacks == 1);
  emitter.CheckScratch(true);
  emitter.Discard();
  fixture.CheckAborted(pc, span);
  // A subsequent reservation consumes fresh storage, including private unwind
  // space; no rewind/reuse occurs when the first prepared span is aborted.
  uint64_t next_pc = 0;
  size_t next_span = 0;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      scratch.data(), CodeInfo(scratch.size()),
      [&](PreparedGuestCode& prepared) {
        next_pc =
            fixture.cache->execute_base_address() + prepared.execute_offset();
        next_span = prepared.allocation_size();
        REQUIRE(next_pc == pc + span);
        fixture.CheckUnpublished(next_pc, next_span);
      }));
  fixture.CheckAborted(pc, span);
  fixture.CheckAborted(next_pc, next_span);
  // This fixed encoding proves destination relocation only. It establishes
  // no HIR producer, guest ABI, live capture or whole-emitter correctness.
}

TEST_CASE("Unpublished x64 finalization failure restores and reuses encoder",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  Fixture fixture;
  XbyakAllocator allocator;
  REQUIRE_FALSE(allocator.useProtect());
  FinalizationEmitter emitter(fixture.backend, &allocator);
  const auto target = fixture.synthetic_target();
  const auto failed_scratch = emitter.Build(target, true);
  uint64_t failed_pc = 0;
  size_t failed_span = 0, callbacks = 0;
  bool caught = false;
  try {
    fixture.cache->WithPreparedGuestCode(
        failed_scratch.data(), CodeInfo(failed_scratch.size()),
        [&](PreparedGuestCode& prepared) {
          ++callbacks;
          failed_pc =
              fixture.cache->execute_base_address() + prepared.execute_offset();
          failed_span = prepared.allocation_size();
          fixture.CheckUnpublished(failed_pc, failed_span);
          emitter.Finalize(prepared, failed_pc);
        });
  } catch (const Xbyak::Error& error) {
    REQUIRE(static_cast<int>(error) == Xbyak::ERR_LABEL_IS_NOT_FOUND);
    caught = true;
  }
  REQUIRE(caught);
  REQUIRE(callbacks == 1);
  emitter.CheckScratch(false);
  emitter.Discard();
  fixture.CheckAborted(failed_pc, failed_span);
  const auto scratch = emitter.Build(target);
  uint64_t pc = 0;
  size_t span = 0;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      scratch.data(), CodeInfo(scratch.size()),
      [&](PreparedGuestCode& prepared) {
        pc = fixture.cache->execute_base_address() + prepared.execute_offset();
        span = prepared.allocation_size();
        REQUIRE(pc == failed_pc + failed_span);
        emitter.Finalize(prepared, pc);
        CheckResolved(prepared, pc, target, scratch);
        fixture.CheckUnpublished(pc, span);
      }));
  emitter.CheckScratch(true);
  emitter.Discard();
  fixture.CheckAborted(failed_pc, failed_span);
  fixture.CheckAborted(pc, span);
}

namespace real_hir {

using namespace hir;
constexpr uint32_t kPC = 0x80005000;
constexpr uint64_t kValue = 0x12345678;
constexpr size_t kMaxBytes = 4096;

size_t GPROffset(size_t reg) {
  return offsetof(ppc::PPCContext, r) + reg * sizeof(uint64_t);
}

class CurrentHIR final : public HIRBuilder {
 public:
  CurrentHIR() : previous_(GetCurrent()) { MakeCurrent(); }
  ~CurrentHIR() override {
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

struct Draft {
  X64Emitter::DraftInfo info{};
  std::vector<uint8_t> bytes;
  std::vector<SourceMapEntry> source_map;
};

class HIREmitter final : public X64Emitter {
 public:
  using X64Emitter::X64Emitter;
  Draft Snapshot(const PreparedDraft& view, const uint8_t* original_scratch) {
    REQUIRE_FALSE(allocator_->useProtect());
    REQUIRE(getCode() == original_scratch);
    REQUIRE(getCode() != view.code().data());
    REQUIRE(getSize() > 0);
    REQUIRE(getSize() <= kMaxBytes);
    REQUIRE(view.info().code_size.total == getSize());
    REQUIRE(view.code().size() == getSize());
    REQUIRE(epilog_label_ == nullptr);
    REQUIRE_FALSE(hasUndefinedLabel());
    REQUIRE(isCalledCalcJmpAddress());
    REQUIRE(stack_size_ == view.info().stack_size);
    Draft draft;
    draft.info = view.info();
    draft.bytes.assign(top_, top_ + getSize());
    source_map_arena_.CloneContents(&draft.source_map);
    REQUIRE(draft.source_map.size() == view.source_map().size());
    for (size_t i = 0; i < draft.source_map.size(); ++i) {
      REQUIRE(draft.source_map[i].guest_address ==
              view.source_map()[i].guest_address);
      REQUIRE(draft.source_map[i].hir_offset ==
              view.source_map()[i].hir_offset);
      REQUIRE(draft.source_map[i].code_offset ==
              view.source_map()[i].code_offset);
    }
    return draft;  // Owned unresolved scratch bytes and scalar/map copies only.
  }

  void CheckDiscarded() {
    REQUIRE(getSize() == 0);
    REQUIRE_FALSE(hasUndefinedLabel());
    REQUIRE_FALSE(isCalledCalcJmpAddress());
    REQUIRE(epilog_label_ == nullptr);
    REQUIRE(tail_code_.empty());
    REQUIRE(label_cache_.empty());
    REQUIRE(guest_module_ == nullptr);
    REQUIRE(current_guest_function_ == 0);
    REQUIRE(current_instr_ == nullptr);
    REQUIRE(debug_info_ == nullptr);
    REQUIRE(debug_info_flags_ == 0);
    REQUIRE(trace_data_ == nullptr);
    REQUIRE(stack_size_ == 0);
    REQUIRE_FALSE(synchronize_stack_on_next_instruction_);
    REQUIRE(mxcsr_mode_ == MXCSRMode::Unknown);
    std::vector<SourceMapEntry> retained;
    source_map_arena_.CloneContents(&retained);
    REQUIRE(retained.empty());
  }
};

using PreparedDraft = X64Emitter::PreparedDraft;
static_assert(!std::is_copy_constructible_v<PreparedDraft>);
static_assert(!std::is_move_constructible_v<PreparedDraft>);
static_assert(
    std::is_same_v<decltype(std::declval<const PreparedDraft&>().code()),
                   std::span<const uint8_t>>);
static_assert(
    std::is_same_v<decltype(std::declval<const PreparedDraft&>().source_map()),
                   std::span<const SourceMapEntry>>);

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

void CheckEffects(Decoder& decoder, unsigned int read, unsigned int write) {
  cs_regs reads{}, writes{};
  uint8_t read_count = 0, write_count = 0;
  REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                         &read_count, writes, &write_count) == CS_ERR_OK);
  REQUIRE(read_count == (read == X86_REG_INVALID ? 0 : 1));
  REQUIRE(write_count == (write == X86_REG_INVALID ? 0 : 1));
  if (read_count) {
    REQUIRE(reads[0] == read);
  }
  if (write_count) {
    REQUIRE(writes[0] == write);
  }
}

void CheckMetadata(const Draft& draft,
                   const std::array<uint32_t, 4>& marker_offsets) {
  const auto& info = draft.info;
  REQUIRE(info.code_size.total == draft.bytes.size());
  REQUIRE(info.code_size.total <= kMaxBytes);
  REQUIRE(info.code_size.prolog > 0);
  REQUIRE(info.code_size.prolog < 256);
  REQUIRE(info.code_size.prolog <= info.code_size.total);
  const auto after_prolog = info.code_size.total - info.code_size.prolog;
  REQUIRE(info.code_size.body <= after_prolog);
  const auto after_body = after_prolog - info.code_size.body;
  REQUIRE(info.code_size.epilog > 0);
  REQUIRE(info.code_size.epilog <= after_body);
  REQUIRE(info.code_size.tail == after_body - info.code_size.epilog);
  REQUIRE(info.prolog_stack_alloc_offset > 0);
  REQUIRE(info.prolog_stack_alloc_offset <= info.code_size.prolog);
  REQUIRE(info.prolog_stack_alloc_offset < 256);
  REQUIRE(info.stack_size == StackLayout::GUEST_STACK_SIZE);
  REQUIRE((info.stack_size + 8) % 16 == 0);
  REQUIRE(info.stack_size % 8 == 0);
  REQUIRE(info.stack_size / 8 < 65536);
  // These are complete emitter unwind inputs, not serialized UNWIND_INFO or
  // installed runtime-table metadata. The native encoder currently couples
  // encoding to registration and is deliberately not called here.
  REQUIRE(draft.source_map.size() == marker_offsets.size());
  const auto epilog = info.code_size.prolog + info.code_size.body;
  for (size_t i = 0; i < draft.source_map.size(); ++i) {
    const auto& entry = draft.source_map[i];
    REQUIRE(entry.guest_address == kPC + i * 4);
    REQUIRE(entry.hir_offset == marker_offsets[i]);
    REQUIRE(entry.code_offset >= info.code_size.prolog);
    REQUIRE(entry.code_offset <= epilog);
    if (i) {
      REQUIRE(draft.source_map[i - 1].code_offset < entry.code_offset);
    }
  }
  REQUIRE(draft.source_map.back().code_offset == epilog);
}

void CheckIntervals(const Draft& draft, const PreparedDraft& prepared,
                    uint64_t pc, size_t assignment) {
  constexpr std::array<unsigned int, X64Emitter::GPR_COUNT> registers = {
      X86_REG_RBX, X86_REG_R10, X86_REG_R11, X86_REG_R12,
      X86_REG_R13, X86_REG_R14, X86_REG_R15};
  REQUIRE(assignment < registers.size());
  const auto condition = registers[assignment];
  Decoder decoder;
  REQUIRE(decoder.instruction);
  for (size_t phase = 0; phase < 3; ++phase) {
    const size_t begin = draft.source_map[phase].code_offset;
    const size_t end = draft.source_map[phase + 1].code_offset;
    REQUIRE(begin < end);
    REQUIRE(end - begin <= 32);
    const uint8_t* cursor = prepared.code().data() + begin;
    size_t remaining = end - begin;
    uint64_t address = pc + begin;
    size_t decoded = 0;
    while (remaining) {
      const auto instruction_offset = address - pc;
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.addr_size == sizeof(uint64_t));
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      if (phase != 1) {
        REQUIRE(decoded == 0);
        REQUIRE(decoder.instruction->id == X86_INS_MOV);
        REQUIRE(x.eflags == 0);
        REQUIRE(x.op_count == 2);
        const auto& destination = x.operands[0];
        const auto& source = x.operands[1];
        REQUIRE(destination.size == sizeof(uint64_t));
        REQUIRE(destination.access == CS_AC_WRITE);
        const auto& memory = phase == 2 ? destination : source;
        REQUIRE(memory.type == X86_OP_MEM);
        REQUIRE(memory.size == sizeof(uint64_t));
        REQUIRE(memory.mem.base == X86_REG_RSI);
        REQUIRE(memory.mem.index == X86_REG_INVALID);
        REQUIRE(memory.mem.segment == X86_REG_INVALID);
        REQUIRE(memory.mem.disp ==
                static_cast<int64_t>(GPROffset(phase == 2 ? 3 : 4)));
        if (phase == 2) {
          REQUIRE(source.type == X86_OP_IMM);
          REQUIRE(static_cast<uint64_t>(source.imm) == kValue);
          CheckEffects(decoder, X86_REG_RSI, X86_REG_INVALID);
        } else {
          REQUIRE(destination.type == X86_OP_REG);
          REQUIRE(destination.reg == condition);
          REQUIRE(source.access == CS_AC_READ);
          CheckEffects(decoder, X86_REG_RSI, condition);
        }
      } else if (decoded == 0) {
        REQUIRE(decoder.instruction->id == X86_INS_TEST);
        REQUIRE(x.op_count == 2);
        constexpr uint64_t kTestFlags =
            X86_EFLAGS_RESET_OF | X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF |
            X86_EFLAGS_UNDEFINED_AF | X86_EFLAGS_MODIFY_PF |
            X86_EFLAGS_RESET_CF;
        REQUIRE(x.eflags == kTestFlags);
        for (size_t i = 0; i < 2; ++i) {
          REQUIRE(x.operands[i].type == X86_OP_REG);
          REQUIRE(x.operands[i].reg == condition);
          REQUIRE(x.operands[i].size == sizeof(uint64_t));
          REQUIRE(x.operands[i].access == CS_AC_READ);
        }
        CheckEffects(decoder, condition, X86_REG_EFLAGS);
      } else {
        REQUIRE(decoded == 1);
        REQUIRE(decoder.instruction->id == X86_INS_JNE);
        REQUIRE(decoder.instruction->size == 6);
        REQUIRE(x.eflags == X86_EFLAGS_TEST_ZF);
        REQUIRE(x.op_count == 1);
        REQUIRE(x.operands[0].type == X86_OP_IMM);
        const auto epilog =
            draft.info.code_size.prolog + draft.info.code_size.body;
        REQUIRE(static_cast<uint64_t>(x.operands[0].imm) == pc + epilog);
        REQUIRE(pc + epilog > decoder.instruction->address);
        REQUIRE(x.encoding.imm_size == sizeof(int32_t));
        REQUIRE(x.encoding.imm_offset + x.encoding.imm_size ==
                decoder.instruction->size);
        const size_t displacement_offset =
            instruction_offset + x.encoding.imm_offset;
        REQUIRE(displacement_offset + sizeof(int32_t) <= end);
        int32_t pending = 0, resolved = 0;
        std::memcpy(&pending, draft.bytes.data() + displacement_offset,
                    sizeof(pending));
        std::memcpy(&resolved, prepared.code().data() + displacement_offset,
                    sizeof(resolved));
        REQUIRE(pending ==
                0);  // Actual AutoGrow forward-label record was pending.
        REQUIRE(resolved > 0);  // The real branch skips the fallthrough store.
        REQUIRE(static_cast<int64_t>(address) + resolved ==
                static_cast<int64_t>(pc + epilog));
        CheckEffects(decoder, X86_REG_EFLAGS, X86_REG_INVALID);
      }
      ++decoded;
    }
    REQUIRE(address == pc + end);
    REQUIRE(decoded == (phase == 1 ? 2 : 1));
  }
  REQUIRE(std::all_of(prepared.storage().begin() + prepared.code().size(),
                      prepared.storage().end(),
                      [](uint8_t byte) { return byte == 0xCC; }));
}

TEST_CASE("Real HIR x64 emission finalizes in unpublished prepared storage",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  REQUIRE(cvars::align_all_basic_blocks == 0);
  Fixture fixture;
  XbyakAllocator allocator;
  REQUIRE_FALSE(allocator.useProtect());
  HIREmitter emitter(fixture.backend, &allocator);
  const size_t unwind_inputs_size = fixture.cache->QueryUnwindStorageSize();
  REQUIRE(unwind_inputs_size > 0);
  REQUIRE(unwind_inputs_size <= kMaxBytes);
  uint64_t previous_pc = 0;
  size_t previous_span = 0;
  // Normal inspection, a throwing inspection and normal reuse use the same
  // production hook/emitter with three fresh, current HIR builders.
  struct InspectionFailure {};
  for (size_t attempt = 0; attempt < 3; ++attempt) {
    CurrentHIR builder;
    builder.SourceOffset(kPC);
    auto condition = builder.LoadContext(GPROffset(4), INT64_TYPE);
    builder.SourceOffset(kPC + 4);
    builder.ReturnTrue(condition);
    builder.SourceOffset(kPC + 8);
    builder.StoreContext(GPROffset(3), builder.LoadConstantUint64(kValue));
    builder.SourceOffset(kPC + 12);
    builder.Return();
    REQUIRE(builder.Finalize());
    REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
    namespace passes = compiler::passes;
    compiler::Compiler compiler(&fixture.processor);
    // Same explicit validation/CFG/real-RA/finalization recipe as draft cases;
    // this does not claim the ordinary translator's complete pass pipeline.
    compiler.AddPass(std::make_unique<passes::ValidationPass>());
    compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
    compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
        fixture.backend->machine_info()));
    compiler.AddPass(std::make_unique<passes::ValidationPass>());
    compiler.AddPass(std::make_unique<passes::FinalizationPass>());
    REQUIRE(compiler.Compile(&builder));
    REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
    REQUIRE(builder.locals().empty());
    REQUIRE_FALSE(condition->IsConstant());
    REQUIRE(condition->type == INT64_TYPE);
    REQUIRE(condition->def);
    REQUIRE(condition->def->dest == condition);
    REQUIRE(condition->def->opcode->num == OPCODE_LOAD_CONTEXT);
    REQUIRE(condition->def->src1.offset == GPROffset(4));
    REQUIRE(condition->reg.set ==
            &fixture.backend->machine_info()->register_sets[0]);
    REQUIRE(condition->reg.index >= 0);
    REQUIRE(static_cast<uint32_t>(condition->reg.index) <
            condition->reg.set->count);
    auto body = builder.first_block();
    REQUIRE(body);
    REQUIRE(condition->def->block == body);
    REQUIRE(body->next);
    REQUIRE(body->next == builder.last_block());
    REQUIRE(body->next->next == nullptr);
    constexpr std::array<uint32_t, 8> opcodes = {
        OPCODE_SOURCE_OFFSET, OPCODE_LOAD_CONTEXT,  OPCODE_SOURCE_OFFSET,
        OPCODE_RETURN_TRUE,   OPCODE_SOURCE_OFFSET, OPCODE_STORE_CONTEXT,
        OPCODE_SOURCE_OFFSET, OPCODE_RETURN};
    std::array<uint32_t, 4> marker_offsets{};
    size_t position = 0, markers = 0;
    for (auto block = body; block; block = block->next) {
      for (auto instr = block->instr_head; instr;
           instr = instr->next, ++position) {
        REQUIRE(position < opcodes.size());
        REQUIRE(instr->opcode->num == opcodes[position]);
        if (instr->opcode->num == OPCODE_SOURCE_OFFSET) {
          REQUIRE(markers < marker_offsets.size());
          REQUIRE(instr->src1.offset == kPC + markers * 4);
          marker_offsets[markers++] =
              uint32_t(block->ordinal << 16) | instr->ordinal;
        } else if (position == 5) {
          REQUIRE(instr->src1.offset == GPROffset(3));
          REQUIRE(instr->src2.value->IsConstant());
          REQUIRE(instr->src2.value->type == INT64_TYPE);
          REQUIRE(instr->src2.value->constant.u64 == kValue);
        } else if (position == 3) {
          REQUIRE(instr->src1.value == condition);
          REQUIRE(block == body);
        }
      }
    }
    REQUIRE(position == opcodes.size());
    REQUIRE(markers == marker_offsets.size());
    emitter.CheckDiscarded();
    const auto original_scratch = emitter.getCode();
    Draft draft;
    uint64_t pc = 0;
    size_t span = 0, callbacks = 0;
    bool completed = false, caught = false;
    try {
      completed = emitter.WithPreparedDraft(builder, [&](const PreparedDraft&
                                                             prepared) {
        ++callbacks;
        pc = fixture.cache->execute_base_address() + prepared.execute_offset();
        span = prepared.allocation_size();
        REQUIRE(reinterpret_cast<uint64_t>(prepared.code().data()) == pc);
        CheckCommitted(pc, span, PAGE_EXECUTE_READWRITE);
        draft = emitter.Snapshot(prepared, original_scratch);
        CheckMetadata(draft, marker_offsets);
        REQUIRE(span ==
                xe::round_up(draft.info.code_size.total, size_t{16}, false) +
                    xe::round_up(unwind_inputs_size, size_t{16}, false));
        if (attempt) {
          REQUIRE(pc == previous_pc + previous_span);
        }
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_inputs_size);
        CheckIntervals(draft, prepared, pc,
                       static_cast<size_t>(condition->reg.index));
        CheckMetadata(draft, marker_offsets);
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_inputs_size);
        size_t recursive_callbacks = 0;
        REQUIRE_FALSE(emitter.WithPreparedDraft(
            builder, [&](const PreparedDraft&) { ++recursive_callbacks; }));
        REQUIRE(recursive_callbacks == 0);
        void* rejected_entry = reinterpret_cast<void*>(uintptr_t{1});
        size_t rejected_size = 1;
        std::vector<SourceMapEntry> rejected_map;
        // Active-transaction rejection precedes GuestFunction access.
        REQUIRE_FALSE(emitter.Emit(nullptr, &builder, 0, nullptr,
                                   &rejected_entry, &rejected_size,
                                   &rejected_map));
        REQUIRE(rejected_entry == nullptr);
        REQUIRE(rejected_size == 0);
        REQUIRE(rejected_map.empty());
        REQUIRE(emitter.getCode() == original_scratch);
        REQUIRE(emitter.isCalledCalcJmpAddress());
        fixture.CheckUnpublished(pc, span);
        if (attempt == 1) {
          throw InspectionFailure{};
        }
      });
    } catch (const InspectionFailure&) {
      caught = true;
    }
    REQUIRE(caught == (attempt == 1));
    REQUIRE(completed == (attempt != 1));
    REQUIRE(callbacks == 1);
    REQUIRE(emitter.getCode() == original_scratch);
    emitter.CheckDiscarded();
    CheckMetadata(draft, marker_offsets);  // Owned metadata survives cleanup.
    fixture.CheckAborted(pc, span);
    REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_inputs_size);
    if (attempt) {
      fixture.CheckAborted(previous_pc, previous_span);
    }
    previous_pc = pc;
    previous_span = span;
    if (attempt == 0) {
#if XE_X64_PROFILER_AVAILABLE == 1
      {
        struct RestoreProfiling {
          const bool original = cvars::instrument_call_times;
          ~RestoreProfiling() { cvars::instrument_call_times = original; }
        } restore;
        cvars::instrument_call_times = true;
        size_t profiling_callbacks = 0;
        REQUIRE_FALSE(emitter.WithPreparedDraft(
            builder, [&](const PreparedDraft&) { ++profiling_callbacks; }));
        REQUIRE(profiling_callbacks == 0);
        REQUIRE(emitter.getCode() == original_scratch);
        emitter.CheckDiscarded();
        fixture.CheckAborted(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_inputs_size);
        // The next exact reservation offset also proves no span was consumed.
        // The private profiler map is unobserved; the production gate rejects
        // before protected Emit can call its mutating record accessor.
      }
      REQUIRE_FALSE(cvars::instrument_call_times);
#endif
      // Orphan/malformed checkpoint HIR is rejected before any emission or
      // reservation. The next transaction's exact offset checks nonallocation.
      CurrentHIR checkpoint;
      checkpoint.PPCCheckpointRequest(1, kPC, 0,
                                      checkpoint.LoadConstantInt64(0));
      size_t rejected_callbacks = 0;
      REQUIRE_FALSE(emitter.WithPreparedDraft(
          checkpoint, [&](const PreparedDraft&) { ++rejected_callbacks; }));
      REQUIRE(rejected_callbacks == 0);
      REQUIRE_FALSE(emitter.WithPreparedDraft(checkpoint, {}));
      emitter.CheckDiscarded();
      fixture.CheckAborted(pc, span);
      REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_inputs_size);
    }
  }
  // The private global Win32 unwind-table counter is unobserved. Existing
  // function/native-unwind lookups and slot checks establish only the tested
  // unpublished ranges. No native unwind encoder, registration, callable
  // entry, guest invocation, complete bank capture or ABI claim is provided.
}

}  // namespace real_hir

namespace canonical_gpr {

using namespace hir;
constexpr uint32_t kPC = 0x80006000;
constexpr size_t kCount = 32;

size_t CaptureGPR(size_t position) {
  REQUIRE(position < kCount);
  // Keep old r3 furthest from its definition, across the sentinel overwrite.
  return position == kCount - 1 ? 3 : position < 3 ? position : position + 1;
}

enum class Kind { Input, Spill, Reload, Overwrite, Output };
struct Move {
  Kind kind;
  size_t phase;
  size_t guest;
  const Value* value;
  Value* slot;
};
struct Plan {
  std::array<Value*, kCount> slots{};
  std::array<uint32_t, 4> markers{};
  std::vector<Move> moves;
  size_t spill_count = 0;
};

void CheckAllocated(const Value* value, const backend::MachineInfo& machine) {
  REQUIRE(value);
  REQUIRE(value->type == INT64_TYPE);
  REQUIRE_FALSE(value->IsConstant());
  REQUIRE(value->def);
  REQUIRE(value->def->dest == value);
  REQUIRE(value->reg.set == &machine.register_sets[0]);
  REQUIRE(value->reg.index >= 0);
  REQUIRE(static_cast<uint32_t>(value->reg.index) < value->reg.set->count);
}

Plan CheckHIR(HIRBuilder& builder, const std::array<Value*, kCount>& inputs,
              const backend::MachineInfo& machine) {
  REQUIRE(machine.register_sets[0].count == X64Emitter::GPR_COUNT);
  REQUIRE(X64Emitter::GPR_COUNT < kCount);
  Plan plan;
  auto body = builder.first_block();
  REQUIRE(body);
  REQUIRE(body->instr_tail);
  REQUIRE(body->instr_tail->opcode->num == OPCODE_RETURN);
  // As in the approved eight-GPR case, RA appends and relocates local ops
  // after Return ended the body, leaving one exact empty terminal scaffold.
  auto scaffold = body->next;
  REQUIRE(scaffold);
  REQUIRE(scaffold == builder.last_block());
  REQUIRE(scaffold->prev == body);
  REQUIRE(scaffold->next == nullptr);
  REQUIRE(scaffold->instr_head == nullptr);
  REQUIRE(scaffold->instr_tail == nullptr);
  REQUIRE(scaffold->label_head == nullptr);
  REQUIRE(scaffold->label_tail == nullptr);
  REQUIRE(scaffold->incoming_edge_head == nullptr);
  REQUIRE(scaffold->outgoing_edge_head == nullptr);
  REQUIRE(scaffold->incoming_values == nullptr);
  std::array<Instr*, kCount> reloads{};
  std::array<bool, kCount> seen_inputs{}, seen_outputs{};
  size_t markers = 0, loads = 0, stores = 0, overwrites = 0;
  auto owner = [&](const Value* value) {
    const auto found = std::find(inputs.begin(), inputs.end(), value);
    REQUIRE(found != inputs.end());
    return static_cast<size_t>(found - inputs.begin());
  };
  for (auto instr = body->instr_head; instr; instr = instr->next) {
    REQUIRE(instr->block == body);
    REQUIRE(instr->flags == 0);
    const auto opcode = instr->opcode->num;
    if (opcode == OPCODE_SOURCE_OFFSET) {
      REQUIRE(markers < plan.markers.size());
      REQUIRE(instr->src1.offset == kPC + markers * 4);
      plan.markers[markers++] = uint32_t(body->ordinal << 16) | instr->ordinal;
      continue;
    }
    REQUIRE(markers > 0);
    const size_t phase = markers - 1;
    if (opcode == OPCODE_LOAD_CONTEXT) {
      REQUIRE(phase == 0);
      REQUIRE(loads < kCount);
      auto value = inputs[loads];
      CheckAllocated(value, machine);
      REQUIRE(value->def == instr);
      REQUIRE(instr->src1.offset == real_hir::GPROffset(loads));
      REQUIRE_FALSE(seen_inputs[loads]);
      seen_inputs[loads] = true;
      plan.moves.push_back({Kind::Input, phase, loads++, value, nullptr});
    } else if (opcode == OPCODE_STORE_LOCAL) {
      REQUIRE(phase == 0);
      const auto guest = owner(instr->src2.value);
      REQUIRE(seen_inputs[guest]);
      REQUIRE(plan.slots[guest] == nullptr);
      auto slot = inputs[guest]->GetLocalSlot();
      REQUIRE(slot);
      REQUIRE(slot->type == INT64_TYPE);
      REQUIRE_FALSE(slot->IsConstant());
      REQUIRE(instr->src1.value == slot);
      REQUIRE(std::find(plan.slots.begin(), plan.slots.end(), slot) ==
              plan.slots.end());
      REQUIRE(std::count(builder.locals().begin(), builder.locals().end(),
                         slot) == 1);
      plan.slots[guest] = slot;
      ++plan.spill_count;
      plan.moves.push_back({Kind::Spill, phase, guest, inputs[guest], slot});
    } else if (opcode == OPCODE_LOAD_LOCAL) {
      REQUIRE(phase == 2);
      REQUIRE(instr->src1.value);
      const auto found =
          std::find(plan.slots.begin(), plan.slots.end(), instr->src1.value);
      REQUIRE(found != plan.slots.end());
      const auto guest = static_cast<size_t>(found - plan.slots.begin());
      REQUIRE(plan.slots[guest]);
      REQUIRE(reloads[guest] == nullptr);
      REQUIRE_FALSE(seen_outputs[guest]);
      CheckAllocated(instr->dest, machine);
      REQUIRE(instr->dest->def == instr);
      REQUIRE(instr->dest->GetLocalSlot() == plan.slots[guest]);
      reloads[guest] = instr;
      plan.moves.push_back(
          {Kind::Reload, phase, guest, instr->dest, plan.slots[guest]});
    } else if (opcode == OPCODE_STORE_CONTEXT) {
      REQUIRE(instr->src2.value->type == INT64_TYPE);
      if (phase == 1) {
        REQUIRE(loads == kCount);
        REQUIRE(overwrites++ == 0);
        REQUIRE(instr->src1.offset == real_hir::GPROffset(3));
        REQUIRE(instr->src2.value->IsConstant());
        REQUIRE(instr->src2.value->constant.u64 == real_hir::kValue);
        plan.moves.push_back({Kind::Overwrite, phase, 3, nullptr, nullptr});
      } else {
        REQUIRE(phase == 2);
        REQUIRE(overwrites == 1);
        const auto guest = CaptureGPR(stores++);
        REQUIRE(instr->src1.offset == real_hir::GPROffset(guest));
        REQUIRE_FALSE(seen_outputs[guest]);
        CheckAllocated(instr->src2.value, machine);
        if (plan.slots[guest]) {
          REQUIRE(reloads[guest]);
          REQUIRE(instr->src2.value == reloads[guest]->dest);
        } else {
          REQUIRE(instr->src2.value == inputs[guest]);
          REQUIRE(inputs[guest]->GetLocalSlot() == nullptr);
        }
        seen_outputs[guest] = true;
        plan.moves.push_back(
            {Kind::Output, phase, guest, instr->src2.value, nullptr});
      }
    } else {
      REQUIRE(opcode == OPCODE_RETURN);
      REQUIRE(phase == 3);
      REQUIRE(instr == body->instr_tail);
    }
  }
  REQUIRE(markers == plan.markers.size());
  REQUIRE(loads == kCount);
  REQUIRE(stores == kCount);
  REQUIRE(overwrites == 1);
  REQUIRE(plan.spill_count <= kCount);
  REQUIRE(plan.spill_count >= kCount - X64Emitter::GPR_COUNT);
  REQUIRE(builder.locals().size() == plan.spill_count);
  REQUIRE(plan.slots[3]);
  for (size_t guest = 0; guest < kCount; ++guest) {
    REQUIRE(seen_inputs[guest]);
    REQUIRE(seen_outputs[guest]);
    REQUIRE((reloads[guest] != nullptr) == (plan.slots[guest] != nullptr));
  }
  return plan;
}

void CheckBytes(const Plan& plan, const X64Emitter::PreparedDraft& view) {
  constexpr std::array<unsigned int, X64Emitter::GPR_COUNT> registers = {
      X86_REG_RBX, X86_REG_R10, X86_REG_R11, X86_REG_R12,
      X86_REG_R13, X86_REG_R14, X86_REG_R15};
  REQUIRE(view.code().size() <= real_hir::kMaxBytes);
  const auto stack = view.info().stack_size;
  REQUIRE(stack >= StackLayout::GUEST_STACK_SIZE + plan.spill_count * 8);
  REQUIRE(stack < StackLayout::GUEST_STACK_SIZE + plan.spill_count * 8 + 16);
  REQUIRE((stack + 8) % 16 == 0);
  std::vector<uint32_t> offsets;
  for (auto slot : plan.slots) {
    if (!slot) {
      continue;
    }
    // Actual Emit assigns these scalar offsets, not native stack addresses.
    REQUIRE(slot->IsConstant());
    REQUIRE(slot->type == INT32_TYPE);
    const auto offset = slot->constant.u32;
    REQUIRE(offset >= StackLayout::GUEST_STACK_SIZE);
    REQUIRE(offset % sizeof(uint64_t) == 0);
    REQUIRE(offset <= stack - sizeof(uint64_t));
    REQUIRE(std::find(offsets.begin(), offsets.end(), offset) == offsets.end());
    offsets.push_back(offset);
  }
  REQUIRE(offsets.size() == plan.spill_count);
  const auto map = view.source_map();
  REQUIRE(map.size() == plan.markers.size());
  REQUIRE(view.info().code_size.prolog <= view.code().size());
  REQUIRE(view.info().code_size.body <=
          view.code().size() - view.info().code_size.prolog);
  const auto epilog = view.info().code_size.prolog + view.info().code_size.body;
  REQUIRE(epilog <= view.code().size());
  for (size_t i = 0; i < map.size(); ++i) {
    REQUIRE(map[i].guest_address == kPC + i * 4);
    REQUIRE(map[i].hir_offset == plan.markers[i]);
    REQUIRE(map[i].code_offset >= view.info().code_size.prolog);
    REQUIRE(map[i].code_offset <= epilog);
    if (i) {
      REQUIRE(map[i - 1].code_offset < map[i].code_offset);
    }
  }
  REQUIRE(map.back().code_offset - map.front().code_offset <= 2048);
  std::array<int, X64Emitter::GPR_COUNT> register_tokens;
  std::array<int, kCount> local_tokens;
  register_tokens.fill(-1);
  local_tokens.fill(-1);
  std::array<size_t, kCount> loads{}, spills{}, reloads{}, writes{};
  size_t position = 0, output = 0, overwrites = 0;
  real_hir::Decoder decoder;
  REQUIRE(decoder.instruction);
  for (size_t phase = 0; phase < 3; ++phase) {
    const size_t begin = map[phase].code_offset;
    const size_t end = map[phase + 1].code_offset;
    const uint8_t* cursor = view.code().data() + begin;
    size_t remaining = end - begin;
    uint64_t address = begin;  // Numeric offset; no executable entry is formed.
    while (remaining) {
      REQUIRE(position < plan.moves.size());
      const auto& expected = plan.moves[position++];
      REQUIRE(expected.phase == phase);
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      REQUIRE(decoder.instruction->id == X86_INS_MOV);
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.op_count == 2);
      REQUIRE(x.addr_size == sizeof(uint64_t));
      REQUIRE(x.eflags == 0);
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      const auto& destination = x.operands[0];
      const auto& source = x.operands[1];
      REQUIRE(destination.size == sizeof(uint64_t));
      REQUIRE(destination.access == CS_AC_WRITE);
      const bool load =
          expected.kind == Kind::Input || expected.kind == Kind::Reload;
      const bool local =
          expected.kind == Kind::Spill || expected.kind == Kind::Reload;
      const auto& memory = load ? source : destination;
      REQUIRE(memory.type == X86_OP_MEM);
      REQUIRE(memory.size == sizeof(uint64_t));
      const auto base = local ? X86_REG_RSP : X86_REG_RSI;
      REQUIRE(memory.mem.base == base);
      REQUIRE(memory.mem.index == X86_REG_INVALID);
      REQUIRE(memory.mem.segment == X86_REG_INVALID);
      const auto offset = local ? expected.slot->constant.u32
                                : real_hir::GPROffset(expected.guest);
      REQUIRE(memory.mem.disp == static_cast<int64_t>(offset));
      cs_regs reads{}, native_writes{};
      uint8_t read_count = 0, write_count = 0;
      REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                             &read_count, native_writes,
                             &write_count) == CS_ERR_OK);
      const bool immediate = expected.kind == Kind::Overwrite;
      REQUIRE(read_count == (load || immediate ? 1 : 2));
      REQUIRE(std::find(reads, reads + read_count, base) != reads + read_count);
      REQUIRE(write_count == (load ? 1 : 0));
      if (immediate) {
        REQUIRE(destination.type == X86_OP_MEM);
        REQUIRE(source.type == X86_OP_IMM);
        REQUIRE(static_cast<uint64_t>(source.imm) == real_hir::kValue);
        REQUIRE(overwrites++ == 0);
        continue;
      }
      const auto& reg = load ? destination : source;
      REQUIRE(reg.type == X86_OP_REG);
      REQUIRE(reg.size == sizeof(uint64_t));
      const auto index = static_cast<size_t>(expected.value->reg.index);
      REQUIRE(index < registers.size());
      REQUIRE(reg.reg == registers[index]);
      if (load) {
        REQUIRE(source.access == CS_AC_READ);
        REQUIRE(native_writes[0] == reg.reg);
        if (local) {
          REQUIRE(local_tokens[expected.guest] ==
                  static_cast<int>(expected.guest));
          REQUIRE(reloads[expected.guest]++ == 0);
          register_tokens[index] = local_tokens[expected.guest];
        } else {
          REQUIRE(loads[expected.guest]++ == 0);
          register_tokens[index] = static_cast<int>(expected.guest);
        }
      } else {
        REQUIRE(destination.type == X86_OP_MEM);
        REQUIRE(source.access == CS_AC_READ);
        REQUIRE(std::find(reads, reads + read_count, reg.reg) !=
                reads + read_count);
        REQUIRE(register_tokens[index] == static_cast<int>(expected.guest));
        if (local) {
          REQUIRE(spills[expected.guest]++ == 0);
          local_tokens[expected.guest] = register_tokens[index];
        } else {
          REQUIRE(overwrites == 1);
          REQUIRE(expected.guest == CaptureGPR(output++));
          REQUIRE(writes[expected.guest]++ == 0);
        }
      }
    }
    REQUIRE(address == end);
  }
  REQUIRE(position == plan.moves.size());
  REQUIRE(output == kCount);
  REQUIRE(overwrites == 1);
  for (size_t guest = 0; guest < kCount; ++guest) {
    REQUIRE(loads[guest] == 1);
    REQUIRE(writes[guest] == 1);
    REQUIRE(spills[guest] == (plan.slots[guest] ? 1 : 0));
    REQUIRE(reloads[guest] == spills[guest]);
  }
}

TEST_CASE("Real x64 draft preserves 32 canonical-entry GPR tokens through RA",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  REQUIRE(cvars::align_all_basic_blocks == 0);
  Fixture fixture;
  XbyakAllocator allocator;
  real_hir::HIREmitter emitter(fixture.backend, &allocator);
  real_hir::CurrentHIR builder;
  builder.SourceOffset(kPC);
  std::array<Value*, kCount> inputs{};
  for (size_t guest = 0; guest < kCount; ++guest) {
    inputs[guest] = builder.LoadContext(real_hir::GPROffset(guest), INT64_TYPE);
  }
  builder.SourceOffset(kPC + 4);
  builder.StoreContext(real_hir::GPROffset(3),
                       builder.LoadConstantUint64(real_hir::kValue));
  builder.SourceOffset(kPC + 8);
  for (size_t position = 0; position < kCount; ++position) {
    const auto guest = CaptureGPR(position);
    builder.StoreContext(real_hir::GPROffset(guest), inputs[guest]);
  }
  builder.SourceOffset(kPC + 12);
  builder.Return();
  REQUIRE(builder.Finalize());
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&fixture.processor);
  // Ordered context effects are retained explicitly. No context-promotion or
  // full ordinary-translator pipeline compatibility is asserted by this case.
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      fixture.backend->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
  const auto plan = CheckHIR(builder, inputs, *fixture.backend->machine_info());
  const auto original_scratch = emitter.getCode();
  const auto unwind_size = fixture.cache->QueryUnwindStorageSize();
  uint64_t pc = 0;
  size_t span = 0, callbacks = 0;
  REQUIRE(emitter.WithPreparedDraft(
      builder, [&](const X64Emitter::PreparedDraft& view) {
        ++callbacks;
        pc = fixture.cache->execute_base_address() + view.execute_offset();
        span = view.allocation_size();
        REQUIRE(reinterpret_cast<uint64_t>(view.code().data()) == pc);
        CheckCommitted(pc, span, PAGE_EXECUTE_READWRITE);
        emitter.Snapshot(view, original_scratch);
        CheckBytes(plan, view);
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
      }));
  REQUIRE(callbacks == 1);
  REQUIRE(emitter.getCode() == original_scratch);
  emitter.CheckDiscarded();
  fixture.CheckAborted(pc, span);
  REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
  uint64_t next_pc = 0;
  size_t next_span = 0;
  const uint8_t trap = 0xCC;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      &trap, CodeInfo(1), [&](PreparedGuestCode& next) {
        next_pc = fixture.cache->execute_base_address() + next.execute_offset();
        next_span = next.allocation_size();
        REQUIRE(next_pc == pc + span);
        fixture.CheckUnpublished(next_pc, next_span);
      }));
  fixture.CheckAborted(pc, span);
  fixture.CheckAborted(next_pc, next_span);
  // Assumes the 32 entry PPCContext GPR fields are canonical. It does not prove
  // an arbitrary optimized site's live register bank, FPR/VMX/flags precision,
  // full-bank capture, guest ABI, native unwind serialization or checkpoint
  // eligibility. No capture-validity mask is set and no bytes are invoked.
}

}  // namespace canonical_gpr

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
