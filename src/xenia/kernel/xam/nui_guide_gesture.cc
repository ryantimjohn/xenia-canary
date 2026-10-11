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
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>

#include "xenia/base/utf8.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

// These are the joints the gesture reads, in the public NUI skeleton layout.
constexpr size_t kHipCenter = 0;
constexpr size_t kShoulderCenter = 2;
constexpr size_t kShoulderLeft = 4;
constexpr size_t kElbowLeft = 5;
constexpr size_t kHandLeft = 7;
constexpr size_t kShoulderRight = 8;
constexpr size_t kElbowRight = 9;
constexpr size_t kHandRight = 11;

// These are the NUI_SKELETON_POSITION_TRACKING_STATE values the rules test for.
constexpr uint32_t kJointNotTracked = 0;
constexpr uint32_t kJointTracked = 2;

// A vector shorter than this has no direction to read.
constexpr float kMinimumLength = 1e-6f;

constexpr float kDegreesPerRadian = 180.0f / std::numbers::pi_v<float>;

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

constexpr uint64_t kMicrosecondsPerMillisecond = 1000;

// A gap tolerance shorter than this would be broken by the ordinary interval
// between two frames, 33 ms at 30 frames a second, and no hold could complete.
constexpr int32_t kMinimumGapMs = 50;

// An ArmJoints names one arm's joints and the joints of the other side that the
// gesture checks the arm against.
struct ArmJoints {
  NuiGuideArm arm;
  size_t shoulder;
  size_t elbow;
  size_t hand;
  size_t other_shoulder;
  size_t other_hand;
};

// The arms are listed in the order SkeletonState keeps their states.
constexpr std::array<ArmJoints, 2> kArms = {{
    {NuiGuideArm::kLeft, kShoulderLeft, kElbowLeft, kHandLeft, kShoulderRight,
     kHandRight},
    {NuiGuideArm::kRight, kShoulderRight, kElbowRight, kHandRight,
     kShoulderLeft, kHandLeft},
}};

// The rules' short names, in the order NuiGuideRule lists them.
struct RuleName {
  NuiGuideRule rule;
  std::string_view name;
};
constexpr std::array<RuleName, 11> kRuleNames = {{
    {kNuiGuideRuleSeen, "seen"},
    {kNuiGuideRuleAngle, "angle"},
    {kNuiGuideRuleExtension, "extension"},
    {kNuiGuideRuleSideways, "sideways"},
    {kNuiGuideRuleTracked, "tracked"},
    {kNuiGuideRuleArm, "arm"},
    {kNuiGuideRuleOtherArmOutOfBand, "other-arm-out-of-band"},
    {kNuiGuideRuleOtherArmAtRest, "other-arm-at-rest"},
    {kNuiGuideRuleOtherHandLowered, "other-hand-lowered"},
    {kNuiGuideRuleArmStill, "arm-still"},
    {kNuiGuideRuleHipStill, "hip-still"},
}};

// The widest angle an arm can make with straight down.
constexpr int32_t kMaximumAngleDegrees = 180;

using Joints = std::span<const NuiGuideVector, kNuiGuideJointCount>;
using TrackingStates = std::span<const uint32_t, kNuiGuideJointCount>;

