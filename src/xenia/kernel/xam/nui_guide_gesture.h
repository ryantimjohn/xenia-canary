/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_GUIDE_GESTURE_H_
#define XENIA_KERNEL_XAM_NUI_GUIDE_GESTURE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "xenia/nui/nui_guide_defaults.h"

namespace xe {
namespace kernel {
namespace xam {

// The number of joints in a NUI skeleton, in the public layout: hip centre 0,
// spine 1, shoulder centre 2, head 3, then shoulder, elbow, wrist and hand for
// the left arm (4 to 7) and the right arm (8 to 11), then hip, knee, ankle and
// foot for the left leg (12 to 15) and the right leg (16 to 19).
constexpr size_t kNuiGuideJointCount = 20;

// A NuiGuideVector is a point or a direction in the sensor's camera space, with
// x to the right, y up and z away from the sensor.
struct NuiGuideVector {
  float x;
  float y;
  float z;
};

// NuiGuideArm says which arm made the gesture.
enum class NuiGuideArm : uint8_t {
  kNone,
  kLeft,
  kRight,
};

// NuiGuideRule names the gesture's rules. A NuiGuideArmTrace keeps the rules
// that a frame broke for an arm as a set of these bits.
enum NuiGuideRule : uint32_t {
  // The frame shows the arm.
  kNuiGuideRuleSeen = 1u << 0,
  // The arm's angle from straight down lies in the band.
  kNuiGuideRuleAngle = 1u << 1,
  // The arm is straight.
  kNuiGuideRuleExtension = 1u << 2,
  // The arm points out to its own side.
  kNuiGuideRuleSideways = 1u << 3,
  // The arm's shoulder, elbow and hand are tracked rather than inferred.
  kNuiGuideRuleTracked = 1u << 4,
  // The arm is one that the settings let make the gesture.
  kNuiGuideRuleArm = 1u << 5,
  // The other arm is not in the band itself.
  kNuiGuideRuleOtherArmOutOfBand = 1u << 6,
  // The other arm hangs at the side.
  kNuiGuideRuleOtherArmAtRest = 1u << 7,
  // The other hand is lower than the hip centre.
  kNuiGuideRuleOtherHandLowered = 1u << 8,
  // While a hold is under way, the arm has kept its direction, shoulder to
  // hand, near the one it had on the hold's first frame.
  kNuiGuideRuleArmStill = 1u << 9,
  // While a hold is under way, the hip centre has stayed near where it was on
  // the hold's first frame.
  kNuiGuideRuleHipStill = 1u << 10,
};

// Returns the short names of the rules in failed, a set of NuiGuideRule bits,
// in the order NuiGuideRule lists them and separated by commas, or "none" for
// an empty set.
std::string FormatNuiGuideRules(uint32_t failed);

// A NuiGuideArmMeasurement holds what one frame shows of one arm and of the
// body around it: the numbers that the gesture's rules compare with their
// thresholds, which the trace reports as well. Angles are in degrees, and
// positions and heights in the skeleton's unit, metres for a NUI skeleton.
struct NuiGuideArmMeasurement {
  // Whether the frame shows the arm: its shoulder, elbow and hand and the
  // other shoulder are tracked or inferred, their positions are numbers, the
  // arm has a direction and the body an axis across the shoulders, and the
  // frame has a gravity axis. The four values after tracked are measured only
  // when it does.
  bool seen = false;
  // Whether the arm's shoulder, elbow and hand are tracked rather than
  // inferred.
  bool tracked = false;
  // The shoulder-to-hand vector's angle from straight down.
  float angle_degrees = 0.0f;
  // The hand's distance from the shoulder as a fraction of the arm's length,
  // shoulder to elbow plus elbow to hand: 1 for a straight arm.
  float extension = 0.0f;
  // Seen from above, the cosine of the angle between the arm's heading and
  // the body's outward axis, which runs from the other shoulder to this one:
  // 1 for an arm straight out to its side, 0 for one pointed straight ahead.
  // An arm that points straight down has no heading, and this is then not a
  // number.
  float sideways = 0.0f;
  // Where the hand is, relative to its own shoulder: the arm's direction.
  NuiGuideVector hand_from_shoulder = {};
  // The other arm's shoulder-to-hand angle from straight down, measured when
  // the other shoulder and the other hand are tracked or inferred.
  bool other_arm_seen = false;
  float other_angle_degrees = 0.0f;
  // How far the other hand is above the hip centre along the gravity axis,
  // negative when it is below, measured when both are tracked or inferred.
  bool other_hand_seen = false;
  float other_hand_height = 0.0f;
  // Where the hip centre is, when it is tracked or inferred.
  bool hip_seen = false;
  NuiGuideVector hip = {};
};

// A NuiGuideArmTrace holds what one call to NuiGuideGesture::Update measured of
// one arm and what it did with the arm's hold, for a diagnostic trace.
struct NuiGuideArmTrace {
  NuiGuideArmMeasurement measurement;
  // The rules that the frame broke for the arm, as NuiGuideRule bits; 0 when
  // the frame qualified.
  uint32_t failed = 0;
  // Whether the arm is in the band: seen, inside the angle band, straight and
  // out to its side.
  bool in_band = false;
  // Whether the frame ended a hold that had gone longer than the gap tolerance
  // without a qualifying frame, started a new hold, or completed the gesture.
  // One frame can end a hold and start the next.
  bool hold_broken = false;
  bool hold_started = false;
  bool completed = false;
  // Whether a hold is under way after the frame, and how long it has lasted
  // from its first frame to this one.
  bool holding = false;
  uint64_t held_us = 0;
  // Whether the arm may complete the gesture after the frame: false from a
  // completion until the arm has been seen out of the band for longer than
  // the gap tolerance.
  bool armed = true;
  // While a hold is under way, how far the arm's direction, shoulder to hand,
  // has turned from the one it had on the hold's first frame, in degrees, and
  // how far the hip centre is from where it was then. Each is not a number
  // when there is no hold or the frame does not show it.
  float arm_turned_degrees = std::numeric_limits<float>::quiet_NaN();
  float hip_moved = std::numeric_limits<float>::quiet_NaN();
};

// A NuiGuideTrace holds what one call to NuiGuideGesture::Update measured of
// one skeleton.
struct NuiGuideTrace {
  uint32_t tracking_id = 0;
  // The trunk's lean, the angle of the line from the hip centre to the
  // shoulder centre from straight up, in degrees; not a number when the frame
  // does not show it. No rule reads it: it is traced because a player leaning
  // through a game can hold an arm the way the gesture does.
  float lean_degrees = std::numeric_limits<float>::quiet_NaN();
  // The left arm's trace, then the right arm's.
  std::array<NuiGuideArmTrace, 2> arms;
};

// NuiGuideArmChoice says which arms may make the gesture.
enum class NuiGuideArmChoice : uint8_t {
  kLeft,
  kRight,
  kEither,
};

// Reads a value of nui_guide_arm, "left", "right" or "either" in any case,
// into choice. Returns false, leaving choice as it was, for any other value.
bool ParseNuiGuideArmChoice(std::string_view value, NuiGuideArmChoice* choice);

// The settings hold the gesture's thresholds. Their defaults are those of the
// cvars that set them, nui_guide_angle_min, nui_guide_angle_max,
// nui_guide_arm, nui_guide_rest_angle_max, nui_guide_hold_ms and
// nui_guide_gap_ms, taken from the same constants.
struct NuiGuideGestureSettings {
  // The arm's angle from straight down must lie in this band, ends included.
  float angle_min_degrees = static_cast<float>(xe::nui::kGuideAngleMinDefault);
  float angle_max_degrees = static_cast<float>(xe::nui::kGuideAngleMaxDefault);
  // The arms that may make the gesture; the console's is the left.
  NuiGuideArmChoice arms = NuiGuideArmChoice::kLeft;
  // The other arm's shoulder-to-hand angle from straight down must be at most
  // this.
  float rest_angle_max_degrees =
      static_cast<float>(xe::nui::kGuideRestAngleMaxDefault);
  // The pose must be held this long.
  uint64_t hold_us = uint64_t(xe::nui::kGuideHoldMsDefault) * 1000;
  // A hold survives a stretch of missing or non-qualifying frames up to this
  // long.
  uint64_t gap_us = uint64_t(xe::nui::kGuideGapMsDefault) * 1000;
};

// Builds the settings from values in the cvars' units: angles in whole degrees
// and times in milliseconds. A band given the wrong way round is turned the
// right way round, a rest angle is held to 0-180 degrees, a negative hold
// counts as none, and a gap below 50 ms counts as 50 ms, since a shorter one
// would be broken by the ordinary interval between two frames.
NuiGuideGestureSettings MakeNuiGuideGestureSettings(
    int32_t angle_min_degrees, int32_t angle_max_degrees, int32_t hold_ms,
    int32_t gap_ms, int32_t rest_angle_max_degrees, NuiGuideArmChoice arms);

// Builds the settings from the cvars. A value of nui_guide_arm that
// ParseNuiGuideArmChoice does not know logs one Warning and means the left
// arm. It is defined in nui_guide_gesture_cvars.cc, which only the kernel
// library compiles: the kernel test suite does not link the cvar definitions.
NuiGuideGestureSettings NuiGuideGestureSettingsFromCvars();

// Recognises the Kinect Guide gesture, the console's system gesture that opens
// the guide over a Kinect title: the left arm held still, straight out to the
// side and down at about 45 degrees, with the right arm hanging at the side,
// for about a second. The settings say which arm may make it.
//
// The recogniser is fed one skeleton per call, in the order the frames arrive,
// and keeps its state per tracking id so that two players cannot complete each
// other's hold. It is pure and deterministic: it reads no clock or cvar and
// logs nothing; the caller passes the time and the thresholds, and may log
// what each call measured, which last_trace() holds. The positions are in
// metres, as a NUI skeleton's are: the hip's stillness rule is a distance, and
// the others are angles and ratios. A joint counts as tracked only in state 2;
// state 1 is inferred and state 0 not tracked.
//
// An arm is in the gesture's band on a frame when all of these hold:
// - The shoulder-to-hand vector's angle from the gravity axis, that is from
//   straight down, lies within [angle_min_degrees, angle_max_degrees].
// - The hand is at least kMinimumExtension of the arm's length (shoulder to
//   elbow plus elbow to hand) away from the shoulder, so the arm is straight.
// - The arm is out to its own side rather than in front. Seen from above, with
//   both vectors projected onto the plane across the gravity axis, the arm's
//   heading lies within 60 degrees of the body's outward axis, which runs from
//   the other shoulder to this one. So a hand pointed at the sensor does not
//   count, and the angle band alone decides how far down the arm may point.
// The rules read the arm's shoulder, elbow and hand and the other shoulder;
// when any of them is not tracked at all, or the frame has no gravity axis,
// the frame does not show the arm.
//
// A frame qualifies for an arm when all of these hold:
// - The arm is in the band, and its shoulder, elbow and hand are tracked
//   rather than inferred.
// - The arm is one that settings.arms allows.
// - The other arm is not in the band itself, and it hangs at the side: its
//   shoulder-to-hand angle from straight down is at most
//   rest_angle_max_degrees, with its shoulder and hand tracked or inferred.
// - The other hand is lower than the hip centre along the gravity axis, both
//   of them tracked or inferred. This keeps a hand raised to steer a cursor
//   from being taken for the gesture, and the rule before it keeps both arms
//   held out together from completing it with each arm in turn.
// - While a hold is under way, the arm is still: its direction, shoulder to
//   hand, is within kMaximumArmTurnDegrees of the one it had on the hold's
//   first frame, and the hip centre within kMaximumHipMovement of where it
//   was. A player moving through a game can hold an arm the way the gesture
//   does for a moment, but seldom keeps it and the body still for a second.
//   The arm's direction is measured rather than the hand's position, because
//   the tracker's arm joints jitter in length: along a still arm the hand
//   can jump by as much as 0.25 m from one frame to the next.
//
// An arm completes the gesture once its frames have qualified for hold_us with
// no stretch longer than gap_us of missing or non-qualifying frames; a longer
// stretch ends the hold, and the next qualifying frame starts a new one, from
// wherever the arm's direction and the hip then are. So an arm that turns and
// stays turned, or a hip that moves and stays moved, starts the hold again.
// After completing, the arm cannot complete again until it has been seen out
// of the band for longer than gap_us, counted from the first frame that showed
// it out, with inferred joints good enough to see it by. A frame that shows
// the arm in the band, even on inferred joints, ends that count; frames that
// do not show the arm and frames missing from the feed neither end the count
// nor re-arm the arm. So holding the pose on through a tracking flicker or a
// pause in the feed, or raising the other hand, does not re-arm it.
//
// Not thread-safe; the caller serialises the calls.
class NuiGuideGesture {
 public:
  // The hand must be at least this fraction of the arm's length from the
  // shoulder.
  static constexpr float kMinimumExtension = 0.9f;
  // Seen from above, the sideways part of the arm must be at least this
  // fraction of its whole horizontal part: 0.5 is cos 60 degrees.
  static constexpr float kMinimumSidewaysFraction = 0.5f;
  // While a hold is under way, the arm's direction, shoulder to hand, must
  // stay within this many degrees of the one it had on the hold's first frame:
  // 0.10 m at the hand of a 0.55 m arm.
  static constexpr float kMaximumArmTurnDegrees = 10.0f;
  // While a hold is under way, the hip centre must stay within this many
  // metres of where it was on the hold's first frame.
  static constexpr float kMaximumHipMovement = 0.15f;
  // The recogniser keeps state for this many tracking ids, as many as a NUI
  // frame has skeleton slots. When a new id finds the table full, the id seen
  // least recently is forgotten.
  static constexpr size_t kSkeletonCapacity = 6;

