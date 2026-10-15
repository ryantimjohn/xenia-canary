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
#include "xenia/cpu/backend/x64/x64_function.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/backend/x64/x64_tracers.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/compiler/compiler_passes.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_hir_builder.h"
#include "xenia/cpu/ppc/ppc_instr.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DECLARE_bool(emit_source_annotations);
DECLARE_bool(instrument_call_times);
DECLARE_uint32(align_all_basic_blocks);
DECLARE_bool(debug);
DECLARE_bool(store_all_context_values);
DECLARE_bool(full_optimization_even_with_debug);
DECLARE_bool(emit_inline_mmio_checks);

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
static_assert(
    std::is_same_v<decltype(std::declval<const PreparedDraft&>().unwind_info()),
                   std::span<const uint8_t>>);

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
  // These are complete emitter unwind inputs. Private encoded bytes are
  // inspected separately; neither snapshot installs runtime-table metadata.
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
  REQUIRE(prepared.unwind_offset() >= prepared.code().size());
  REQUIRE(prepared.unwind_offset() <= prepared.storage().size());
  REQUIRE(std::all_of(prepared.storage().begin() + prepared.code().size(),
                      prepared.storage().begin() + prepared.unwind_offset(),
                      [](uint8_t byte) { return byte == 0xCC; }));
  // Actual emitted small-frame inputs are privately serialized before this
  // read-only view. Decode the native format without installing a table entry.
  const auto unwind = prepared.unwind_info();
  REQUIRE(unwind.size() >= 10);
  REQUIRE(unwind[0] == 1);  // Version 1, flags 0.
  REQUIRE(unwind[1] == draft.info.code_size.prolog);
  REQUIRE(unwind[2] == 1);  // One UWOP_ALLOC_SMALL slot.
  REQUIRE(unwind[3] == 0);  // No frame register.
  REQUIRE(unwind[4] == draft.info.prolog_stack_alloc_offset);
  REQUIRE((unwind[5] & 15) == 2);
  REQUIRE((unwind[5] >> 4) == draft.info.stack_size / 8 - 1);
  // Preserve the legacy odd-padding location rather than silently correcting
  // it: alignment bytes remain traps, while the following slot is zeroed.
  REQUIRE(unwind[6] == 0xCC);
  REQUIRE(unwind[7] == 0xCC);
  REQUIRE(unwind[8] == 0);
  REQUIRE(unwind[9] == 0);
  REQUIRE(std::all_of(unwind.begin() + 10, unwind.end(),
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
  // unpublished ranges. Private byte serialization does not prove OS unwind
  // execution, registration, callable entry, guest invocation, complete bank
  // capture or guest ABI correctness.
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
        // At least 32 - 7 distinct INT64 spills force the actual frame beyond
        // UWOP_ALLOC_SMALL. Check the producer's private bytes independently
        // from the emitted stack/prolog scalars, without publishing a table.
        static_assert(StackLayout::GUEST_STACK_SIZE +
                          (kCount - X64Emitter::GPR_COUNT) * sizeof(uint64_t) >
                      128);
        const auto& info = view.info();
        REQUIRE(info.stack_size > 128);
        REQUIRE(info.stack_size < 65536);
        REQUIRE(info.stack_size % 8 == 0);
        REQUIRE(info.code_size.prolog < 256);
        REQUIRE(info.prolog_stack_alloc_offset > 0);
        REQUIRE(info.prolog_stack_alloc_offset <= info.code_size.prolog);
        REQUIRE(view.code().size() == info.code_size.total);
        REQUIRE(view.storage().size() == span);
        REQUIRE(view.code().data() == view.storage().data());
        REQUIRE(view.unwind_offset() == (view.code().size() + 15) / 16 * 16);
        REQUIRE(view.unwind_offset() <= span);
        const auto unwind = view.unwind_info();
        REQUIRE(unwind.size() == unwind_size);
        REQUIRE(unwind.size() >= 8);
        REQUIRE(unwind.data() == view.storage().data() + view.unwind_offset());
        REQUIRE(unwind.size() == span - view.unwind_offset());
        const auto scaled_stack = info.stack_size / 8;
        const std::array<uint8_t, 8> expected_unwind = {
            1,
            static_cast<uint8_t>(info.code_size.prolog),
            2,
            0,
            static_cast<uint8_t>(info.prolog_stack_alloc_offset),
            1,  // UWOP_ALLOC_LARGE, OpInfo 0: one following size slot.
            static_cast<uint8_t>(scaled_stack),
            static_cast<uint8_t>(scaled_stack >> 8)};
        REQUIRE(std::equal(expected_unwind.begin(), expected_unwind.end(),
                           unwind.begin()));
        REQUIRE(std::all_of(view.storage().begin() + view.code().size(),
                            view.storage().begin() + view.unwind_offset(),
                            [](uint8_t byte) { return byte == 0xCC; }));
        REQUIRE(std::all_of(unwind.begin() + expected_unwind.size(),
                            unwind.end(),
                            [](uint8_t byte) { return byte == 0xCC; }));
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
  // full-bank capture, guest ABI, OS unwinding or checkpoint
  // eligibility. No capture-validity mask is set and no bytes are invoked.
}

}  // namespace canonical_gpr

namespace midblock_gpr {

using namespace hir;
constexpr uint32_t kPC = 0x80007004;
constexpr uint64_t kOverwrite = 0x13579BDF;
constexpr uint32_t kWord = (32u << 26) | (3u << 21) | (3u << 16) | 0xFFFC;
using Capture = ppc::DormantCheckpointGprCapture;

// Runs on the real optimization pipeline, before RA can reuse freed Instr
// storage. Observes promotion itself, then the simplified SSA operand.
class CaptureProof final : public compiler::CompilerPass {
 public:
  CaptureProof(const Capture& capture, Value* computed, bool promoted,
               bool& observed)
      : capture_(capture),
        computed_(computed),
        promoted_(promoted),
        observed_(observed) {}
  bool Run(HIRBuilder* builder) override {
    size_t context_stores = 0, captures = 0;
    for (auto i = builder->first_block()->instr_head; i; i = i->next) {
      if (i->opcode->num == OPCODE_STORE_CONTEXT) {
        ++context_stores;
        REQUIRE(i->src1.offset == real_hir::GPROffset(3));
        REQUIRE(i->src2.value->IsConstant());
        REQUIRE(i->src2.value->constant.u64 == kOverwrite);
      }
      if (i->opcode->num == OPCODE_STORE_LOCAL &&
          i->src1.value == capture_.gpr_slots[3]) {
        ++captures;
        if (promoted_) {
          REQUIRE(i->src2.value->def);
          REQUIRE(i->src2.value->def->opcode->num == OPCODE_ASSIGN);
          REQUIRE(i->src2.value->def->src1.value == computed_);
        } else {
          REQUIRE(i->src2.value == computed_);
        }
      }
    }
    REQUIRE(context_stores ==
            1);  // The earlier computed context store is gone.
    REQUIRE(captures == 1);
    observed_ = true;
    return true;
  }

