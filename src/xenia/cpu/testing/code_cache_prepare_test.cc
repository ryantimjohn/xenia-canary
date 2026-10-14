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
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/mutex.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

namespace xe::cpu::testing {
namespace {

using backend::EmitFunctionInfo;
using backend::x64::X64CodeCache;
using backend::x64::X64Function;
using PreparedGuestCode = X64CodeCache::PreparedGuestCode;
static_assert(!std::is_copy_constructible_v<PreparedGuestCode>);
static_assert(!std::is_move_constructible_v<PreparedGuestCode>);
constexpr uint32_t kOldAddress = 0x80001000;
constexpr uint32_t kNextAddress = kOldAddress + 4;
constexpr std::array<uint8_t, 8> kOrdinaryBytes = {0x90, 0x90, 0x90, 0x90,
                                                   0x90, 0x90, 0x90, 0xC3};
constexpr std::array<uint8_t, 23> kDraftBytes = {1,  2,  3,  4,  5,  6,  7,  8,
                                                 9,  10, 11, 12, 13, 14, 15, 16,
                                                 17, 18, 19, 20, 21, 22, 23};

// Own real function metadata without creating a frontend/backend or translating
// guest code. None of the ordinary fixture bytes or prepared bytes are invoked.
class FixtureModule final : public Module {
 public:
  explicit FixtureModule(Processor* processor) : Module(processor) {}
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t) override {
    return nullptr;
  }

 private:
  std::string name_ = "CodeCachePrepareTest";
};

struct Fixture {
  Memory memory;
  Processor processor{&memory, nullptr};
  FixtureModule module{&processor};
  std::unique_ptr<X64CodeCache> cache = X64CodeCache::Create();

  Fixture() {
    REQUIRE(cache);
    REQUIRE(cache->Initialize());
    REQUIRE(cache->has_indirection_table());
    cache->CommitExecutableRange(kOldAddress, kOldAddress + 0x1000);
  }

