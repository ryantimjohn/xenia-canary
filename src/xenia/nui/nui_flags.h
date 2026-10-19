/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_FLAGS_H_
#define XENIA_NUI_NUI_FLAGS_H_

#include <cstdint>

#include "xenia/base/cvar.h"

DECLARE_bool(nui_allow_tilt);
DECLARE_bool(nui_log_messages);
DECLARE_bool(nui_observe_tracking);
DECLARE_int32(nui_log_play_frames);
DECLARE_int32(nui_tilt_override);
DECLARE_bool(nui_accelerometer);
DECLARE_bool(nui_show_depth_preview);
DECLARE_string(nui_record);
DECLARE_string(nui_replay);
DECLARE_bool(nui_guide_gesture);
DECLARE_int32(nui_guide_angle_min);
DECLARE_int32(nui_guide_angle_max);
DECLARE_int32(nui_guide_hold_ms);
DECLARE_int32(nui_guide_gap_ms);
DECLARE_string(nui_guide_arm);
DECLARE_int32(nui_guide_rest_angle_max);

namespace xe {
namespace nui {

// How far the Kinect's tilt motor can move the sensor head, in degrees from
// level: every backend clamps a title's tilt request to this range, a replay
// included, and it is the range nui_tilt_override may name.
inline constexpr int32_t kTiltMinimumDegrees = -27;
inline constexpr int32_t kTiltMaximumDegrees = 27;

// Writes the angle nui_tilt_override fixes and returns true when it is set to
// one the motor can reach, [kTiltMinimumDegrees, kTiltMaximumDegrees];
// anything else, the default of 1000 included, means the override is off and
// leaves out_degrees alone. Every
// backend that reports a tilt asks this first: the override outranks the
// sensor, measured or not.
bool GetTiltOverrideDegrees(int32_t* out_degrees);

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_FLAGS_H_