 private:
  const Capture& capture_;
  Value* computed_;
  bool promoted_;
  bool& observed_;
};

struct Local {
  Value* slot;
  size_t width;
  int capture;  // 0..31 GPR, 32 EA, -1 actual RA spill.
  uint32_t offset = 0;
  size_t reads = 0, writes = 0;
};

std::vector<Local> CheckHIR(HIRBuilder& builder, const Capture& capture) {
  std::vector<Local> locals;
  REQUIRE(builder.locals().size() > 33);
  for (auto slot : builder.locals()) {
    REQUIRE(slot);
    REQUIRE_FALSE(slot->IsConstant());
    int owner = -1;
    for (size_t reg = 0; reg < capture.gpr_slots.size(); ++reg) {
      if (slot == capture.gpr_slots[reg]) {
        owner = static_cast<int>(reg);
      }
    }
    if (slot == capture.ea_slot) {
      owner = 32;
    }
    REQUIRE(slot->type == (owner == 32 ? INT32_TYPE : INT64_TYPE));
    REQUIRE(std::none_of(locals.begin(), locals.end(), [&](const Local& local) {
      return local.slot == slot;
    }));
    locals.push_back({slot, owner == 32 ? 4u : 8u, owner});
  }
  auto body = builder.first_block();
  REQUIRE(body);
  REQUIRE(body->instr_tail);
  REQUIRE(body->instr_tail->opcode->num == OPCODE_RETURN);
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
  size_t capture_count = 0, context_stores = 0, adds = 0, subs = 0,
         truncates = 0;
  for (auto i = body->instr_head; i; i = i->next) {
    REQUIRE(i->block == body);
    REQUIRE(i->flags == 0);
    switch (i->opcode->num) {
      case OPCODE_LOAD_LOCAL:
      case OPCODE_STORE_LOCAL: {
        const auto found = std::find_if(
            locals.begin(), locals.end(),
            [&](const Local& local) { return local.slot == i->src1.value; });
        REQUIRE(found != locals.end());
        if (i->opcode->num == OPCODE_LOAD_LOCAL) {
          REQUIRE(found->capture == -1);
          REQUIRE(i->dest->GetLocalSlot() == found->slot);
          REQUIRE(i->dest->type == INT64_TYPE);
          ++found->reads;
        } else {
          REQUIRE(i->src2.value->type ==
                  (found->width == 4 ? INT32_TYPE : INT64_TYPE));
          if (found->capture >= 0) {
            REQUIRE(found->capture == static_cast<int>(capture_count++));
          } else {
            REQUIRE(i->src2.value->GetLocalSlot() == found->slot);
          }
          ++found->writes;
        }
        break;
      }
      case OPCODE_STORE_CONTEXT:
        REQUIRE(i->src1.offset == real_hir::GPROffset(3));
        REQUIRE(i->src2.value->IsConstant());
        REQUIRE(i->src2.value->constant.u64 == kOverwrite);
        ++context_stores;
        break;
      case OPCODE_LOAD_CONTEXT:
        REQUIRE(i->dest->type == INT64_TYPE);
        break;
      case OPCODE_ADD:
      case OPCODE_SUB:
        REQUIRE(i->dest->type == INT64_TYPE);
        REQUIRE(i->src2.value->IsConstant());
        // Actual SimplificationPass::CheckAdd rewrites the negative EA ADD
        // into SUB 4. The computed pre-capture +1 remains ADD.
        REQUIRE(i->src2.value->constant.i64 ==
                (i->opcode->num == OPCODE_ADD ? 1 : 4));
        i->opcode->num == OPCODE_ADD ? ++adds : ++subs;
        break;
      case OPCODE_TRUNCATE:
        REQUIRE(i->dest->type == INT32_TYPE);
        REQUIRE(i->src1.value->type == INT64_TYPE);
        ++truncates;
        break;
      default:
        REQUIRE((i->opcode->num == OPCODE_SOURCE_OFFSET ||
                 i->opcode->num == OPCODE_RETURN));
        break;
    }
  }
  REQUIRE(capture_count == 33);
  REQUIRE(context_stores == 1);
  REQUIRE(adds == 1);
  REQUIRE(subs == 1);
  REQUIRE(truncates == 1);
  REQUIRE(locals.size() - 33 >= 32 - X64Emitter::GPR_COUNT);
  for (const auto& local : locals) {
    REQUIRE(local.writes == 1);
    REQUIRE((local.capture >= 0 ? local.reads == 0 : local.reads > 0));
  }
  return locals;
}

// Independent symbolic native interpreter. A token is entry GPR plus a signed
// constant, optionally reduced modulo 2^32. No concrete register sample or HIR
// operation sequence supplies the decoded value; only actual operands do.
struct Token {
  int guest = -1;  // -1 unknown, -2 literal constant.
  int64_t delta = 0;
  bool low32 = false;
};

size_t Register(const cs_x86_op& operand) {
  REQUIRE(operand.type == X86_OP_REG);
  constexpr std::array<unsigned, 7> full = {
      X86_REG_RBX, X86_REG_R10, X86_REG_R11, X86_REG_R12,
      X86_REG_R13, X86_REG_R14, X86_REG_R15};
  constexpr std::array<unsigned, 7> low = {
      X86_REG_EBX,  X86_REG_R10D, X86_REG_R11D, X86_REG_R12D,
      X86_REG_R13D, X86_REG_R14D, X86_REG_R15D};
  REQUIRE((operand.size == 4 || operand.size == 8));
  const auto& names = operand.size == 8 ? full : low;
  const auto found = std::find(names.begin(), names.end(), operand.reg);
  REQUIRE(found != names.end());
  return static_cast<size_t>(found - names.begin());
}

void CheckBytes(std::vector<Local>& locals,
                const X64Emitter::PreparedDraft& view) {
  REQUIRE(view.code().size() <= real_hir::kMaxBytes);
  const auto stack = view.info().stack_size;
  REQUIRE((stack + 8) % 16 == 0);
  for (auto& local : locals) {
    REQUIRE(local.slot->IsConstant());
    REQUIRE(local.slot->type == INT32_TYPE);
    local.offset = local.slot->constant.u32;
    REQUIRE(local.offset >= StackLayout::GUEST_STACK_SIZE);
    REQUIRE(local.offset % local.width == 0);
    REQUIRE(local.width <= stack);
    REQUIRE(local.offset <= stack - local.width);
  }
  for (size_t a = 0; a < locals.size(); ++a) {
    for (size_t b = a + 1; b < locals.size(); ++b) {
      REQUIRE((locals[a].offset + locals[a].width <= locals[b].offset ||
               locals[b].offset + locals[b].width <= locals[a].offset));
    }
  }
  const auto map = view.source_map();
  REQUIRE(map.size() == 4);
  REQUIRE(view.info().code_size.prolog <= view.code().size());
  REQUIRE(view.info().code_size.body <=
          view.code().size() - view.info().code_size.prolog);
  for (size_t i = 0; i < map.size(); ++i) {
    REQUIRE(map[i].guest_address == kPC - 4 + i * 4);
    REQUIRE(map[i].code_offset >= view.info().code_size.prolog);
    REQUIRE(map[i].code_offset <=
            view.info().code_size.prolog + view.info().code_size.body);
    if (i) {
      REQUIRE(map[i - 1].code_offset < map[i].code_offset);
    }
  }
  REQUIRE(map.back().code_offset <= view.code().size());
  REQUIRE(map.back().code_offset - map.front().code_offset <= 4096);
  std::array<Token, 7> registers{};
  std::array<size_t, 32> inputs{};
  std::vector<Token> memory(locals.size());
  std::vector<size_t> reads(locals.size()), writes(locals.size());
  size_t captures = 0, plus_one = 0, minus_four = 0, overwrites = 0;
  real_hir::Decoder decoder;
  REQUIRE(decoder.instruction);
  for (size_t phase = 0; phase < 3; ++phase) {
    const uint8_t* cursor = view.code().data() + map[phase].code_offset;
    size_t remaining = map[phase + 1].code_offset - map[phase].code_offset;
    uint64_t address = map[phase].code_offset;
    while (remaining) {
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      const auto opcode = decoder.instruction->id;
      const bool add = opcode == X86_INS_ADD, sub = opcode == X86_INS_SUB;
      const bool inc = opcode == X86_INS_INC, arithmetic = add || sub || inc;
      REQUIRE((opcode == X86_INS_MOV || arithmetic));
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.addr_size == 8);
      REQUIRE(x.op_count == (inc ? 1 : 2));
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      const uint64_t arithmetic_flags =
          X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_PF |
          X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF;
      REQUIRE(x.eflags == (add || sub ? arithmetic_flags | X86_EFLAGS_MODIFY_CF
                           : inc      ? arithmetic_flags
                                      : 0));
      const auto& destination = x.operands[0];
      const auto& source = x.operands[inc ? 0 : 1];
      REQUIRE(destination.access ==
              (arithmetic ? CS_AC_READ | CS_AC_WRITE : CS_AC_WRITE));
      std::vector<unsigned> expected_reads, expected_writes;
      auto read = [&](unsigned reg) {
        if (std::find(expected_reads.begin(), expected_reads.end(), reg) ==
            expected_reads.end()) {
          expected_reads.push_back(reg);
        }
      };
      if (destination.type == X86_OP_REG) {
        Register(destination);
        expected_writes.push_back(destination.reg);
        if (arithmetic) {
          read(destination.reg);
        }
      }
      if (!inc && source.type == X86_OP_REG) {
        Register(source);
        read(source.reg);
      }
      if (destination.type == X86_OP_MEM ||
          (!inc && source.type == X86_OP_MEM)) {
        REQUIRE_FALSE(arithmetic);
        const auto& mem = destination.type == X86_OP_MEM ? destination : source;
        REQUIRE((mem.size == 4 || mem.size == 8));
        REQUIRE((mem.mem.base == X86_REG_RSI || mem.mem.base == X86_REG_RSP));
        REQUIRE(mem.mem.index == X86_REG_INVALID);
        REQUIRE(mem.mem.segment == X86_REG_INVALID);
        read(mem.mem.base);
      }
      if (arithmetic) {
        expected_writes.push_back(X86_REG_EFLAGS);
      }
      cs_regs actual_reads{}, actual_writes{};
      uint8_t read_count = 0, write_count = 0;
      REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, actual_reads,
                             &read_count, actual_writes,
                             &write_count) == CS_ERR_OK);
      std::sort(expected_reads.begin(), expected_reads.end());
      std::sort(expected_writes.begin(), expected_writes.end());
      std::sort(actual_reads, actual_reads + read_count);
      std::sort(actual_writes, actual_writes + write_count);
      REQUIRE(read_count == expected_reads.size());
      REQUIRE(write_count == expected_writes.size());
      REQUIRE(std::equal(expected_reads.begin(), expected_reads.end(),
                         actual_reads));
      REQUIRE(std::equal(expected_writes.begin(), expected_writes.end(),
                         actual_writes));
      auto register_value = [&](const cs_x86_op& op) {
        auto token = registers[Register(op)];
        REQUIRE(token.guest != -1);
        if (op.size == 4) {
          token.low32 = true;
        }
        return token;
      };
      auto local_index = [&](const cs_x86_op& op) {
        REQUIRE(op.mem.base == X86_REG_RSP);
        const auto found =
            std::find_if(locals.begin(), locals.end(), [&](const Local& local) {
              return op.mem.disp == local.offset && op.size == local.width;
            });
        REQUIRE(found != locals.end());
        return static_cast<size_t>(found - locals.begin());
      };
      if (arithmetic) {
        REQUIRE(destination.type == X86_OP_REG);
        REQUIRE(destination.size == 8);
        auto left = register_value(destination);
        Token right{-2,
                    inc                         ? 1
                    : source.type == X86_OP_IMM ? source.imm
                                                : 0,
                    false};
        if (!inc && source.type != X86_OP_IMM) {
          REQUIRE(source.size == 8);
          right = register_value(source);
        }
        if (left.guest == -2) {
          REQUIRE_FALSE(sub);
          std::swap(left, right);
        }
        REQUIRE(left.guest == 3);
        REQUIRE_FALSE(left.low32);
        REQUIRE(right.guest == -2);
        REQUIRE_FALSE(right.low32);
        if (sub) {
          right.delta = -right.delta;
        }
        if (phase == 0) {
          REQUIRE_FALSE(sub);
          REQUIRE(left.delta == 0);
          REQUIRE(right.delta == 1);
          REQUIRE(plus_one++ == 0);
        } else {
          REQUIRE(phase == 1);
          REQUIRE(sub);
          REQUIRE(left.delta == 1);
          REQUIRE(right.delta == -4);
          REQUIRE(minus_four++ == 0);
        }
        registers[Register(destination)] = {3, left.delta + right.delta, false};
      } else {
        Token value;
        if (source.type == X86_OP_REG) {
          value = register_value(source);
        } else if (source.type == X86_OP_IMM) {
          REQUIRE(
              (source.imm == 1 || source.imm == 4 || source.imm == kOverwrite));
          value = {-2, source.imm, false};
        } else {
          REQUIRE(source.type == X86_OP_MEM);
          REQUIRE(source.access == CS_AC_READ);
          REQUIRE(destination.type == X86_OP_REG);
          REQUIRE(destination.size == source.size);
          if (source.mem.base == X86_REG_RSI) {
            REQUIRE(source.size == 8);
            REQUIRE(source.mem.disp >= real_hir::GPROffset(0));
            const auto offset = source.mem.disp - real_hir::GPROffset(0);
            REQUIRE(offset % 8 == 0);
            REQUIRE(offset / 8 < inputs.size());
            const auto guest = static_cast<size_t>(offset / 8);
            REQUIRE(phase == (guest == 3 ? 0 : 1));
            REQUIRE(inputs[guest]++ == 0);
            value = {static_cast<int>(guest), 0, false};
          } else {
            const auto local = local_index(source);
            REQUIRE(locals[local].capture == -1);
            REQUIRE(writes[local] > 0);
            ++reads[local];
            value = memory[local];
            REQUIRE(value.guest >= 0);
          }
        }
        if (destination.type == X86_OP_REG) {
          if (source.type == X86_OP_REG) {
            REQUIRE(source.size == destination.size);
          }
          if (destination.size == 4) {
            REQUIRE(phase == 1);
            REQUIRE(value.guest == 3);
            REQUIRE(value.delta == -3);
            value.low32 = true;  // Includes architectural upper-half clearing.
          }
          registers[Register(destination)] = value;
        } else {
          REQUIRE(destination.type == X86_OP_MEM);
          if (destination.mem.base == X86_REG_RSI) {
            REQUIRE(phase == 2);
            REQUIRE(destination.size == 8);
            REQUIRE(destination.mem.disp == real_hir::GPROffset(3));
            REQUIRE(value.guest == -2);
            REQUIRE(value.delta == kOverwrite);
            REQUIRE(overwrites++ == 0);
          } else {
            const auto local = local_index(destination);
            REQUIRE(source.type == X86_OP_REG);
            REQUIRE(source.size == destination.size);
            const auto owner = locals[local].capture;
            if (owner >= 0) {
              REQUIRE(phase == 1);
              REQUIRE(owner == static_cast<int>(captures++));
              REQUIRE(value.guest == (owner == 32 ? 3 : owner));
              REQUIRE(value.delta == (owner == 32 ? -3 : owner == 3 ? 1 : 0));
              REQUIRE(value.low32 == (owner == 32));
            } else {
              REQUIRE(value.guest >= 0);
              REQUIRE_FALSE(value.low32);
            }
            memory[local] = value;
            ++writes[local];
          }
        }
      }
    }
    REQUIRE(address == map[phase + 1].code_offset);
  }
  REQUIRE(captures == 33);
  REQUIRE(plus_one == 1);
  REQUIRE(minus_four == 1);
  REQUIRE(overwrites == 1);
  for (auto count : inputs) {
    REQUIRE(count == 1);
  }
  for (size_t i = 0; i < locals.size(); ++i) {
    REQUIRE(reads[i] == locals[i].reads);
    REQUIRE(writes[i] == locals[i].writes);
  }
}

