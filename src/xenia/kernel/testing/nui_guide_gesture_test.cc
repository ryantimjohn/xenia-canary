/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_guide_gesture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "xenia/nui/nui_guide_defaults.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {
namespace {

// This is the public NUI skeleton layout, written out here rather than taken
// from the recogniser so that a wrong index there shows up as a failing test.
enum Joint : size_t {
  kHipCenter = 0,
  kSpine = 1,
  kShoulderCenter = 2,
  kHead = 3,
  kShoulderLeft = 4,
  kElbowLeft = 5,
  kWristLeft = 6,
  kHandLeft = 7,
  kShoulderRight = 8,
  kElbowRight = 9,
  kWristRight = 10,
  kHandRight = 11,
  kHipLeft = 12,
  kKneeLeft = 13,
  kAnkleLeft = 14,
  kFootLeft = 15,
  kHipRight = 16,
  kKneeRight = 17,
  kAnkleRight = 18,
  kFootRight = 19,
};

constexpr uint32_t kNotTracked = 0;
constexpr uint32_t kInferred = 1;
constexpr uint32_t kTracked = 2;

// These are the defaults of the gesture's cvars.
constexpr float kAngleMinDegrees = 30.0f;
constexpr float kAngleMaxDegrees = 60.0f;
constexpr float kRestAngleMaxDegrees = 20.0f;
constexpr uint64_t kHoldUs = 1000000;
constexpr uint64_t kGapUs = 200000;

// The sensor delivers 30 frames a second.
constexpr uint64_t kFrameUs = 33333;

constexpr uint32_t kPlayerId = 1;
constexpr uint32_t kOtherPlayerId = 2;

// Straight down in the sensor's camera space is -y, since y grows upwards.
constexpr NuiGuideVector kCameraDown = {0.0f, -1.0f, 0.0f};

// These are the player's measurements, in metres. Both arm segments are the
// same length so that a bent arm can be built from an isosceles triangle.
constexpr float kBodyDepth = 2.5f;
constexpr float kShoulderHeight = 0.45f;
constexpr float kShoulderHalfWidth = 0.2f;
constexpr float kArmSegmentLength = 0.275f;
constexpr float kArmLength = 2.0f * kArmSegmentLength;
// The wrist sits this far along the way from the elbow to the hand.
constexpr float kWristAlongForearm = 0.8f;

enum class Side { kLeft, kRight };

struct TestSkeleton {
  std::array<NuiGuideVector, kNuiGuideJointCount> joints;
  std::array<uint32_t, kNuiGuideJointCount> states;
};

NuiGuideVector Add(const NuiGuideVector& a, const NuiGuideVector& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

NuiGuideVector Scale(const NuiGuideVector& v, float factor) {
  return {v.x * factor, v.y * factor, v.z * factor};
}

float Dot(const NuiGuideVector& a, const NuiGuideVector& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Radians(float degrees) {
  return degrees * std::numbers::pi_v<float> / 180.0f;
}

NuiGuideGestureSettings DefaultSettings() {
  NuiGuideGestureSettings settings;
  settings.angle_min_degrees = kAngleMinDegrees;
  settings.angle_max_degrees = kAngleMaxDegrees;
  settings.arms = NuiGuideArmChoice::kLeft;
  settings.rest_angle_max_degrees = kRestAngleMaxDegrees;
  settings.hold_us = kHoldUs;
  settings.gap_us = kGapUs;
  return settings;
}

// The defaults, with either arm allowed to make the gesture.
NuiGuideGestureSettings EitherArmSettings() {
  NuiGuideGestureSettings settings = DefaultSettings();
  settings.arms = NuiGuideArmChoice::kEither;
  return settings;
}

// Puts one arm at angle_degrees from straight down, swung forward_degrees from
// the body's side towards the sensor, with the hand reach times the arm's full
// length from the shoulder. When the reach is short of 1 the elbow bends
// towards the sensor, keeping both segments their length.
void PlaceArm(TestSkeleton& skeleton, Side side, float angle_degrees,
              float forward_degrees = 0.0f, float reach = 1.0f) {
  const bool left = side == Side::kLeft;
  const size_t shoulder = left ? kShoulderLeft : kShoulderRight;
  const size_t elbow = left ? kElbowLeft : kElbowRight;
  const size_t wrist = left ? kWristLeft : kWristRight;
  const size_t hand = left ? kHandLeft : kHandRight;

  // The player faces the sensor, so their front is towards -z, and their
  // right is towards +x.
  const float outward = left ? -1.0f : 1.0f;
  const float angle = Radians(angle_degrees);
  const float forward = Radians(forward_degrees);
  const NuiGuideVector direction = {
      outward * std::sin(angle) * std::cos(forward), -std::cos(angle),
      -std::sin(angle) * std::sin(forward)};

  // The elbow bends along the unit vector perpendicular to the arm that
  // points most towards the sensor.
  const NuiGuideVector towards_sensor = {0.0f, 0.0f, -1.0f};
  NuiGuideVector bend =
      Add(towards_sensor, Scale(direction, -Dot(towards_sensor, direction)));
  const float bend_length = std::sqrt(Dot(bend, bend));
  bend = bend_length > 1e-6f ? Scale(bend, 1.0f / bend_length)
                             : NuiGuideVector{0.0f, 1.0f, 0.0f};

  const float half_reach = reach * kArmLength / 2.0f;
  const float bend_depth = std::sqrt(std::max(
      0.0f, kArmSegmentLength * kArmSegmentLength - half_reach * half_reach));

  const NuiGuideVector shoulder_position = skeleton.joints[shoulder];
  const NuiGuideVector elbow_position =
      Add(Add(shoulder_position, Scale(direction, half_reach)),
          Scale(bend, bend_depth));
  const NuiGuideVector hand_position =
      Add(shoulder_position, Scale(direction, 2.0f * half_reach));
  skeleton.joints[elbow] = elbow_position;
  skeleton.joints[hand] = hand_position;
  skeleton.joints[wrist] = Add(
      elbow_position, Scale(Add(hand_position, Scale(elbow_position, -1.0f)),
                            kWristAlongForearm));
}

// Returns a player standing 2.5 m in front of the sensor, facing it, every
// joint tracked, with both arms hanging straight down. The hanging hands are
// below the hip centre.
TestSkeleton StandingSkeleton() {
  TestSkeleton skeleton;
  auto place = [&skeleton](size_t joint, float x, float y) {
    skeleton.joints[joint] = {x, y, kBodyDepth};
  };
  place(kHipCenter, 0.0f, 0.0f);
  place(kSpine, 0.0f, 0.2f);
  place(kShoulderCenter, 0.0f, kShoulderHeight);
  place(kHead, 0.0f, 0.65f);
  place(kShoulderLeft, -kShoulderHalfWidth, kShoulderHeight);
  place(kShoulderRight, kShoulderHalfWidth, kShoulderHeight);
  place(kHipLeft, -0.1f, -0.05f);
  place(kKneeLeft, -0.1f, -0.45f);
  place(kAnkleLeft, -0.1f, -0.85f);
  place(kFootLeft, -0.1f, -0.9f);
  place(kHipRight, 0.1f, -0.05f);
  place(kKneeRight, 0.1f, -0.45f);
  place(kAnkleRight, 0.1f, -0.85f);
  place(kFootRight, 0.1f, -0.9f);
  skeleton.states.fill(kTracked);
  PlaceArm(skeleton, Side::kLeft, 0.0f);
  PlaceArm(skeleton, Side::kRight, 0.0f);
  return skeleton;
}

// Returns the standing player with one arm held out and down at
// angle_degrees.
TestSkeleton ArmOut(Side side, float angle_degrees,
                    float forward_degrees = 0.0f, float reach = 1.0f) {
  TestSkeleton skeleton = StandingSkeleton();
  PlaceArm(skeleton, side, angle_degrees, forward_degrees, reach);
  return skeleton;
}

// Returns the standing player with the left arm out at angle_degrees and its
// hand reach_offset metres further from the shoulder, along the arm, than the
// arm is long: the tracker's hand joint jitters so along an arm held still.
TestSkeleton JitteredArm(float angle_degrees, float reach_offset) {
  TestSkeleton skeleton = ArmOut(Side::kLeft, angle_degrees);
  const NuiGuideVector shoulder = skeleton.joints[kShoulderLeft];
  const NuiGuideVector reach =
      Add(skeleton.joints[kHandLeft], Scale(shoulder, -1.0f));
  skeleton.joints[kHandLeft] =
      Add(shoulder, Scale(reach, (kArmLength + reach_offset) / kArmLength));
  return skeleton;
}

// Returns the skeleton with every joint moved by offset, as when the player
// steps.
TestSkeleton Shifted(const TestSkeleton& skeleton,
                     const NuiGuideVector& offset) {
  TestSkeleton shifted = skeleton;
  for (NuiGuideVector& joint : shifted.joints) {
    joint = Add(joint, offset);
  }
  return shifted;
}

// Turns a vector by degrees about the camera's z axis.
NuiGuideVector Roll(const NuiGuideVector& v, float degrees) {
  const float c = std::cos(Radians(degrees));
  const float s = std::sin(Radians(degrees));
  return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

// Turns the whole skeleton by degrees about the camera's z axis through its hip
// centre, the way a sensor rolled on its base sees an upright player.
TestSkeleton Rolled(const TestSkeleton& skeleton, float degrees) {
  TestSkeleton rolled = skeleton;
  const NuiGuideVector hip = skeleton.joints[kHipCenter];
  for (size_t i = 0; i < kNuiGuideJointCount; ++i) {
    rolled.joints[i] =
        Add(hip, Roll(Add(skeleton.joints[i], Scale(hip, -1.0f)), degrees));
  }
  return rolled;
}

bool Feed(NuiGuideGesture& gesture, const TestSkeleton& skeleton,
          uint64_t timestamp_us, uint32_t tracking_id = kPlayerId,
          const NuiGuideVector& gravity_axis = kCameraDown) {
  return gesture.Update(tracking_id, skeleton.joints, skeleton.states,
                        gravity_axis, timestamp_us);
}

// Feeds the skeleton on every frame from start_us up to but not including
// end_us and returns the timestamps of the frames that completed the gesture.
std::vector<uint64_t> FireTimes(
    NuiGuideGesture& gesture, const TestSkeleton& skeleton, uint64_t start_us,
    uint64_t end_us, uint32_t tracking_id = kPlayerId,
    const NuiGuideVector& gravity_axis = kCameraDown) {
  std::vector<uint64_t> fire_times;
  for (uint64_t t = start_us; t < end_us; t += kFrameUs) {
    if (Feed(gesture, skeleton, t, tracking_id, gravity_axis)) {
      fire_times.push_back(t);
    }
  }
  return fire_times;
}

// Returns whether a fresh recogniser with the settings completes the gesture
// on the skeleton held for three hold times.
bool FiresWith(const NuiGuideGestureSettings& settings,
               const TestSkeleton& skeleton,
               const NuiGuideVector& gravity_axis = kCameraDown) {
  NuiGuideGesture gesture(settings);
  return !FireTimes(gesture, skeleton, 0, 3 * kHoldUs, kPlayerId, gravity_axis)
              .empty();
}

// Fires does the same with the default settings.
bool Fires(const TestSkeleton& skeleton,
           const NuiGuideVector& gravity_axis = kCameraDown) {
  return FiresWith(DefaultSettings(), skeleton, gravity_axis);
}

// A fired arm is taken away at kAwayStartUs, after the gesture has completed
// once, and brought back at kBackUs, twice the gap tolerance later.
constexpr uint64_t kAwayStartUs = 3 * kHoldUs;
constexpr uint64_t kBackUs = kAwayStartUs + 2 * kGapUs;

// Holds the left arm out until kAwayStartUs, which completes the gesture once,
// feeds away from then until kBackUs, and holds the arm out again for three
// hold times. A null away feeds nothing in between, as when the skeleton drops
// out of the feed. Returns the timestamps of every completion.
std::vector<uint64_t> FireTimesAcrossAway(
    const TestSkeleton* away,
    const NuiGuideVector& away_gravity_axis = kCameraDown) {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  std::vector<uint64_t> fire_times = FireTimes(gesture, held, 0, kAwayStartUs);
  if (away) {
    const std::vector<uint64_t> away_fire_times = FireTimes(
        gesture, *away, kAwayStartUs, kBackUs, kPlayerId, away_gravity_axis);
    fire_times.insert(fire_times.end(), away_fire_times.begin(),
                      away_fire_times.end());
  }
  const std::vector<uint64_t> back_fire_times =
      FireTimes(gesture, held, kBackUs, kBackUs + 3 * kHoldUs);
  fire_times.insert(fire_times.end(), back_fire_times.begin(),
                    back_fire_times.end());
  return fire_times;
}

// Returns whether the gesture completed on the first frame at or after the
// hold time counted from hold_start_us.
bool FiredOnTime(uint64_t fire_time_us, uint64_t hold_start_us) {
  return fire_time_us >= hold_start_us + kHoldUs &&
         fire_time_us < hold_start_us + kHoldUs + kFrameUs;
}

// Returns the first frame of a run fed every frame period from start_us that
// comes more than the gap tolerance after last_us: the frame that ends a hold
// whose last qualifying frame came at last_us.
uint64_t FirstFrameAfterGap(uint64_t start_us, uint64_t last_us) {
  uint64_t t = start_us;
  while (t - last_us <= kGapUs) {
    t += kFrameUs;
  }
  return t;
}

// Returns the last frame fed before end_us by a run from 0.
uint64_t LastFrameBefore(uint64_t end_us) {
  return (end_us - 1) / kFrameUs * kFrameUs;
}

}  // namespace

TEST_CASE("An arm held out and down at 45 degrees fires after the hold time",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  CHECK(gesture.last_fired_arm() == NuiGuideArm::kNone);

  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, ArmOut(Side::kLeft, 45.0f), 0, 3 * kHoldUs);

  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
  CHECK(gesture.last_fired_arm() == NuiGuideArm::kLeft);
}

TEST_CASE("The hold completes on the first frame at or after the hold time",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton skeleton = ArmOut(Side::kLeft, 45.0f);
  constexpr uint64_t kStepUs = 100000;
  for (uint64_t t = 0; t < kHoldUs; t += kStepUs) {
    INFO("t = " << t);
    CHECK_FALSE(Feed(gesture, skeleton, t));
  }
  CHECK_FALSE(Feed(gesture, skeleton, kHoldUs - 1));
  CHECK(Feed(gesture, skeleton, kHoldUs));
}

TEST_CASE("A straight-down arm does not fire", "[nui][guide_gesture]") {
  CHECK_FALSE(Fires(StandingSkeleton()));
  CHECK_FALSE(Fires(ArmOut(Side::kLeft, 0.0f)));
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f)));
}