  explicit NuiGuideGesture(const NuiGuideGestureSettings& settings);

  // Feeds one skeleton of a frame. joints are the skeleton's positions and
  // tracking_states their NUI tracking states (0 not tracked, 1 inferred,
  // 2 tracked), both in the public joint order. gravity_axis points down in the
  // same space; it need not be of unit length, and a zero vector means the
  // frame shows neither arm. Returns true when the gesture completed on this
  // call; last_fired_arm() then says which arm made it. If both arms complete
  // on the same call, which takes a contrived pose, the left arm is reported.
  // Every call replaces last_trace() with what it measured and did.
  bool Update(uint32_t tracking_id,
              std::span<const NuiGuideVector, kNuiGuideJointCount> joints,
              std::span<const uint32_t, kNuiGuideJointCount> tracking_states,
              const NuiGuideVector& gravity_axis, uint64_t timestamp_us);

  // Returns the arm of the most recent completion, or kNone before the first
  // one and after Reset.
  NuiGuideArm last_fired_arm() const { return last_fired_arm_; }

  // Returns what the most recent Update measured and did, for the skeleton it
  // was fed; an empty trace before the first one and after Reset.
  const NuiGuideTrace& last_trace() const { return last_trace_; }

  // Forgets every tracking id's hold and re-arms both arms.
  void Reset();