TEST_CASE("Dormant mid-block GPR capture survives promotion and real RA",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  REQUIRE(cvars::align_all_basic_blocks == 0);
  REQUIRE((cvars::full_optimization_even_with_debug ||
           (!cvars::debug && !cvars::store_all_context_values)));
  Fixture fixture;
  real_hir::CurrentHIR builder;
  builder.SourceOffset(kPC - 4);
  auto entry = builder.LoadContext(real_hir::GPROffset(3), INT64_TYPE);
  auto computed = builder.Add(entry, builder.LoadConstantInt64(1));
  builder.StoreContext(real_hir::GPROffset(3), computed);
  builder.SourceOffset(kPC);
  Capture capture;
  REQUIRE(ppc::BuildDormantCheckpointGprCapture(builder, kPC, kWord, &capture));
  REQUIRE(builder.locals().size() == 33);
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
  builder.SourceOffset(kPC + 4);
  builder.StoreContext(real_hir::GPROffset(3),
                       builder.LoadConstantUint64(kOverwrite));
  builder.SourceOffset(kPC + 8);
  builder.Return();
  REQUIRE(builder.Finalize());
  bool promoted = false, optimized = false;
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&fixture.processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::ContextPromotionPass>());
  compiler.AddPass(
      std::make_unique<CaptureProof>(capture, computed, true, promoted));
  auto simplify = std::make_unique<passes::ConditionalGroupPass>();
  simplify->AddPass(std::make_unique<passes::SimplificationPass>());
  simplify->AddPass(std::make_unique<passes::ConstantPropagationPass>());
  compiler.AddPass(std::move(simplify));
  compiler.AddPass(std::make_unique<passes::DeadCodeEliminationPass>());
  compiler.AddPass(
      std::make_unique<CaptureProof>(capture, computed, false, optimized));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      fixture.backend->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE(promoted);
  REQUIRE(optimized);
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
  auto locals = CheckHIR(builder, capture);
  XbyakAllocator allocator;
  real_hir::HIREmitter emitter(fixture.backend, &allocator);
  const auto scratch = emitter.getCode();
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
        emitter.Snapshot(view, scratch);
        CheckBytes(locals, view);
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
      }));
  REQUIRE(callbacks == 1);
  REQUIRE(emitter.getCode() == scratch);
  emitter.CheckDiscarded();
  fixture.CheckAborted(pc, span);
  REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
  const uint8_t trap = 0xCC;
  uint64_t next_pc = 0;
  size_t next_span = 0;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      &trap, CodeInfo(1), [&](PreparedGuestCode& next) {
        next_pc = fixture.cache->execute_base_address() + next.execute_offset();
        next_span = next.allocation_size();
        REQUIRE(next_pc == pc + span);
        fixture.CheckUnpublished(next_pc, next.allocation_size());
      }));
  fixture.CheckAborted(pc, span);
  fixture.CheckAborted(next_pc, next_span);
  // GPR-only function-local capture, independent of Xbox exception
  // serialization. No faulting guest memory, invocation record,
  // full-state/precision validity, checkpoint pairing, publication, mapping
  // lease, entry or invocation claim.
}

TEST_CASE(
    "Dormant mid-block capture rejects unsupported input without mutation",
    "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  for (size_t variant = 0; variant < 10; ++variant) {
    real_hir::CurrentHIR builder;
    builder.SourceOffset(kPC - 4);
    auto input = builder.LoadContext(real_hir::GPROffset(3), INT64_TYPE);
    if (variant == 2) {
      builder.CallIndirect(builder.LoadConstantUint64(0));
    }
    if (variant == 3) {
      builder.MarkLabel(builder.NewLabel());
    }
    if (variant == 4) {
      builder.LoadContext(real_hir::GPROffset(3) + 4, INT64_TYPE);
    }
    if (variant == 5) {
      builder.AllocLocal(INT32_TYPE);
    }
    if (variant == 6) {
      builder.StoreContext(real_hir::GPROffset(3),
                           builder.LoadConstantInt32(7));
    }
    builder.SourceOffset(kPC);
    if (variant == 7) {
      builder.Nop();
    }
    Capture output;
    if (variant == 8) {
      output.gpr_slots[0] = input;
    }
    const auto prior_output = output;
    const auto prior_locals = builder.locals();
    const auto prior_ordinal = builder.max_value_ordinal();
    const auto prior_current = builder.current_block();
    std::vector<Instr*> instructions;
    for (auto block = builder.first_block(); block; block = block->next) {
      for (auto instr = block->instr_head; instr; instr = instr->next) {
        instructions.push_back(instr);
      }
    }
    REQUIRE_FALSE(ppc::BuildDormantCheckpointGprCapture(
        builder, variant == 1 ? kPC + 1 : kPC,
        variant == 0 ? (33u << 26) : kWord, variant == 9 ? nullptr : &output));
    REQUIRE(output.gpr_slots == prior_output.gpr_slots);
    REQUIRE(output.ea_slot == prior_output.ea_slot);
    REQUIRE(builder.locals() == prior_locals);
    REQUIRE(builder.max_value_ordinal() == prior_ordinal);
    REQUIRE(builder.current_block() == prior_current);
    size_t position = 0;
    for (auto block = builder.first_block(); block; block = block->next) {
      for (auto instr = block->instr_head; instr; instr = instr->next) {
        REQUIRE(position < instructions.size());
        REQUIRE(instr == instructions[position++]);
      }
    }
    REQUIRE(position == instructions.size());
  }
}

}  // namespace midblock_gpr