TEST_CASE("Only arms inside the angle band fire", "[nui][guide_gesture]") {
  for (float angle : {15.0f, 65.0f, 80.0f, 90.0f}) {
    INFO("angle " << angle);
    CHECK_FALSE(Fires(ArmOut(Side::kLeft, angle)));
  }
  for (float angle : {35.0f, 45.0f, 55.0f}) {
    INFO("angle " << angle);
    CHECK(Fires(ArmOut(Side::kLeft, angle)));
  }
}

TEST_CASE("The angle band comes from the settings", "[nui][guide_gesture]") {
  NuiGuideGestureSettings settings = DefaultSettings();
  settings.angle_min_degrees = 40.0f;
  settings.angle_max_degrees = 50.0f;
  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kLeft, 35.0f)));
  CHECK(FiresWith(settings, ArmOut(Side::kLeft, 45.0f)));
  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kLeft, 55.0f)));
}

TEST_CASE("An uneven band is measured from straight down",
          "[nui][guide_gesture]") {
  // A band that is not centred on 45 degrees tells an angle measured from
  // straight down apart from one measured from the horizontal.
  NuiGuideGestureSettings settings = DefaultSettings();
  settings.angle_min_degrees = 35.0f;
  settings.angle_max_degrees = 50.0f;
  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kLeft, 33.0f)));
  CHECK(FiresWith(settings, ArmOut(Side::kLeft, 37.0f)));
  CHECK(FiresWith(settings, ArmOut(Side::kLeft, 48.0f)));
  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kLeft, 52.0f)));
}

