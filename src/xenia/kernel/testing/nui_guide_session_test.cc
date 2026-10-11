/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_guide_session.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {
namespace {

constexpr uint32_t kPositionOnly = 1;
constexpr uint32_t kTracked = 2;
constexpr uint32_t kPlayerId = 66;
constexpr uint32_t kOtherPlayerId = 67;

// These are the joints of the public NUI skeleton layout the tests place.
constexpr size_t kHipCenter = 0;
constexpr size_t kShoulderCenter = 2;
constexpr size_t kShoulderLeft = 4;
constexpr size_t kElbowLeft = 5;
constexpr size_t kHandLeft = 7;
constexpr size_t kShoulderRight = 8;
constexpr size_t kElbowRight = 9;
constexpr size_t kHandRight = 11;

// The sensor delivers 30 frames a second, and the default hold is a second.
constexpr uint64_t kFrameUs = 33333;
constexpr uint64_t kHoldUs = 1000000;

// Returns an empty frame on the heap: it is 2784 bytes long.
std::unique_ptr<NuiMsg2B002> EmptyFrame() {
  auto frame = std::make_unique<NuiMsg2B002>();
  std::memset(frame.get(), 0, sizeof(NuiMsg2B002));
  return frame;
}

// Returns a frame on the heap with the given ids tracked in its first slots.
std::unique_ptr<NuiMsg2B002> FrameWith(uint32_t first_id,
                                       uint32_t second_id = 0) {
  auto frame = EmptyFrame();
  if (first_id) {
    frame->skeletons[0].tracking_state = kTracked;
    frame->skeletons[0].tracking_id = first_id;
  }
  if (second_id) {
    frame->skeletons[1].tracking_state = kTracked;
    frame->skeletons[1].tracking_id = second_id;
  }
  return frame;
}

enum class Pose {
  // Both arms hang at the sides.
  kStanding,
  // The left arm is held straight out to the side and down at 45 degrees.
  kGesture,
};

// Writes a player into one of the frame's slots, 2.5 m in front of the sensor
// with every joint tracked, in metres. The frame has no floor, so gravity is
// the camera's down axis.
void PutPlayer(NuiMsg2B002& frame, size_t slot_index, uint32_t tracking_id,
               Pose pose, uint32_t slot_state = kTracked) {
  X_NUI_SKELETON_DATA& slot = frame.skeletons[slot_index];
  slot.tracking_state = slot_state;
  slot.tracking_id = tracking_id;
  auto place = [&slot](size_t joint, float x, float y) {
    slot.skeleton_positions[joint].x = x;
    slot.skeleton_positions[joint].y = y;
    slot.skeleton_positions[joint].z = 2.5f;
    slot.skeleton_positions[joint].w = 1.0f;
    slot.position_tracking_state[joint] = kTracked;
  };
  for (size_t joint = 0; joint < kNuiSkeletonPositionCount; ++joint) {
    place(joint, 0.0f, 0.0f);
  }
  place(kHipCenter, 0.0f, 0.0f);
  place(kShoulderCenter, 0.0f, 0.45f);
  place(kShoulderLeft, -0.2f, 0.45f);
  place(kShoulderRight, 0.2f, 0.45f);
  place(kElbowRight, 0.2f, 0.175f);
  place(kHandRight, 0.2f, -0.1f);
  if (pose == Pose::kGesture) {
    // Both segments are 0.275 m long, 45 degrees out from straight down.
    place(kElbowLeft, -0.39445f, 0.25555f);
    place(kHandLeft, -0.58891f, 0.06109f);
  } else {
    place(kElbowLeft, -0.2f, 0.175f);
    place(kHandLeft, -0.2f, -0.1f);
  }
}

// Feeds the frame every frame period from start_us until one changes the guide
// or three hold times have gone by, and returns the last result; *time_us is
// when that frame came.
NuiGuideFrameResult FeedUntilChange(NuiGuideSession& session,
                                    NuiGuideGesture* gesture,
                                    const NuiMsg2B002& frame, uint64_t start_us,
                                    uint64_t* time_us) {
  NuiGuideFrameResult result;
  for (uint64_t t = start_us; t < start_us + 3 * kHoldUs; t += kFrameUs) {
    result = AdvanceNuiGuide(session, gesture, frame, t);
    *time_us = t;
    if (result.change != NuiGuideSession::Change::kNone) {
      break;
    }
  }
  return result;
}

}  // namespace

TEST_CASE("A fresh guide session is closed", "[nui][guide_session]") {
  NuiGuideSession session;
  CHECK_FALSE(session.open());
  CHECK(session.tracking_id() == 0);
  CHECK_FALSE(session.TakesFrame(*FrameWith(kPlayerId)));
}

TEST_CASE("A gesture opens the guide for the player who made it",
          "[nui][guide_session]") {
  NuiGuideSession session;
  CHECK(session.OnGestureCompleted(kPlayerId) ==
        NuiGuideSession::Change::kOpened);
  CHECK(session.open());
  CHECK(session.tracking_id() == kPlayerId);
}

TEST_CASE("Only the engaged player's gesture closes the guide",
          "[nui][guide_session]") {
  NuiGuideSession session;
  REQUIRE(session.OnGestureCompleted(kPlayerId) ==
          NuiGuideSession::Change::kOpened);

  // Anybody else's gesture, a re-acquired player's new id included, is
  // ignored while the guide is up.
  CHECK(session.OnGestureCompleted(kOtherPlayerId) ==
        NuiGuideSession::Change::kNone);
  CHECK(session.open());
  CHECK(session.tracking_id() == kPlayerId);

  CHECK(session.OnGestureCompleted(kPlayerId) ==
        NuiGuideSession::Change::kClosed);
  CHECK_FALSE(session.open());
  CHECK(session.tracking_id() == 0);

  // Closed, the next gesture opens it again, for whoever made it.
  CHECK(session.OnGestureCompleted(kOtherPlayerId) ==
        NuiGuideSession::Change::kOpened);
  CHECK(session.tracking_id() == kOtherPlayerId);
}

TEST_CASE("A gesture without a tracking id does nothing",
          "[nui][guide_session]") {
  NuiGuideSession session;
  CHECK(session.OnGestureCompleted(0) == NuiGuideSession::Change::kNone);
  CHECK_FALSE(session.open());
}

TEST_CASE("Closing the guide reports whether it was open",
          "[nui][guide_session]") {
  NuiGuideSession session;
  CHECK_FALSE(session.Close());
  REQUIRE(session.OnGestureCompleted(kPlayerId) ==
          NuiGuideSession::Change::kOpened);
  CHECK(session.Close());
  CHECK_FALSE(session.open());
  CHECK(session.tracking_id() == 0);
  CHECK_FALSE(session.Close());
}

TEST_CASE("The open guide takes the frames that hold the engaged player",
          "[nui][guide_session]") {
  NuiGuideSession session;
  REQUIRE(session.OnGestureCompleted(kPlayerId) ==
          NuiGuideSession::Change::kOpened);

  CHECK(session.TakesFrame(*FrameWith(kPlayerId)));
  CHECK(session.TakesFrame(*FrameWith(kOtherPlayerId, kPlayerId)));
  // A frame without the engaged player is left to the title.
  CHECK_FALSE(session.TakesFrame(*FrameWith(kOtherPlayerId)));
  CHECK_FALSE(session.TakesFrame(*FrameWith(0)));

  // Closed, no frame is taken.
  REQUIRE(session.Close());
  CHECK_FALSE(session.TakesFrame(*FrameWith(kPlayerId)));
}

TEST_CASE("A completed gesture opens the guide, which takes the frame",
          "[nui][guide_session]") {
  NuiGuideSession session;
  NuiGuideGesture gesture{NuiGuideGestureSettings()};
  auto frame = EmptyFrame();
  PutPlayer(*frame, 2, kPlayerId, Pose::kGesture);

  // Before the hold completes, nothing changes and no frame is taken.
  for (uint64_t t = 0; t < kHoldUs - kFrameUs; t += kFrameUs) {
    const NuiGuideFrameResult result =
        AdvanceNuiGuide(session, &gesture, *frame, t);
    REQUIRE(result.change == NuiGuideSession::Change::kNone);
    REQUIRE_FALSE(result.takes);
  }

  uint64_t time_us = 0;
  const NuiGuideFrameResult result =
      FeedUntilChange(session, &gesture, *frame, kHoldUs - kFrameUs, &time_us);
  REQUIRE(result.change == NuiGuideSession::Change::kOpened);
  CHECK(time_us >= kHoldUs);
  CHECK(result.change_tracking_id == kPlayerId);
  CHECK(result.change_arm == NuiGuideArm::kLeft);
  CHECK(result.takes);
  CHECK(result.taken_tracking_id == kPlayerId);
  // The gesture's hand is the higher one, out to the left and low.
  CHECK(result.hand_valid);
  CHECK(result.hand_x == 0.0f);
  CHECK(result.hand_y == 1.0f);
  CHECK(session.open());
  CHECK(session.tracking_id() == kPlayerId);
}

TEST_CASE("Only tracked skeletons are fed to the recogniser",
          "[nui][guide_session]") {
  NuiGuideSession session;
  NuiGuideGesture gesture{NuiGuideGestureSettings()};
  auto frame = EmptyFrame();
  PutPlayer(*frame, 0, kPlayerId, Pose::kGesture, kPositionOnly);

  uint64_t time_us = 0;
  const NuiGuideFrameResult result =
      FeedUntilChange(session, &gesture, *frame, 0, &time_us);
  CHECK(result.change == NuiGuideSession::Change::kNone);
  CHECK_FALSE(session.open());
}

TEST_CASE("A frame changes the guide at most once", "[nui][guide_session]") {
  NuiGuideSession session;
  NuiGuideGesture gesture{NuiGuideGestureSettings()};
  auto frame = EmptyFrame();

  SECTION("The engaged player closes it; another's completion is ignored") {
    REQUIRE(session.OnGestureCompleted(kPlayerId) ==
            NuiGuideSession::Change::kOpened);
    // Both complete on the same frame, the other player in the earlier slot.
    PutPlayer(*frame, 0, kOtherPlayerId, Pose::kGesture);
    PutPlayer(*frame, 1, kPlayerId, Pose::kGesture);
    uint64_t time_us = 0;
    const NuiGuideFrameResult result =
        FeedUntilChange(session, &gesture, *frame, 0, &time_us);
    CHECK(result.change == NuiGuideSession::Change::kClosed);
    CHECK(result.change_tracking_id == kPlayerId);
    CHECK_FALSE(result.takes);
    CHECK_FALSE(session.open());
  }

  SECTION("The same, with the engaged player in the earlier slot") {
    REQUIRE(session.OnGestureCompleted(kPlayerId) ==
            NuiGuideSession::Change::kOpened);
    PutPlayer(*frame, 0, kPlayerId, Pose::kGesture);
    PutPlayer(*frame, 1, kOtherPlayerId, Pose::kGesture);
    uint64_t time_us = 0;
    const NuiGuideFrameResult result =
        FeedUntilChange(session, &gesture, *frame, 0, &time_us);
    // The other player's completion does not open it again.
    CHECK(result.change == NuiGuideSession::Change::kClosed);
    CHECK(result.change_tracking_id == kPlayerId);
    CHECK_FALSE(result.takes);
    CHECK_FALSE(session.open());
  }

  SECTION("Closed, the first completion in slot order opens it") {
    PutPlayer(*frame, 0, kOtherPlayerId, Pose::kGesture);
    PutPlayer(*frame, 1, kPlayerId, Pose::kGesture);
    uint64_t time_us = 0;
    const NuiGuideFrameResult result =
        FeedUntilChange(session, &gesture, *frame, 0, &time_us);
    CHECK(result.change == NuiGuideSession::Change::kOpened);
    CHECK(result.change_tracking_id == kOtherPlayerId);
    CHECK(result.takes);
    CHECK(result.taken_tracking_id == kOtherPlayerId);
    CHECK(session.tracking_id() == kOtherPlayerId);
  }

  // Either way, the next frames change nothing: both completions were used.
  uint64_t time_us = 0;
  const NuiGuideFrameResult next = FeedUntilChange(
      session, &gesture, *frame, 3 * kHoldUs + kFrameUs, &time_us);
  CHECK(next.change == NuiGuideSession::Change::kNone);
}

TEST_CASE("The result keeps the recogniser's trace of each tracked skeleton",
          "[nui][guide_session]") {
  NuiGuideSession session;
  NuiGuideGesture gesture{NuiGuideGestureSettings()};
  auto frame = EmptyFrame();
  PutPlayer(*frame, 1, kPlayerId, Pose::kGesture);
  PutPlayer(*frame, 2, kOtherPlayerId, Pose::kStanding, kPositionOnly);
  PutPlayer(*frame, 4, kOtherPlayerId + 1, Pose::kStanding);

  NuiGuideFrameResult result = AdvanceNuiGuide(session, &gesture, *frame, 0);
  REQUIRE(result.trace_count == 2);
  CHECK(result.traces[0].tracking_id == kPlayerId);
  CHECK(result.traces[0].arms[0].in_band);
  CHECK(result.traces[0].arms[0].hold_started);
  CHECK(result.traces[1].tracking_id == kOtherPlayerId + 1);
  CHECK_FALSE(result.traces[1].arms[0].in_band);

  // Without a recogniser nobody is fed, and there is nothing to trace.
  result = AdvanceNuiGuide(session, nullptr, *frame, kFrameUs);
  CHECK(result.trace_count == 0);
}

TEST_CASE("Without a recogniser a frame never changes the guide",
          "[nui][guide_session]") {
  NuiGuideSession session;
  auto frame = EmptyFrame();
  PutPlayer(*frame, 0, kPlayerId, Pose::kGesture);

  uint64_t time_us = 0;
  NuiGuideFrameResult result =
      FeedUntilChange(session, nullptr, *frame, 0, &time_us);
  CHECK(result.change == NuiGuideSession::Change::kNone);
  CHECK_FALSE(result.takes);

  // An open guide still takes the engaged player's frames.
  REQUIRE(session.OnGestureCompleted(kPlayerId) ==
          NuiGuideSession::Change::kOpened);
  result = FeedUntilChange(session, nullptr, *frame, 0, &time_us);
  CHECK(result.change == NuiGuideSession::Change::kNone);
  CHECK(result.takes);
  CHECK(result.taken_tracking_id == kPlayerId);
  CHECK(session.open());
}

TEST_CASE("The hand is read from the engaged player's tracked slot",
          "[nui][guide_session]") {
  NuiGuideSession session;
  REQUIRE(session.OnGestureCompleted(kPlayerId) ==
          NuiGuideSession::Change::kOpened);
  auto frame = EmptyFrame();
  // Another player holds the gesture in an earlier slot; the engaged one
  // stands with both hands hanging at the same height, where the left one
  // steers.
  PutPlayer(*frame, 0, kOtherPlayerId, Pose::kGesture);
  PutPlayer(*frame, 3, kPlayerId, Pose::kStanding);

  NuiGuideFrameResult result = AdvanceNuiGuide(session, nullptr, *frame, 0);
  CHECK(result.takes);
  REQUIRE(result.hand_valid);
  // The hanging left hand is 0.05 m inside the box's left edge.
  CHECK(result.hand_x == Approx(0.1f));
  CHECK(result.hand_y == 1.0f);

  // A position-only slot is still the engaged player, but shows no hands.
  frame->skeletons[3].tracking_state = kPositionOnly;
  result = AdvanceNuiGuide(session, nullptr, *frame, kFrameUs);
  CHECK(result.takes);
  CHECK_FALSE(result.hand_valid);

  // A frame without the engaged player is neither taken nor read.
  frame->skeletons[3].tracking_state = 0;
  result = AdvanceNuiGuide(session, nullptr, *frame, 2 * kFrameUs);
  CHECK_FALSE(result.takes);
  CHECK_FALSE(result.hand_valid);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