namespace capture_stw {

using namespace hir;
using Capture = ppc::DormantCheckpointGprCapture;
using Local = midblock_gpr::Local;
constexpr uint32_t kPC = 0x80008004;
constexpr uint32_t kWord = 0x91400000;  // stw r10,0(r0), literal zero base.
constexpr uint64_t kR0 = 0x2468ACE0;    // Deliberately synthetic nonzero r0.
constexpr uint64_t kSentinel = 0x13579BDF;

class CurrentPPC final : public ppc::PPCHIRBuilder {
 public:
  explicit CurrentPPC(ppc::PPCFrontend* frontend)
      : PPCHIRBuilder(frontend), previous_(GetCurrent()) {
    MakeCurrent();
  }
  ~CurrentPPC() override {
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

class BindingProof final : public compiler::CompilerPass {
 public:
  BindingProof(const Capture& capture, Value* computed, Instr* access,
               bool promoted, bool& observed)
      : capture_(capture),
        computed_(computed),
        access_(access),
        promoted_(promoted),
        observed_(observed) {}
  bool Run(HIRBuilder* builder) override {
    size_t context_stores = 0, captures = 0, accesses = 0;
    for (auto i = builder->first_block()->instr_head; i; i = i->next) {
      if (i->opcode->num == OPCODE_STORE_CONTEXT) {
        ++context_stores;
        REQUIRE(i->src2.value->IsConstant());
        const auto reg = i->src1.offset;
        REQUIRE(
            (reg == real_hir::GPROffset(0) || reg == real_hir::GPROffset(10)));
        REQUIRE(i->src2.value->constant.u64 ==
                (reg == real_hir::GPROffset(0) ? kR0 : kSentinel));
      }
      if (i->opcode->num == OPCODE_STORE_LOCAL &&
          i->src1.value == capture_.gpr_slots[10]) {
        ++captures;
        auto value = i->src2.value;
        if (promoted_) {
          REQUIRE(value->def);
          REQUIRE(value->def->opcode->num == OPCODE_ASSIGN);
          value = value->def->src1.value;
        }
        REQUIRE(value == computed_);
      }
      if (i == access_) {
        ++accesses;
      }
    }
    REQUIRE(context_stores == 2);  // Earlier computed r10 store was removed.
    REQUIRE(captures == 1);
    REQUIRE(accesses == 1);
    REQUIRE(access_->opcode->num == OPCODE_STORE_OFFSET);
    REQUIRE(access_->src1.value->IsConstant());
    REQUIRE(access_->src1.value->constant.u64 == 0);
    REQUIRE(access_->src2.value->IsConstant());
    REQUIRE(access_->src2.value->constant.u64 == 0);
    auto value = access_->src3.value;
    REQUIRE(value->type == INT32_TYPE);
    REQUIRE(value->def);
    if (promoted_) {
      REQUIRE(access_->flags == 0);
      REQUIRE(value->def->opcode->num == OPCODE_BYTE_SWAP);
      value = value->def->src1.value;
      REQUIRE(value->def);
    } else {
      REQUIRE(access_->flags == LOAD_STORE_BYTE_SWAP);
    }
    REQUIRE(value->def->opcode->num == OPCODE_TRUNCATE);
    value = value->def->src1.value;
    if (promoted_) {
      REQUIRE(value->def);
      REQUIRE(value->def->opcode->num == OPCODE_ASSIGN);
      value = value->def->src1.value;
    }
    REQUIRE(value == computed_);
    observed_ = true;
    return true;
  }

 private:
  const Capture& capture_;
  Value* computed_;
  Instr* access_;
  bool promoted_;
  bool& observed_;
};

std::vector<Local> CheckHIR(HIRBuilder& builder, const Capture& capture) {
  std::vector<Local> locals;
  REQUIRE(builder.locals().size() > 33);
  for (auto slot : builder.locals()) {
    REQUIRE(slot);
    REQUIRE_FALSE(slot->IsConstant());
    int owner = -1;
    for (size_t reg = 0; reg < 32; ++reg) {
      if (slot == capture.gpr_slots[reg]) {
        owner = static_cast<int>(reg);
      }
    }
    if (slot == capture.ea_slot) {
      owner = 32;
    }
    REQUIRE(slot->type == (owner == 32 ? INT32_TYPE : INT64_TYPE));
    REQUIRE(std::none_of(locals.begin(), locals.end(), [&](const Local& local) {
      return local.slot == slot;
    }));
    locals.push_back({slot, owner == 32 ? 4u : 8u, owner});
  }
  auto body = builder.first_block();
  REQUIRE(body);
  REQUIRE(body->instr_tail);
  REQUIRE(body->instr_tail->opcode->num == OPCODE_RETURN);
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
  size_t captures = 0, stores = 0, adds = 0, truncates = 0, context_stores = 0;
  for (auto i = body->instr_head; i; i = i->next) {
    REQUIRE(i->block == body);
    if (i->opcode->num != OPCODE_STORE_OFFSET) {
      REQUIRE(i->flags == 0);
    }
    switch (i->opcode->num) {
      case OPCODE_LOAD_LOCAL:
      case OPCODE_STORE_LOCAL: {
        auto found = std::find_if(
            locals.begin(), locals.end(),
            [&](const Local& l) { return l.slot == i->src1.value; });
        REQUIRE(found != locals.end());
        if (i->opcode->num == OPCODE_LOAD_LOCAL) {
          REQUIRE(found->capture == -1);
          REQUIRE(i->dest->GetLocalSlot() == found->slot);
          REQUIRE(i->dest->type == INT64_TYPE);
          ++found->reads;
        } else {
          REQUIRE(i->src2.value->type ==
                  (found->width == 4 ? INT32_TYPE : INT64_TYPE));
          if (found->capture >= 0) {
            REQUIRE(stores == 0);
            REQUIRE(found->capture == static_cast<int>(captures++));
          } else {
            REQUIRE(i->src2.value->GetLocalSlot() == found->slot);
          }
          ++found->writes;
        }
        break;
      }
      case OPCODE_STORE_OFFSET:
        REQUIRE(captures == 33);
        REQUIRE(stores++ == 0);
        REQUIRE(i->flags == LOAD_STORE_BYTE_SWAP);
        REQUIRE(i->src1.value->IsConstant());
        REQUIRE(i->src1.value->constant.u64 == 0);
        REQUIRE(i->src2.value->IsConstant());
        REQUIRE(i->src2.value->constant.u64 == 0);
        REQUIRE(i->src3.value->type == INT32_TYPE);
        break;
      case OPCODE_STORE_CONTEXT:
        REQUIRE(i->src2.value->IsConstant());
        if (i->src1.offset == real_hir::GPROffset(0)) {
          REQUIRE(captures == 0);
          REQUIRE(i->src2.value->constant.u64 == kR0);
        } else {
          REQUIRE(i->src1.offset == real_hir::GPROffset(10));
          REQUIRE(stores == 1);
          REQUIRE(i->src2.value->constant.u64 == kSentinel);
        }
        ++context_stores;
        break;
      case OPCODE_LOAD_CONTEXT:
        REQUIRE(i->dest->type == INT64_TYPE);
        break;
      case OPCODE_ADD:
        REQUIRE(i->dest->type == INT64_TYPE);
        REQUIRE(i->src2.value->IsConstant());
        REQUIRE(i->src2.value->constant.u64 == 1);
        ++adds;
        break;
      case OPCODE_TRUNCATE:
        REQUIRE(i->dest->type == INT32_TYPE);
        REQUIRE(i->src1.value->type == INT64_TYPE);
        ++truncates;
        break;
      default:
        REQUIRE((i->opcode->num == OPCODE_SOURCE_OFFSET ||
                 i->opcode->num == OPCODE_RETURN));
    }
  }
  REQUIRE(captures == 33);
  REQUIRE(stores == 1);
  REQUIRE(context_stores == 2);
  REQUIRE(adds == 1);
  REQUIRE(truncates == 1);
  // Synthetic constant r0 leaves 31 nonconstant current GPR values.
  REQUIRE(locals.size() - 33 >= 31 - X64Emitter::GPR_COUNT);
  for (const auto& local : locals) {
    REQUIRE(local.writes == 1);
    REQUIRE((local.capture >= 0 ? local.reads == 0 : local.reads > 0));
  }
  return locals;
}

struct Token {
  int guest = -1;  // -1 unknown, -2 literal; otherwise entry GPR index.
  int64_t delta = 0;
  bool low32 = false;  // Truncated value with known zero upper half in a reg.
  bool swapped = false;
};

size_t Register(const cs_x86_op& op) {
  // Seven allocator-visible registers plus the ordinary store's ECX scratch.
  constexpr std::array<unsigned, 8> full = {
      X86_REG_RBX, X86_REG_R10, X86_REG_R11, X86_REG_R12,
      X86_REG_R13, X86_REG_R14, X86_REG_R15, X86_REG_RCX};
  constexpr std::array<unsigned, 8> low = {
      X86_REG_EBX,  X86_REG_R10D, X86_REG_R11D, X86_REG_R12D,
      X86_REG_R13D, X86_REG_R14D, X86_REG_R15D, X86_REG_ECX};
  REQUIRE(op.type == X86_OP_REG);
  REQUIRE((op.size == 4 || op.size == 8));
  const auto& names = op.size == 8 ? full : low;
  const auto found = std::find(names.begin(), names.end(), op.reg);
  REQUIRE(found != names.end());
  return static_cast<size_t>(found - names.begin());
}

void CheckBytes(std::vector<Local>& locals,
                const X64Emitter::PreparedDraft& view, bool membase_low_zero,
                bool movbe_enabled) {
  REQUIRE(view.code().size() <= real_hir::kMaxBytes);
  const auto stack = view.info().stack_size;
  REQUIRE((stack + 8) % 16 == 0);
  for (auto& local : locals) {
    REQUIRE(local.slot->IsConstant());
    REQUIRE(local.slot->type == INT32_TYPE);
    local.offset = local.slot->constant.u32;
    REQUIRE(local.offset >= StackLayout::GUEST_STACK_SIZE);
    REQUIRE(local.offset % local.width == 0);
    REQUIRE(local.width <= stack);
    REQUIRE(local.offset <= stack - local.width);
  }
  for (size_t a = 0; a < locals.size(); ++a) {
    for (size_t b = a + 1; b < locals.size(); ++b) {
      REQUIRE((locals[a].offset + locals[a].width <= locals[b].offset ||
               locals[b].offset + locals[b].width <= locals[a].offset));
    }
  }
  const auto map = view.source_map();
  REQUIRE(map.size() == 4);
  REQUIRE(view.info().code_size.prolog <= view.code().size());
  REQUIRE(view.info().code_size.body <=
          view.code().size() - view.info().code_size.prolog);
  for (size_t i = 0; i < map.size(); ++i) {
    REQUIRE(map[i].guest_address == kPC - 4 + i * 4);
    REQUIRE(map[i].code_offset >= view.info().code_size.prolog);
    REQUIRE(map[i].code_offset <=
            view.info().code_size.prolog + view.info().code_size.body);
    if (i) {
      REQUIRE(map[i - 1].code_offset < map[i].code_offset);
    }
  }
  REQUIRE(map.back().code_offset <= view.code().size());
  REQUIRE(map.back().code_offset - map.front().code_offset <= 4096);
  std::array<Token, 8> registers{};
  std::array<size_t, 32> inputs{};
  std::vector<Token> memory(locals.size());
  std::vector<size_t> reads(locals.size()), writes(locals.size());
  size_t captures = 0, adds = 0, r0_stores = 0, guest_stores = 0,
         overwrites = 0, swaps = 0, scratch_copies = 0;
  real_hir::Decoder decoder;
  REQUIRE(decoder.instruction);
  for (size_t phase = 0; phase < 3; ++phase) {
    const uint8_t* cursor = view.code().data() + map[phase].code_offset;
    size_t remaining = map[phase + 1].code_offset - map[phase].code_offset;
    uint64_t address = map[phase].code_offset;
    while (remaining) {
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      const auto opcode = decoder.instruction->id;
      const bool add = opcode == X86_INS_ADD, inc = opcode == X86_INS_INC;
      const bool arithmetic = add || inc;
      const bool swap = opcode == X86_INS_BSWAP,
                 movbe = opcode == X86_INS_MOVBE;
      REQUIRE((opcode == X86_INS_MOV || arithmetic || swap || movbe));
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.addr_size == 8);
      REQUIRE(x.op_count == (inc || swap ? 1 : 2));
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      const uint64_t flags = X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_OF |
                             X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_SF |
                             X86_EFLAGS_MODIFY_ZF;
      REQUIRE(x.eflags == (add   ? flags | X86_EFLAGS_MODIFY_CF
                           : inc ? flags
                                 : 0));
      const auto& destination = x.operands[0];
      const auto& source = x.operands[inc || swap ? 0 : 1];
      REQUIRE(destination.access ==
              (arithmetic || swap ? CS_AC_READ | CS_AC_WRITE : CS_AC_WRITE));
      std::vector<unsigned> expected_reads, expected_writes;
      auto read = [&](unsigned reg) {
        if (std::find(expected_reads.begin(), expected_reads.end(), reg) ==
            expected_reads.end()) {
          expected_reads.push_back(reg);
        }
      };
      if (destination.type == X86_OP_REG) {
        Register(destination);
        expected_writes.push_back(destination.reg);
        if (arithmetic || swap) {
          read(destination.reg);
        }
      }
      if (!inc && !swap && source.type == X86_OP_REG) {
        if (source.reg == X86_REG_EDI) {
          REQUIRE(source.size == 4);
          REQUIRE(membase_low_zero);
        } else {
          Register(source);
        }
        REQUIRE(source.access == CS_AC_READ);
        read(source.reg);
      }
      if (destination.type == X86_OP_MEM ||
          (!inc && !swap && source.type == X86_OP_MEM)) {
        REQUIRE_FALSE((arithmetic || swap));
        const auto& mem = destination.type == X86_OP_MEM ? destination : source;
        REQUIRE((mem.size == 4 || mem.size == 8));
        REQUIRE((mem.mem.base == X86_REG_RSI || mem.mem.base == X86_REG_RSP ||
                 mem.mem.base == X86_REG_RDI));
        REQUIRE(mem.mem.index == X86_REG_INVALID);
        REQUIRE(mem.mem.segment == X86_REG_INVALID);
        read(mem.mem.base);
      }
      if (arithmetic) {
        expected_writes.push_back(X86_REG_EFLAGS);
      }
      cs_regs actual_reads{}, actual_writes{};
      uint8_t read_count = 0, write_count = 0;
      REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, actual_reads,
                             &read_count, actual_writes,
                             &write_count) == CS_ERR_OK);
      std::sort(expected_reads.begin(), expected_reads.end());
      std::sort(expected_writes.begin(), expected_writes.end());
      std::sort(actual_reads, actual_reads + read_count);
      std::sort(actual_writes, actual_writes + write_count);
      REQUIRE(read_count == expected_reads.size());
      REQUIRE(write_count == expected_writes.size());
      REQUIRE(std::equal(expected_reads.begin(), expected_reads.end(),
                         actual_reads));
      REQUIRE(std::equal(expected_writes.begin(), expected_writes.end(),
                         actual_writes));
      auto register_value = [&](const cs_x86_op& op) {
        if (op.reg == X86_REG_EDI) {
          REQUIRE(op.size == 4);
          REQUIRE(membase_low_zero);
          return Token{-2, 0, true, false};
        }
        auto token = registers[Register(op)];
        REQUIRE(token.guest != -1);
        if (op.size == 4) {
          token.low32 = true;
        }
        return token;
      };
      auto local_index = [&](const cs_x86_op& op) {
        REQUIRE(op.mem.base == X86_REG_RSP);
        auto found =
            std::find_if(locals.begin(), locals.end(), [&](const Local& l) {
              return op.mem.disp == l.offset && op.size == l.width;
            });
        REQUIRE(found != locals.end());
        return static_cast<size_t>(found - locals.begin());
      };
      if (arithmetic) {
        REQUIRE(phase == 0);
        REQUIRE(destination.type == X86_OP_REG);
        REQUIRE(destination.size == 8);
        auto left = register_value(destination);
        Token right{-2, inc ? 1 : source.type == X86_OP_IMM ? source.imm : 0};
        if (!inc && source.type != X86_OP_IMM) {
          right = register_value(source);
        }
        if (left.guest == -2) {
          std::swap(left, right);
        }
        REQUIRE(left.guest == 10);
        REQUIRE(left.delta == 0);
        REQUIRE_FALSE((left.low32 || left.swapped));
        REQUIRE(right.guest == -2);
        REQUIRE(right.delta == 1);
        REQUIRE_FALSE((right.low32 || right.swapped));
        REQUIRE(adds++ == 0);
        registers[Register(destination)] = {10, 1, false, false};
        continue;
      }
      if (swap) {
        REQUIRE(phase == 1);
        REQUIRE_FALSE(movbe_enabled);
        REQUIRE(destination.reg == X86_REG_ECX);
        REQUIRE(destination.size == 4);
        auto value = register_value(destination);
        REQUIRE(value.guest == 10);
        REQUIRE(value.delta == 1);
        REQUIRE(value.low32);
        REQUIRE_FALSE(value.swapped);
        REQUIRE(scratch_copies == 1);
        REQUIRE(swaps++ == 0);
        value.swapped = true;
        // BSWAP ECX writes ECX, also clearing RCX's upper half.
        registers[Register(destination)] = value;
        continue;
      }
      Token value;
      if (source.type == X86_OP_REG) {
        value = register_value(source);
      } else if (source.type == X86_OP_IMM) {
        REQUIRE((source.imm == 0 || source.imm == 1 || source.imm == kR0 ||
                 source.imm == kSentinel));
        value = {-2, source.imm};
      } else {
        REQUIRE(source.type == X86_OP_MEM);
        REQUIRE(source.access == CS_AC_READ);
        REQUIRE(destination.type == X86_OP_REG);
        REQUIRE(destination.size == source.size);
        if (source.mem.base == X86_REG_RSI) {
          REQUIRE(source.size == 8);
          REQUIRE(source.mem.disp >= real_hir::GPROffset(0));
          const auto offset = source.mem.disp - real_hir::GPROffset(0);
          REQUIRE(offset % 8 == 0);
          REQUIRE(offset / 8 < inputs.size());
          const auto guest = static_cast<size_t>(offset / 8);
          REQUIRE(guest != 0);
          REQUIRE(phase == (guest == 10 ? 0 : 1));
          REQUIRE(inputs[guest]++ == 0);
          value = {static_cast<int>(guest), 0};
        } else {
          const auto local = local_index(source);
          REQUIRE(locals[local].capture == -1);
          REQUIRE(writes[local] > 0);
          ++reads[local];
          value = memory[local];
          REQUIRE(value.guest >= 0);
        }
      }
      if (destination.type == X86_OP_REG) {
        REQUIRE_FALSE(movbe);
        if (source.type == X86_OP_REG) {
          REQUIRE(source.size == destination.size);
        }
        if (destination.size == 4) {
          value.low32 = true;  // MOV r32 clears upper32.
        }
        const auto reg = Register(destination);
        if (reg == 7) {
          REQUIRE(phase == 1);
          REQUIRE_FALSE(movbe_enabled);
          REQUIRE(destination.size == 4);
          REQUIRE(source.type == X86_OP_REG);
          REQUIRE(source.reg != X86_REG_ECX);
          REQUIRE(value.guest == 10);
          REQUIRE(value.delta == 1);
          REQUIRE(value.low32);
          REQUIRE_FALSE(value.swapped);
          REQUIRE(scratch_copies++ == 0);
        }
        registers[reg] = value;
      } else {
        REQUIRE(destination.type == X86_OP_MEM);
        if (destination.mem.base == X86_REG_RDI) {
          REQUIRE(phase == 1);
          REQUIRE(captures == 33);
          REQUIRE(r0_stores == 1);
          REQUIRE(destination.mem.disp == 0);
          REQUIRE(destination.size == 4);
          REQUIRE(source.type == X86_OP_REG);
          REQUIRE(source.size == 4);
          REQUIRE(value.guest == 10);
          REQUIRE(value.delta == 1);
          REQUIRE(value.low32);
          if (movbe) {
            REQUIRE(movbe_enabled);
            REQUIRE_FALSE(value.swapped);
            value.swapped = true;  // MOVBE stores byte-reversed uint32(token).
          } else {
            REQUIRE_FALSE(movbe_enabled);
            REQUIRE(source.reg == X86_REG_ECX);
            REQUIRE(value.swapped);
          }
          REQUIRE(value.swapped);
          REQUIRE(guest_stores++ == 0);
        } else if (destination.mem.base == X86_REG_RSI) {
          REQUIRE_FALSE(movbe);
          REQUIRE(destination.size == 8);
          REQUIRE(value.guest == -2);
          REQUIRE_FALSE(value.swapped);
          if (destination.mem.disp == real_hir::GPROffset(0)) {
            REQUIRE(phase == 0);
            REQUIRE(value.delta == kR0);
            REQUIRE(r0_stores++ == 0);
          } else {
            REQUIRE(phase == 2);
            REQUIRE(guest_stores == 1);
            REQUIRE(destination.mem.disp == real_hir::GPROffset(10));
            REQUIRE(value.delta == kSentinel);
            REQUIRE(overwrites++ == 0);
          }
        } else {
          REQUIRE_FALSE(movbe);
          const auto local = local_index(destination);
          const auto owner = locals[local].capture;
          if (owner >= 0) {
            REQUIRE(phase == 1);
            REQUIRE(owner == static_cast<int>(captures++));
            if (owner == 0 || owner == 32) {
              REQUIRE(value.guest == -2);
              REQUIRE(value.delta == (owner == 0 ? kR0 : 0));
            } else {
              REQUIRE(value.guest == owner);
              REQUIRE(value.delta == (owner == 10 ? 1 : 0));
              REQUIRE_FALSE(value.low32);
            }
            REQUIRE_FALSE(value.swapped);
          } else {
            REQUIRE(value.guest >= 0);
            REQUIRE_FALSE((value.low32 || value.swapped));
          }
          memory[local] = value;
          ++writes[local];
        }
      }
    }
    REQUIRE(address == map[phase + 1].code_offset);
  }
  REQUIRE(captures == 33);
  REQUIRE(adds == 1);
  REQUIRE(r0_stores == 1);
  REQUIRE(guest_stores == 1);
  REQUIRE(overwrites == 1);
  REQUIRE(swaps == (movbe_enabled ? 0 : 1));
  REQUIRE(scratch_copies == (movbe_enabled ? 0 : 1));
  for (size_t reg = 0; reg < 32; ++reg) {
    REQUIRE(inputs[reg] == (reg == 0 ? 0 : 1));
  }
  for (size_t i = 0; i < locals.size(); ++i) {
    REQUIRE(reads[i] == locals[i].reads);
    REQUIRE(writes[i] == locals[i].writes);
  }
}

