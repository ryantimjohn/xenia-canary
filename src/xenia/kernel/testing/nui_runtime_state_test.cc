/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_runtime_state.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

TEST_CASE("Unsupported NUI initialization cannot publish partial state",
          "[nui][runtime]") {
  NuiRuntimeState state;
  CHECK_FALSE(state.Initialize(4, 11775, 0x40000200));
  CHECK_FALSE(state.Initialize(5, 11775, 0x80000200));
  auto rejected = state.snapshot();
  CHECK_FALSE(rejected.initialized);
  CHECK(rejected.xam_version == 0);
  CHECK(rejected.xtl_version == 0);
  CHECK(rejected.flags == 0);

  REQUIRE(state.Initialize(5, 11775, 0x40000200));
  state.SetEngagedTrackingId(19);
  state.SetTiltCallback(0x82000100);
  state.SetIdentityCallback(0x82000200);
  CHECK_FALSE(state.Initialize(6, 12345, 0x200));
  CHECK_FALSE(state.Initialize(5, 12345, 0x80000000));
  rejected = state.snapshot();
  CHECK(rejected.initialized);
  CHECK(rejected.xam_version == 5);
  CHECK(rejected.xtl_version == 11775);
  CHECK(rejected.flags == 0x40000200);
  CHECK(rejected.engaged_tracking_id == 19);
  CHECK(rejected.tilt_callback == 0x82000100);
  CHECK(rejected.identity_callback == 0x82000200);
}

TEST_CASE("Runtime shutdown clears registration and permits reinitialization",
          "[nui][runtime]") {
  NuiRuntimeState state;
  REQUIRE(state.Initialize(5, 11775, 0x40000200));
  state.SetEngagedTrackingId(19);
  state.SetTiltCallback(0x82000100);
  state.SetIdentityCallback(0x82000200);
  state.SetSessionId(0x1020304050607080);

  state.Reset();
  const auto reset = state.snapshot();
  CHECK_FALSE(reset.initialized);
  CHECK(reset.xam_version == 0);
  CHECK(reset.xtl_version == 0);
  CHECK(reset.flags == 0);
  CHECK(reset.engaged_tracking_id == 0);
  CHECK(reset.tilt_callback == 0);
  CHECK(reset.identity_callback == 0);
  // The identity session belongs to this kernel, not the old registration.
  CHECK(state.GetOrCreateSessionId() == 0x1020304050607080);

  REQUIRE(state.Initialize(5, 12345, 0x200));
  const auto restarted = state.snapshot();
  CHECK(restarted.initialized);
  CHECK(restarted.xam_version == 5);
  CHECK(restarted.xtl_version == 12345);
  CHECK(restarted.flags == 0x200);
  CHECK(restarted.engaged_tracking_id == 0);
  CHECK(restarted.tilt_callback == 0);
  CHECK(restarted.identity_callback == 0);

  state.Reset();
  state.Reset();
  CHECK_FALSE(state.snapshot().initialized);
  REQUIRE(state.Initialize(5, 12345, 0));
}

TEST_CASE("Compatibility setters share runtime metadata and bound versions",
          "[nui][runtime]") {
  NuiRuntimeState state;
  state.SetVersion(5, 0);
  state.SetVersion(11775, 1);
  state.SetFlags(0x200);
  state.SetVersion(0xFFFFFFFFFFFFFFFF, 2);
  const auto metadata = state.snapshot();
  CHECK(metadata.xam_version == 5);
  CHECK(metadata.xtl_version == 11775);
  CHECK(metadata.flags == 0x200);
  // Individual metadata setters do not impersonate a successful initialize.
  CHECK_FALSE(metadata.initialized);
}

TEST_CASE("A HUD engagement update cannot publish into a reset runtime",
          "[nui][runtime]") {
  NuiRuntimeState state;
  CHECK_FALSE(state.EngageTrackingId(19));
  REQUIRE(state.Initialize(5, 11775, 0x200));
  REQUIRE(state.EngageTrackingId(19));
  REQUIRE(state.EngageTrackingId(0));
  CHECK(state.snapshot().engaged_tracking_id == 19);

  state.Reset();
  CHECK_FALSE(state.EngageTrackingId(23));
  CHECK_FALSE(state.EngageTrackingId(0));
  CHECK(state.snapshot().engaged_tracking_id == 0);

  REQUIRE(state.Initialize(5, 11775, 0x200));
  REQUIRE(state.EngageTrackingId(23));
  CHECK(state.snapshot().engaged_tracking_id == 23);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