  const uint32_t* slots() const {
    return reinterpret_cast<const uint32_t*>(uintptr_t{kOldAddress});
  }
};

EmitFunctionInfo CodeInfo(size_t size) {
  EmitFunctionInfo info{};
  info.code_size.body = size;
  info.code_size.total = size;
  return info;
}

uint64_t PlaceOrdinary(Fixture& fixture, X64Function& function) {
  void* execute = nullptr;
  void* write = nullptr;
  const auto info = CodeInfo(kOrdinaryBytes.size());
  // Exercise the unchanged ordinary placement and native unwind-registration
  // path. This leaf-shaped byte fixture supplies no executable test entry.
  fixture.cache->PlaceGuestCode(function.address(),
                                const_cast<uint8_t*>(kOrdinaryBytes.data()),
                                info, &function, execute, write);
  REQUIRE(execute);
  REQUIRE(write);
  function.Setup(static_cast<uint8_t*>(execute), kOrdinaryBytes.size());
  const auto pc = reinterpret_cast<uint64_t>(execute);
  REQUIRE(fixture.cache->LookupFunction(pc) == &function);
  REQUIRE(fixture.cache->LookupUnwindInfo(pc));
  DWORD64 image_base = 0;
  REQUIRE(RtlLookupFunctionEntry(pc, &image_base, nullptr));
  REQUIRE(fixture.slots()[(function.address() - kOldAddress) / 4] == pc);
  return pc;
}

void CheckUnpublished(Fixture& fixture, uint64_t pc, size_t span_size) {
  REQUIRE(span_size > 0);
  for (uint64_t probe : {pc, pc + span_size - 1}) {
    CHECK(fixture.cache->LookupFunction(probe) == nullptr);
    CHECK(fixture.cache->LookupUnwindInfo(probe) == nullptr);
    DWORD64 image_base = 0;
    CHECK(RtlLookupFunctionEntry(probe, &image_base, nullptr) == nullptr);
  }
}

void CheckAbortedBytes(uint64_t pc, size_t span_size) {
  // A read-only inspection while the owning native cache remains alive. No
  // callback write pointer or reference to its destroyed scoped view is used.
  auto bytes = reinterpret_cast<const uint8_t*>(pc);
  CHECK(std::all_of(bytes, bytes + span_size,
                    [](uint8_t byte) { return byte == 0xCC; }));
}

void CheckOldPlacement(Fixture& fixture, X64Function& function, uint64_t pc,
                       uint32_t slot, void* unwind) {
  REQUIRE(function.machine_code() == reinterpret_cast<uint8_t*>(pc));
  REQUIRE(function.machine_code_length() == kOrdinaryBytes.size());
  CHECK(std::memcmp(function.machine_code(), kOrdinaryBytes.data(),
                    kOrdinaryBytes.size()) == 0);
  CHECK(fixture.cache->LookupFunction(pc) == &function);
  CHECK(fixture.cache->LookupUnwindInfo(pc) == unwind);
  CHECK(fixture.slots()[0] == slot);
}

TEST_CASE("Unpublished cache preparation automatically aborts outside region",
          "[cpu][code-cache-prepare]") {
  Fixture fixture;
  X64Function old_function(&fixture.module, kOldAddress);
  const auto old_pc = PlaceOrdinary(fixture, old_function);
  const auto old_slot = fixture.slots()[0];
  const auto next_slot = fixture.slots()[1];
  auto* old_unwind = fixture.cache->LookupUnwindInfo(old_pc);
  uint64_t prepared_pc = 0;
  size_t span_size = 0;
  size_t callbacks = 0;
  const size_t unwind_size = fixture.cache->QueryUnwindStorageSize();
  REQUIRE(unwind_size > 0);
  REQUIRE(fixture.cache->QueryUnwindStorageSize() == unwind_size);
  REQUIRE(fixture.cache->WithPreparedGuestCode(
      kDraftBytes.data(), CodeInfo(kDraftBytes.size()),
      [&](PreparedGuestCode& prepared) {
        ++callbacks;
        REQUIRE(prepared.state() == PreparedGuestCode::State::kPrepared);
        REQUIRE(prepared.write_data());
        REQUIRE(prepared.code_size() == kDraftBytes.size());
        CHECK(std::memcmp(prepared.write_data(), kDraftBytes.data(),
                          kDraftBytes.size()) == 0);
        REQUIRE(prepared.execute_offset() % 16 == 0);
        span_size = prepared.allocation_size();
        REQUIRE(span_size % 16 == 0);
        REQUIRE(span_size ==
                xe::round_up(prepared.code_size(), size_t{16}, false) +
                    xe::round_up(unwind_size, size_t{16}, false));
        prepared_pc =
            fixture.cache->execute_base_address() + prepared.execute_offset();
        CheckUnpublished(fixture, prepared_pc, span_size);
        CheckOldPlacement(fixture, old_function, old_pc, old_slot, old_unwind);
        CHECK(fixture.slots()[1] == next_slot);
        // A different thread tries the shared region without blocking. This
        // would fail if preparation retained the region across this callback.
        bool region_available = false;
        std::thread observer([&] {
          xe::global_unique_lock_type lock(xe::global_critical_region::mutex(),
                                           std::try_to_lock);
          region_available = lock.owns_lock();
        });
        observer.join();
        REQUIRE(region_available);
        std::memset(prepared.write_data(), 0xA5, prepared.code_size());
      }));
  CHECK(callbacks == 1);
  CheckUnpublished(fixture, prepared_pc, span_size);
  CheckAbortedBytes(prepared_pc, span_size);
  CheckOldPlacement(fixture, old_function, old_pc, old_slot, old_unwind);
  CHECK(fixture.slots()[1] == next_slot);
  X64Function next_function(&fixture.module, kNextAddress);
  const auto next_pc = PlaceOrdinary(fixture, next_function);
  // Abort consumes the entire span: ordinary placement must not reuse it.
  CHECK(next_pc == prepared_pc + span_size);
  CheckUnpublished(fixture, prepared_pc, span_size);
  CheckAbortedBytes(prepared_pc, span_size);
  CheckOldPlacement(fixture, old_function, old_pc, old_slot, old_unwind);
}

TEST_CASE("Unpublished cache preparation rejects overflow and capacity",
          "[cpu][code-cache-prepare]") {
  Fixture fixture;
  X64Function old_function(&fixture.module, kOldAddress);
  const auto old_pc = PlaceOrdinary(fixture, old_function);
  const auto old_slot = fixture.slots()[0];
  const auto next_slot = fixture.slots()[1];
  auto* old_unwind = fixture.cache->LookupUnwindInfo(old_pc);
  size_t callbacks = 0;
  auto writer = [&](PreparedGuestCode&) { ++callbacks; };
  CHECK_FALSE(fixture.cache->WithPreparedGuestCode(
      nullptr, CodeInfo(kDraftBytes.size()), writer));
  for (size_t size : {size_t{0}, std::numeric_limits<size_t>::max(),
                      fixture.cache->total_size()}) {
    // The tiny source deliberately cannot supply these oversized requests;
    // they must be rejected before any source copy or callback invocation.
    CHECK_FALSE(fixture.cache->WithPreparedGuestCode(kDraftBytes.data(),
                                                     CodeInfo(size), writer));
  }
  CHECK(callbacks == 0);
  CHECK(fixture.slots()[1] == next_slot);
  CheckOldPlacement(fixture, old_function, old_pc, old_slot, old_unwind);
  X64Function next_function(&fixture.module, kNextAddress);
  const auto next_pc = PlaceOrdinary(fixture, next_function);
  const size_t old_span =
      xe::round_up(kOrdinaryBytes.size(), size_t{16}) +
      xe::round_up(fixture.cache->QueryUnwindStorageSize(), size_t{16});
  CHECK(next_pc == old_pc + old_span);
  CheckOldPlacement(fixture, old_function, old_pc, old_slot, old_unwind);
}

struct CallbackFailure {};

TEST_CASE("Unpublished cache preparation aborts a throwing callback",
          "[cpu][code-cache-prepare]") {
  Fixture fixture;
  X64Function old_function(&fixture.module, kOldAddress);
  const auto old_pc = PlaceOrdinary(fixture, old_function);
  const auto old_slot = fixture.slots()[0];
  const auto next_slot = fixture.slots()[1];
  auto* old_unwind = fixture.cache->LookupUnwindInfo(old_pc);
  uint64_t prepared_pc = 0;
  size_t span_size = 0;
  size_t callbacks = 0;
  REQUIRE_THROWS_AS(fixture.cache->WithPreparedGuestCode(
                        kDraftBytes.data(), CodeInfo(kDraftBytes.size()),
                        [&](PreparedGuestCode& prepared) {
                          ++callbacks;
                          prepared_pc = fixture.cache->execute_base_address() +
                                        prepared.execute_offset();
                          span_size = prepared.allocation_size();
                          CheckUnpublished(fixture, prepared_pc, span_size);
                          std::memset(prepared.write_data(), 0xA5,
                                      prepared.code_size());
                          throw CallbackFailure{};
                        }),
                    CallbackFailure);
  REQUIRE(callbacks == 1);
  CheckUnpublished(fixture, prepared_pc, span_size);
  CheckAbortedBytes(prepared_pc, span_size);
  CheckOldPlacement(fixture, old_function, old_pc, old_slot, old_unwind);
  CHECK(fixture.slots()[1] == next_slot);
  X64Function next_function(&fixture.module, kNextAddress);
  CHECK(PlaceOrdinary(fixture, next_function) == prepared_pc + span_size);
  CheckUnpublished(fixture, prepared_pc, span_size);
  CheckAbortedBytes(prepared_pc, span_size);
}

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
