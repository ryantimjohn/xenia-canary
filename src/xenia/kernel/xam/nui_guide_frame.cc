/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_guide_frame.h"

#include <algorithm>
#include <cmath>

namespace xe {
namespace kernel {
namespace xam {

namespace {

// NUI_SKELETON_NOT_TRACKED and NUI_SKELETON_POSITION_NOT_TRACKED are both 0.
constexpr uint32_t kNotTracked = 0;

// A floor normal shorter than this was not found by the tracker.
constexpr float kMinimumNormalLength = 1e-3f;

bool IsFinite(const NuiGuideVector& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Returns whether a joint can steer: tracked or inferred, at a real position.
bool IsJointSeen(const NuiGuideSkeleton& skeleton, size_t joint) {
  return skeleton.tracking_states[joint] != kNotTracked &&
         IsFinite(skeleton.joints[joint]);
}

}  // namespace

NuiGuideSkeleton ReadNuiGuideSkeleton(const X_NUI_SKELETON_DATA& skeleton) {
  NuiGuideSkeleton result;
  for (size_t i = 0; i < kNuiGuideJointCount; ++i) {
    const X_NUI_VECTOR4& position = skeleton.skeleton_positions[i];
    result.joints[i] = {position.x, position.y, position.z};
    result.tracking_states[i] = skeleton.position_tracking_state[i];
  }
  return result;
}

NuiGuideVector NuiGuideGravityAxis(const NuiMsg2B002& frame) {
  const NuiGuideVector normal = {frame.floor_clip_plane.x,
                                 frame.floor_clip_plane.y,
                                 frame.floor_clip_plane.z};
  const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                 normal.z * normal.z);
  // The comparison fails for a length that is not a number.
  if (!(length >= kMinimumNormalLength && std::isfinite(length))) {
    return {0.0f, -1.0f, 0.0f};
  }
  // The floor's normal points up, out of the floor, so gravity is its reverse.
  return {-normal.x / length, -normal.y / length, -normal.z / length};
}

bool NuiGuideFrameHoldsTrackingId(const NuiMsg2B002& frame,
                                  uint32_t tracking_id) {
  if (!tracking_id) {
    return false;
  }
  for (const X_NUI_SKELETON_DATA& skeleton : frame.skeletons) {
    if (skeleton.tracking_state != kNotTracked &&
        skeleton.tracking_id == tracking_id) {
      return true;
    }
  }
  return false;
}

bool MapNuiGuideHand(const NuiGuideSkeleton& skeleton, float* x, float* y) {
  if (!IsJointSeen(skeleton, kShoulderCenter)) {
    return false;
  }
  const bool left_seen = IsJointSeen(skeleton, kHandLeft);
  const bool right_seen = IsJointSeen(skeleton, kHandRight);
  if (!left_seen && !right_seen) {
    return false;
  }
  // The higher hand is the one raised to steer; the other hangs at the side.
  size_t hand = left_seen ? kHandLeft : kHandRight;
  if (left_seen && right_seen &&
      skeleton.joints[kHandRight].y > skeleton.joints[kHandLeft].y) {
    hand = kHandRight;
  }

  const NuiGuideVector& centre = skeleton.joints[kShoulderCenter];
  const NuiGuideVector& position = skeleton.joints[hand];
  const float left_edge = centre.x - kNuiGuideHandBoxWidth / 2.0f;
  const float top_edge = centre.y + kNuiGuideHandBoxHeight / 2.0f;
  *x = std::clamp((position.x - left_edge) / kNuiGuideHandBoxWidth, 0.0f, 1.0f);
  *y = std::clamp((top_edge - position.y) / kNuiGuideHandBoxHeight, 0.0f, 1.0f);
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
