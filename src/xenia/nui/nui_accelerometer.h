/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_ACCELEROMETER_H_
#define XENIA_NUI_NUI_ACCELEROMETER_H_

#include <cmath>
#include <cstdint>

namespace xe {
namespace nui {

// What a sensor's accelerometer says about where the sensor points. A reading
// is a gravity vector in g along the sensor's own axes, as the Kinect for
// Windows runtime documents it: the vector points at the floor, y is up, z is
// the way the sensor looks and x is to its left, so a level sensor reads
// (0, -1, 0). The accelerometer sits in the head the tilt motor moves, so the
// reading follows the head and is measured against gravity rather than
// against the sensor's base, just like the elevation angle the title sets.

// The sign that turns the reading's z leg into the sine of the elevation.
// A head pitched up by e reads z = -sin(e): the reading points down, and
// once the head looks up, down lies partly behind it. The elevation is
// therefore atan2(-z, -y), positive for "look up" like the angle
// NuiCameraElevationSetAngle takes. The sign came from the documented axes
// and was confirmed on a Kinect for Windows sensor: a reading of
// (-0.027, -0.943, 0.343) g gives -20.0 degrees, and the runtime's own
// NuiCameraElevationGetAngle said -20 for the same head. An Xbox 360 sensor
// whose motor works confirmed it in motion: commanded to +27 it read +27.2 and
// to -27, -27.6 (tools/kinect_motor_test.ps1). If a sensor pitched up ever
// reads a positive z, this constant is the one thing to flip, and the block
// the guest reads flips with it.
constexpr float kAccelerometerElevationSign = -1.0f;

// A reading whose length is outside this range is not one gravity: nothing
// has been measured yet, or the sensor is being shaken or carried.
constexpr float kAccelerometerMinimumG = 0.5f;
constexpr float kAccelerometerMaximumG = 1.5f;

// Radians to degrees, for the elevation below.
constexpr double kAccelerometerDegreesPerRadian =
    180.0 / 3.14159265358979323846;

// True when the reading can be gravity acting on a sensor that stands on its
// base: every leg is a number, the length is about one gravity, and gravity
// pulls toward the sensor's underside. A reading that fails this would turn
// into a confident and wrong angle, so it is not used at all.
inline bool IsPlausibleGravityReading(const float* xyz) {
  if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) ||
      !std::isfinite(xyz[2])) {
    return false;
  }
  const float length =
      std::sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]);
  if (length < kAccelerometerMinimumG || length > kAccelerometerMaximumG) {
    return false;
  }
  return -xyz[1] > 0.0f;
}

// The elevation of the sensor's line of sight above the horizontal, in
// degrees, positive when it looks up. The x leg is left out, as the guest's
// own tilt driver leaves it out when it reads the block: a base rolled five
// degrees to one side moves the answer by less than a tenth of a degree.
inline double AccelerometerElevationDegrees(const float* xyz) {
  return std::atan2(static_cast<double>(kAccelerometerElevationSign * xyz[2]),
                    static_cast<double>(-xyz[1])) *
         kAccelerometerDegreesPerRadian;
}

// How far a measurement has to move from the whole-degree angle reported
// before it, before a different angle is reported.
constexpr double kAccelerometerHoldDegrees = 0.75;

// How far the elevation has to move from the one last logged before it is
// logged again.
constexpr double kAccelerometerLogStepDegrees = 1.0;

// The whole-degree angle to report for a measured elevation, given the one
// reported before. A head resting near a half degree reads either side of
// it from one reading to the next, and the title compares the whole-degree
// angle against a threshold, so the reported angle only moves once the
// measurement is kAccelerometerHoldDegrees away from it; it then lands on
// the nearest whole degree, however far the head went.
inline int32_t HoldElevationDegrees(int32_t reported, double measured) {
  if (std::fabs(measured - static_cast<double>(reported)) <
      kAccelerometerHoldDegrees) {
    return reported;
  }
  return static_cast<int32_t>(std::lround(measured));
}

// True when the elevation has travelled far enough from the one last logged
// to be worth another line, so that noise around a boundary stays quiet.
inline bool ElevationMovedEnoughToLog(double logged, double measured) {
  return std::fabs(measured - logged) >= kAccelerometerLogStepDegrees;
}

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_ACCELEROMETER_H_
