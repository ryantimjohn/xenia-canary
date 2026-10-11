/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_flags.h"

#include <cstdint>
#include <string_view>

#include "xenia/nui/nui_guide_defaults.h"
#include "xenia/nui/testing/scoped_tilt_override.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

TEST_CASE("The tilt range is the motor's travel", "[nui][nui_flags]") {
  CHECK(kTiltMinimumDegrees == -27);
  CHECK(kTiltMaximumDegrees == 27);
}

TEST_CASE("The tilt override is off at its default", "[nui][nui_flags]") {
  ScopedTiltOverride scoped(1000);
  int32_t degrees = 99;
  CHECK_FALSE(GetTiltOverrideDegrees(&degrees));
  CHECK(degrees == 99);
}

TEST_CASE("The tilt override holds any angle the motor can reach",
          "[nui][nui_flags]") {
  for (int32_t override_degrees : {-27, -12, 0, 8, 27}) {
    ScopedTiltOverride scoped(override_degrees);
    int32_t degrees = 99;
    REQUIRE(GetTiltOverrideDegrees(&degrees));
    REQUIRE(degrees == override_degrees);
  }
}

TEST_CASE("A tilt override the motor cannot reach counts as off",
          "[nui][nui_flags]") {
  for (int32_t override_degrees : {-1000, -28, 28, 999}) {
    ScopedTiltOverride scoped(override_degrees);
    int32_t degrees = 99;
    REQUIRE_FALSE(GetTiltOverrideDegrees(&degrees));
    REQUIRE(degrees == 99);
  }
}

TEST_CASE("The accelerometer is read unless the user turns it off",
          "[nui][nui_flags]") {
  CHECK(cvars::nui_accelerometer);
}

TEST_CASE("The guide gesture's cvars default to the shared defaults",
          "[nui][nui_flags]") {
  CHECK(kGuideAngleMinDefault == 30);
  CHECK(kGuideAngleMaxDefault == 60);
  CHECK(kGuideHoldMsDefault == 1000);
  CHECK(kGuideGapMsDefault == 200);
  CHECK(std::string_view(kGuideArmDefault) == "left");
  CHECK(kGuideRestAngleMaxDefault == 20);
  CHECK(cvars::nui_guide_angle_min == kGuideAngleMinDefault);
  CHECK(cvars::nui_guide_angle_max == kGuideAngleMaxDefault);
  CHECK(cvars::nui_guide_hold_ms == kGuideHoldMsDefault);
  CHECK(cvars::nui_guide_gap_ms == kGuideGapMsDefault);
  CHECK(cvars::nui_guide_arm == kGuideArmDefault);
  CHECK(cvars::nui_guide_rest_angle_max == kGuideRestAngleMaxDefault);
}

}  // namespace test
}  // namespace nui
}  // namespace xe
