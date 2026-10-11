/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_GUIDE_DEFAULTS_H_
#define XENIA_NUI_NUI_GUIDE_DEFAULTS_H_

#include <cstdint>

namespace xe {
namespace nui {

// The Kinect Guide gesture's default thresholds, in the cvars' units. They are
// the defaults of the nui_guide_* cvars (nui_flags.cc) and, converted, of
// xam's NuiGuideGestureSettings, which the kernel test suite builds without
// the cvars; both take them from here so that the two cannot drift apart.

// The band of angles from straight down that the arm must lie in, in degrees
// (nui_guide_angle_min and nui_guide_angle_max).
inline constexpr int32_t kGuideAngleMinDefault = 30;
inline constexpr int32_t kGuideAngleMaxDefault = 60;
// How long the pose must be held, in milliseconds (nui_guide_hold_ms).
inline constexpr int32_t kGuideHoldMsDefault = 1000;
// How long a stretch of missed or off-pose frames the hold survives, in
// milliseconds (nui_guide_gap_ms).
inline constexpr int32_t kGuideGapMsDefault = 200;
// Which arm makes the gesture: "left", "right" or "either" (nui_guide_arm).
// The console's gesture is made with the left arm.
inline constexpr char kGuideArmDefault[] = "left";
// How far from straight down the other arm may hang while one arm makes the
// gesture, in degrees (nui_guide_rest_angle_max). It lies between the angles
// measured in deliberate gestures and in game poses that opened the guide by
// accident; "The Kinect Guide" in docs/nui.md has the numbers.
inline constexpr int32_t kGuideRestAngleMaxDefault = 20;

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_GUIDE_DEFAULTS_H_
