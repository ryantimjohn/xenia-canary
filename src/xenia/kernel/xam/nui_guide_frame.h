/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_GUIDE_FRAME_H_
#define XENIA_KERNEL_XAM_NUI_GUIDE_FRAME_H_

#include <array>
#include <cstdint>

#include "xenia/kernel/xam/nui_guide_gesture.h"
#include "xenia/kernel/xam/nui_messages.h"

namespace xe {
namespace kernel {
namespace xam {

// These functions read what the Kinect Guide needs out of a HUD skeleton frame
// (message 0x2B002). They work on a copy of the frame in host memory, so that
// they can be tested without a guest.

static_assert(kNuiGuideJointCount == kNuiSkeletonPositionCount,
              "the recogniser takes a whole NUI skeleton");

// NUI_SKELETON_TRACKED is the only slot state whose joints are filled in.
constexpr uint32_t kNuiSkeletonTracked = 2;

// These are the joints of a skeleton that the guide and the play-frame log
// read, by their index in the public NUI skeleton layout.
constexpr size_t kHipCenter = 0;
constexpr size_t kShoulderCenter = 2;
constexpr size_t kHead = 3;
constexpr size_t kHandLeft = 7;
constexpr size_t kHandRight = 11;
constexpr size_t kFootLeft = 15;
constexpr size_t kFootRight = 19;

// A NuiGuideSkeleton holds a skeleton slot's joints and their tracking states
// as host values. The positions are in metres in the sensor's camera space.
struct NuiGuideSkeleton {
  std::array<NuiGuideVector, kNuiGuideJointCount> joints;
  std::array<uint32_t, kNuiGuideJointCount> tracking_states;
};

// Reads one skeleton slot's joints out of the frame's big-endian data.
NuiGuideSkeleton ReadNuiGuideSkeleton(const X_NUI_SKELETON_DATA& skeleton);

// Returns the direction of gravity in the frame's camera space, of unit
// length: the reverse of the floor plane's normal when the title's tracker has
// found the floor, and straight down the camera's y axis when it has not.
NuiGuideVector NuiGuideGravityAxis(const NuiMsg2B002& frame);

// Returns whether a slot of the frame, tracked or position-only, holds
// tracking_id. No slot holds the id 0.
bool NuiGuideFrameHoldsTrackingId(const NuiMsg2B002& frame,
                                  uint32_t tracking_id);

// A raised hand steers the guide's cursor over a box in front of the
// shoulders, centred on the shoulder centre; these are its size in metres.
constexpr float kNuiGuideHandBoxWidth = 0.5f;
constexpr float kNuiGuideHandBoxHeight = 0.4f;

// Maps the skeleton's higher hand onto the guide's overlay. x runs from 0 at
// the box's left edge to 1 at its right, y from 0 at its top to 1 at its
// bottom, and a hand outside the box is held at its edge. Only a hand that is
// tracked or inferred counts. Returns false when there is no such hand or the
// shoulder centre is not tracked or inferred.
bool MapNuiGuideHand(const NuiGuideSkeleton& skeleton, float* x, float* y);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_GUIDE_FRAME_H_
