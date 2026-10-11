/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_guide_gesture.h"

#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/nui/nui_flags.h"

namespace xe {
namespace kernel {
namespace xam {

// Kept apart from nui_guide_gesture.cc so that the kernel test suite can
// compile the recogniser without linking the cvar definitions.
NuiGuideGestureSettings NuiGuideGestureSettingsFromCvars() {
  NuiGuideArmChoice arms = NuiGuideArmChoice::kLeft;
  if (!ParseNuiGuideArmChoice(cvars::nui_guide_arm, &arms)) {
    // A recogniser is made for every title, but one Warning a run is enough.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      XELOGW(
          "NUI: nui_guide_arm is \"{}\", which is not left, right or either; "
          "the left arm makes the Kinect Guide gesture",
          cvars::nui_guide_arm);
    }
  }
  return MakeNuiGuideGestureSettings(
      cvars::nui_guide_angle_min, cvars::nui_guide_angle_max,
      cvars::nui_guide_hold_ms, cvars::nui_guide_gap_ms,
      cvars::nui_guide_rest_angle_max, arms);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
