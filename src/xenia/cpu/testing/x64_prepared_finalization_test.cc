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
#include <vector>

#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

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

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
