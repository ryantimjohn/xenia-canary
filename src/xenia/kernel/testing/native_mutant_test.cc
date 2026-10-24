/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/mutant_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace test {

using namespace std::chrono_literals;

namespace {

constexpr uintptr_t kOwner = 1;
constexpr uintptr_t kOtherOwner = 2;
constexpr uint32_t kGuestOwner = 0x80000100;
constexpr uint32_t kOtherGuestOwner = 0x80000200;

struct GuardedMutant {
  std::array<uint8_t, 16> before;
  X_KMUTANT native{};
  std::array<uint8_t, 16> after;

  GuardedMutant() { std::memset(this, 0xA5, sizeof(*this)); }

  bool CanariesIntact() const {
    const auto is_canary = [](uint8_t value) { return value == 0xA5; };
    return std::all_of(before.begin(), before.end(), is_canary) &&
           std::all_of(after.begin(), after.end(), is_canary);
  }
};

threading::WaitResult Acquire(MutantState& state, uintptr_t owner,
                              uint32_t guest_owner, X_KMUTANT* native = nullptr,
                              std::chrono::milliseconds timeout = 0ms) {
  const auto result = threading::Wait(state.wait_handle(), false, timeout);
  if (result == threading::WaitResult::kSuccess) {
    state.Acquired(owner, guest_owner, native);
  }
  return result;
}

}  // namespace

TEST_CASE("Native mutant initialization occupies exactly the guest object",
          "[kernel][mutant]") {
  GuardedMutant buffer;
  InitializeNativeMutant(&buffer.native, false, kGuestOwner);
  CHECK(buffer.CanariesIntact());
  CHECK(buffer.native.header.type == MutantObject);
  CHECK(buffer.native.header.signal_state == 1);
  CHECK(buffer.native.owner == 0);
  CHECK_FALSE(buffer.native.abandoned);
  const auto* bytes = reinterpret_cast<const uint8_t*>(&buffer.native);
  CHECK(bytes[4] == 0);
  CHECK(bytes[5] == 0);
  CHECK(bytes[6] == 0);
  CHECK(bytes[7] == 1);

  MutantState state;
  REQUIRE(state.InitializeNative(&buffer.native, kOwner, kGuestOwner));
  buffer.native.header.wait_list.flink_ptr = kXObjSignature;
  buffer.native.header.wait_list.blink_ptr = 0xF8000100;
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &buffer.native) ==
          threading::WaitResult::kSuccess);
  CHECK(buffer.native.header.signal_state == 0);
  CHECK(buffer.native.owner == kGuestOwner);
  int32_t previous = 17;
  REQUIRE(state.Release(kOwner, false, false, &previous, &buffer.native) ==
          X_STATUS_SUCCESS);
  CHECK(previous == 0);
  CHECK(buffer.native.header.signal_state == 1);
  CHECK(buffer.native.owner == 0);
  CHECK(buffer.native.header.wait_list.flink_ptr == kXObjSignature);
  CHECK(buffer.native.header.wait_list.blink_ptr == 0xF8000100);
  CHECK(buffer.CanariesIntact());
}

TEST_CASE("Initially owned native mutants retain recursive ownership",
          "[kernel][mutant]") {
  X_KMUTANT native{};
  InitializeNativeMutant(&native, true, kGuestOwner);
  MutantState state;
  REQUIRE(state.InitializeNative(&native, kOwner, kGuestOwner));
  CHECK(state.ownership().owner == kOwner);
  CHECK(state.ownership().depth == 1);
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &native) ==
          threading::WaitResult::kSuccess);
  CHECK(native.header.signal_state == uint32_t(-1));

  int32_t previous = 17;
  REQUIRE(state.Release(kOwner, false, false, &previous, &native) ==
          X_STATUS_SUCCESS);
  CHECK(previous == -1);
  CHECK(native.owner == kGuestOwner);
  CHECK(native.header.signal_state == 0);
  CHECK(state.ownership().depth == 1);
  REQUIRE(state.Release(kOwner, false, false, &previous, &native) ==
          X_STATUS_SUCCESS);
  CHECK(previous == 0);
  CHECK(native.owner == 0);
  CHECK(native.header.signal_state == 1);
  CHECK(state.ownership().depth == 0);
}

TEST_CASE("Reinitialization preserves an existing native wrapper",
          "[kernel][mutant]") {
  GuardedMutant buffer;
  REQUIRE(InitializeNativeMutant(&buffer.native, true, kGuestOwner));
  buffer.native.header.wait_list.flink_ptr = kXObjSignature;
  buffer.native.header.wait_list.blink_ptr = 0xF8000100;
  const auto original = buffer.native;
  CHECK_FALSE(InitializeNativeMutant(&buffer.native, false, 0));
  CHECK(std::memcmp(&buffer.native, &original, sizeof(original)) == 0);
  CHECK(buffer.CanariesIntact());
}

