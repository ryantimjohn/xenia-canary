/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_TILT_GATE_H_
#define XENIA_NUI_NUI_TILT_GATE_H_

#include <chrono>
#include <cstdint>

namespace xe {
namespace nui {

// Decides which of a title's tilt requests are handed to the motor.
//
// The guest's auto-tilt driver asks again whenever the head it reads back is
// more than two degrees from where it asked it to go. Once the tilt status
// block carries a measured accelerometer, a head that cannot get there - a
// base that stands at a slant, a motor at the end of its travel - keeps the
// driver asking, and every request that reached the motor would hold the tilt
// thread, and with it the accelerometer reads, for a second or so, or be
// refused by the driver's rate limit.
// A repeat of the target the motor was last given is therefore dropped until
// the motor has had a settle window to get there, and a motor that keeps
// refusing is reported once per window rather than once per request.
//
// Not thread-safe: the one thread that drives the motor owns it.
class TiltRequestGate {
 public:
  using Clock = std::chrono::steady_clock;

  // How long after a SetAngle returns a repeat of its target is dropped.
  static constexpr std::chrono::milliseconds kSettleWindow{3000};

  // True when a request for the given angle, arriving now, should reach the
  // motor: anything except a repeat of the last target inside its window.
  bool ShouldApply(int32_t degrees, Clock::time_point now) const {
    return !has_last_target_ || degrees != last_target_degrees_ ||
           now - last_return_ >= kSettleWindow;
  }

  // Records that SetAngle for the given angle has returned, whether the
  // motor took the command or the driver refused it: a refused repeat inside
  // the window would only be refused again.
  void OnSetAngleReturned(int32_t degrees, Clock::time_point now) {
    has_last_target_ = true;
    last_target_degrees_ = degrees;
    last_return_ = now;
  }

  // True when a refusal happening now should be reported: the first one, and
  // then at most one per settle window.
  bool ShouldReportRefusal(Clock::time_point now) {
    if (has_reported_refusal_ && now - last_refusal_report_ < kSettleWindow) {
      return false;
    }
    has_reported_refusal_ = true;
    last_refusal_report_ = now;
    return true;
  }

 private:
  bool has_last_target_ = false;
  int32_t last_target_degrees_ = 0;
  Clock::time_point last_return_;
  bool has_reported_refusal_ = false;
  Clock::time_point last_refusal_report_;
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_TILT_GATE_H_