TEST_CASE(
    "Dormant capture binds current GPR state to actual ordinary STW bytes",
    "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  REQUIRE(cvars::align_all_basic_blocks == 0);
  REQUIRE((cvars::full_optimization_even_with_debug ||
           (!cvars::debug && !cvars::store_all_context_values)));
  struct RestoreMMIO {
    bool original = cvars::emit_inline_mmio_checks;
    ~RestoreMMIO() { cvars::emit_inline_mmio_checks = original; }
  } restore_mmio;
  cvars::emit_inline_mmio_checks =
      false;  // This bounded direct-store recipe only.
  Fixture fixture;
  CurrentPPC builder(fixture.processor.frontend());
  builder.SourceOffset(kPC - 4);
  // Synthetic setup bypasses StoreGPR's per-instruction trace bookkeeping,
  // which is initialized by the full PPCHIRBuilder::Emit loop, not its ctor.
  builder.StoreContext(real_hir::GPROffset(0),
                       builder.LoadConstantUint64(kR0));
  auto entry = builder.LoadGPR(10);
  auto computed = builder.Add(entry, builder.LoadConstantInt64(1));
  builder.StoreContext(real_hir::GPROffset(10), computed);
  builder.SourceOffset(kPC);
  Capture capture;
  REQUIRE(ppc::BuildDormantCheckpointGprCapture(builder, kPC, kWord, &capture));
  REQUIRE(builder.locals().size() == 33);
  ppc::InstrData instruction{};
  instruction.code = kWord;
  instruction.address = kPC;
  instruction.opcode = ppc::LookupOpcode(kWord);
  instruction.opcode_info = &ppc::GetOpcodeInfo(instruction.opcode);
  REQUIRE(instruction.opcode == ppc::PPCOpcode::stw);
  REQUIRE(instruction.opcode_info->emit);
  REQUIRE(instruction.opcode_info->emit(builder, instruction) == 0);
  auto access = builder.current_block()->instr_tail;
  REQUIRE(access->opcode->num == OPCODE_STORE_OFFSET);
  builder.SourceOffset(kPC + 4);
  builder.StoreContext(real_hir::GPROffset(10),
                       builder.LoadConstantUint64(kSentinel));
  builder.SourceOffset(kPC + 8);
  builder.Return();
  REQUIRE(builder.Finalize());
  bool promoted = false, optimized = false;
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&fixture.processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::ContextPromotionPass>());
  compiler.AddPass(std::make_unique<BindingProof>(capture, computed, access,
                                                  true, promoted));
  auto simplify = std::make_unique<passes::ConditionalGroupPass>();
  simplify->AddPass(std::make_unique<passes::SimplificationPass>());
  simplify->AddPass(std::make_unique<passes::ConstantPropagationPass>());
  compiler.AddPass(std::move(simplify));
  REQUIRE(fixture.backend->machine_info()->supports_extended_load_store);
  REQUIRE(fixture.backend->machine_info()->register_sets[0].count == 7);
  REQUIRE(X64Emitter::GPR_COUNT == 7);
  compiler.AddPass(std::make_unique<passes::MemorySequenceCombinationPass>());
  compiler.AddPass(std::make_unique<passes::SimplificationPass>());
  compiler.AddPass(std::make_unique<passes::DeadCodeEliminationPass>());
  compiler.AddPass(std::make_unique<BindingProof>(capture, computed, access,
                                                  false, optimized));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      fixture.backend->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE(promoted);
  REQUIRE(optimized);
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
  auto locals = CheckHIR(builder, capture);
  XbyakAllocator allocator;
  real_hir::HIREmitter emitter(fixture.backend, &allocator);
  const auto scratch = emitter.getCode();
  const auto unwind_size = fixture.cache->QueryUnwindStorageSize();
  const bool zero_low = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                            fixture.memory.virtual_membase())) == 0;
  REQUIRE(emitter.CanUseMembaseLow32As0() == zero_low);
  const bool movbe = emitter.IsFeatureEnabled(kX64EmitMovbe);
  INFO("Observed host-selected ordinary store encoding: "
       << (movbe ? "MOVBE32" : "ECX/BSWAP/MOV32"));
  uint64_t pc = 0;
  size_t span = 0, callbacks = 0;
  REQUIRE(emitter.WithPreparedDraft(
      builder, [&](const X64Emitter::PreparedDraft& view) {
        ++callbacks;
        pc = fixture.cache->execute_base_address() + view.execute_offset();
        span = view.allocation_size();
        REQUIRE(reinterpret_cast<uint64_t>(view.code().data()) == pc);
        CheckCommitted(pc, span, PAGE_EXECUTE_READWRITE);
        emitter.Snapshot(view, scratch);
        CheckBytes(locals, view, zero_low, movbe);
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
      }));
  REQUIRE(callbacks == 1);
  REQUIRE(emitter.getCode() == scratch);
  emitter.CheckDiscarded();
  fixture.CheckAborted(pc, span);
  REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
  const uint8_t trap = 0xCC;
  uint64_t next_pc = 0;
  size_t next_span = 0;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      &trap, CodeInfo(1), [&](PreparedGuestCode& next) {
        next_pc = fixture.cache->execute_base_address() + next.execute_offset();
        next_span = next.allocation_size();
        REQUIRE(next_pc == pc + span);
        fixture.CheckUnpublished(next_pc, next_span);
      }));
  fixture.CheckAborted(pc, span);
  fixture.CheckAborted(next_pc, next_span);
  // No prepared byte is invoked. This synthetic straight-line GPR-only case
  // proves the observed encoding path, not both host feature selections. The
  // actual game's preceding callback-install call/CFG is unsupported by the
  // capture helper. No checkpoint pair, full bank/precision validity, Xbox ABI,
  // invocation ownership, publication or mapping-lease admission is
  // established.
}

}  // namespace capture_stw

namespace local_i64_constants {

constexpr uint32_t kPC = 0x80009000;
constexpr std::array<uint64_t, 8> kValues = {
    0x000000002468ACE0ull,  // The nonzero captured r0 in the ordinary STW case.
    0x000000007FFFFFFFull, 0x0000000080000000ull, 0xFFFFFFFF80000000ull,
    0xFFFFFFFF7FFFFFFFull, 0x0123456789ABCDEFull, 0x8000000000000000ull,
    0xFFFFFFFFFFFFFFFFull};

// Reconstruct the semantic eight-byte local value from every decoded write.
// This does not select the writer's immediate or split-immediate strategy.
void CheckStores(const std::array<hir::Value*, kValues.size()>& slots,
                 const X64Emitter::PreparedDraft& view, uint64_t membase) {
  REQUIRE(view.code().size() <= real_hir::kMaxBytes);
  const auto map = view.source_map();
  REQUIRE(map.size() == kValues.size() + 1);
  REQUIRE(view.info().code_size.prolog <= view.code().size());
  REQUIRE(view.info().code_size.body <=
          view.code().size() - view.info().code_size.prolog);
  std::array<uint32_t, kValues.size()> offsets{};
  for (size_t row = 0; row < slots.size(); ++row) {
    REQUIRE(slots[row]->IsConstant());
    REQUIRE(slots[row]->type == hir::INT32_TYPE);
    offsets[row] = slots[row]->constant.u32;
    REQUIRE(offsets[row] >= StackLayout::GUEST_STACK_SIZE);
    REQUIRE(offsets[row] % 8 == 0);
    REQUIRE(view.info().stack_size >= 8);
    REQUIRE(offsets[row] <= view.info().stack_size - 8);
    for (size_t earlier = 0; earlier < row; ++earlier) {
      REQUIRE(offsets[earlier] != offsets[row]);
    }
  }
  for (size_t marker = 0; marker < map.size(); ++marker) {
    REQUIRE(map[marker].guest_address ==
            kPC + static_cast<uint32_t>(marker * 4));
    REQUIRE(map[marker].code_offset >= view.info().code_size.prolog);
    REQUIRE(map[marker].code_offset <= view.code().size());
    REQUIRE(map[marker].code_offset <=
            view.info().code_size.prolog + view.info().code_size.body);
    if (marker) {
      REQUIRE(map[marker - 1].code_offset < map[marker].code_offset);
    }
  }
  real_hir::Decoder decoder;
  REQUIRE(decoder.instruction);
  for (size_t row = 0; row < kValues.size(); ++row) {
    std::array<bool, 8> written{};
    std::array<uint8_t, 8> bytes{};
    const uint8_t* cursor = view.code().data() + map[row].code_offset;
    size_t remaining = map[row + 1].code_offset - map[row].code_offset;
    REQUIRE(remaining <= 32);
    uint64_t address = map[row].code_offset;
    while (remaining) {
      REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                             decoder.instruction));
      REQUIRE(decoder.instruction->id == X86_INS_MOV);
      REQUIRE(decoder.instruction->detail);
      const auto& x = decoder.instruction->detail->x86;
      REQUIRE(x.op_count == 2);
      REQUIRE(x.addr_size == 8);
      REQUIRE(x.eflags == 0);
      for (auto prefix : x.prefix) {
        REQUIRE(prefix == 0);
      }
      const auto& destination = x.operands[0];
      const auto& source = x.operands[1];
      REQUIRE(destination.type == X86_OP_MEM);
      REQUIRE(destination.access == CS_AC_WRITE);
      REQUIRE((destination.size == 4 || destination.size == 8));
      const auto width = static_cast<size_t>(destination.size);
      REQUIRE(destination.mem.base == X86_REG_RSP);
      REQUIRE(destination.mem.index == X86_REG_INVALID);
      REQUIRE(destination.mem.segment == X86_REG_INVALID);
      REQUIRE(destination.mem.disp >= static_cast<int64_t>(offsets[row]));
      const auto first =
          static_cast<size_t>(destination.mem.disp - offsets[row]);
      REQUIRE(first <= size_t{8} - width);
      REQUIRE(first % width == 0);
      uint64_t value = 0;
      std::vector<unsigned> expected_reads{X86_REG_RSP};
      if (source.type == X86_OP_IMM) {
        value = static_cast<uint64_t>(source.imm);
      } else {
        REQUIRE(source.type == X86_OP_REG);
        REQUIRE(source.access == CS_AC_READ);
        REQUIRE(source.size == destination.size);
        // MovMem64 may use the known membase value itself, or its known-zero
        // low half. No unassigned allocator/scratch register is accepted.
        REQUIRE(source.reg == (source.size == 8 ? X86_REG_RDI : X86_REG_EDI));
        if (source.size == 4) {
          REQUIRE(static_cast<uint32_t>(membase) == 0);
          value = 0;
        } else {
          value = membase;
        }
        expected_reads.push_back(source.reg);
      }
      cs_regs reads{}, writes{};
      uint8_t read_count = 0, write_count = 0;
      REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                             &read_count, writes, &write_count) == CS_ERR_OK);
      REQUIRE(write_count == 0);
      std::sort(expected_reads.begin(), expected_reads.end());
      std::sort(reads, reads + read_count);
      REQUIRE(read_count == expected_reads.size());
      REQUIRE(std::equal(expected_reads.begin(), expected_reads.end(), reads));
      for (size_t byte = 0; byte < width; ++byte) {
        REQUIRE_FALSE(written[first + byte]);
        written[first + byte] = true;
        bytes[first + byte] = static_cast<uint8_t>(value >> (byte * 8));
      }
    }
    REQUIRE(address == map[row + 1].code_offset);
    for (size_t byte = 0; byte < bytes.size(); ++byte) {
      REQUIRE(written[byte]);
      REQUIRE(bytes[byte] == static_cast<uint8_t>(kValues[row] >> (byte * 8)));
    }
  }
}