TEST_CASE("The hold time comes from the settings", "[nui][guide_gesture]") {
  NuiGuideGestureSettings settings = DefaultSettings();
  settings.hold_us = 2 * kHoldUs;
  NuiGuideGesture gesture(settings);
  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, ArmOut(Side::kLeft, 45.0f), 0, 4 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(fire_times[0] >= 2 * kHoldUs);
  CHECK(fire_times[0] < 2 * kHoldUs + kFrameUs);
}

TEST_CASE("An arm pointed at the sensor does not fire",
          "[nui][guide_gesture]") {
  // Seen from above, an arm swung fully forward points at the sensor, and one
  // swung 70 degrees forward points closer to the sensor than to the side.
  // The right arm is checked with the settings that let it make the gesture.
  const NuiGuideGestureSettings either = EitherArmSettings();
  CHECK_FALSE(Fires(ArmOut(Side::kLeft, 45.0f, 90.0f)));
  CHECK_FALSE(Fires(ArmOut(Side::kLeft, 45.0f, 70.0f)));
  CHECK_FALSE(FiresWith(either, ArmOut(Side::kRight, 45.0f, 90.0f)));
  CHECK_FALSE(FiresWith(either, ArmOut(Side::kRight, 45.0f, 70.0f)));
  // An arm swung 30 degrees forward is still out to the side.
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f, 30.0f)));
  CHECK(FiresWith(either, ArmOut(Side::kRight, 45.0f, 30.0f)));
}

TEST_CASE("The side rule leaves the angle band alone", "[nui][guide_gesture]") {
  // An arm low in the band and swung a little forward, the way many people
  // raise an arm to the side, counts.
  CHECK(Fires(ArmOut(Side::kLeft, 32.0f, 25.0f)));
  CHECK(FiresWith(EitherArmSettings(), ArmOut(Side::kRight, 32.0f, 25.0f)));

  // A band that reaches below 30 degrees is not cut short by the side rule.
  NuiGuideGestureSettings settings = DefaultSettings();
  settings.angle_min_degrees = 20.0f;
  CHECK(FiresWith(settings, ArmOut(Side::kLeft, 25.0f)));
  CHECK_FALSE(Fires(ArmOut(Side::kLeft, 25.0f)));
}

TEST_CASE("A bent arm does not fire", "[nui][guide_gesture]") {
  CHECK_FALSE(Fires(ArmOut(Side::kLeft, 45.0f, 0.0f, 0.8f)));
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f, 0.0f, 0.95f)));
}

TEST_CASE("A gap longer than the tolerance resets the hold",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  constexpr uint64_t kGapStartUs = 600000;
  // The last frame before the gap is at 566661 us, so resuming at 900000 us
  // leaves a gap of about 333 ms.
  constexpr uint64_t kResumeUs = 900000;

  SECTION("Missing frames") {
    CHECK(FireTimes(gesture, held, 0, kGapStartUs).empty());
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, held, kResumeUs, kResumeUs + 2 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], kResumeUs));
  }

  SECTION("Frames outside the band") {
    CHECK(FireTimes(gesture, held, 0, kGapStartUs).empty());
    CHECK(
        FireTimes(gesture, StandingSkeleton(), kGapStartUs, kResumeUs).empty());
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, held, kResumeUs, kResumeUs + 2 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], kResumeUs));
  }
}

TEST_CASE("A gap within the tolerance keeps the hold", "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  constexpr uint64_t kGapStartUs = 600000;
  // The last frame before the gap is at 566661 us, so resuming at 750000 us
  // leaves a gap of about 183 ms.
  constexpr uint64_t kResumeUs = 750000;

  SECTION("Missing frames") {
    CHECK(FireTimes(gesture, held, 0, kGapStartUs).empty());
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, held, kResumeUs, 3 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], 0));
  }

  SECTION("Frames outside the band") {
    CHECK(FireTimes(gesture, held, 0, kGapStartUs).empty());
    CHECK(
        FireTimes(gesture, StandingSkeleton(), kGapStartUs, kResumeUs).empty());
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, held, kResumeUs, 3 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], 0));
  }
}

TEST_CASE("An arm joint that is not tracked does not count",
          "[nui][guide_gesture]") {
  for (size_t joint : {kShoulderLeft, kElbowLeft, kHandLeft}) {
    for (uint32_t state : {kNotTracked, kInferred}) {
      INFO("joint " << joint << " state " << state);
      TestSkeleton skeleton = ArmOut(Side::kLeft, 45.0f);
      skeleton.states[joint] = state;
      CHECK_FALSE(Fires(skeleton));
    }
  }
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f)));
}

TEST_CASE(
    "The other hand, the other shoulder and the hip centre may be inferred "
    "but not missing",
    "[nui][guide_gesture]") {
  for (size_t joint : {kHandRight, kShoulderRight, kHipCenter}) {
    INFO("joint " << joint);
    TestSkeleton skeleton = ArmOut(Side::kLeft, 45.0f);
    skeleton.states[joint] = kInferred;
    CHECK(Fires(skeleton));
    skeleton.states[joint] = kNotTracked;
    CHECK_FALSE(Fires(skeleton));
  }
}

TEST_CASE("After firing, the pose does not fire again until the arm leaves",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  const TestSkeleton lowered = StandingSkeleton();

  REQUIRE(FireTimes(gesture, held, 0, 3 * kHoldUs).size() == 1);
  // Holding on does not fire again.
  CHECK(FireTimes(gesture, held, 3 * kHoldUs, 6 * kHoldUs).empty());
  // Nor does dropping the arm for less than the gap tolerance.
  constexpr uint64_t kShortDropEndUs = 6 * kHoldUs + 100000;
  CHECK(FireTimes(gesture, lowered, 6 * kHoldUs, kShortDropEndUs).empty());
  CHECK(FireTimes(gesture, held, kShortDropEndUs, 9 * kHoldUs).empty());
  // Dropping it for longer re-arms it, and the next hold fires.
  constexpr uint64_t kLongDropEndUs = 9 * kHoldUs + 400000;
  CHECK(FireTimes(gesture, lowered, 9 * kHoldUs, kLongDropEndUs).empty());
  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, held, kLongDropEndUs, kLongDropEndUs + 3 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], kLongDropEndUs));
}

