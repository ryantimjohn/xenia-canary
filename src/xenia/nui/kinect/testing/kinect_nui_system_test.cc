/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/kinect/kinect_nui_system.h"

#include <cstdint>

#include "third_party/catch/include/catch.hpp"
#include "xenia/nui/testing/scoped_tilt_override.h"

namespace xe {
namespace nui {
namespace kinect {
namespace test {

using xe::nui::test::ScopedTiltOverride;

// A Kinect backend that is never set up, so it loads no runtime and starts no
// pump: nothing here needs Kinect10.dll or a sensor. Starting the tilt thread
// takes both, so what can be seen here is only whether a call asks for it.
class UnsetKinectNuiSystem : public KinectNuiSystem {
 public:
  using KinectNuiSystem::tilt_wanted;
};

TEST_CASE("The Kinect backend wants no tilt thread before a tilt call",
          "[nui][kinect][tilt]") {
  UnsetKinectNuiSystem system;
  CHECK_FALSE(system.tilt_wanted());
  // The depth side is no tilt call: a title that only takes depth frames never
  // has the motor or the accelerometer touched.
  CHECK_FALSE(system.IsDevicePresent());
  CHECK(system.latest_sequence() == 0);
  CHECK(system.AcquireLatestDepthFrame() == nullptr);
  CHECK_FALSE(system.tilt_wanted());
}

TEST_CASE("Every tilt call asks the Kinect backend for its tilt thread",
          "[nui][kinect][tilt]") {
  // With no sensor ready each call answers what it always has, nothing, and
  // asks all the same: the pump starts the thread once the sensor is ready.
  UnsetKinectNuiSystem system;
  SECTION("GetTiltDegrees") {
    int32_t degrees = 0;
    CHECK_FALSE(system.GetTiltDegrees(&degrees));
  }
  SECTION("SetTiltDegrees") { CHECK_FALSE(system.SetTiltDegrees(-25)); }
  SECTION("GetGravityVector") {
    float xyz[3] = {};
    CHECK_FALSE(system.GetGravityVector(xyz));
  }
  SECTION("IsTiltMotorMoving") { CHECK_FALSE(system.IsTiltMotorMoving()); }
  CHECK(system.tilt_wanted());
}

TEST_CASE("Peeking at the tilt angle does not ask for the tilt thread",
          "[nui][kinect][tilt]") {
  // The depth preview overlay shows the angle on every draw. That is no
  // title's tilt call, so it must not get the thread started for a title that
  // never asked for it.
  UnsetKinectNuiSystem system;
  int32_t degrees = 0;
  CHECK_FALSE(system.PeekTiltDegrees(&degrees));
  CHECK_FALSE(system.PeekTiltDegrees(nullptr));
  CHECK_FALSE(system.tilt_wanted());
}

TEST_CASE("Peeking at the tilt angle gives the angle a title's read does",
          "[nui][kinect][tilt]") {
  // With no sensor, nui_tilt_override is the one angle there is to give.
  ScopedTiltOverride override_degrees(-9);
  UnsetKinectNuiSystem system;
  int32_t peeked = 0;
  REQUIRE(system.PeekTiltDegrees(&peeked));
  CHECK(peeked == -9);
  CHECK_FALSE(system.tilt_wanted());

  int32_t read = 0;
  REQUIRE(system.GetTiltDegrees(&read));
  CHECK(read == peeked);
  CHECK(system.tilt_wanted());
}

}  // namespace test
}  // namespace kinect
}  // namespace nui
}  // namespace xe