NuiGuideVector Subtract(const NuiGuideVector& a, const NuiGuideVector& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

NuiGuideVector Scale(const NuiGuideVector& v, float factor) {
  return {v.x * factor, v.y * factor, v.z * factor};
}

float Dot(const NuiGuideVector& a, const NuiGuideVector& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Length(const NuiGuideVector& v) { return std::sqrt(Dot(v, v)); }

bool IsFinite(const NuiGuideVector& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Returns the part of v across the unit vector down, the part seen from above.
NuiGuideVector Horizontal(const NuiGuideVector& v, const NuiGuideVector& down) {
  return Subtract(v, Scale(down, Dot(v, down)));
}

// Returns whether a joint in this tracking state has a position to read,
// tracked or inferred.
bool IsShown(uint32_t tracking_state) {
  return tracking_state != kJointNotTracked;
}

// Returns whether the settings let arm make the gesture.
bool IsArmChosen(NuiGuideArmChoice arms, NuiGuideArm arm) {
  switch (arms) {
    case NuiGuideArmChoice::kLeft:
      return arm == NuiGuideArm::kLeft;
    case NuiGuideArmChoice::kRight:
      return arm == NuiGuideArm::kRight;
    case NuiGuideArmChoice::kEither:
      return true;
  }
  return false;
}

// Returns whether the arm's own shoulder, elbow and hand are tracked rather
// than inferred.
bool IsArmTracked(const ArmJoints& arm, TrackingStates tracking_states) {
  return tracking_states[arm.shoulder] == kJointTracked &&
         tracking_states[arm.elbow] == kJointTracked &&
         tracking_states[arm.hand] == kJointTracked;
}

// Returns the angle between a and b, in degrees, or not a number when either
// is too short to have a direction.
float AngleBetween(const NuiGuideVector& a, const NuiGuideVector& b) {
  const float length_a = Length(a);
  const float length_b = Length(b);
  if (!(length_a >= kMinimumLength && length_b >= kMinimumLength)) {
    return kNotANumber;
  }
  const float cosine =
      std::clamp(Dot(a, b) / (length_a * length_b), -1.0f, 1.0f);
  return std::acos(cosine) * kDegreesPerRadian;
}

// Returns the angle between v and the unit vector axis, in degrees, or not a
// number when v is too short to have a direction.
float AngleFrom(const NuiGuideVector& v, const NuiGuideVector& axis) {
  const float length = Length(v);
  if (!(length >= kMinimumLength)) {
    return kNotANumber;
  }
  const float cosine = std::clamp(Dot(v, axis) / length, -1.0f, 1.0f);
  return std::acos(cosine) * kDegreesPerRadian;
}

// Measures one arm, and the other arm and the hip centre it is checked
// against, from a frame whose gravity axis is down, of unit length.
NuiGuideArmMeasurement MeasureArm(const ArmJoints& arm, Joints joints,
                                  TrackingStates tracking_states,
                                  const NuiGuideVector& down) {
  NuiGuideArmMeasurement measurement;
  measurement.tracked = IsArmTracked(arm, tracking_states);

  // The other arm and the hip centre are measured whether the frame shows
  // this arm or not.
  const NuiGuideVector& other_shoulder = joints[arm.other_shoulder];
  const NuiGuideVector& other_hand = joints[arm.other_hand];
  const NuiGuideVector& hip = joints[kHipCenter];
  const bool other_shoulder_shown =
      IsShown(tracking_states[arm.other_shoulder]) && IsFinite(other_shoulder);
  const bool other_hand_shown =
      IsShown(tracking_states[arm.other_hand]) && IsFinite(other_hand);
  const bool hip_shown = IsShown(tracking_states[kHipCenter]) && IsFinite(hip);
  if (other_shoulder_shown && other_hand_shown) {
    measurement.other_angle_degrees =
        AngleFrom(Subtract(other_hand, other_shoulder), down);
    measurement.other_arm_seen = std::isfinite(measurement.other_angle_degrees);
  }
  if (other_hand_shown && hip_shown) {
    measurement.other_hand_seen = true;
    measurement.other_hand_height = -Dot(Subtract(other_hand, hip), down);
  }
  if (hip_shown) {
    measurement.hip_seen = true;
    measurement.hip = hip;
  }

  if (!IsShown(tracking_states[arm.shoulder]) ||
      !IsShown(tracking_states[arm.elbow]) ||
      !IsShown(tracking_states[arm.hand]) || !other_shoulder_shown) {
    return measurement;
  }
  const NuiGuideVector& shoulder = joints[arm.shoulder];
  const NuiGuideVector& elbow = joints[arm.elbow];
  const NuiGuideVector& hand = joints[arm.hand];
  if (!IsFinite(shoulder) || !IsFinite(elbow) || !IsFinite(hand)) {
    return measurement;
  }

  // Without a direction for the arm or an axis across the body there is
  // nothing to measure.
  const NuiGuideVector reach = Subtract(hand, shoulder);
  const float reach_length = Length(reach);
  const NuiGuideVector outward =
      Horizontal(Subtract(shoulder, other_shoulder), down);
  const float outward_length = Length(outward);
  if (!(reach_length >= kMinimumLength && outward_length >= kMinimumLength)) {
    return measurement;
  }

  measurement.seen = true;
  measurement.hand_from_shoulder = reach;
  measurement.angle_degrees = AngleFrom(reach, down);
  // The arm is at least as long as the reach, which is not zero.
  measurement.extension = reach_length / (Length(Subtract(elbow, shoulder)) +
                                          Length(Subtract(hand, elbow)));
  // An arm that points straight down has no heading.
  const NuiGuideVector reach_across = Horizontal(reach, down);
  const float reach_across_length = Length(reach_across);
  measurement.sideways =
      reach_across_length >= kMinimumLength
          ? Dot(reach_across, outward) / (reach_across_length * outward_length)
          : kNotANumber;
  return measurement;
}

// Measures the trunk's lean from straight up, in degrees, from a frame whose
// gravity axis is down, of unit length; not a number when the frame does not
// show the hip centre and the shoulder centre.
float MeasureLean(Joints joints, TrackingStates tracking_states,
                  const NuiGuideVector& down) {
  const NuiGuideVector& hip = joints[kHipCenter];
  const NuiGuideVector& shoulder_center = joints[kShoulderCenter];
  if (!IsShown(tracking_states[kHipCenter]) ||
      !IsShown(tracking_states[kShoulderCenter]) || !IsFinite(hip) ||
      !IsFinite(shoulder_center)) {
    return kNotANumber;
  }
  return AngleFrom(Subtract(shoulder_center, hip), Scale(down, -1.0f));
}

uint64_t MillisecondsToMicroseconds(int32_t milliseconds) {
  return static_cast<uint64_t>(std::max<int32_t>(milliseconds, 0)) *
         kMicrosecondsPerMillisecond;
}

}  // namespace

std::string FormatNuiGuideRules(uint32_t failed) {
  std::string names;
  for (const RuleName& rule_name : kRuleNames) {
    if (!(failed & rule_name.rule)) {
      continue;
    }
    if (!names.empty()) {
      names += ',';
    }
    names += rule_name.name;
  }
  return names.empty() ? std::string("none") : names;
}

bool ParseNuiGuideArmChoice(std::string_view value, NuiGuideArmChoice* choice) {
  if (xe::utf8::equal_case(value, "left")) {
    *choice = NuiGuideArmChoice::kLeft;
  } else if (xe::utf8::equal_case(value, "right")) {
    *choice = NuiGuideArmChoice::kRight;
  } else if (xe::utf8::equal_case(value, "either")) {
    *choice = NuiGuideArmChoice::kEither;
  } else {
    return false;
  }
  return true;
}

NuiGuideGestureSettings MakeNuiGuideGestureSettings(
    int32_t angle_min_degrees, int32_t angle_max_degrees, int32_t hold_ms,
    int32_t gap_ms, int32_t rest_angle_max_degrees, NuiGuideArmChoice arms) {
  NuiGuideGestureSettings settings;
  settings.angle_min_degrees =
      static_cast<float>(std::min(angle_min_degrees, angle_max_degrees));
  settings.angle_max_degrees =
      static_cast<float>(std::max(angle_min_degrees, angle_max_degrees));
  settings.arms = arms;
  settings.rest_angle_max_degrees = static_cast<float>(
      std::clamp(rest_angle_max_degrees, 0, kMaximumAngleDegrees));
  settings.hold_us = MillisecondsToMicroseconds(hold_ms);
  settings.gap_us = MillisecondsToMicroseconds(std::max(gap_ms, kMinimumGapMs));
  return settings;
}

NuiGuideGesture::NuiGuideGesture(const NuiGuideGestureSettings& settings)
    : settings_(settings) {}

bool NuiGuideGesture::Update(
    uint32_t tracking_id,
    std::span<const NuiGuideVector, kNuiGuideJointCount> joints,
    std::span<const uint32_t, kNuiGuideJointCount> tracking_states,
    const NuiGuideVector& gravity_axis, uint64_t timestamp_us) {
  SkeletonState& skeleton = StateFor(tracking_id);

  const float gravity_length = Length(gravity_axis);
  const bool has_gravity =
      std::isfinite(gravity_length) && gravity_length >= kMinimumLength;
  const NuiGuideVector down = has_gravity
                                  ? Scale(gravity_axis, 1.0f / gravity_length)
                                  : NuiGuideVector{};

  // The trace holds the measurements the rules read, so that what is logged
  // is what was decided on.
  last_trace_ = NuiGuideTrace();
  last_trace_.tracking_id = tracking_id;
  if (has_gravity) {
    last_trace_.lean_degrees = MeasureLean(joints, tracking_states, down);
  }
  for (size_t i = 0; i < kArms.size(); ++i) {
    NuiGuideArmTrace& arm_trace = last_trace_.arms[i];
    if (has_gravity) {
      arm_trace.measurement =
          MeasureArm(kArms[i], joints, tracking_states, down);
    }
    arm_trace.failed = BandRulesFailed(arm_trace.measurement);
    arm_trace.in_band = arm_trace.failed == 0;
  }

  NuiGuideArm fired_arm = NuiGuideArm::kNone;
  for (size_t i = 0; i < kArms.size(); ++i) {
    NuiGuideArmTrace& arm_trace = last_trace_.arms[i];
    const NuiGuideArmMeasurement& measurement = arm_trace.measurement;
    if (!measurement.tracked) {
      arm_trace.failed |= kNuiGuideRuleTracked;
    }
    if (!IsArmChosen(settings_.arms, kArms[i].arm)) {
      arm_trace.failed |= kNuiGuideRuleArm;
    }
    // With both arms in the band the pose is not the gesture, whichever of
    // them the hands' heights would allow.
    if (last_trace_.arms[kArms.size() - 1 - i].in_band) {
      arm_trace.failed |= kNuiGuideRuleOtherArmOutOfBand;
    }
    // The other arm must hang at the side.
    if (!(measurement.other_arm_seen && measurement.other_angle_degrees <=
                                            settings_.rest_angle_max_degrees)) {
      arm_trace.failed |= kNuiGuideRuleOtherArmAtRest;
    }
    // The comparison fails for a height that is not a number.
    if (!(measurement.other_hand_seen &&
          measurement.other_hand_height < 0.0f)) {
      arm_trace.failed |= kNuiGuideRuleOtherHandLowered;
    }
    const ArmView view = !measurement.seen   ? ArmView::kUnseen
                         : arm_trace.in_band ? ArmView::kInBand
                                             : ArmView::kOutOfBand;
    // Both arms advance on every frame, so a completion of one does not stall
    // the other's hold.
    if (AdvanceArm(skeleton.arms[i], view, arm_trace.failed == 0, measurement,
                   timestamp_us, arm_trace) &&
        fired_arm == NuiGuideArm::kNone) {
      fired_arm = kArms[i].arm;
    }
  }

  if (fired_arm == NuiGuideArm::kNone) {
    return false;
  }
  last_fired_arm_ = fired_arm;
  return true;
}

void NuiGuideGesture::Reset() {
  skeletons_.fill(SkeletonState());
  last_fired_arm_ = NuiGuideArm::kNone;
  last_trace_ = NuiGuideTrace();
}

NuiGuideGesture::SkeletonState& NuiGuideGesture::StateFor(
    uint32_t tracking_id) {
  ++update_count_;
  // A free entry's last_update is 0, and every entry in use has fed a call, so
  // the smallest last_update is a free entry if there is one and otherwise the
  // entry fed least recently.
  SkeletonState* claim = &skeletons_[0];
  for (SkeletonState& skeleton : skeletons_) {
    if (skeleton.in_use && skeleton.tracking_id == tracking_id) {
      skeleton.last_update = update_count_;
      return skeleton;
    }
    if (skeleton.last_update < claim->last_update) {
      claim = &skeleton;
    }
  }
  *claim = SkeletonState();
  claim->in_use = true;
  claim->tracking_id = tracking_id;
  claim->last_update = update_count_;
  return *claim;
}

uint32_t NuiGuideGesture::BandRulesFailed(
    const NuiGuideArmMeasurement& measurement) const {
  if (!measurement.seen) {
    return kNuiGuideRuleSeen;
  }
  // Every check passes only when its condition holds, so a value that is not
  // a number fails the check instead of slipping through it.
  uint32_t failed = 0;
  // The arm must point out and down: its angle from straight down must lie in
  // the band.
  if (!(measurement.angle_degrees >= settings_.angle_min_degrees &&
        measurement.angle_degrees <= settings_.angle_max_degrees)) {
    failed |= kNuiGuideRuleAngle;
  }
  // The arm must be straight, with the hand almost as far from the shoulder
  // as the arm is long.
  if (!(measurement.extension >= kMinimumExtension)) {
    failed |= kNuiGuideRuleExtension;
  }
  // The arm must be out to its own side. Seen from above, its heading must lie
  // within 60 degrees of the outward axis; an arm that points straight down
  // has no heading and is not out to the side.
  if (!(measurement.sideways >= kMinimumSidewaysFraction)) {
    failed |= kNuiGuideRuleSideways;
  }
  return failed;
}

bool NuiGuideGesture::AdvanceArm(ArmState& arm, ArmView view, bool qualifies,
                                 const NuiGuideArmMeasurement& measurement,
                                 uint64_t timestamp_us,
                                 NuiGuideArmTrace& trace) const {
  // A completed arm re-arms once it has been seen out of the band for longer
  // than the gap tolerance, counted from the first frame that showed it out.
  // A frame that shows it in the band ends the count; a frame that does not
  // show it, or no frame at all, leaves the count as it is.
  if (!arm.armed) {
    if (view == ArmView::kInBand) {
      arm.leaving = false;
    } else if (view == ArmView::kOutOfBand) {
      if (!arm.leaving || timestamp_us < arm.leaving_since_us) {
        arm.leaving = true;
        arm.leaving_since_us = timestamp_us;
      } else if (timestamp_us - arm.leaving_since_us > settings_.gap_us) {
        arm.armed = true;
        arm.leaving = false;
      }
    }
  }

  // A hold is over once too long has passed since its last qualifying frame,
  // and the next qualifying frame starts a new one.
  if (arm.holding && GapExceeded(arm.last_qualified_us, timestamp_us)) {
    arm.holding = false;
    trace.hold_broken = true;
  }

  // While a hold is under way the arm's direction and the hip centre must
  // stay near where they were on its first frame. A frame on which either has
  // gone further does not qualify, so an arm or a body that moves and stays
  // moved ends the hold once the gap tolerance has gone by, the way an arm out
  // of the band does. The comparisons fail for a value that is not a number.
  if (arm.holding) {
    if (measurement.seen) {
      trace.arm_turned_degrees = AngleBetween(measurement.hand_from_shoulder,
                                              arm.hold_hand_from_shoulder);
      if (!(trace.arm_turned_degrees <= kMaximumArmTurnDegrees)) {
        trace.failed |= kNuiGuideRuleArmStill;
        qualifies = false;
      }
    }
    if (measurement.hip_seen) {
      trace.hip_moved = Length(Subtract(measurement.hip, arm.hold_hip));
      if (!(trace.hip_moved <= kMaximumHipMovement)) {
        trace.failed |= kNuiGuideRuleHipStill;
        qualifies = false;
      }
    }
  }

  // A qualifying frame shows the arm and the hip centre, so a hold's first
  // frame has both to measure from.
  if (qualifies) {
    if (!arm.holding) {
      arm.holding = true;
      arm.hold_start_us = timestamp_us;
      arm.hold_hand_from_shoulder = measurement.hand_from_shoulder;
      arm.hold_hip = measurement.hip;
      trace.hold_started = true;
      trace.arm_turned_degrees = 0.0f;
      trace.hip_moved = 0.0f;
    }
    arm.last_qualified_us = timestamp_us;
  }

  // A hold under way has not outlasted the gap tolerance, so its first frame
  // is not after this one.
  trace.holding = arm.holding;
  if (arm.holding) {
    trace.held_us = timestamp_us - arm.hold_start_us;
  }

  const bool completes = qualifies && arm.armed &&
                         timestamp_us - arm.hold_start_us >= settings_.hold_us;
  if (completes) {
    arm.armed = false;
    arm.leaving = false;
  }
  trace.completed = completes;
  trace.armed = arm.armed;
  return completes;
}

bool NuiGuideGesture::GapExceeded(uint64_t earlier_us, uint64_t now_us) const {
  return now_us < earlier_us || now_us - earlier_us > settings_.gap_us;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
