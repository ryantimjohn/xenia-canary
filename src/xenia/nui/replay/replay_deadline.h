/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_REPLAY_REPLAY_DEADLINE_H_
#define XENIA_NUI_REPLAY_REPLAY_DEADLINE_H_

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace xe {
namespace nui {
namespace replay {

// Paces depth replay against one host-clock anchor. All recorded intervals
// contribute to a cumulative deadline, so file reads, frame processing and
// rounded Windows waits do not lengthen every interval in the recording.
class ReplayDeadline {
 public:
  using Clock = std::chrono::steady_clock;

  void Reset() { anchored_ = false; }

  // Called when the first frame of a recording, including after a guest
  // restart, is ready to publish. The first file read is outside the phase.
  void Anchor(Clock::time_point first_frame_ready) {
    deadline_ = first_frame_ready;
    anchored_ = true;
  }

  // Called after reading the next frame. The caller waits for the returned
  // whole milliseconds using its interruptible shutdown-event wait. Rounding
  // up avoids publishing before the microsecond deadline; the next call still
  // uses the original deadline, not the rounded completion time.
  std::chrono::milliseconds WaitForNext(uint64_t previous_recorded_us,
                                        uint64_t recorded_us, bool eof_wrap,
                                        Clock::time_point now) {
    // The caller anchors on the first frame before requesting any next wait.
    if (!anchored_) {
      return std::chrono::milliseconds::zero();
    }
    constexpr uint64_t kDefaultGapUs = 33000;
    constexpr uint64_t kMaximumGapUs = 200000;
    const uint64_t gap_us =
        eof_wrap || recorded_us <= previous_recorded_us
            ? kDefaultGapUs
            : std::min(recorded_us - previous_recorded_us, kMaximumGapUs);
    // Clamp in uint64_t before conversion to signed chrono: a corrupt or
    // discontinuous recording timestamp can otherwise overflow that type.
    deadline_ += std::chrono::microseconds(static_cast<int64_t>(gap_us));
    if (now >= deadline_) {
      return std::chrono::milliseconds::zero();
    }
    return std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now);
  }

 private:
  bool anchored_ = false;
  Clock::time_point deadline_ = {};
};

}  // namespace replay
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_REPLAY_REPLAY_DEADLINE_H_