TEST_CASE("Raising the other hand does not re-arm a fired arm",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  TestSkeleton other_hand_raised = held;
  PlaceArm(other_hand_raised, Side::kRight, 90.0f, 90.0f);

  REQUIRE(FireTimes(gesture, held, 0, 3 * kHoldUs).size() == 1);
  CHECK(
      FireTimes(gesture, other_hand_raised, 3 * kHoldUs, 4 * kHoldUs).empty());
  CHECK(FireTimes(gesture, held, 4 * kHoldUs, 7 * kHoldUs).empty());
}

TEST_CASE("A held pose does not re-arm while a joint is only inferred",
          "[nui][guide_gesture]") {
  TestSkeleton hand_inferred = ArmOut(Side::kLeft, 45.0f);
  hand_inferred.states[kHandLeft] = kInferred;
  const std::vector<uint64_t> fire_times = FireTimesAcrossAway(&hand_inferred);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
}

TEST_CASE("A held pose does not re-arm while the skeleton is missing",
          "[nui][guide_gesture]") {
  const std::vector<uint64_t> fire_times = FireTimesAcrossAway(nullptr);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
}

TEST_CASE("A lowered arm re-arms with its hand inferred",
          "[nui][guide_gesture]") {
  TestSkeleton lowered = StandingSkeleton();
  lowered.states[kHandLeft] = kInferred;
  const std::vector<uint64_t> fire_times = FireTimesAcrossAway(&lowered);
  REQUIRE(fire_times.size() == 2);
  CHECK(FiredOnTime(fire_times[1], kBackUs));
}

TEST_CASE("A lowered arm that cannot be seen does not re-arm",
          "[nui][guide_gesture]") {
  for (size_t joint : {kShoulderLeft, kElbowLeft, kHandLeft, kShoulderRight}) {
    INFO("joint " << joint);
    TestSkeleton lowered = StandingSkeleton();
    lowered.states[joint] = kNotTracked;
    CHECK(FireTimesAcrossAway(&lowered).size() == 1);
  }
  const TestSkeleton lowered = StandingSkeleton();
  CHECK(
      FireTimesAcrossAway(&lowered, NuiGuideVector{0.0f, 0.0f, 0.0f}).size() ==
      1);
  // Seen, the same lowered arm re-arms.
  CHECK(FireTimesAcrossAway(&lowered).size() == 2);
}

TEST_CASE("Short drops with the pose between them do not add up",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  const TestSkeleton lowered = StandingSkeleton();
  // Each drop is shorter than the gap tolerance; the two together are longer.
  constexpr uint64_t kFirstDropEndUs = kAwayStartUs + 150000;
  constexpr uint64_t kSecondDropStartUs = kAwayStartUs + 250000;
  constexpr uint64_t kSecondDropEndUs = kAwayStartUs + 400000;

  REQUIRE(FireTimes(gesture, held, 0, kAwayStartUs).size() == 1);
  CHECK(FireTimes(gesture, lowered, kAwayStartUs, kFirstDropEndUs).empty());
  CHECK(FireTimes(gesture, held, kFirstDropEndUs, kSecondDropStartUs).empty());
  CHECK(FireTimes(gesture, lowered, kSecondDropStartUs, kSecondDropEndUs)
            .empty());
  CHECK(
      FireTimes(gesture, held, kSecondDropEndUs, kSecondDropEndUs + 3 * kHoldUs)
          .empty());
}

TEST_CASE("With either arm allowed, the right arm works like the left",
          "[nui][guide_gesture]") {
  const NuiGuideGestureSettings settings = EitherArmSettings();
  NuiGuideGesture gesture(settings);
  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, ArmOut(Side::kRight, 45.0f), 0, 3 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
  CHECK(gesture.last_fired_arm() == NuiGuideArm::kRight);

  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kRight, 0.0f)));
  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kRight, 15.0f)));
  CHECK_FALSE(FiresWith(settings, ArmOut(Side::kRight, 80.0f)));
  CHECK(FiresWith(settings, ArmOut(Side::kRight, 35.0f)));
  CHECK(FiresWith(settings, ArmOut(Side::kRight, 55.0f)));
}

TEST_CASE("A raised other hand blocks the gesture", "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  // The other hand raised in front of the shoulder, the way a player steers
  // the cursor.
  TestSkeleton other_hand_raised = held;
  PlaceArm(other_hand_raised, Side::kRight, 90.0f, 90.0f);

  CHECK(FireTimes(gesture, other_hand_raised, 0, 3 * kHoldUs).empty());
  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, held, 3 * kHoldUs, 6 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 3 * kHoldUs));
}

TEST_CASE("Both arms held out together do not fire", "[nui][guide_gesture]") {
  // Both arms out and down, low enough that each hand is below the hip centre:
  // either arm on its own would count, but the pair is not the gesture.
  TestSkeleton both_out = ArmOut(Side::kLeft, 32.0f);
  PlaceArm(both_out, Side::kRight, 32.0f);
  CHECK_FALSE(Fires(both_out));
  CHECK_FALSE(FiresWith(EitherArmSettings(), both_out));
  CHECK(Fires(ArmOut(Side::kLeft, 32.0f)));
  CHECK(FiresWith(EitherArmSettings(), ArmOut(Side::kRight, 32.0f)));

  // The other arm's rest angle is not the only guard: with any angle allowed
  // for it, an arm in the band still stops the other from counting.
  NuiGuideGestureSettings any_rest = EitherArmSettings();
  any_rest.rest_angle_max_degrees = 180.0f;
  CHECK_FALSE(FiresWith(any_rest, both_out));

  // An arm short of the band beside one in it leaves the gesture to that one.
  TestSkeleton one_out = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(one_out, Side::kRight, 15.0f);
  CHECK(Fires(one_out));
}

TEST_CASE("Only the left arm makes the gesture by default",
          "[nui][guide_gesture]") {
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f)));
  CHECK_FALSE(Fires(ArmOut(Side::kRight, 45.0f)));

  NuiGuideGestureSettings right = DefaultSettings();
  right.arms = NuiGuideArmChoice::kRight;
  CHECK(FiresWith(right, ArmOut(Side::kRight, 45.0f)));
  CHECK_FALSE(FiresWith(right, ArmOut(Side::kLeft, 45.0f)));

  CHECK(FiresWith(EitherArmSettings(), ArmOut(Side::kLeft, 45.0f)));
  CHECK(FiresWith(EitherArmSettings(), ArmOut(Side::kRight, 45.0f)));
}

TEST_CASE("The left arm fires with the right arm hanging at the side",
          "[nui][guide_gesture]") {
  for (float right_angle : {0.0f, 7.0f, 14.0f, 19.0f}) {
    INFO("right arm at " << right_angle);
    TestSkeleton skeleton = ArmOut(Side::kLeft, 45.0f);
    PlaceArm(skeleton, Side::kRight, right_angle);
    CHECK(Fires(skeleton));
  }
  // The rest angle is measured from straight down whichever way the arm
  // hangs, so one a little in front of the body counts too.
  TestSkeleton hanging_forward = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(hanging_forward, Side::kRight, 15.0f, 90.0f);
  CHECK(Fires(hanging_forward));
}