TEST_CASE("Unpublished x64 local stores retain nonzero INT64 constant bits",
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
  real_hir::CurrentHIR builder;
  std::array<hir::Value*, kValues.size()> slots{};
  for (size_t row = 0; row < slots.size(); ++row) {
    slots[row] = builder.AllocLocal(hir::INT64_TYPE);
    builder.SourceOffset(kPC + static_cast<uint32_t>(row * 4));
    builder.StoreLocal(slots[row], builder.LoadConstantUint64(kValues[row]));
  }
  builder.SourceOffset(kPC + static_cast<uint32_t>(kValues.size() * 4));
  builder.Return();
  REQUIRE(builder.Finalize());
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&fixture.processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      fixture.backend->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE(builder.locals().size() == kValues.size());
  XbyakAllocator allocator;
  real_hir::HIREmitter emitter(fixture.backend, &allocator);
  const auto scratch = emitter.getCode();
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
        emitter.Snapshot(view, scratch);
        CheckStores(
            slots, view,
            reinterpret_cast<uintptr_t>(fixture.memory.virtual_membase()));
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
      }));
  REQUIRE(callbacks == 1);
  REQUIRE(emitter.getCode() == scratch);
  emitter.CheckDiscarded();
  fixture.CheckAborted(pc, span);
  REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
  // Compiled and decoded only. No emitted instruction, guest access or entry
  // is invoked; this does not establish game-site or checkpoint eligibility.
}

}  // namespace local_i64_constants

namespace postcall_gpr {

using namespace hir;
using Capture = ppc::DormantCheckpointGprCapture;
using Local = midblock_gpr::Local;
constexpr uint32_t kPC = 0x80009010;
constexpr uint32_t kCallPC = kPC - 8;
constexpr uint32_t kWord = 0x91400000;

Value* Prefix(HIRBuilder& builder, X64Function& callee) {
  builder.SourceOffset(kPC - 16);
  auto before = builder.LoadContext(real_hir::GPROffset(3), INT64_TYPE);
  builder.StoreContext(real_hir::GPROffset(3),
                       builder.Add(before, builder.LoadConstantInt64(99)));
  builder.StoreContext(real_hir::GPROffset(10), builder.LoadConstantInt64(55));
  builder.SourceOffset(kCallPC);
  builder.SetReturnAddress(builder.LoadConstantUint64(kCallPC + 4));
  builder.StoreContext(offsetof(ppc::PPCContext, lr),
                       builder.LoadConstantUint64(kCallPC + 4));
  builder.Call(&callee);
  builder.SourceOffset(kCallPC + 4);
  builder.StoreContext(real_hir::GPROffset(10), builder.LoadConstantInt64(1));
  builder.SourceOffset(kPC);
  return before;
}

class PromotionProof final : public compiler::CompilerPass {
 public:
  PromotionProof(Block* body, const Capture& capture, bool& observed)
      : body_(body), capture_(capture), observed_(observed) {}
  bool Run(HIRBuilder* builder) override {
    const auto predecessor = builder->first_block();
    REQUIRE(predecessor->next == body_);
    REQUIRE(body_->prev == predecessor);
    REQUIRE(body_->next == nullptr);
    REQUIRE(predecessor->incoming_edge_head == nullptr);
    REQUIRE(body_->outgoing_edge_head == nullptr);
    auto branch = predecessor->instr_tail;
    REQUIRE(branch->opcode->num == OPCODE_BRANCH);
    REQUIRE(branch->src1.label->block == body_);
    REQUIRE(branch->prev->opcode->num == OPCODE_CALL);
    REQUIRE(branch->prev->flags == 0);
    auto edge = body_->incoming_edge_head;
    REQUIRE(edge);
    REQUIRE(edge == predecessor->outgoing_edge_head);
    REQUIRE(edge->src == predecessor);
    REQUIRE(edge->dest == body_);
    REQUIRE(edge->flags == (Edge::UNCONDITIONAL | Edge::DOMINATES));
    REQUIRE(edge->incoming_next == nullptr);
    REQUIRE(edge->incoming_prev == nullptr);
    REQUIRE(edge->outgoing_next == nullptr);
    REQUIRE(edge->outgoing_prev == nullptr);
    size_t captures = 0, context_stores = 0;
    for (auto i = body_->instr_head; i; i = i->next) {
      if (i->opcode->num == OPCODE_STORE_CONTEXT) {
        REQUIRE(i->src1.offset == real_hir::GPROffset(10));
        REQUIRE(i->src2.value->IsConstant());
        REQUIRE(i->src2.value->constant.u64 == capture_stw::kSentinel);
        ++context_stores;  // The earlier post-call r10=1 context store is dead.
      }
      if (i->opcode->num != OPCODE_STORE_LOCAL) {
        continue;
      }
      if (captures < 32) {
        REQUIRE(i->src1.value == capture_.gpr_slots[captures]);
        auto definition = i->src2.value->def;
        REQUIRE(definition);
        REQUIRE(definition->block == body_);
        if (captures == 10) {
          REQUIRE(definition->opcode->num == OPCODE_ASSIGN);
          REQUIRE(definition->src1.value->IsConstant());
          REQUIRE(definition->src1.value->constant.u64 == 1);
        } else {
          REQUIRE(definition->opcode->num == OPCODE_LOAD_CONTEXT);
          REQUIRE(definition->src1.offset == real_hir::GPROffset(captures));
        }
      } else {
        REQUIRE(captures == 32);
        REQUIRE(i->src1.value == capture_.ea_slot);
        REQUIRE(i->src2.value->IsConstant());
        REQUIRE(i->src2.value->type == INT32_TYPE);
        REQUIRE(i->src2.value->constant.u32 == 0);
      }
      ++captures;
    }
    REQUIRE(captures == 33);
    REQUIRE(context_stores == 1);
    observed_ = true;
    return true;
  }

