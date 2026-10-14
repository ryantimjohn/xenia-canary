/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/mutant_state.h"

#include <chrono>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace test {

#if XE_PLATFORM_WIN32
TEST_CASE("Host abandonment resets recursion when opaque owner tokens repeat",
          "[kernel][mutant][host-abandonment]") {
  constexpr uintptr_t old_owner = 1;
  constexpr uintptr_t new_owner = old_owner;
  constexpr uint32_t old_guest_owner = 0x80000100;
  constexpr uint32_t new_guest_owner = old_guest_owner;
  X_KMUTANT native{};
  REQUIRE(InitializeNativeMutant(&native, false, 0));
  MutantState state;
  REQUIRE(state.InitializeNative(&native, old_owner, old_guest_owner));

  threading::WaitResult first = threading::WaitResult::kFailed;
  threading::WaitResult recursive = threading::WaitResult::kFailed;
  std::thread previous_owner([&] {
    first = threading::Wait(state.wait_handle(), false,
                            std::chrono::milliseconds(0));
    if (first == threading::WaitResult::kSuccess) {
      state.Acquired(old_owner, old_guest_owner, &native);
      recursive = threading::Wait(state.wait_handle(), false,
                                  std::chrono::milliseconds(0));
      if (recursive == threading::WaitResult::kSuccess) {
        state.Acquired(old_owner, old_guest_owner, &native);
      }
    }
    // Exiting while holding the real Windows mutex causes host abandonment.
    // This does not invoke the unsupported explicit Ke abandon operation.
  });
  previous_owner.join();
  REQUIRE(first == threading::WaitResult::kSuccess);
  REQUIRE(recursive == threading::WaitResult::kSuccess);
  CHECK(state.ownership().owner == old_owner);
  CHECK(state.ownership().depth == 2);
  CHECK(native.header.signal_state == uint32_t(-1));

  // WAIT_ABANDONED grants one host acquisition to this thread. The normal
  // acquisition callback must replace the terminated owner's stale depth
  // even when both opaque guest owner addresses have been recycled.
  REQUIRE(threading::Wait(state.wait_handle(), false,
                          std::chrono::milliseconds(0)) ==
          threading::WaitResult::kAbandoned);
  state.Acquired(new_owner, new_guest_owner, &native);
  CHECK(state.ownership().owner == new_owner);
  CHECK(state.ownership().depth == 1);
  CHECK(native.owner == new_guest_owner);
  CHECK(native.header.signal_state == 0);
  CHECK_FALSE(native.abandoned);

  int32_t previous_state = 17;
  REQUIRE(state.Release(new_owner, false, false, &previous_state, &native) ==
          X_STATUS_SUCCESS);
  CHECK(previous_state == 0);
  CHECK(state.ownership().depth == 0);
  CHECK(native.owner == 0);
  CHECK(native.header.signal_state == 1);
}

TEST_CASE(
    "Abandoned wait-all retains live recursion with recycled owner tokens",
    "[kernel][mutant][host-abandonment][wait-all]") {
  constexpr uintptr_t reused_owner = 1;
  constexpr uint32_t reused_guest_owner = 0x80000100;
  X_KMUTANT abandoned_native{};
  REQUIRE(InitializeNativeMutant(&abandoned_native, false, 0));
  MutantState abandoned;
  REQUIRE(abandoned.InitializeNative(&abandoned_native, reused_owner,
                                     reused_guest_owner));

  threading::WaitResult first = threading::WaitResult::kFailed;
  threading::WaitResult recursive = threading::WaitResult::kFailed;
  std::thread previous_owner([&] {
    first = threading::Wait(abandoned.wait_handle(), false,
                            std::chrono::milliseconds(0));
    if (first == threading::WaitResult::kSuccess) {
      abandoned.Acquired(reused_owner, reused_guest_owner, &abandoned_native);
      recursive = threading::Wait(abandoned.wait_handle(), false,
                                  std::chrono::milliseconds(0));
      if (recursive == threading::WaitResult::kSuccess) {
        abandoned.Acquired(reused_owner, reused_guest_owner, &abandoned_native);
      }
    }
  });
  previous_owner.join();
  REQUIRE(first == threading::WaitResult::kSuccess);
  REQUIRE(recursive == threading::WaitResult::kSuccess);
  REQUIRE(abandoned.ownership().depth == 2);

  // The calling host thread now represents the recycled opaque guest owner
  // and already holds a different live mutant recursively.
  X_KMUTANT live_native{};
  REQUIRE(InitializeNativeMutant(&live_native, true, reused_guest_owner));
  MutantState live;
  REQUIRE(
      live.InitializeNative(&live_native, reused_owner, reused_guest_owner));
  REQUIRE(threading::Wait(live.wait_handle(), false,
                          std::chrono::milliseconds(0)) ==
          threading::WaitResult::kSuccess);
  live.Acquired(reused_owner, reused_guest_owner, &live_native);
  REQUIRE(live.ownership().depth == 2);

  threading::WaitHandle* handles[] = {live.wait_handle(),
                                      abandoned.wait_handle()};
  REQUIRE(threading::WaitAll(handles, 2, false, std::chrono::milliseconds(0)) ==
          threading::WaitResult::kAbandoned);
  // Aggregate abandonment cannot reset every acquired mutant. Ordinary
  // accounting must distinguish the two actual host-thread lifetimes.
  live.Acquired(reused_owner, reused_guest_owner, &live_native);
  abandoned.Acquired(reused_owner, reused_guest_owner, &abandoned_native);
  CHECK(live.ownership().depth == 3);
  CHECK(live_native.header.signal_state == uint32_t(-2));
  CHECK(abandoned.ownership().depth == 1);
  CHECK(abandoned_native.header.signal_state == 0);

  int32_t previous_state = 17;
  REQUIRE(abandoned.Release(reused_owner, false, false, &previous_state,
                            &abandoned_native) == X_STATUS_SUCCESS);
  CHECK(previous_state == 0);
  CHECK(abandoned.ownership().depth == 0);
  for (int32_t expected : {-2, -1, 0}) {
    REQUIRE(live.Release(reused_owner, false, false, &previous_state,
                         &live_native) == X_STATUS_SUCCESS);
    CHECK(previous_state == expected);
  }
  CHECK(live.ownership().depth == 0);
  CHECK(live_native.header.signal_state == 1);
  CHECK(abandoned_native.header.signal_state == 1);
}
#endif  // XE_PLATFORM_WIN32

}  // namespace test
}  // namespace kernel
}  // namespace xe
