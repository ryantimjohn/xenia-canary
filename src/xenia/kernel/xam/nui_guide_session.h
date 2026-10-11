/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_GUIDE_SESSION_H_
#define XENIA_KERNEL_XAM_NUI_GUIDE_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "xenia/kernel/xam/nui_guide_frame.h"
#include "xenia/kernel/xam/nui_guide_gesture.h"
#include "xenia/kernel/xam/nui_messages.h"

namespace xe {
namespace kernel {
namespace xam {

// A NuiGuideSession says whether the Kinect Guide is up and whose skeleton it
// has taken, and decides what a completed gesture or a HUD skeleton frame
// does to it. It holds no lock and has no side effects; NuiState serialises
// the calls and does the logging and the notifying.
class NuiGuideSession {
 public:
  // A Change says what a call did to the guide.
  enum class Change {
    kNone,
    kOpened,
    kClosed,
  };

  bool open() const { return open_; }

  // Returns the tracking id of the player whose gesture opened the guide, or
  // 0 while it is closed.
  uint32_t tracking_id() const { return tracking_id_; }

  // A player completed the gesture. With the guide closed, the guide opens and
  // takes that player. With it open, only the player it took closes it again;
  // anybody else, a player the tracker lost and found again under a new id
  // included, is ignored. An id of 0 names nobody and does nothing.
  Change OnGestureCompleted(uint32_t tracking_id) {
    if (!tracking_id) {
      return Change::kNone;
    }
    if (!open_) {
      open_ = true;
      tracking_id_ = tracking_id;
      return Change::kOpened;
    }
    if (tracking_id != tracking_id_) {
      return Change::kNone;
    }
    Close();
    return Change::kClosed;
  }

  // Closes the guide, whatever asks. Returns whether it was open.
  bool Close() {
    if (!open_) {
      return false;
    }
    open_ = false;
    tracking_id_ = 0;
    return true;
  }

  // Returns whether the guide takes a HUD skeleton frame, which xam answers
  // with 0x8301000B: while it is open, a frame that holds the player it took.
  bool TakesFrame(const NuiMsg2B002& frame) const {
    return open_ && NuiGuideFrameHoldsTrackingId(frame, tracking_id_);
  }

 private:
  bool open_ = false;
  uint32_t tracking_id_ = 0;
};

// A NuiGuideFrameResult says what one HUD skeleton frame did to the guide and
// what the guide makes of the frame.
struct NuiGuideFrameResult {
  // The frame's change to the guide, if it made one, and the player and the
  // arm whose completed gesture made it.
  NuiGuideSession::Change change = NuiGuideSession::Change::kNone;
  uint32_t change_tracking_id = 0;
  NuiGuideArm change_arm = NuiGuideArm::kNone;
  // Whether the guide takes the frame, and the player whose skeleton it has
  // taken.
  bool takes = false;
  uint32_t taken_tracking_id = 0;
  // Where the engaged player's higher hand points on the overlay (see
  // MapNuiGuideHand), when the frame shows that player's hands.
  bool hand_valid = false;
  float hand_x = 0.0f;
  float hand_y = 0.0f;
  // What the recogniser measured of each skeleton it was fed, in slot order,
  // for the diagnostic trace; the first trace_count entries are filled.
  std::array<NuiGuideTrace, kNuiSkeletonCount> traces;
  size_t trace_count = 0;
};

// Runs one HUD skeleton frame through the guide. Every tracked skeleton is fed
// to gesture, so that each keeps its own hold, and the result keeps the
// recogniser's trace of each; a null gesture feeds nobody, and the guide then
// stays as it is. A frame changes the guide at most once.
// While the guide is up, only the engaged player's completion counts, and it
// closes the guide; another player's completion on the same frame is ignored,
// as it would be on any frame, and does not open the guide again. While the
// guide is closed, the first completion in slot order opens it, and any other
// on the same frame is ignored. Last, the engaged player's hand is read and
// the guide decides whether it takes the frame.
NuiGuideFrameResult AdvanceNuiGuide(NuiGuideSession& session,
                                    NuiGuideGesture* gesture,
                                    const NuiMsg2B002& frame,
                                    uint64_t timestamp_us);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_GUIDE_SESSION_H_