 private:
  Block* body_;
  const Capture& capture_;
  bool& observed_;
};

std::vector<Local> CheckHIR(HIRBuilder& builder, Block* body,
                            const Capture& capture,
                            const backend::MachineInfo& machine) {
  std::vector<Local> locals;
  for (auto slot : builder.locals()) {
    int owner = -1;
    for (size_t reg = 0; reg < 32; ++reg) {
      if (slot == capture.gpr_slots[reg]) {
        owner = static_cast<int>(reg);
      }
    }
    if (slot == capture.ea_slot) {
      owner = 32;
    }
    REQUIRE(slot->type == (owner == 32 ? INT32_TYPE : INT64_TYPE));
    REQUIRE(std::none_of(locals.begin(), locals.end(),
                         [&](const Local& l) { return l.slot == slot; }));
    locals.push_back({slot, owner == 32 ? 4u : 8u, owner});
  }
  REQUIRE(locals.size() >= 33);
  REQUIRE(locals.size() - 33 >= 31 - X64Emitter::GPR_COUNT);
  size_t captures = 0, loads = 0;
  bool interval = false, finished = false;
  for (auto i = body->instr_head; i; i = i->next) {
    if (i->opcode->num == OPCODE_SOURCE_OFFSET) {
      if (i->src1.offset == kPC) {
        interval = true;
      }
      if (i->src1.offset == kPC + 4) {
        interval = false;
        finished = true;
      }
      continue;
    }
    if (!interval) {
      continue;
    }
    REQUIRE(i->flags == 0);
    if (i->opcode->num == OPCODE_LOAD_CONTEXT) {
      canonical_gpr::CheckAllocated(i->dest, machine);
      REQUIRE(i->dest->def->block == body);
      REQUIRE(i->src1.offset != real_hir::GPROffset(10));
      ++loads;
      continue;
    }
    REQUIRE((i->opcode->num == OPCODE_LOAD_LOCAL ||
             i->opcode->num == OPCODE_STORE_LOCAL));
    auto found =
        std::find_if(locals.begin(), locals.end(),
                     [&](const Local& l) { return l.slot == i->src1.value; });
    REQUIRE(found != locals.end());
    if (i->opcode->num == OPCODE_LOAD_LOCAL) {
      REQUIRE(found->capture == -1);
      canonical_gpr::CheckAllocated(i->dest, machine);
      REQUIRE(i->dest->GetLocalSlot() == found->slot);
      ++found->reads;
    } else {
      REQUIRE(i->src2.value->type ==
              (found->width == 4 ? INT32_TYPE : INT64_TYPE));
      if (found->capture >= 0) {
        REQUIRE(found->capture == static_cast<int>(captures++));
        if (found->capture == 10 || found->capture == 32) {
          REQUIRE(i->src2.value->IsConstant());
          if (found->capture == 10) {
            REQUIRE(i->src2.value->constant.u64 == 1);
          } else {
            REQUIRE(i->src2.value->constant.u32 == 0);
          }
        } else {
          canonical_gpr::CheckAllocated(i->src2.value, machine);
        }
      } else {
        canonical_gpr::CheckAllocated(i->src2.value, machine);
        REQUIRE(i->src2.value->GetLocalSlot() == found->slot);
      }
      ++found->writes;
    }
  }
  REQUIRE(finished);
  REQUIRE(captures == 33);
  REQUIRE(loads == 31);
  for (const auto& local : locals) {
    REQUIRE(local.writes == 1);
    REQUIRE((local.capture >= 0 ? local.reads == 0 : local.reads > 0));
  }
  return locals;
}

// The optional prefix is admitted only by CheckStackSync below. Tokens denote
// fresh post-call context inputs, with no relation to pre-call values or to an
// observed callee. Existing entry/STW decoders are intentionally unchanged.
void CheckBytes(std::vector<Local>& locals,
                const X64Emitter::PreparedDraft& view, bool membase_low_zero,
                size_t prefix_size = 0) {
  REQUIRE(view.code().size() <= real_hir::kMaxBytes);
  const auto stack = view.info().stack_size;
  REQUIRE((stack + 8) % 16 == 0);
  for (auto& local : locals) {
    REQUIRE(local.slot->IsConstant());
    REQUIRE(local.slot->type == INT32_TYPE);
    local.offset = local.slot->constant.u32;
    REQUIRE(local.offset >= StackLayout::GUEST_STACK_SIZE);
    REQUIRE(local.offset % local.width == 0);
    REQUIRE(local.width <= stack);
    REQUIRE(local.offset <= stack - local.width);
  }
  for (size_t a = 0; a < locals.size(); ++a) {
    for (size_t b = a + 1; b < locals.size(); ++b) {
      REQUIRE((locals[a].offset + locals[a].width <= locals[b].offset ||
               locals[b].offset + locals[b].width <= locals[a].offset));
    }
  }
  const auto map = view.source_map();
  constexpr std::array<uint32_t, 7> pcs = {kPC - 16, kCallPC, kCallPC + 4, kPC,
                                           kPC + 4,  kPC + 8, kPC + 12};
  REQUIRE(map.size() == pcs.size());
  const auto prolog = view.info().code_size.prolog;
  REQUIRE(prolog <= view.code().size());
  REQUIRE(view.info().code_size.body <= view.code().size() - prolog);
  for (size_t n = 0; n < map.size(); ++n) {
    REQUIRE(map[n].guest_address == pcs[n]);
    REQUIRE(map[n].code_offset >= prolog);
    REQUIRE(map[n].code_offset <= prolog + view.info().code_size.body);
    if (n) {
      REQUIRE(map[n].code_offset >= map[n - 1].code_offset);
    }
  }
  REQUIRE(map[3].code_offset < map[4].code_offset);
  REQUIRE(map[4].code_offset - map[3].code_offset <= 2048);
  using Token = midblock_gpr::Token;
  std::array<Token, X64Emitter::GPR_COUNT> registers{};
  std::vector<Token> memory(locals.size());
  std::vector<size_t> reads(locals.size()), writes(locals.size());
  std::array<size_t, 32> inputs{};
  size_t captures = 0;
  real_hir::Decoder decoder;
  REQUIRE(decoder.instruction);
  REQUIRE(prefix_size < map[4].code_offset - map[3].code_offset);
  const uint8_t* cursor = view.code().data() + map[3].code_offset + prefix_size;
  size_t remaining = map[4].code_offset - map[3].code_offset - prefix_size;
  uint64_t address = map[3].code_offset + prefix_size;
  while (remaining) {
    REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                           decoder.instruction));
    REQUIRE(decoder.instruction->id == X86_INS_MOV);
    REQUIRE(decoder.instruction->detail);
    const auto& x = decoder.instruction->detail->x86;
    REQUIRE(x.addr_size == 8);
    REQUIRE(x.op_count == 2);
    REQUIRE(x.eflags == 0);
    for (auto prefix : x.prefix) {
      REQUIRE(prefix == 0);
    }
    const auto& destination = x.operands[0];
    const auto& source = x.operands[1];
    // This recipe's HIR operations each lower to one memory MOV. Reject
    // unaccounted register copies/materializations as well as non-MOV effects.
    REQUIRE(((destination.type == X86_OP_REG && source.type == X86_OP_MEM) ||
             (destination.type == X86_OP_MEM && source.type != X86_OP_MEM)));
    REQUIRE((destination.size == 4 || destination.size == 8));
    REQUIRE(destination.access == CS_AC_WRITE);
    REQUIRE(source.access == (source.type == X86_OP_IMM ? 0 : CS_AC_READ));
    std::vector<unsigned> expected_reads, expected_writes;
    auto read = [&](unsigned reg) {
      if (std::find(expected_reads.begin(), expected_reads.end(), reg) ==
          expected_reads.end()) {
        expected_reads.push_back(reg);
      }
    };
    auto local_index = [&](const cs_x86_op& op) {
      REQUIRE(op.mem.base == X86_REG_RSP);
      auto found =
          std::find_if(locals.begin(), locals.end(), [&](const Local& l) {
            return op.mem.disp == l.offset && op.size == l.width;
          });
      REQUIRE(found != locals.end());
      return static_cast<size_t>(found - locals.begin());
    };
    for (const auto* op : {&source, &destination}) {
      if (op->type == X86_OP_MEM) {
        REQUIRE((op->size == 4 || op->size == 8));
        REQUIRE((op->mem.base == X86_REG_RSI || op->mem.base == X86_REG_RSP));
        REQUIRE(op->mem.index == X86_REG_INVALID);
        REQUIRE(op->mem.segment == X86_REG_INVALID);
        read(op->mem.base);
      }
    }
    Token value;
    if (source.type == X86_OP_IMM) {
      value = {-2, source.imm, false};
    } else if (source.type == X86_OP_REG) {
      read(source.reg);
      if (source.reg == X86_REG_EDI) {
        REQUIRE(membase_low_zero);
        REQUIRE(source.size == 4);
        value = {-2, 0, true};
      } else {
        value = registers[midblock_gpr::Register(source)];
        REQUIRE(value.guest != -1);
        if (source.size == 4) {
          value.low32 = true;
        }
      }
    } else {
      REQUIRE(source.type == X86_OP_MEM);
      if (source.mem.base == X86_REG_RSI) {
        REQUIRE(source.size == 8);
        REQUIRE(source.mem.disp >= real_hir::GPROffset(0));
        const auto offset = source.mem.disp - real_hir::GPROffset(0);
        REQUIRE(offset % 8 == 0);
        REQUIRE(offset / 8 < 32);
        const auto reg = static_cast<size_t>(offset / 8);
        REQUIRE(reg != 10);
        REQUIRE(inputs[reg]++ == 0);
        value = {static_cast<int>(reg), 0, false};
      } else {
        const auto local = local_index(source);
        REQUIRE(locals[local].capture == -1);
        REQUIRE(writes[local] == 1);
        value = memory[local];
        ++reads[local];
      }
    }
    if (destination.type == X86_OP_REG) {
      const auto reg = midblock_gpr::Register(destination);
      if (destination.size == 4) {
        // A real 32-bit destination clears its upper half. Only literals may
        // subsequently serve a full-width capture; unknown tokens stay narrow.
        if (value.guest == -2) {
          value.delta = static_cast<uint32_t>(value.delta);
        }
        value.low32 = value.guest != -2;
      }
      registers[reg] = value;
      expected_writes.push_back(destination.reg);
    } else {
      REQUIRE(destination.type == X86_OP_MEM);
      const auto local = local_index(destination);
      const auto owner = locals[local].capture;
      if (owner >= 0) {
        REQUIRE(owner == static_cast<int>(captures++));
        if (owner == 10 || owner == 32) {
          REQUIRE(value.guest == -2);
          REQUIRE(value.delta == (owner == 10 ? 1 : 0));
        } else {
          REQUIRE(value.guest == owner);
          REQUIRE(value.delta == 0);
          REQUIRE_FALSE(value.low32);
        }
      } else {
        REQUIRE(value.guest >= 0);
        REQUIRE(value.guest != 10);
        REQUIRE_FALSE(value.low32);
      }
      memory[local] = value;
      ++writes[local];
    }
    cs_regs actual_reads{}, actual_writes{};
    uint8_t read_count = 0, write_count = 0;
    REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, actual_reads,
                           &read_count, actual_writes,
                           &write_count) == CS_ERR_OK);
    std::sort(expected_reads.begin(), expected_reads.end());
    std::sort(expected_writes.begin(), expected_writes.end());
    std::sort(actual_reads, actual_reads + read_count);
    std::sort(actual_writes, actual_writes + write_count);
    REQUIRE(read_count == expected_reads.size());
    REQUIRE(write_count == expected_writes.size());
    REQUIRE(
        std::equal(expected_reads.begin(), expected_reads.end(), actual_reads));
    REQUIRE(std::equal(expected_writes.begin(), expected_writes.end(),
                       actual_writes));
  }
  REQUIRE(address == map[4].code_offset);
  REQUIRE(captures == 33);
  for (size_t reg = 0; reg < 32; ++reg) {
    REQUIRE(inputs[reg] == (reg == 10 ? 0 : 1));
  }
  for (size_t i = 0; i < locals.size(); ++i) {
    REQUIRE(reads[i] == locals[i].reads);
    REQUIRE(writes[i] == locals[i].writes);
  }
}

// This one recipe uses the size16 helper. Decode only its TEST/JNZ prefix and
// the branch-selected, terminal CALL/data/JMP stub, never the inline data or
// unrelated prolog/epilog/tails. All addresses are numeric observations only.
size_t CheckStackSync(const X64Emitter::PreparedDraft& view, uint64_t pc,
                      const X64Backend& backend) {
  REQUIRE(cvars::enable_host_guest_stack_synchronization);
  const auto code = view.code();
  REQUIRE(reinterpret_cast<uint64_t>(code.data()) == pc);
  REQUIRE(view.info().code_size.total == code.size());
  REQUIRE(code.size() <= real_hir::kMaxBytes);
  REQUIRE(view.source_map().size() == 7);
  REQUIRE(view.source_map()[3].guest_address == kPC);
  REQUIRE(view.source_map()[4].guest_address == kPC + 4);
  const size_t begin = view.source_map()[3].code_offset;
  const size_t end = view.source_map()[4].code_offset;
  REQUIRE(begin < end);
  REQUIRE(end <= code.size());
  constexpr size_t kPrefixSize = 12;
  REQUIRE(end - begin > kPrefixSize);
  constexpr std::array<uint8_t, 8> kPrefix = {0xF7, 0xC4, 0x0F, 0,
                                              0,    0,    0x0F, 0x85};
  REQUIRE(std::equal(kPrefix.begin(), kPrefix.end(), code.data() + begin));
  real_hir::Decoder decoder;
  REQUIRE(decoder.instruction);
  const uint8_t* cursor = code.data() + begin;
  size_t remaining = kPrefixSize;
  uint64_t address = pc + begin;
  auto decode = [&](unsigned id, size_t size) {
    const auto start = address;
    REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                           decoder.instruction));
    REQUIRE(decoder.instruction->id == id);
    REQUIRE(decoder.instruction->size == size);
    REQUIRE(decoder.instruction->address == start);
    REQUIRE(decoder.instruction->detail);
    const auto& x = decoder.instruction->detail->x86;
    REQUIRE(x.addr_size == 8);
    for (auto prefix : x.prefix) {
      REQUIRE(prefix == 0);
    }
  };
  auto effects = [&](std::vector<unsigned> expected_reads,
                     std::vector<unsigned> expected_writes) {
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
    REQUIRE(std::equal(expected_writes.begin(), expected_writes.end(), writes));
  };
  decode(X86_INS_TEST, 6);
  const auto& test = decoder.instruction->detail->x86;
  constexpr uint64_t kTestFlags =
      X86_EFLAGS_RESET_OF | X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF |
      X86_EFLAGS_UNDEFINED_AF | X86_EFLAGS_MODIFY_PF | X86_EFLAGS_RESET_CF;
  REQUIRE(test.eflags == kTestFlags);
  REQUIRE(test.op_count == 2);
  REQUIRE(test.operands[0].type == X86_OP_REG);
  REQUIRE(test.operands[0].reg == X86_REG_ESP);
  REQUIRE(test.operands[0].size == 4);
  REQUIRE(test.operands[0].access == CS_AC_READ);
  REQUIRE(test.operands[1].type == X86_OP_IMM);
  REQUIRE(test.operands[1].imm == 15);
  REQUIRE(test.operands[1].size == 4);
  REQUIRE(test.operands[1].access == 0);
  effects({X86_REG_ESP}, {X86_REG_EFLAGS});
  decode(X86_INS_JNE, 6);
  const auto& branch = decoder.instruction->detail->x86;
  REQUIRE(branch.eflags == X86_EFLAGS_TEST_ZF);
  REQUIRE(branch.op_count == 1);
  REQUIRE(branch.operands[0].type == X86_OP_IMM);
  REQUIRE(branch.operands[0].access == 0);
  const auto stub = static_cast<uint64_t>(branch.operands[0].imm);
  int32_t branch_displacement = 0;
  std::memcpy(&branch_displacement, code.data() + begin + 8, 4);
  REQUIRE(branch_displacement > 0);
  REQUIRE(stub == address + branch_displacement);
  effects({X86_REG_EFLAGS}, {});
  REQUIRE(remaining == 0);
  const auto resume = address;
  REQUIRE(resume == pc + begin + kPrefixSize);
  REQUIRE(stub >= pc);
  const uint64_t stub_offset = stub - pc;
  const size_t prolog = view.info().code_size.prolog;
  REQUIRE(prolog <= code.size());
  REQUIRE(view.info().code_size.body <= code.size() - prolog);
  REQUIRE(stub_offset >= prolog + view.info().code_size.body);
  REQUIRE(stub_offset <= code.size());
  // The overflow tail was queued by the prolog before this synchronization
  // tail. This fixture queues no later tails; total size bounds this stub.
  constexpr size_t kStubSize = 5 + 2 + 5;
  REQUIRE(code.size() - stub_offset == kStubSize);
  REQUIRE(view.info().stack_size >= 256);
  REQUIRE(view.info().stack_size < 65536);
  const auto helper =
      backend.synchronize_guest_and_host_stack_helper_for_size(2);
  REQUIRE(helper);
  cursor = code.data() + stub_offset;
  remaining = 5;  // CALL only; the following two bytes are data.
  address = stub;
  REQUIRE(*cursor == 0xE8);
  decode(X86_INS_CALL, 5);
  const auto& call = decoder.instruction->detail->x86;
  REQUIRE(call.eflags == 0);
  REQUIRE(call.op_count == 1);
  REQUIRE(call.operands[0].type == X86_OP_IMM);
  REQUIRE(call.operands[0].access == 0);
  REQUIRE(static_cast<uint64_t>(call.operands[0].imm) ==
          reinterpret_cast<uint64_t>(helper));
  effects({X86_REG_RSP, X86_REG_RIP}, {X86_REG_RSP});
  REQUIRE(remaining == 0);
  const uint32_t frame_size = cursor[0] | (uint32_t{cursor[1]} << 8);
  REQUIRE(frame_size == view.info().stack_size);
  cursor += 2;
  address += 2;
  remaining = 5;
  REQUIRE(*cursor == 0xE9);
  decode(X86_INS_JMP, 5);
  const auto& jump = decoder.instruction->detail->x86;
  REQUIRE(jump.eflags == 0);
  REQUIRE(jump.op_count == 1);
  REQUIRE(jump.operands[0].type == X86_OP_IMM);
  REQUIRE(jump.operands[0].access == 0);
  REQUIRE(static_cast<uint64_t>(jump.operands[0].imm) == resume);
  effects({}, {});
  REQUIRE(remaining == 0);
  REQUIRE(cursor == code.data() + code.size());
  REQUIRE(address == pc + code.size());
  return kPrefixSize;
}