TEST_CASE("Late native wrapping cannot steal an existing owner",
          "[kernel][mutant]") {
  X_KMUTANT native{};
  InitializeNativeMutant(&native, true, kGuestOwner);
  const auto original = native;
  MutantState foreign;
  CHECK_FALSE(foreign.InitializeNative(&native, kOtherOwner, kOtherGuestOwner));
  CHECK(foreign.wait_handle() == nullptr);
  CHECK(std::memcmp(&native, &original, sizeof(native)) == 0);

  MutantState unknown;
  CHECK_FALSE(unknown.InitializeNative(&native, kOwner, 0));
  CHECK(std::memcmp(&native, &original, sizeof(native)) == 0);

  native.header.signal_state = uint32_t(-1);
  MutantState recursive;
  CHECK_FALSE(recursive.InitializeNative(&native, kOwner, kGuestOwner));
}

TEST_CASE("Failed releases preserve native ownership and output",
          "[kernel][mutant]") {
  X_KMUTANT native{};
  InitializeNativeMutant(&native, true, kGuestOwner);
  MutantState state;
  REQUIRE(state.InitializeNative(&native, kOwner, kGuestOwner));
  int32_t previous = 17;
  CHECK(state.Release(kOtherOwner, false, false, &previous, &native) ==
        X_STATUS_MUTANT_NOT_OWNED);
  CHECK(state.Release(kOwner, true, false, &previous, &native) ==
        X_STATUS_NOT_IMPLEMENTED);
  CHECK(state.Release(kOwner, false, true, &previous, &native) ==
        X_STATUS_NOT_IMPLEMENTED);
  CHECK(previous == 17);
  CHECK(state.ownership().owner == kOwner);
  CHECK(state.ownership().depth == 1);
  CHECK(native.owner == kGuestOwner);
  CHECK(native.header.signal_state == 0);

  // Even if the caller supplies the owner's token, its distinct host-thread
  // lifetime cookie rejects release without changing ownership state.
  X_STATUS host_rejection = X_STATUS_SUCCESS;
  std::thread foreign([&] {
    host_rejection = state.Release(kOwner, false, false, &previous, &native);
  });
  foreign.join();
  CHECK(host_rejection == X_STATUS_MUTANT_NOT_OWNED);
  CHECK(previous == 17);
  CHECK(native.owner == kGuestOwner);
  CHECK(state.ownership().depth == 1);
  REQUIRE(state.Release(kOwner, false, false, &previous, &native) ==
          X_STATUS_SUCCESS);
}

TEST_CASE("A failed host release leaves ownership and output unchanged",
          "[kernel][mutant]") {
  X_KMUTANT native{};
  REQUIRE(InitializeNativeMutant(&native, true, kGuestOwner));
  MutantState state;
  REQUIRE(state.InitializeNative(&native, kOwner, kGuestOwner));
  // Deliberately bypass bookkeeping to exercise an actual host release
  // failure with the correct owner and host lifetime cookie still recorded.
  REQUIRE(static_cast<threading::Mutant*>(state.wait_handle())->Release());
  const auto before = native;
  int32_t previous = 17;
  CHECK(state.Release(kOwner, false, false, &previous, &native) ==
        X_STATUS_MUTANT_NOT_OWNED);
  CHECK(state.ownership().owner == kOwner);
  CHECK(state.ownership().depth == 1);
  CHECK(previous == 17);
  CHECK(std::memcmp(&native, &before, sizeof(native)) == 0);
}