TEST_CASE("The left arm does not fire with the right arm out or raised",
          "[nui][guide_gesture]") {
  // Out at 23 degrees, the right arm is short of the band and its hand is
  // below the hip centre, but it does not hang at the side.
  TestSkeleton past_rest = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(past_rest, Side::kRight, 23.0f);
  CHECK_FALSE(Fires(past_rest));

  // Out at 32 degrees and swung forward, the right arm is not in the band,
  // since it does not point to the side, and its hand is below the hip
  // centre: only the rest angle tells this pose from the gesture.
  TestSkeleton out_forward = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(out_forward, Side::kRight, 32.0f, 70.0f);
  CHECK_FALSE(Fires(out_forward));
  NuiGuideGestureSettings any_rest = DefaultSettings();
  any_rest.rest_angle_max_degrees = 180.0f;
  CHECK(FiresWith(any_rest, out_forward));

  // Both arms out at about 45 degrees.
  TestSkeleton both_out = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(both_out, Side::kRight, 40.0f);
  CHECK_FALSE(Fires(both_out));
  CHECK_FALSE(FiresWith(EitherArmSettings(), both_out));

  // Raised to the side, and raised in front.
  TestSkeleton raised_side = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(raised_side, Side::kRight, 120.0f);
  CHECK_FALSE(Fires(raised_side));
  TestSkeleton raised_front = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(raised_front, Side::kRight, 90.0f, 90.0f);
  CHECK_FALSE(Fires(raised_front));
}

TEST_CASE("The rest angle comes from the settings", "[nui][guide_gesture]") {
  NuiGuideGestureSettings settings = DefaultSettings();
  TestSkeleton skeleton = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(skeleton, Side::kRight, 28.0f);
  CHECK_FALSE(FiresWith(settings, skeleton));
  settings.rest_angle_max_degrees = 30.0f;
  CHECK(FiresWith(settings, skeleton));

  settings.rest_angle_max_degrees = 15.0f;
  PlaceArm(skeleton, Side::kRight, 17.0f);
  CHECK_FALSE(FiresWith(settings, skeleton));
  PlaceArm(skeleton, Side::kRight, 13.0f);
  CHECK(FiresWith(settings, skeleton));
}

TEST_CASE("An arm that turns too far in a hold breaks it and starts again",
          "[nui][guide_gesture]") {
  // Raised from 40 to 55 degrees, the arm turns 15 degrees, more than the 10
  // allowed; both angles are in the band.
  const TestSkeleton first = ArmOut(Side::kLeft, 40.0f);
  const TestSkeleton moved = ArmOut(Side::kLeft, 55.0f);
  constexpr uint64_t kMoveUs = 510000;
  NuiGuideGesture gesture(DefaultSettings());
  const NuiGuideArmTrace& left = gesture.last_trace().arms[0];
  REQUIRE(FireTimes(gesture, first, 0, kMoveUs).empty());

  // The moved frames do not qualify, but the hold lasts out the gap
  // tolerance.
  REQUIRE_FALSE(Feed(gesture, moved, kMoveUs));
  CHECK(left.in_band);
  CHECK(left.failed == kNuiGuideRuleArmStill);
  CHECK(left.arm_turned_degrees > NuiGuideGesture::kMaximumArmTurnDegrees);
  CHECK(left.arm_turned_degrees == Approx(15.0f).margin(0.01f));
  CHECK(left.holding);

  // Then it ends, and the same frame starts a hold from where the arm is.
  const uint64_t restart_us =
      FirstFrameAfterGap(kMoveUs, LastFrameBefore(kMoveUs));
  REQUIRE(FireTimes(gesture, moved, kMoveUs + kFrameUs, restart_us).empty());
  REQUIRE_FALSE(Feed(gesture, moved, restart_us));
  CHECK(left.hold_broken);
  CHECK(left.hold_started);
  CHECK(left.arm_turned_degrees == 0.0f);
  CHECK(left.failed == 0);

  const std::vector<uint64_t> fire_times = FireTimes(
      gesture, moved, restart_us + kFrameUs, restart_us + 2 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], restart_us));
}

TEST_CASE("An arm that turns a little, or briefly, keeps the hold",
          "[nui][guide_gesture]") {
  constexpr uint64_t kMoveUs = 510000;
  NuiGuideGesture gesture(DefaultSettings());
  REQUIRE(FireTimes(gesture, ArmOut(Side::kLeft, 45.0f), 0, kMoveUs).empty());

  SECTION("A little") {
    // From 45 to 50 degrees the arm turns 5 degrees.
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, ArmOut(Side::kLeft, 50.0f), kMoveUs, 3 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], 0));
  }

  SECTION("Too far, and back within the gap tolerance") {
    // At 57 degrees the arm has turned 12 degrees, for 100 ms.
    constexpr uint64_t kBackUs = kMoveUs + 100000;
    CHECK(FireTimes(gesture, ArmOut(Side::kLeft, 57.0f), kMoveUs, kBackUs)
              .empty());
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, ArmOut(Side::kLeft, 45.0f), kBackUs, 3 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], 0));
  }
}

TEST_CASE("An arm that keeps turning never completes the hold",
          "[nui][guide_gesture]") {
  // The arm rises steadily from 31 to 59 degrees over 1.9 s, in the band all
  // the way; held still that long, it would have fired.
  NuiGuideGesture gesture(DefaultSettings());
  uint64_t t = 0;
  for (float angle = 31.0f; angle <= 59.0f; angle += 0.5f, t += kFrameUs) {
    INFO("angle " << angle);
    CHECK_FALSE(Feed(gesture, ArmOut(Side::kLeft, angle), t));
  }
  CHECK(t > 3 * kHoldUs / 2);
}

TEST_CASE("A hip that moves too far in a hold breaks it",
          "[nui][guide_gesture]") {
  // A step of 0.2 m moves the hip past the 0.15 m allowed; the arm keeps its
  // direction.
  const TestSkeleton first = ArmOut(Side::kLeft, 45.0f);
  const TestSkeleton stepped = Shifted(first, {0.2f, 0.0f, 0.0f});
  constexpr uint64_t kStepUs = 510000;
  NuiGuideGesture gesture(DefaultSettings());
  const NuiGuideArmTrace& left = gesture.last_trace().arms[0];
  REQUIRE(FireTimes(gesture, first, 0, kStepUs).empty());

  REQUIRE_FALSE(Feed(gesture, stepped, kStepUs));
  CHECK(left.failed == kNuiGuideRuleHipStill);
  CHECK(left.arm_turned_degrees == Approx(0.0f).margin(0.1f));
  CHECK(left.hip_moved == Approx(0.2f).margin(0.0001f));

  const uint64_t restart_us =
      FirstFrameAfterGap(kStepUs, LastFrameBefore(kStepUs));
  CHECK(FireTimes(gesture, stepped, kStepUs + kFrameUs, restart_us).empty());
  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, stepped, restart_us, restart_us + 2 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], restart_us));
}

TEST_CASE("A still arm whose hand joint jitters keeps the hold",
          "[nui][guide_gesture]") {
  // The tracker's hand joint can jump 0.05-0.17 m from one frame to the next
  // along an arm whose angle changes by less than a degree. Here the hold's
  // first frame is one of those outliers, 0.09 m further out than the frames
  // after it, which scatter 0.02-0.08 m short of the arm's length.
  constexpr std::array<float, 6> kReachOffsets = {-0.03f, -0.06f, -0.02f,
                                                  -0.08f, -0.04f, -0.07f};
  constexpr std::array<float, 6> kAngleOffsets = {-0.3f, 0.2f,  -0.4f,
                                                  0.1f,  -0.2f, 0.3f};
  NuiGuideGesture gesture(DefaultSettings());
  std::vector<uint64_t> fire_times;
  if (Feed(gesture, JitteredArm(45.4f, 0.09f), 0)) {
    fire_times.push_back(0);
  }
  size_t i = 0;
  for (uint64_t t = kFrameUs; t < 3 * kHoldUs; t += kFrameUs, ++i) {
    const TestSkeleton skeleton =
        JitteredArm(45.0f + kAngleOffsets[i % kAngleOffsets.size()],
                    kReachOffsets[i % kReachOffsets.size()]);
    if (Feed(gesture, skeleton, t)) {
      fire_times.push_back(t);
    }
  }
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
}

