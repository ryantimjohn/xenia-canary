/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_TILT_STATUS_H_
#define XENIA_KERNEL_XAM_NUI_TILT_STATUS_H_

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

#include "xenia/kernel/xam/nui_messages.h"
#include "xenia/nui/nui_accelerometer.h"

namespace xe {
namespace kernel {
namespace xam {

// The two words XamNuiCameraElevationGetAngle hands back: the tilt angle in
// its first argument and the tilt status in its second. A title reads the two
// apart. 4D5308ED compares the angle against a remembered one with a
// magnitude threshold (0x82a5fbd0) and tests single bits of the status
// (0x82a5f864 tests 0x2, 0x82a596bc tests 0x1), so a sensor that is sitting
// where it was asked to sit has to report a status of zero. They are kept in
// their own header because they are the whole of what the guest sees of the
// tilt motor's condition, and because they can then be tested on their own.

// Nothing is wrong with the sensor and nothing is in progress.
constexpr uint32_t kNuiTiltStatusOk = 0x0;

// The tilt motor is on its way somewhere, so the angle just read is not where
// the sensor is going to end up. 4D5308ED' tilt driver gives up for
// this frame when it sees this bit.
constexpr uint32_t kNuiTiltStatusBusy = 0x1;

// The sensor cannot get to where it was asked to go and wants the user to
// look at it. 4D5308ED holds its engagement HUD in "check the sensor"
// for as long as this bit is set, and ignores every skeleton it is tracking.
constexpr uint32_t kNuiTiltStatusNeedsAttention = 0x2;

// Builds the status word for a sensor that is or is not mid-move, and does or
// does not want looking at.
constexpr uint32_t NuiTiltStatusWord(bool busy, bool needs_attention) {
  return (busy ? kNuiTiltStatusBusy : kNuiTiltStatusOk) |
         (needs_attention ? kNuiTiltStatusNeedsAttention : kNuiTiltStatusOk);
}

// Packs a tilt angle into the DWORD the guest hands in and reads back as a
// LONG. A Kinect tilts through [-27, 27] degrees, so most of the angles it can
// report are negative and every one of them has to survive the trip.
constexpr uint32_t NuiTiltAngleWord(int32_t degrees) {
  return static_cast<uint32_t>(degrees);
}

// The length of the accelerometer vector the tilt-status block reports. A real
// sensor reports gravity in its own units; the driver only ever divides one
// leg by another, so the scale cancels and any magnitude large enough to keep
// the rounding small will do. 981 is a milli-g reading of one gravity, and at
// that length a whole degree is seventeen counts.
constexpr int32_t kNuiTiltGravity = 981;

// Radians per degree, for the sine and cosine below.
constexpr double kNuiTiltRadiansPerDegree = 3.14159265358979323846 / 180.0;

// The bits of the block's own flags word that the driver reads as "the motor
// is still moving" (& 3 at 0x82d2dd90, and the public accessor 0x82d2c6d8).
// While they are set it waits for the move instead of comparing the angle.
// The block's other two known bits, & 4 and & 8, make the driver return 25
// and 31 (0x82d2dee4, 0x82d2ddc8); what those mean is not known, so no bit
// here says "cannot get there, stop asking", and xenia sets neither.
constexpr uint32_t kNuiTiltBlockMoving = 0x3;

// The three accelerometer longs of the block, +0x20..+0x28, in its own
// encoding and scale. They are a type of their own because the block carries
// them twice: once for where the sensor is now, and once in its tail for
// where it was when the title last remembered the floor.
struct NuiTiltStatusLegs {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
};

// The legs for a gravity whose components along the sensor's up axis and its
// line of sight, in g, are given. The sideways leg stays zero (see
// NuiTiltStatusLegsFromGravity).
inline NuiTiltStatusLegs NuiTiltStatusLegsForGravityLegs(double cosine_leg,
                                                         double sine_leg) {
  NuiTiltStatusLegs legs;
  legs.y = static_cast<int32_t>(
      std::lround(static_cast<double>(kNuiTiltGravity) * cosine_leg));
  legs.z = static_cast<int32_t>(
      std::lround(static_cast<double>(kNuiTiltGravity) * sine_leg));
  return legs;
}

// The legs of a sensor at the given angle.
inline NuiTiltStatusLegs NuiTiltStatusLegsForAngle(int32_t degrees) {
  const double radians =
      static_cast<double>(degrees) * kNuiTiltRadiansPerDegree;
  return NuiTiltStatusLegsForGravityLegs(std::cos(radians), std::sin(radians));
}

// The legs of a gravity vector the sensor's accelerometer measured (xyz in g
// on the host runtime's axes; see nui_accelerometer.h), for a sensor whose
// real position is known rather than assumed. They are the up-pointing ones
// NuiTiltStatusLegsForAngle gives, so the host's down-pointing y leg is
// negated and its z leg takes the same sign the elevation does: the driver's
// atan2(accel_z, accel_y) is then exactly
// xe::nui::AccelerometerElevationDegrees of the reading, and the block and
// the reported angle agree. The reading's own length is kept, as a real
// sensor's would be; only the ratio of the legs means anything to the
// driver. The x leg stays zero, as it does for an angle. The tilt driver
// ignores it, but the gravity axis the title builds from the block,
// normalize(-x, y, z), does not, and which way the guest counts a sideways
// roll is not known; a wrong guess would lean every skeleton the wrong way,
// where zero leaves the axis upright across the sensor.
inline NuiTiltStatusLegs NuiTiltStatusLegsFromGravity(const float* xyz) {
  return NuiTiltStatusLegsForGravityLegs(
      -static_cast<double>(xyz[1]),
      static_cast<double>(xe::nui::kAccelerometerElevationSign * xyz[2]));
}

// Fills the block XamNuiCameraTiltGetStatus hands back, for a sensor whose
// accelerometer legs are given: NuiTiltStatusLegsForAngle makes them for an
// angle, and NuiTiltStatusLegsFromGravity for a measured reading.
//
// The NUI runtime's auto-tilt driver does not believe the angle it is told:
// it works out where the camera is pointing from the block's accelerometer,
// as atan2(accel_z, accel_y) * 180/pi (0x82d2dd40-0x82d2dd94), and keeps
// commanding the motor until that is within two degrees of the angle it asked
// for (0x82d2df54-0x82d2df84 and 0x82d2e048-0x82d2e078, with the 2.0 at
// 0x82001168 and the 57.2957763671875 at 0x82018068). Only the ratio of the
// two legs decides that answer, which is why the magnitude below is arbitrary
// and why this cannot be got wrong by a scale factor - only by a sign.
//
// The flags word is kNuiTiltBlockMoving while the motor is on its way and
// zero once it is not. A block made from angle legs reports the angle the
// backend reports, which without an accelerometer is the one last asked for, so
// the driver settles one poll after each move. A block made from measured legs
// settles only once the head really is within the two degrees; a head that
// cannot get there - a base at a slant, a motor at the end of its travel -
// leaves the driver asking again, and nothing in the block can tell it to stop,
// because no known flag means that. The Kinect backend is what keeps that
// bounded: it drops a repeat of the target it last gave the motor until the
// motor has had a few seconds to get there.
// The reserved region and the floor tail are cleared, because the caller's
// block is a stack buffer it only fills the first two fields of; the tail is
// FillNuiTiltStatusFloor's to write afterwards, when there is a floor.
//
// The whole 0x50 bytes are written. The export validates the caller's range,
// cb_size and signature before this fill; internal callers provide a complete
// non-null block. Nothing is left as the caller wrote it.
inline void FillNuiTiltStatusLegs(X_NUI_TILT_STATUS* block,
                                  const NuiTiltStatusLegs& legs, bool moving) {
  std::memset(block, 0, sizeof(*block));
  // Re-stamped rather than left at the zero the memset just wrote: these are
  // the two constants every observed caller puts in the block before the call
  // (0x82d2dd24 builds them, 0x82d2dd34 stores them), so writing them back is
  // what makes the block the caller reads the block it handed over.
  block->cb_size = kNuiTiltStatusSize;
  block->signature = kNuiTiltStatusSignature;

  block->accel_x = legs.x;
  block->accel_y = legs.y;
  block->accel_z = legs.z;
  // Not kNuiTiltStatusBusy or kNuiTiltStatusOk: this is the block's own flags
  // word, whose bits are not the ones XamNuiCameraElevationGetAngle reports.
  block->status_flags = moving ? kNuiTiltBlockMoving : 0;
}

// A floor the title handed XamNuiCameraRememberFloor, with what the block
// said about the sensor when it did.
struct NuiRememberedFloor {
  // The plane as the export received it (DecodeNuiFloorPlane): the unit
  // normal's three components, then the sensor's height above the floor in
  // millimetres, where the HUD skeleton frame carries metres. All zeros
  // until the title has remembered a floor.
  std::array<float, 4> plane = {};
  // The export's third argument, whose meaning is not known.
  uint32_t flag = 0;
  // The accelerometer legs the block would have carried at that moment, or
  // nothing when the sensor's angle could not be read then.
  std::optional<NuiTiltStatusLegs> legs;
};

// Writes the tail of the block (+0x30..+0x4C) from a remembered floor, the
// way the NUI runtime reads it back: the plane as it was received, d in
// millimetres - the runtime aims from d with millimetre constants, and the
// same floor in metres would aim it 17-21 degrees above level - then a
// flags word of zero, then the legs of the moment the floor was remembered.
// Nothing before +0x30 is touched, so the call goes after the fill,
// FillNuiTiltStatusLegs. Returns false and writes nothing, which leaves the
// tail as the fill cleared it, when there is no floor the runtime could aim
// from: no legs, or a d that is not a finite, positive height, or a plane that
// is not finite. A d of zero is the runtime's own "no floor", which is what it
// sees from a block with a clear tail.
//
// The tail holds the plane and the legs as one pair, and nothing here checks
// that they still describe the room. The runtime aims from d as it stands,
// and by its reading of the two accelerometers it turns the plane through the
// difference between the tail's legs and the block's before it publishes it
// (0x82d2d540), which is right for a head that the motor or a hand has
// tilted. The pair stays fresh only because the title remembers the floor
// again whenever its tracker has held one steady for a while, and on every
// poll of a search: 700-2,500 times a session. If the stand were moved -
// raised, lowered, or set down at a slant - while no floor was in view, the
// pair would go on describing the old place until the tracker fitted a floor
// again: the runtime would aim for a floor at the old height and publish a
// floor clip plane where the floor no longer is. A console presumably makes
// the runtime search again after such a move (bit 0x10 of the block's
// status_flags, +0x2C, forces a search, 0x82d2cdf0); xenia has nothing that
// notices one.
inline bool FillNuiTiltStatusFloor(const NuiRememberedFloor& floor,
                                   X_NUI_TILT_STATUS* block) {
  const std::array<float, 4>& plane = floor.plane;
  if (!floor.legs || !std::isfinite(plane[0]) || !std::isfinite(plane[1]) ||
      !std::isfinite(plane[2]) || !std::isfinite(plane[3]) ||
      plane[3] <= 0.0f) {
    return false;
  }
  for (size_t i = 0; i < plane.size(); ++i) {
    block->floor_plane[i] = plane[i];
  }
  block->floor_flags = 0u;
  block->floor_accel_x = floor.legs->x;
  block->floor_accel_y = floor.legs->y;
  block->floor_accel_z = floor.legs->z;
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_TILT_STATUS_H_