void RunCapture(bool synchronize_stack) {
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  REQUIRE(cvars::align_all_basic_blocks == 0);
  REQUIRE((cvars::full_optimization_even_with_debug ||
           (!cvars::debug && !cvars::store_all_context_values)));
  struct RestoreOptions {
    bool sync = cvars::enable_host_guest_stack_synchronization;
    bool mmio = cvars::emit_inline_mmio_checks;
    ~RestoreOptions() {
      cvars::enable_host_guest_stack_synchronization = sync;
      cvars::emit_inline_mmio_checks = mmio;
    }
  } restore_options;
  // Set synchronization before backend helper initialization. The following
  // ordinary STW is emitted with inline MMIO checks off in both fixtures.
  cvars::enable_host_guest_stack_synchronization = synchronize_stack;
  cvars::emit_inline_mmio_checks = false;
  Fixture fixture;
  REQUIRE(cvars::enable_host_guest_stack_synchronization == synchronize_stack);
  X64Function callee(nullptr,
                     kUnusedGuestSlot + 4);  // Unregistered, never called.
  REQUIRE(callee.machine_code() == nullptr);
  capture_stw::CurrentPPC builder(fixture.processor.frontend());
  Prefix(builder, callee);
  auto body = builder.current_block();
  Capture capture;
  REQUIRE_FALSE(
      ppc::BuildDormantCheckpointGprCapture(builder, kPC, kWord, &capture));
  REQUIRE(builder.locals().empty());
  REQUIRE(ppc::BuildDormantPostCallCheckpointGprCapture(builder, kPC, kWord,
                                                        &capture));
  // A separate source delimiter bounds capture bytes only. The following
  // ordinary STW is emitted, but its CALL-to-STW dataflow is not this oracle.
  builder.SourceOffset(kPC + 4);
  ppc::InstrData instruction{};
  instruction.code = kWord;
  instruction.address = kPC + 4;
  instruction.opcode = ppc::LookupOpcode(kWord);
  instruction.opcode_info = &ppc::GetOpcodeInfo(instruction.opcode);
  REQUIRE(instruction.opcode == ppc::PPCOpcode::stw);
  REQUIRE(instruction.opcode_info->emit);
  REQUIRE(instruction.opcode_info->emit(builder, instruction) == 0);
  builder.SourceOffset(kPC + 8);
  builder.StoreContext(real_hir::GPROffset(10),
                       builder.LoadConstantUint64(capture_stw::kSentinel));
  builder.SourceOffset(kPC + 12);
  builder.Return();
  REQUIRE(builder.Finalize());
  bool observed = false;
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&fixture.processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::ContextPromotionPass>());
  compiler.AddPass(std::make_unique<PromotionProof>(body, capture, observed));
  auto simplify = std::make_unique<passes::ConditionalGroupPass>();
  simplify->AddPass(std::make_unique<passes::SimplificationPass>());
  simplify->AddPass(std::make_unique<passes::ConstantPropagationPass>());
  compiler.AddPass(std::move(simplify));
  compiler.AddPass(std::make_unique<passes::MemorySequenceCombinationPass>());
  compiler.AddPass(std::make_unique<passes::SimplificationPass>());
  compiler.AddPass(std::make_unique<passes::DeadCodeEliminationPass>());
  REQUIRE(fixture.backend->machine_info()->register_sets[0].count == 7);
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      fixture.backend->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE(observed);
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
  auto locals =
      CheckHIR(builder, body, capture, *fixture.backend->machine_info());
  XbyakAllocator allocator;
  real_hir::HIREmitter emitter(fixture.backend, &allocator);
  const auto scratch = emitter.getCode();
  const auto unwind_size = fixture.cache->QueryUnwindStorageSize();
  const bool zero_low = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                            fixture.memory.virtual_membase())) == 0;
  REQUIRE(emitter.CanUseMembaseLow32As0() == zero_low);
  uint64_t pc = 0;
  size_t span = 0, callbacks = 0;
  REQUIRE(emitter.WithPreparedDraft(
      builder, [&](const X64Emitter::PreparedDraft& view) {
        ++callbacks;
        pc = fixture.cache->execute_base_address() + view.execute_offset();
        span = view.allocation_size();
        emitter.Snapshot(view, scratch);
        const size_t prefix_size =
            synchronize_stack ? CheckStackSync(view, pc, *fixture.backend) : 0;
        CheckBytes(locals, view, zero_low, prefix_size);
        fixture.CheckUnpublished(pc, span);
        REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
      }));
  REQUIRE(callbacks == 1);
  REQUIRE(callee.machine_code() == nullptr);
  REQUIRE(emitter.getCode() == scratch);
  emitter.CheckDiscarded();
  fixture.CheckAborted(pc, span);
  const uint8_t trap = 0xCC;
  size_t next_span = 0;
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      &trap, CodeInfo(1), [&](PreparedGuestCode& next) {
        next_span = next.allocation_size();
        REQUIRE(fixture.cache->execute_base_address() + next.execute_offset() ==
                pc + span);
        fixture.CheckUnpublished(pc + span, next.allocation_size());
      }));
  fixture.CheckAborted(pc, span);
  fixture.CheckAborted(pc + span, next_span);
  // No prepared code, call target, or guest store is invoked. No whole CALL
  // decoder, callee-runtime, actual DC3 CFG, combined post-call STW dataflow,
  // checkpoint pair, precision/ABI, mapping lease or publication proof.
}

TEST_CASE("Dormant post-call capture uses fresh context and current r10",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  RunCapture(false);
}

TEST_CASE("Dormant post-call capture bounds default-on stack synchronization",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  REQUIRE(cvars::enable_host_guest_stack_synchronization);
  RunCapture(true);
}

TEST_CASE("Dormant post-call capture rejects unsupported prefixes unchanged",
          "[cpu][code-cache-prepare][x64-prepared-finalization]") {
  for (size_t variant = 0; variant < 18; ++variant) {
    X64Function callee(nullptr, variant == 16   ? kUnusedGuestSlot + 1
                                : variant == 17 ? 0
                                                : kUnusedGuestSlot + 4);
    BuiltinFunction builtin(nullptr, kUnusedGuestSlot + 4);
    real_hir::CurrentHIR builder;
    auto before = Prefix(builder, callee);
    auto body = builder.current_block();
    auto predecessor = builder.first_block();
    auto call = predecessor->instr_tail;
    switch (variant) {
      case 0:
        call->flags = CALL_TAIL;
        break;
      case 1:
        call->flags = CALL_POSSIBLE_RETURN;
        break;
      case 2:
        call->src1.symbol = nullptr;
        break;
      case 3:
        call->opcode = &OPCODE_CALL_TRUE_info;
        break;
      case 4:
        call->opcode = &OPCODE_CALL_INDIRECT_info;
        break;
      case 5:
        call->prev->src1.offset = real_hir::GPROffset(3);
        break;
      case 6:
        call->prev->src2.value = builder.LoadConstantInt64(0);
        break;
      case 7:
        call->prev->prev->src1.value = builder.LoadConstantInt64(0);
        break;
      case 8:
        builder.MarkLabel(builder.NewLabel(), body);
        break;
      case 9:
        builder.MarkLabel(builder.NewLabel(), predecessor);
        break;
      case 10:
        builder.AddEdge(predecessor, body, Edge::UNCONDITIONAL);
        break;
      case 11:
        builder.StoreContext(real_hir::GPROffset(3),
                             before);  // Cross-call SSA.
        builder.SourceOffset(kPC + 4);
        break;
      case 12:
        builder.StoreLocal(builder.AllocLocal(INT64_TYPE),
                           builder.LoadConstantInt64(1));
        builder.SourceOffset(kPC + 4);
        break;
      case 13:
        builder.Call(&callee);  // Third block; no generic CFG admission.
        builder.SourceOffset(kPC + 4);
        break;
      case 14:
        body->instr_head->src1.offset = kCallPC;
        break;
      case 15:
        call->src1.symbol = &builtin;
        break;
    }
    const uint32_t pc = variant >= 11 && variant <= 13 ? kPC + 4 : kPC;
    Capture output;
    const auto prior_output = output;
    const auto prior_locals = builder.locals();
    const auto prior_ordinal = builder.max_value_ordinal();
    const auto prior_current = builder.current_block();
    std::vector<Instr*> instructions;
    for (auto block = builder.first_block(); block; block = block->next) {
      for (auto i = block->instr_head; i; i = i->next) {
        instructions.push_back(i);
      }
    }
    REQUIRE_FALSE(ppc::BuildDormantPostCallCheckpointGprCapture(
        builder, pc, kWord, &output));
    REQUIRE(output.gpr_slots == prior_output.gpr_slots);
    REQUIRE(output.ea_slot == prior_output.ea_slot);
    REQUIRE(builder.locals() == prior_locals);
    REQUIRE(builder.max_value_ordinal() == prior_ordinal);
    REQUIRE(builder.current_block() == prior_current);
    size_t position = 0;
    for (auto block = builder.first_block(); block; block = block->next) {
      for (auto i = block->instr_head; i; i = i->next) {
        REQUIRE(position < instructions.size());
        REQUIRE(i == instructions[position++]);
      }
    }
    REQUIRE(position == instructions.size());
  }
}

}  // namespace postcall_gpr

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