TEST_CASE("A small step keeps the hold: it does not turn the arm",
          "[nui][guide_gesture]") {
  // A step of 0.12 m is inside the hip's limit, and moves the hand as far
  // without turning the arm.
  const TestSkeleton first = ArmOut(Side::kLeft, 45.0f);
  constexpr uint64_t kStepUs = 510000;
  NuiGuideGesture gesture(DefaultSettings());
  REQUIRE(FireTimes(gesture, first, 0, kStepUs).empty());
  const std::vector<uint64_t> fire_times = FireTimes(
      gesture, Shifted(first, {0.12f, 0.0f, 0.0f}), kStepUs, 3 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
}

TEST_CASE("Skeletons keep separate holds", "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  const TestSkeleton lowered = StandingSkeleton();
  // The first player holds the pose until 600 ms; the second starts at
  // 500 ms. Sharing one hold, the second would fire at about 1 s.
  constexpr uint64_t kFirstStopsUs = 600000;
  constexpr uint64_t kSecondStartsUs = 500000;

  std::vector<uint64_t> first_fires;
  std::vector<uint64_t> second_fires;
  for (uint64_t t = 0; t < 3 * kHoldUs; t += kFrameUs) {
    if (Feed(gesture, t < kFirstStopsUs ? held : lowered, t, kPlayerId)) {
      first_fires.push_back(t);
    }
    if (Feed(gesture, t < kSecondStartsUs ? lowered : held, t,
             kOtherPlayerId)) {
      second_fires.push_back(t);
    }
  }

  CHECK(first_fires.empty());
  REQUIRE(second_fires.size() == 1);
  // The second player's first frame in the pose is the first frame at or
  // after 500 ms.
  const uint64_t second_start =
      (kSecondStartsUs + kFrameUs - 1) / kFrameUs * kFrameUs;
  CHECK(FiredOnTime(second_fires[0], second_start));
}

TEST_CASE("A skeleton keeps its hold while others come and go",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  const TestSkeleton lowered = StandingSkeleton();

  std::vector<uint64_t> fire_times;
  uint32_t passer_by = 100;
  for (uint64_t t = 0; t < 3 * kHoldUs; t += kFrameUs) {
    if (Feed(gesture, held, t, kPlayerId)) {
      fire_times.push_back(t);
    }
    // A new tracking id every frame, more than the recogniser has room for.
    CHECK_FALSE(Feed(gesture, lowered, t, passer_by++));
  }

  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], 0));
}

TEST_CASE("Reset forgets holds and re-arms", "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);

  SECTION("A hold in progress starts again") {
    constexpr uint64_t kResetUs = 600000;
    CHECK(FireTimes(gesture, held, 0, kResetUs).empty());
    gesture.Reset();
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, held, kResetUs, kResetUs + 2 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], kResetUs));
  }

  SECTION("A fired arm fires again after a full hold") {
    REQUIRE(FireTimes(gesture, held, 0, 3 * kHoldUs).size() == 1);
    REQUIRE(gesture.last_fired_arm() == NuiGuideArm::kLeft);
    gesture.Reset();
    CHECK(gesture.last_fired_arm() == NuiGuideArm::kNone);
    const std::vector<uint64_t> fire_times =
        FireTimes(gesture, held, 3 * kHoldUs, 6 * kHoldUs);
    REQUIRE(fire_times.size() == 1);
    CHECK(FiredOnTime(fire_times[0], 3 * kHoldUs));
  }
}

TEST_CASE("The angle is measured from the gravity axis the caller passes",
          "[nui][guide_gesture]") {
  // Seen by a sensor rolled 25 degrees, the arm is 20 degrees from the
  // camera's down axis but still 45 degrees from gravity.
  constexpr float kRollDegrees = 25.0f;
  const TestSkeleton rolled = Rolled(ArmOut(Side::kLeft, 45.0f), kRollDegrees);
  const NuiGuideVector rolled_down = Roll(kCameraDown, kRollDegrees);

  CHECK(Fires(rolled, rolled_down));
  CHECK_FALSE(Fires(rolled, kCameraDown));
}

TEST_CASE("The gravity axis need not be of unit length",
          "[nui][guide_gesture]") {
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f), NuiGuideVector{0.0f, -9.8f, 0.0f}));
}

TEST_CASE("A zero gravity axis never fires", "[nui][guide_gesture]") {
  CHECK_FALSE(
      Fires(ArmOut(Side::kLeft, 45.0f), NuiGuideVector{0.0f, 0.0f, 0.0f}));
}

TEST_CASE("A position that is not a number does not count",
          "[nui][guide_gesture]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (size_t joint : {kShoulderLeft, kElbowLeft, kHandLeft, kShoulderRight,
                       kHandRight, kHipCenter}) {
    INFO("joint " << joint);
    TestSkeleton skeleton = ArmOut(Side::kLeft, 45.0f);
    skeleton.joints[joint] = {nan, nan, nan};
    CHECK_FALSE(Fires(skeleton));
  }
  CHECK_FALSE(Fires(ArmOut(Side::kLeft, 45.0f), NuiGuideVector{nan, nan, nan}));
  CHECK(Fires(ArmOut(Side::kLeft, 45.0f)));
}

