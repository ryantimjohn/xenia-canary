/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_TILT_CALL_WATCH_H_
#define XENIA_NUI_NUI_TILT_CALL_WATCH_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace xe {
namespace nui {

// How long the Kinect backend gives each kind of call into the sensor before
// it counts as stuck and the tilt thread is given up on. A read comes back in
// milliseconds. A motor command blocks for a second or so by design, and
// beside the pump's frame fetches it has been seen to run past three seconds;
// since the pump never waits for it, a long ceiling costs the depth frames
// nothing, while giving up on a command that was merely slow would cost the
// accelerometer for the rest of the session.
constexpr std::chrono::milliseconds kTiltReadDue{2000};
constexpr std::chrono::milliseconds kTiltCommandDue{10000};
// A motor command slower than this is reported, though not given up on.
constexpr std::chrono::milliseconds kTiltSlowCommand{2000};

// The hand-off between a thread that calls into a driver which can block for
// good and the thread that watches it. One word says what the caller is
// doing: kIdle between calls, the time the call in flight is due back by
// while there is one, and kTakenOver once the watcher has given up on the
// caller. Each call carries its own due time, so a call that blocks by design
// can be given longer than one that should return at once. Every change is a
// compare-exchange, so a call that returns just as the watcher gives up is
// decided one way or the other, never both: either the caller clears its
// stamp first and carries on, or the watcher swaps in kTakenOver first and
// the caller, when its call returns, makes no other and touches nothing.
//
// Due times are in milliseconds and must be positive. A stale due time cannot
// be mistaken for a newer call's: the watcher only acts on a call it has seen
// in flight past its due time, so the caller's next call can only begin after
// that and is due back later still.
class TiltCallWatch {
 public:
  static constexpr int64_t kIdle = 0;
  static constexpr int64_t kTakenOver = -1;

  // Caller, before each call, with the time it must have returned by. False,
  // changing nothing, once the watcher has taken over; the call must then not
  // be made.
  bool BeginCall(int64_t due_ms) {
    int64_t expected = kIdle;
    return stamp_.compare_exchange_strong(
        expected, due_ms, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  // Caller, after each call returns. False when the watcher took over while
  // the call was in flight; the caller must then leave at once.
  bool EndCall() {
    int64_t current = stamp_.load(std::memory_order_acquire);
    if (current <= kIdle) {
      return false;
    }
    return stamp_.compare_exchange_strong(
        current, kIdle, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  // Watcher. When the call in flight is due back by, or kIdle or kTakenOver.
  int64_t CallDueBy() const { return stamp_.load(std::memory_order_acquire); }

  // Watcher. Takes over the call that was due back by observed_due, if it is
  // still the one in flight. True when it did.
  bool TryTakeOver(int64_t observed_due) {
    if (observed_due <= kIdle) {
      return false;
    }
    return stamp_.compare_exchange_strong(observed_due, kTakenOver,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  // Watcher. Takes over the call in flight if it is overdue at now_ms. True
  // when it did.
  bool TakeOverIfOverdue(int64_t now_ms) {
    const int64_t due = CallDueBy();
    if (due <= kIdle || now_ms < due) {
      return false;
    }
    return TryTakeOver(due);
  }

  // Watcher. Takes over whatever the caller is doing. True when a call was in
  // flight at that moment; false when the caller was between calls, in which
  // case its next call is refused and no call of it is left in the driver.
  bool TakeOver() {
    return stamp_.exchange(kTakenOver, std::memory_order_acq_rel) > kIdle;
  }

  bool IsTakenOver() const { return CallDueBy() == kTakenOver; }

  // Makes the watch ready for a new caller. Only while no caller uses it.
  void Reset() { stamp_.store(kIdle, std::memory_order_release); }

 private:
  std::atomic<int64_t> stamp_ = {kIdle};
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_TILT_CALL_WATCH_H_