TEST_CASE("A contending thread acquires only after the final release",
          "[kernel][mutant]") {
  X_KMUTANT native{};
  InitializeNativeMutant(&native, false, 0);
  MutantState state;
  REQUIRE(state.InitializeNative(&native, kOwner, kGuestOwner));
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &native) ==
          threading::WaitResult::kSuccess);
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &native) ==
          threading::WaitResult::kSuccess);

  auto first_checked = threading::Event::CreateManualResetEvent(false);
  auto try_again = threading::Event::CreateManualResetEvent(false);
  auto second_checked = threading::Event::CreateManualResetEvent(false);
  auto acquired = threading::Event::CreateManualResetEvent(false);
  auto finish = threading::Event::CreateManualResetEvent(false);
  threading::WaitResult first = threading::WaitResult::kFailed;
  threading::WaitResult second = threading::WaitResult::kFailed;
  threading::WaitResult final = threading::WaitResult::kFailed;
  X_STATUS release = X_STATUS_UNSUCCESSFUL;
  std::thread other([&] {
    first = Acquire(state, kOtherOwner, kOtherGuestOwner, &native);
    first_checked->Set();
    threading::Wait(try_again.get(), false, 2s);
    second = Acquire(state, kOtherOwner, kOtherGuestOwner, &native);
    second_checked->Set();
    final = Acquire(state, kOtherOwner, kOtherGuestOwner, &native, 2s);
    acquired->Set();
    threading::Wait(finish.get(), false, 2s);
    if (final == threading::WaitResult::kSuccess) {
      release = state.Release(kOtherOwner, false, false, nullptr, &native);
    }
  });

  CHECK(threading::Wait(first_checked.get(), false, 2s) ==
        threading::WaitResult::kSuccess);
  int32_t previous = 17;
  CHECK(state.Release(kOwner, false, false, &previous, &native) ==
        X_STATUS_SUCCESS);
  CHECK(previous == -1);
  try_again->Set();
  CHECK(threading::Wait(second_checked.get(), false, 2s) ==
        threading::WaitResult::kSuccess);
  CHECK(state.Release(kOwner, false, false, &previous, &native) ==
        X_STATUS_SUCCESS);
  CHECK(previous == 0);
  CHECK(threading::Wait(acquired.get(), false, 2s) ==
        threading::WaitResult::kSuccess);
  CHECK(state.ownership().owner == kOtherOwner);
  CHECK(state.ownership().depth == 1);
  CHECK(native.owner == kOtherGuestOwner);
  CHECK(native.header.signal_state == 0);
  finish->Set();
  other.join();
  CHECK(first == threading::WaitResult::kTimeout);
  CHECK(second == threading::WaitResult::kTimeout);
  CHECK(final == threading::WaitResult::kSuccess);
  CHECK(release == X_STATUS_SUCCESS);
  CHECK(native.owner == 0);
  CHECK(native.header.signal_state == 1);
}

TEST_CASE("Accounted release followed by wait preserves recursive depth",
          "[kernel][mutant]") {
  X_KMUTANT native{};
  REQUIRE(InitializeNativeMutant(&native, true, kGuestOwner));
  MutantState state;
  REQUIRE(state.InitializeNative(&native, kOwner, kGuestOwner));
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &native) ==
          threading::WaitResult::kSuccess);
  int32_t previous = 17;
  REQUIRE(state.Release(kOwner, false, false, &previous, &native) ==
          X_STATUS_SUCCESS);
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &native) ==
          threading::WaitResult::kSuccess);
  CHECK(state.ownership().depth == 2);
  CHECK(native.header.signal_state == uint32_t(-1));
  REQUIRE(state.Release(kOwner, false, false, &previous, &native) ==
          X_STATUS_SUCCESS);
  CHECK(previous == -1);
  REQUIRE(state.Release(kOwner, false, false, &previous, &native) ==
          X_STATUS_SUCCESS);
  CHECK(previous == 0);
  CHECK(native.header.signal_state == 1);
  CHECK(native.owner == 0);
}

TEST_CASE("Native updates use the current supplied memory view",
          "[kernel][mutant]") {
  X_KMUTANT original{};
  InitializeNativeMutant(&original, false, 0);
  MutantState state;
  REQUIRE(state.InitializeNative(&original, kOwner, kGuestOwner));
  X_KMUTANT current = original;
  REQUIRE(Acquire(state, kOwner, kGuestOwner, &current) ==
          threading::WaitResult::kSuccess);
  CHECK(original.header.signal_state == 1);
  CHECK(original.owner == 0);
  CHECK(current.header.signal_state == 0);
  CHECK(current.owner == kGuestOwner);
  REQUIRE(state.Release(kOwner, false, false, nullptr, &current) ==
          X_STATUS_SUCCESS);
  CHECK(current.header.signal_state == 1);
  CHECK(current.owner == 0);
}

TEST_CASE("Legacy restore reacquires one ownership without guest allocation",
          "[kernel][mutant]") {
  MutantState state;
  REQUIRE(state.Initialize(false, 0));
  state.SetRestoredOwner(kOwner);
  CHECK(state.ownership().owner == kOwner);
  CHECK(state.ownership().depth == 0);
  REQUIRE(Acquire(state, kOwner, kGuestOwner) ==
          threading::WaitResult::kSuccess);
  CHECK(state.ownership().depth == 1);
  int32_t previous = 17;
  REQUIRE(state.Release(kOwner, false, false, &previous) == X_STATUS_SUCCESS);
  CHECK(previous == 0);
  CHECK(state.ownership().owner == 0);
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