TEST_CASE("The settings are built from the cvars' degrees and milliseconds",
          "[nui][guide_gesture]") {
  constexpr NuiGuideArmChoice kLeft = NuiGuideArmChoice::kLeft;
  const NuiGuideGestureSettings settings =
      MakeNuiGuideGestureSettings(30, 60, 1000, 200, 20, kLeft);
  CHECK(settings.angle_min_degrees == kAngleMinDegrees);
  CHECK(settings.angle_max_degrees == kAngleMaxDegrees);
  CHECK(settings.arms == NuiGuideArmChoice::kLeft);
  CHECK(settings.rest_angle_max_degrees == kRestAngleMaxDegrees);
  CHECK(settings.hold_us == kHoldUs);
  CHECK(settings.gap_us == kGapUs);
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, 200, 20,
                                    NuiGuideArmChoice::kEither)
            .arms == NuiGuideArmChoice::kEither);

  // The defaults are what the cvars' defaults, which the kernel suite does not
  // link, build.
  NuiGuideArmChoice default_arms = NuiGuideArmChoice::kEither;
  REQUIRE(ParseNuiGuideArmChoice(xe::nui::kGuideArmDefault, &default_arms));
  const NuiGuideGestureSettings from_cvar_defaults =
      MakeNuiGuideGestureSettings(
          xe::nui::kGuideAngleMinDefault, xe::nui::kGuideAngleMaxDefault,
          xe::nui::kGuideHoldMsDefault, xe::nui::kGuideGapMsDefault,
          xe::nui::kGuideRestAngleMaxDefault, default_arms);
  const NuiGuideGestureSettings defaults;
  CHECK(defaults.angle_min_degrees == from_cvar_defaults.angle_min_degrees);
  CHECK(defaults.angle_max_degrees == from_cvar_defaults.angle_max_degrees);
  CHECK(defaults.arms == from_cvar_defaults.arms);
  CHECK(defaults.rest_angle_max_degrees ==
        from_cvar_defaults.rest_angle_max_degrees);
  CHECK(defaults.hold_us == from_cvar_defaults.hold_us);
  CHECK(defaults.gap_us == from_cvar_defaults.gap_us);

  // A negative hold counts as none. A gap tolerance below 50 ms, shorter than
  // two frames apart, counts as 50 ms.
  constexpr uint64_t kMinimumGapUs = 50000;
  const NuiGuideGestureSettings short_times =
      MakeNuiGuideGestureSettings(30, 60, -1, 10, 20, kLeft);
  CHECK(short_times.hold_us == 0);
  CHECK(short_times.gap_us == kMinimumGapUs);
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, -200, 20, kLeft).gap_us ==
        kMinimumGapUs);
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, 50, 20, kLeft).gap_us ==
        kMinimumGapUs);
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, 51, 20, kLeft).gap_us ==
        51000);

  // A band given the wrong way round is turned the right way round.
  const NuiGuideGestureSettings inverted =
      MakeNuiGuideGestureSettings(60, 30, 1000, 200, 20, kLeft);
  CHECK(inverted.angle_min_degrees == kAngleMinDegrees);
  CHECK(inverted.angle_max_degrees == kAngleMaxDegrees);

  // A rest angle is held to the angles an arm can make.
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, 200, -5, kLeft)
            .rest_angle_max_degrees == 0.0f);
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, 200, 180, kLeft)
            .rest_angle_max_degrees == 180.0f);
  CHECK(MakeNuiGuideGestureSettings(30, 60, 1000, 200, 500, kLeft)
            .rest_angle_max_degrees == 180.0f);
}

TEST_CASE("nui_guide_arm's values are read in any case",
          "[nui][guide_gesture]") {
  NuiGuideArmChoice choice = NuiGuideArmChoice::kEither;
  REQUIRE(ParseNuiGuideArmChoice("left", &choice));
  CHECK(choice == NuiGuideArmChoice::kLeft);
  REQUIRE(ParseNuiGuideArmChoice("Right", &choice));
  CHECK(choice == NuiGuideArmChoice::kRight);
  REQUIRE(ParseNuiGuideArmChoice("EITHER", &choice));
  CHECK(choice == NuiGuideArmChoice::kEither);
  // Anything else is refused and leaves the choice alone.
  for (const char* value : {"", "both", "lefty", " left", "l"}) {
    INFO("value '" << value << "'");
    NuiGuideArmChoice unchanged = NuiGuideArmChoice::kRight;
    CHECK_FALSE(ParseNuiGuideArmChoice(value, &unchanged));
    CHECK(unchanged == NuiGuideArmChoice::kRight);
  }
}

TEST_CASE("A timestamp that goes backwards starts a new hold",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  constexpr uint64_t kRestartUs = 100000;

  CHECK(FireTimes(gesture, held, 0, 900000).empty());
  const std::vector<uint64_t> fire_times =
      FireTimes(gesture, held, kRestartUs, kRestartUs + 2 * kHoldUs);
  REQUIRE(fire_times.size() == 1);
  CHECK(FiredOnTime(fire_times[0], kRestartUs));
}

TEST_CASE("The rules' names are listed in order", "[nui][guide_gesture]") {
  CHECK(FormatNuiGuideRules(0) == "none");
  CHECK(FormatNuiGuideRules(kNuiGuideRuleAngle) == "angle");
  CHECK(FormatNuiGuideRules(kNuiGuideRuleOtherHandLowered | kNuiGuideRuleSeen |
                            kNuiGuideRuleExtension) ==
        "seen,extension,other-hand-lowered");
  CHECK(FormatNuiGuideRules(kNuiGuideRuleSideways | kNuiGuideRuleTracked |
                            kNuiGuideRuleOtherArmOutOfBand) ==
        "sideways,tracked,other-arm-out-of-band");
  CHECK(FormatNuiGuideRules(kNuiGuideRuleHipStill | kNuiGuideRuleArm |
                            kNuiGuideRuleOtherArmAtRest |
                            kNuiGuideRuleArmStill) ==
        "arm,other-arm-at-rest,arm-still,hip-still");
}

TEST_CASE("The trace reports the numbers the rules read",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  CHECK(gesture.last_trace().tracking_id == 0);
  REQUIRE_FALSE(Feed(gesture, ArmOut(Side::kLeft, 45.0f), 0));
  const NuiGuideTrace& trace = gesture.last_trace();
  CHECK(trace.tracking_id == kPlayerId);
  CHECK(trace.lean_degrees == Approx(0.0f).margin(0.01f));

  // The left arm is straight, out to its side at 45 degrees, and the right
  // hangs with its hand 0.1 m below the hip centre.
  const NuiGuideArmTrace& left = trace.arms[0];
  CHECK(left.measurement.seen);
  CHECK(left.measurement.tracked);
  CHECK(left.measurement.angle_degrees == Approx(45.0f).margin(0.01f));
  CHECK(left.measurement.extension == Approx(1.0f).margin(0.001f));
  CHECK(left.measurement.sideways == Approx(1.0f).margin(0.001f));
  CHECK(left.measurement.other_arm_seen);
  CHECK(left.measurement.other_angle_degrees == Approx(0.0f).margin(0.01f));
  CHECK(left.measurement.other_hand_seen);
  CHECK(left.measurement.other_hand_height == Approx(-0.1f).margin(0.001f));
  CHECK(left.measurement.hip_seen);
  CHECK(left.failed == 0);
  CHECK(left.in_band);
  CHECK(left.hold_started);
  CHECK_FALSE(left.hold_broken);
  CHECK_FALSE(left.completed);
  CHECK(left.holding);
  CHECK(left.held_us == 0);
  CHECK(left.armed);
  CHECK(left.arm_turned_degrees == 0.0f);
  CHECK(left.hip_moved == 0.0f);

  // The right arm points straight down, so it has no heading, and is not the
  // arm that makes the gesture by default; the raised left arm is in the
  // band, and its hand is above the hip centre.
  const NuiGuideArmTrace& right = trace.arms[1];
  CHECK(right.measurement.seen);
  CHECK(right.measurement.angle_degrees == Approx(0.0f).margin(0.01f));
  CHECK(std::isnan(right.measurement.sideways));
  CHECK(right.measurement.other_angle_degrees == Approx(45.0f).margin(0.01f));
  CHECK(right.measurement.other_hand_height > 0.0f);
  CHECK_FALSE(right.in_band);
  CHECK(right.failed ==
        (kNuiGuideRuleAngle | kNuiGuideRuleSideways | kNuiGuideRuleArm |
         kNuiGuideRuleOtherArmOutOfBand | kNuiGuideRuleOtherArmAtRest |
         kNuiGuideRuleOtherHandLowered));
  CHECK_FALSE(right.hold_started);
  CHECK_FALSE(right.holding);
  CHECK(std::isnan(right.arm_turned_degrees));
  CHECK(std::isnan(right.hip_moved));
}

