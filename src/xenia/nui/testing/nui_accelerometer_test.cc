/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_accelerometer.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

namespace {

constexpr double kPi = 3.14159265358979323846;

// What a sensor whose head is pitched up by the given angle reads, on the
// axes the Kinect for Windows runtime documents: the vector points at the
// floor, in g, with y up and z the way the sensor looks, so a level sensor
// reads (0, -1, 0) and a head pitched up sees gravity pull back along -z.
void ReadingForElevation(double degrees, float* xyz) {
  const double radians = degrees * kPi / 180.0;
  xyz[0] = 0.0f;
  xyz[1] = static_cast<float>(-std::cos(radians));
  xyz[2] = static_cast<float>(-std::sin(radians));
}

}  // namespace

TEST_CASE("A level sensor's accelerometer reads an elevation of zero",
          "[nui][accelerometer]") {
  const float level[3] = {0.0f, -1.0f, 0.0f};
  CHECK(AccelerometerElevationDegrees(level) == Approx(0.0).margin(1e-9));
}

TEST_CASE("The accelerometer's elevation has the elevation API's sign",
          "[nui][accelerometer]") {
  // NuiCameraElevationSetAngle takes positive angles as "look up", and the
  // title compares what it reads back against what it asked for, so a head
  // pitched up has to come back positive and one pitched down negative.
  float xyz[3];
  ReadingForElevation(10.0, xyz);
  CHECK(AccelerometerElevationDegrees(xyz) == Approx(10.0).margin(1e-3));
  ReadingForElevation(-10.0, xyz);
  CHECK(AccelerometerElevationDegrees(xyz) == Approx(-10.0).margin(1e-3));
}

TEST_CASE("Every angle the motor can reach comes back out of the reading",
          "[nui][accelerometer]") {
  for (int32_t degrees = -27; degrees <= 27; ++degrees) {
    float xyz[3];
    ReadingForElevation(degrees, xyz);
    REQUIRE(AccelerometerElevationDegrees(xyz) ==
            Approx(static_cast<double>(degrees)).margin(1e-3));
  }
}

TEST_CASE("The elevation depends only on the direction of the reading",
          "[nui][accelerometer]") {
  // An accelerometer is never exactly one g, and the answer must not move
  // with the length of the vector, only with where it points.
  float xyz[3];
  ReadingForElevation(-8.5, xyz);
  float scaled[3] = {xyz[0] * 0.97f, xyz[1] * 0.97f, xyz[2] * 0.97f};
  CHECK(AccelerometerElevationDegrees(scaled) == Approx(-8.5).margin(1e-3));
}

TEST_CASE("A reading that is one gravity toward the floor is plausible",
          "[nui][accelerometer]") {
  const float level[3] = {0.0f, -1.0f, 0.0f};
  CHECK(IsPlausibleGravityReading(level));
  float xyz[3];
  ReadingForElevation(27.0, xyz);
  CHECK(IsPlausibleGravityReading(xyz));
  ReadingForElevation(-27.0, xyz);
  CHECK(IsPlausibleGravityReading(xyz));
  // A little noise and a slightly rolled base are what a real sensor reads.
  const float noisy[3] = {0.05f, -0.97f, 0.12f};
  CHECK(IsPlausibleGravityReading(noisy));
}

TEST_CASE("A reading that cannot be gravity on a standing sensor is refused",
          "[nui][accelerometer]") {
  // Nothing measured yet: atan2(0, 0) would be a confident and wrong zero.
  const float zero[3] = {0.0f, 0.0f, 0.0f};
  CHECK_FALSE(IsPlausibleGravityReading(zero));
  // Far too short or far too long to be one gravity.
  const float weak[3] = {0.0f, -0.3f, 0.0f};
  CHECK_FALSE(IsPlausibleGravityReading(weak));
  const float strong[3] = {0.0f, -2.0f, 0.0f};
  CHECK_FALSE(IsPlausibleGravityReading(strong));
  // Gravity pulling toward the sensor's top: the sensor is upside down, or
  // the runtime's axes are not the ones this code assumes. Either way the
  // angle made from it would be nonsense.
  const float upside_down[3] = {0.0f, 1.0f, 0.0f};
  CHECK_FALSE(IsPlausibleGravityReading(upside_down));
  // Not a number at all.
  const float not_a_number[3] = {0.0f, -1.0f,
                                 std::numeric_limits<float>::quiet_NaN()};
  CHECK_FALSE(IsPlausibleGravityReading(not_a_number));
  const float infinite[3] = {std::numeric_limits<float>::infinity(), -1.0f,
                             0.0f};
  CHECK_FALSE(IsPlausibleGravityReading(infinite));
}

TEST_CASE("The reported angle holds until the measurement moves away from it",
          "[nui][accelerometer]") {
  // A head resting near a half degree reads either side of it from one
  // reading to the next. The title compares the whole-degree angle against a
  // threshold, so the angle it is told only moves once the measurement is
  // three quarters of a degree away from it.
  CHECK(kAccelerometerHoldDegrees == 0.75);
  CHECK(HoldElevationDegrees(5, 5.0) == 5);
  CHECK(HoldElevationDegrees(5, 5.6) == 5);
  CHECK(HoldElevationDegrees(5, 4.3) == 5);
  CHECK(HoldElevationDegrees(5, 5.74) == 5);
  CHECK(HoldElevationDegrees(5, 5.75) == 6);
  CHECK(HoldElevationDegrees(5, 4.25) == 4);
  CHECK(HoldElevationDegrees(6, 5.4) == 6);
  CHECK(HoldElevationDegrees(6, 5.2) == 5);
  CHECK(HoldElevationDegrees(-8, -8.6) == -8);
  CHECK(HoldElevationDegrees(-8, -8.8) == -9);
  // A move of many degrees lands on the nearest whole degree, not one step.
  CHECK(HoldElevationDegrees(27, -24.8) == -25);
}

TEST_CASE("A jittering head does not flip the reported angle",
          "[nui][accelerometer]") {
  int32_t held = HoldElevationDegrees(0, 7.49);
  REQUIRE(held == 7);
  uint32_t changes = 0;
  for (int i = 0; i < 100; ++i) {
    const double measured = (i % 2) ? 7.52 : 7.46;
    const int32_t next = HoldElevationDegrees(held, measured);
    changes += next != held ? 1u : 0u;
    held = next;
  }
  CHECK(changes == 0u);
  CHECK(held == 7);
}

TEST_CASE("The elevation is logged again only after a whole degree of travel",
          "[nui][accelerometer]") {
  CHECK(kAccelerometerLogStepDegrees == 1.0);
  CHECK_FALSE(ElevationMovedEnoughToLog(7.49, 7.52));
  CHECK_FALSE(ElevationMovedEnoughToLog(7.49, 8.4));
  CHECK_FALSE(ElevationMovedEnoughToLog(7.49, 6.6));
  CHECK(ElevationMovedEnoughToLog(7.5, 8.5));
  CHECK(ElevationMovedEnoughToLog(7.49, 8.6));
  CHECK(ElevationMovedEnoughToLog(7.49, 6.2));
  CHECK(ElevationMovedEnoughToLog(27.0, -25.0));
}

}  // namespace test
}  // namespace nui
}  // namespace xe
