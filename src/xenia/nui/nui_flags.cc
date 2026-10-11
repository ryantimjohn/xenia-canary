/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_flags.h"

#include "xenia/nui/nui_guide_defaults.h"

DEFINE_bool(nui_allow_tilt, true, "Allow titles to move the Kinect tilt motor.",
            "Kinect");

DEFINE_bool(nui_log_messages, false,
            "Log every NUI (Kinect) xam message and XamNui* call with a hex "
            "dump of its buffer.",
            "Kinect");

DEFINE_int32(nui_log_play_frames, 0,
             "Every N-th NUI skeleton frame, log each tracked skeleton's "
             "position and key joints, the floor plane, and a summary of the "
             "player mask; 0 is off.",
             "Kinect");

DEFINE_int32(nui_tilt_override, 1000,
             "If between -27 and 27, report this tilt angle to titles instead "
             "of the sensor's.",
             "Kinect");

DEFINE_bool(nui_accelerometer, true,
            "Read the Kinect's accelerometer for the real tilt angle. Off "
            "keeps the requested-angle model.",
            "Kinect");

DEFINE_bool(nui_show_depth_preview, false,
            "Show an overlay with the depth frames fed to the guest.",
            "Kinect");

DEFINE_string(nui_record, "",
              "Record every published depth frame to this file (raw .xdepth) "
              "for --nui=replay.",
              "Kinect");

DEFINE_string(nui_replay, "",
              "Depth recording (.xdepth) to publish with --nui=replay; loops "
              "when it ends.",
              "Kinect");

DEFINE_bool(nui_guide_gesture, true,
            "Watch the skeletons a Kinect title hands xam for the Kinect Guide "
            "gesture, the left arm held still out to the side and down at "
            "about 45 degrees with the right arm at the side.",
            "Kinect");

DEFINE_int32(nui_guide_angle_min, xe::nui::kGuideAngleMinDefault,
             "An arm making the Kinect Guide gesture must be at least this "
             "many degrees from straight down.",
             "Kinect");

DEFINE_int32(nui_guide_angle_max, xe::nui::kGuideAngleMaxDefault,
             "An arm making the Kinect Guide gesture must be at most this many "
             "degrees from straight down.",
             "Kinect");

DEFINE_int32(nui_guide_hold_ms, xe::nui::kGuideHoldMsDefault,
             "The Kinect Guide gesture must be held for this many "
             "milliseconds.",
             "Kinect");

DEFINE_int32(nui_guide_gap_ms, xe::nui::kGuideGapMsDefault,
             "The Kinect Guide gesture's hold survives this many milliseconds "
             "of missed or off-pose frames; values below 50 count as 50.",
             "Kinect");

DEFINE_string(nui_guide_arm, xe::nui::kGuideArmDefault,
              "Which arm makes the Kinect Guide gesture: left, as on the "
              "console, right, or either. Any other value means left.",
              "Kinect");

DEFINE_int32(nui_guide_rest_angle_max, xe::nui::kGuideRestAngleMaxDefault,
             "While one arm makes the Kinect Guide gesture, the other must "
             "hang at most this many degrees from straight down.",
             "Kinect");

namespace xe {
namespace nui {

bool GetTiltOverrideDegrees(int32_t* out_degrees) {
  const int32_t override_degrees = cvars::nui_tilt_override;
  if (override_degrees < kTiltMinimumDegrees ||
      override_degrees > kTiltMaximumDegrees) {
    return false;
  }
  *out_degrees = override_degrees;
  return true;
}

}  // namespace nui
}  // namespace xe