TEST_CASE("The trace names the rules a frame breaks", "[nui][guide_gesture]") {
  auto left_failed = [](const TestSkeleton& skeleton) {
    NuiGuideGesture gesture(DefaultSettings());
    Feed(gesture, skeleton, 0);
    return gesture.last_trace().arms[0].failed;
  };
  CHECK(left_failed(ArmOut(Side::kLeft, 45.0f)) == 0);
  CHECK(left_failed(ArmOut(Side::kLeft, 70.0f)) == kNuiGuideRuleAngle);
  CHECK(left_failed(ArmOut(Side::kLeft, 45.0f, 0.0f, 0.8f)) ==
        kNuiGuideRuleExtension);
  CHECK(left_failed(ArmOut(Side::kLeft, 45.0f, 90.0f)) ==
        kNuiGuideRuleSideways);

  TestSkeleton hand_inferred = ArmOut(Side::kLeft, 45.0f);
  hand_inferred.states[kHandLeft] = kInferred;
  CHECK(left_failed(hand_inferred) == kNuiGuideRuleTracked);

  // Not tracked at all, the hand leaves nothing of the arm to measure.
  TestSkeleton hand_lost = ArmOut(Side::kLeft, 45.0f);
  hand_lost.states[kHandLeft] = kNotTracked;
  CHECK(left_failed(hand_lost) == (kNuiGuideRuleSeen | kNuiGuideRuleTracked));

  TestSkeleton both_out = ArmOut(Side::kLeft, 32.0f);
  PlaceArm(both_out, Side::kRight, 32.0f);
  CHECK(left_failed(both_out) ==
        (kNuiGuideRuleOtherArmOutOfBand | kNuiGuideRuleOtherArmAtRest));

  // The right arm out at 28 degrees is short of the band, and its hand is
  // below the hip centre, but it does not hang at the side.
  TestSkeleton other_arm_out = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(other_arm_out, Side::kRight, 28.0f);
  CHECK(left_failed(other_arm_out) == kNuiGuideRuleOtherArmAtRest);

  TestSkeleton other_hand_raised = ArmOut(Side::kLeft, 45.0f);
  PlaceArm(other_hand_raised, Side::kRight, 90.0f, 90.0f);
  CHECK(left_failed(other_hand_raised) ==
        (kNuiGuideRuleOtherArmAtRest | kNuiGuideRuleOtherHandLowered));

  // The right arm alone is in the band, but is not the default's arm.
  NuiGuideGesture right_gesture(DefaultSettings());
  Feed(right_gesture, ArmOut(Side::kRight, 45.0f), 0);
  CHECK(right_gesture.last_trace().arms[1].failed == kNuiGuideRuleArm);

  // Without a gravity axis nothing is measured.
  NuiGuideGesture gesture(DefaultSettings());
  Feed(gesture, ArmOut(Side::kLeft, 45.0f), 0, kPlayerId,
       NuiGuideVector{0.0f, 0.0f, 0.0f});
  const NuiGuideTrace& trace = gesture.last_trace();
  CHECK_FALSE(trace.arms[0].measurement.seen);
  CHECK(trace.arms[0].failed & kNuiGuideRuleSeen);
  CHECK(std::isnan(trace.lean_degrees));
}

TEST_CASE("The trace follows a hold to its completion and after",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  const NuiGuideArmTrace& left = gesture.last_trace().arms[0];

  uint64_t t = 0;
  for (; !Feed(gesture, held, t); t += kFrameUs) {
    INFO("t = " << t);
    REQUIRE(t < 2 * kHoldUs);
    CHECK(left.hold_started == (t == 0));
    CHECK(left.holding);
    CHECK(left.held_us == t);
    CHECK_FALSE(left.completed);
    CHECK(left.armed);
  }
  CHECK(FiredOnTime(t, 0));
  CHECK(left.completed);
  CHECK(left.held_us == t);
  CHECK_FALSE(left.armed);

  // Held on, the pose keeps the hold going but cannot complete again.
  t += kFrameUs;
  CHECK_FALSE(Feed(gesture, held, t));
  CHECK_FALSE(left.completed);
  CHECK_FALSE(left.hold_started);
  CHECK(left.holding);
  CHECK_FALSE(left.armed);
}

TEST_CASE("The trace says when a hold breaks", "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const TestSkeleton held = ArmOut(Side::kLeft, 45.0f);
  const TestSkeleton lowered = StandingSkeleton();
  const NuiGuideArmTrace& left = gesture.last_trace().arms[0];

  constexpr uint64_t kLowerUs = 600000;
  REQUIRE(FireTimes(gesture, held, 0, kLowerUs).empty());
  const uint64_t last_held_us = (kLowerUs - 1) / kFrameUs * kFrameUs;
  size_t breaks = 0;
  for (uint64_t t = kLowerUs; t < kLowerUs + 3 * kGapUs; t += kFrameUs) {
    INFO("t = " << t);
    Feed(gesture, lowered, t);
    CHECK_FALSE(left.hold_started);
    // The hold lasts until the gap tolerance has gone by.
    if (left.hold_broken) {
      ++breaks;
      CHECK(t - last_held_us > kGapUs);
      CHECK(t - kFrameUs - last_held_us <= kGapUs);
    }
    CHECK(left.holding == (t - last_held_us <= kGapUs));
  }
  CHECK(breaks == 1);
}

TEST_CASE("The trace measures how far the arm turns and the hip moves",
          "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  const NuiGuideArmTrace& left = gesture.last_trace().arms[0];
  REQUIRE_FALSE(Feed(gesture, ArmOut(Side::kLeft, 45.0f), 0));

  // Raised to 50 degrees, the arm has turned 5 degrees.
  REQUIRE_FALSE(Feed(gesture, ArmOut(Side::kLeft, 50.0f), kFrameUs));
  CHECK(left.holding);
  CHECK(left.arm_turned_degrees == Approx(5.0f).margin(0.01f));
  CHECK(left.hip_moved == Approx(0.0f).margin(0.0001f));

  // A step moves the hip but does not turn the arm.
  REQUIRE_FALSE(Feed(gesture,
                     Shifted(ArmOut(Side::kLeft, 45.0f), {0.05f, 0.0f, 0.02f}),
                     2 * kFrameUs));
  CHECK(left.arm_turned_degrees == Approx(0.0f).margin(0.1f));
  CHECK(left.hip_moved ==
        Approx(std::sqrt(0.05f * 0.05f + 0.02f * 0.02f)).margin(0.0001f));
}

TEST_CASE("The trace's lean is measured from the gravity axis",
          "[nui][guide_gesture]") {
  constexpr float kRollDegrees = 10.0f;
  const TestSkeleton rolled = Rolled(StandingSkeleton(), kRollDegrees);
  NuiGuideGesture gesture(DefaultSettings());
  Feed(gesture, rolled, 0);
  CHECK(gesture.last_trace().lean_degrees ==
        Approx(kRollDegrees).margin(0.01f));
  Feed(gesture, rolled, kFrameUs, kPlayerId, Roll(kCameraDown, kRollDegrees));
  CHECK(gesture.last_trace().lean_degrees == Approx(0.0f).margin(0.01f));

  TestSkeleton no_shoulder_centre = StandingSkeleton();
  no_shoulder_centre.states[kShoulderCenter] = kNotTracked;
  Feed(gesture, no_shoulder_centre, 2 * kFrameUs);
  CHECK(std::isnan(gesture.last_trace().lean_degrees));
}

TEST_CASE("Reset clears the trace", "[nui][guide_gesture]") {
  NuiGuideGesture gesture(DefaultSettings());
  Feed(gesture, ArmOut(Side::kLeft, 45.0f), 0);
  REQUIRE(gesture.last_trace().tracking_id == kPlayerId);
  gesture.Reset();
  CHECK(gesture.last_trace().tracking_id == 0);
  CHECK_FALSE(gesture.last_trace().arms[0].holding);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