 private:
  // A frame's view of one arm, whether its joints are tracked or inferred.
  enum class ArmView {
    // The frame does not show the arm: a joint the rules read is not tracked
    // at all, or there is no gravity axis.
    kUnseen,
    // At least one of the band's rules fails.
    kOutOfBand,
    // Every one of the band's rules holds.
    kInBand,
  };

  // An ArmState follows one arm's hold and whether the arm may complete.
  struct ArmState {
    // Whether a hold is under way; the fields after it are only meaningful
    // while one is. A hold is over once the gap tolerance has gone by without
    // a qualifying frame.
    bool holding = false;
    uint64_t hold_start_us = 0;
    uint64_t last_qualified_us = 0;
    // Where the hand, relative to its shoulder, and the hip centre were on the
    // hold's first frame.
    NuiGuideVector hold_hand_from_shoulder = {};
    NuiGuideVector hold_hip = {};
    // False from a completion until the arm has been seen out of the band for
    // longer than the gap tolerance.
    bool armed = true;
    // Whether a disarmed arm is being seen out of the band, and when the first
    // frame that showed it out was.
    bool leaving = false;
    uint64_t leaving_since_us = 0;
  };

  // A SkeletonState holds what the recogniser keeps for one tracking id.
  struct SkeletonState {
    bool in_use = false;
    uint32_t tracking_id = 0;
    // The Update call that last fed this tracking id, for forgetting the one
    // seen least recently.
    uint64_t last_update = 0;
    std::array<ArmState, 2> arms;
  };

