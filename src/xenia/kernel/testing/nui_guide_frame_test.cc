/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_guide_frame.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {
namespace {

// These are the joints of the public NUI skeleton layout the tests place.
constexpr size_t kShoulderCenter = 2;
constexpr size_t kHandLeft = 7;
constexpr size_t kHandRight = 11;

constexpr uint32_t kNotTracked = 0;
constexpr uint32_t kPositionOnly = 1;
constexpr uint32_t kInferred = 1;
constexpr uint32_t kTracked = 2;

// Returns a zeroed frame on the heap: it is 2784 bytes long.
std::unique_ptr<NuiMsg2B002> EmptyFrame() {
  auto frame = std::make_unique<NuiMsg2B002>();
  std::memset(frame.get(), 0, sizeof(NuiMsg2B002));
  return frame;
}

// Returns a skeleton with every joint tracked at the origin, the shoulder
// centre at (0.1, 0.45, 2.5) metres and both hands hanging at hip height.
NuiGuideSkeleton StandingSkeleton() {
  NuiGuideSkeleton skeleton;
  skeleton.joints.fill({0.0f, 0.0f, 2.5f});
  skeleton.tracking_states.fill(kTracked);
  skeleton.joints[kShoulderCenter] = {0.1f, 0.45f, 2.5f};
  skeleton.joints[kHandLeft] = {-0.15f, -0.1f, 2.5f};
  skeleton.joints[kHandRight] = {0.35f, -0.1f, 2.5f};
  return skeleton;
}

}  // namespace

TEST_CASE("A skeleton slot is read into host values", "[nui][guide_frame]") {
  auto frame = EmptyFrame();
  X_NUI_SKELETON_DATA& slot = frame->skeletons[3];
  slot.skeleton_positions[kHandRight].x = 0.25f;
  slot.skeleton_positions[kHandRight].y = -0.5f;
  slot.skeleton_positions[kHandRight].z = 2.0f;
  slot.skeleton_positions[kHandRight].w = 1.0f;
  slot.position_tracking_state[kHandRight] = kTracked;
  slot.position_tracking_state[kHandLeft] = kInferred;

  const NuiGuideSkeleton skeleton = ReadNuiGuideSkeleton(slot);

  CHECK(skeleton.joints[kHandRight].x == 0.25f);
  CHECK(skeleton.joints[kHandRight].y == -0.5f);
  CHECK(skeleton.joints[kHandRight].z == 2.0f);
  CHECK(skeleton.tracking_states[kHandRight] == kTracked);
  CHECK(skeleton.tracking_states[kHandLeft] == kInferred);
  CHECK(skeleton.tracking_states[kShoulderCenter] == kNotTracked);
}

TEST_CASE("Gravity is the reverse of the floor plane's normal",
          "[nui][guide_frame]") {
  auto frame = EmptyFrame();
  // The floor under a sensor pitched about 9 degrees down, 1.3 m below it.
  frame->floor_clip_plane.x = 0.0f;
  frame->floor_clip_plane.y = 0.98768834f;
  frame->floor_clip_plane.z = -0.15643447f;
  frame->floor_clip_plane.w = 1.3f;

  const NuiGuideVector gravity = NuiGuideGravityAxis(*frame);

  CHECK(gravity.x == Approx(0.0f));
  CHECK(gravity.y == Approx(-0.98768834f));
  CHECK(gravity.z == Approx(0.15643447f));
}

TEST_CASE("Without a floor, gravity is straight down the camera's y axis",
          "[nui][guide_frame]") {
  auto frame = EmptyFrame();
  NuiGuideVector gravity = NuiGuideGravityAxis(*frame);
  CHECK(gravity.x == 0.0f);
  CHECK(gravity.y == -1.0f);
  CHECK(gravity.z == 0.0f);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  frame->floor_clip_plane.x = nan;
  frame->floor_clip_plane.y = nan;
  frame->floor_clip_plane.z = nan;
  gravity = NuiGuideGravityAxis(*frame);
  CHECK(gravity.x == 0.0f);
  CHECK(gravity.y == -1.0f);
  CHECK(gravity.z == 0.0f);
}

TEST_CASE("A frame holds a tracking id in a tracked or position-only slot",
          "[nui][guide_frame]") {
  auto frame = EmptyFrame();
  frame->skeletons[2].tracking_state = kTracked;
  frame->skeletons[2].tracking_id = 66;
  frame->skeletons[4].tracking_state = kPositionOnly;
  frame->skeletons[4].tracking_id = 67;
  // A slot that is not tracked can still carry a stale id.
  frame->skeletons[5].tracking_state = kNotTracked;
  frame->skeletons[5].tracking_id = 68;

  CHECK(NuiGuideFrameHoldsTrackingId(*frame, 66));
  CHECK(NuiGuideFrameHoldsTrackingId(*frame, 67));
  CHECK_FALSE(NuiGuideFrameHoldsTrackingId(*frame, 68));
  CHECK_FALSE(NuiGuideFrameHoldsTrackingId(*frame, 69));
  CHECK_FALSE(NuiGuideFrameHoldsTrackingId(*frame, 0));
}

TEST_CASE("The higher hand is mapped over the box in front of the shoulders",
          "[nui][guide_frame]") {
  NuiGuideSkeleton skeleton = StandingSkeleton();
  float x = -1.0f;
  float y = -1.0f;

  // At the shoulder centre's height and across from it: the middle.
  skeleton.joints[kHandRight] = {0.1f, 0.45f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(0.5f));
  CHECK(y == Approx(0.5f));

  // The box is 0.5 m wide and 0.4 m high: its top left and bottom right.
  skeleton.joints[kHandRight] = {0.1f - 0.25f, 0.45f + 0.2f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(0.0f).margin(1e-5));
  CHECK(y == Approx(0.0f).margin(1e-5));
  skeleton.joints[kHandRight] = {0.1f + 0.25f, 0.45f - 0.2f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(1.0f));
  CHECK(y == Approx(1.0f));

  // A quarter of the way across and three quarters of the way down.
  skeleton.joints[kHandRight] = {0.1f - 0.125f, 0.45f - 0.1f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(0.25f));
  CHECK(y == Approx(0.75f));
}

TEST_CASE("A hand outside the box is held at its edge", "[nui][guide_frame]") {
  NuiGuideSkeleton skeleton = StandingSkeleton();
  float x = -1.0f;
  float y = -1.0f;
  skeleton.joints[kHandRight] = {1.0f, 1.5f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == 1.0f);
  CHECK(y == 0.0f);
  skeleton.joints[kHandLeft] = {-1.0f, 2.0f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == 0.0f);
  CHECK(y == 0.0f);
}

TEST_CASE("The hand that steers is the higher one that can be seen",
          "[nui][guide_frame]") {
  NuiGuideSkeleton skeleton = StandingSkeleton();
  float x = -1.0f;
  float y = -1.0f;

  // The left hand raised in front of the shoulders, the right one hanging.
  skeleton.joints[kHandLeft] = {0.1f - 0.125f, 0.45f, 2.2f};
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(0.25f));
  CHECK(y == Approx(0.5f));

  // Inferred is good enough.
  skeleton.tracking_states[kHandLeft] = kInferred;
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(0.25f));

  // A hand that is not tracked at all does not count, however high it reads:
  // the hanging right hand steers instead.
  skeleton.tracking_states[kHandLeft] = kNotTracked;
  REQUIRE(MapNuiGuideHand(skeleton, &x, &y));
  CHECK(x == Approx(1.0f));
  CHECK(y == Approx(1.0f));

  // Neither hand, or no shoulder centre, and there is nothing to map.
  skeleton.tracking_states[kHandRight] = kNotTracked;
  CHECK_FALSE(MapNuiGuideHand(skeleton, &x, &y));
  skeleton = StandingSkeleton();
  skeleton.tracking_states[kShoulderCenter] = kNotTracked;
  CHECK_FALSE(MapNuiGuideHand(skeleton, &x, &y));

  // A hand whose position is not a number does not count either.
  skeleton = StandingSkeleton();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  skeleton.joints[kHandLeft] = {nan, nan, nan};
  skeleton.joints[kHandRight] = {nan, nan, nan};
  CHECK_FALSE(MapNuiGuideHand(skeleton, &x, &y));
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
