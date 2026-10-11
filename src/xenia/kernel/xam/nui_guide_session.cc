/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_guide_session.h"

namespace xe {
namespace kernel {
namespace xam {

NuiGuideFrameResult AdvanceNuiGuide(NuiGuideSession& session,
                                    NuiGuideGesture* gesture,
                                    const NuiMsg2B002& frame,
                                    uint64_t timestamp_us) {
  NuiGuideFrameResult result;
  if (gesture) {
    const NuiGuideVector gravity = NuiGuideGravityAxis(frame);
    for (const X_NUI_SKELETON_DATA& slot : frame.skeletons) {
      if (slot.tracking_state != kNuiSkeletonTracked) {
        continue;
      }
      const uint32_t tracking_id = slot.tracking_id;
      const NuiGuideSkeleton skeleton = ReadNuiGuideSkeleton(slot);
      // Every tracked skeleton is fed, even after the frame has changed the
      // guide, so that each player's hold stays in step with the frames. There
      // are as many traces as slots.
      const bool completed =
          gesture->Update(tracking_id, skeleton.joints,
                          skeleton.tracking_states, gravity, timestamp_us);
      result.traces[result.trace_count++] = gesture->last_trace();
      if (!completed || result.change != NuiGuideSession::Change::kNone) {
        continue;
      }
      const NuiGuideSession::Change change =
          session.OnGestureCompleted(tracking_id);
      if (change != NuiGuideSession::Change::kNone) {
        result.change = change;
        result.change_tracking_id = tracking_id;
        result.change_arm = gesture->last_fired_arm();
      }
    }
  }

  if (session.open()) {
    for (const X_NUI_SKELETON_DATA& slot : frame.skeletons) {
      if (slot.tracking_state == kNuiSkeletonTracked &&
          slot.tracking_id == session.tracking_id()) {
        result.hand_valid = MapNuiGuideHand(ReadNuiGuideSkeleton(slot),
                                            &result.hand_x, &result.hand_y);
        break;
      }
    }
  }
  result.takes = session.TakesFrame(frame);
  result.taken_tracking_id = session.tracking_id();
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