  // Returns the state kept for tracking_id, claiming a free entry or the one
  // seen least recently for an id the table does not hold.
  SkeletonState& StateFor(uint32_t tracking_id);

  // Returns the band's rules, as NuiGuideRule bits, that a measured arm
  // breaks: whether the frame shows it, and its angle, extension and heading.
  uint32_t BandRulesFailed(const NuiGuideArmMeasurement& measurement) const;

  // Advances one arm's hold and arming by a frame whose measurement of the arm
  // is measurement, and records what it did in trace. qualifies says whether
  // the frame passes every rule but stillness, which this checks against the
  // hold under way. Returns true when the arm completes the gesture.
  bool AdvanceArm(ArmState& arm, ArmView view, bool qualifies,
                  const NuiGuideArmMeasurement& measurement,
                  uint64_t timestamp_us, NuiGuideArmTrace& trace) const;

  // Returns whether more than the gap tolerance separates earlier_us from
  // now_us. A clock that went backwards counts as a gap.
  bool GapExceeded(uint64_t earlier_us, uint64_t now_us) const;

  NuiGuideGestureSettings settings_;
  std::array<SkeletonState, kSkeletonCapacity> skeletons_;
  uint64_t update_count_ = 0;
  NuiGuideArm last_fired_arm_ = NuiGuideArm::kNone;
  NuiGuideTrace last_trace_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_GUIDE_GESTURE_H_
