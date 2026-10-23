// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private diagnostic; never upstream with SDK-derived work.
#ifndef XENIA_APU_AUDIO_PUMP_OBSERVER_H_
#define XENIA_APU_AUDIO_PUMP_OBSERVER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace xe::apu::pump_observer {

constexpr uint64_t kWindowUs = 5000000;
constexpr uint32_t kMaxWindows = 120;
constexpr uint32_t kMaxSessions = 16;
constexpr uint32_t kMaxLifecycleRows = 64;

struct LifecycleClaim {
  uint64_t serial = 0;
  bool emit = false;
  bool cutoff = false;
};

// Registration serials distinguish reuse of a client slot across intact
// lifecycle rows. Nonregistration claims also cover whole-owner invalidation
// on Restore/Shutdown. After the one cutoff row, later identity is explicitly
// unknown rather than inferred from slot, callback, or guest heap pointer.
class LifecycleBudget {
 public:
  LifecycleClaim Claim(bool registration) {
    const uint64_t row = observed_.fetch_add(1, std::memory_order_relaxed);
    if (row > kMaxLifecycleRows) return {};
    if (row == kMaxLifecycleRows) return {0, true, true};
    return {registration ? serial_.fetch_add(1, std::memory_order_relaxed) + 1
                         : 0,
            true, false};
  }
  uint64_t observed() const {
    return observed_.load(std::memory_order_relaxed);
  }
  bool identity_unknown() const { return observed() > kMaxLifecycleRows; }

 private:
  std::atomic<uint64_t> observed_{0};
  std::atomic<uint64_t> serial_{0};
};

struct WindowClaim {
  bool emit = false;
  bool final = false;
};

// All workers in the process share the output cap, even if an AudioSystem is
// replaced. The worker-local Collector remains the bounded window calculator.
class WindowBudget {
 public:
  bool ClaimSession() {
    return sessions_.fetch_add(1, std::memory_order_relaxed) < kMaxSessions;
  }
  WindowClaim Claim() {
    const uint32_t row = emitted_.fetch_add(1, std::memory_order_relaxed);
    return {row < kMaxWindows, row + 1 == kMaxWindows};
  }
  bool exhausted() const {
    return emitted_.load(std::memory_order_relaxed) >= kMaxWindows;
  }

 private:
  std::atomic<uint32_t> sessions_{0};
  std::atomic<uint32_t> emitted_{0};
};

enum class Outcome : uint8_t {
  kNoCallback,
  kInterrupted,
  kTokenTimeout,
  kTokenOther,
  kDriverMissing,
  kExecuted,
  kCount,
};

struct Stamp {
  uint64_t steady_us = 0;
  uint64_t qpc = 0;
  uint64_t query_cost_ticks = 0;
  bool present = false;
};

struct Attempt {
  uint64_t selected_now_us = 0;
  uint64_t deadline_us = 0;
  uint64_t scheduled_next_us = 0;
  uint32_t client_index = 0;
  uint32_t callback = 0;
  uint32_t wrapped_callback_arg = 0;
  Outcome outcome = Outcome::kNoCallback;
  Stamp selected, wake, callback_begin, callback_end;
  bool logging_adjacent = false;
};

struct Detail {
  Attempt attempt{};
  bool present = false;
};

struct Report {
  uint64_t window_start_us = 0;
  uint64_t window_end_us = 0;
  uint64_t first_selected_qpc = 0;
  uint64_t last_selected_qpc = 0;
  uint64_t skipped_empty_windows = 0;
  uint64_t selected = 0;
  uint64_t stamp_bracket_ticks = 0;
  uint64_t explicit_qpc_queries = 0;
  uint64_t invalid_qpc_stamps = 0;
  uint64_t logging_adjacent = 0;
  std::array<uint64_t, static_cast<std::size_t>(Outcome::kCount)> outcomes{};
  Detail worst_selected, worst_wake, worst_callback, worst_token_timeout;
  bool cap_reached = false;
};

constexpr uint64_t PositiveLateness(uint64_t at_us, uint64_t deadline_us) {
  return at_us > deadline_us ? at_us - deadline_us : 0;
}

// A single worker owns this fixed-size collector. Observe never logs, blocks,
// or retains a callback/driver pointer. A returned report describes only the
// completed preceding window; the current window is deliberately not a result.
class Collector {
 public:
  bool Observe(const Attempt& attempt, Report* completed) {
    if (exhausted_) return false;
    if (!active_) {
      Start(attempt.selected_now_us);
    }
    bool has_completed = false;
    if (attempt.selected_now_us >= current_.window_end_us) {
      if (completed) {
        *completed = current_;
        ++reports_emitted_;
        completed->cap_reached = reports_emitted_ == kMaxWindows;
        has_completed = true;
      } else {
        // A caller without a report sink cannot silently certify coverage.
        exhausted_ = true;
        return false;
      }
      if (reports_emitted_ == kMaxWindows) {
        exhausted_ = true;
        return true;
      }
      const uint64_t elapsed = attempt.selected_now_us -
                               current_.window_start_us;
      const uint64_t skipped = elapsed / kWindowUs - 1;
      Start(attempt.selected_now_us - elapsed % kWindowUs);
      current_.skipped_empty_windows = skipped;
    }
    Add(attempt);
    return has_completed;
  }

  bool exhausted() const { return exhausted_; }
  uint32_t reports_emitted() const { return reports_emitted_; }

 private:
  void Start(uint64_t start_us) {
    current_ = {};
    current_.window_start_us = start_us;
    current_.window_end_us = start_us + kWindowUs;
    active_ = true;
  }
  static void KeepLarger(Detail& slot, const Attempt& attempt,
                         uint64_t candidate, uint64_t previous) {
    if (!slot.present || candidate > previous) {
      slot = {attempt, true};
    }
  }
  static uint64_t CallbackDuration(const Attempt& attempt) {
    return attempt.callback_begin.present && attempt.callback_end.present &&
                   attempt.callback_end.steady_us >=
                       attempt.callback_begin.steady_us
               ? attempt.callback_end.steady_us -
                     attempt.callback_begin.steady_us
               : 0;
  }
  void Add(const Attempt& attempt) {
    ++current_.selected;
    ++current_.outcomes[static_cast<std::size_t>(attempt.outcome)];
    current_.logging_adjacent += attempt.logging_adjacent;
    if (current_.selected == 1) {
      current_.first_selected_qpc = attempt.selected.qpc;
    }
    current_.last_selected_qpc = attempt.selected.qpc;
    for (const Stamp* stamp : {&attempt.selected, &attempt.wake,
                               &attempt.callback_begin,
                               &attempt.callback_end}) {
      if (!stamp->present) continue;
      current_.explicit_qpc_queries += 2;
      current_.stamp_bracket_ticks += stamp->query_cost_ticks;
      current_.invalid_qpc_stamps += stamp->qpc == 0;
    }
    // next_pump_us starts at zero for a new/restored client. This is an
    // unarmed deadline, not a large lateness relative to the steady epoch.
    if (attempt.deadline_us) {
      KeepLarger(current_.worst_selected, attempt,
                 PositiveLateness(attempt.selected_now_us,
                                  attempt.deadline_us),
                 PositiveLateness(
                     current_.worst_selected.attempt.selected_now_us,
                     current_.worst_selected.attempt.deadline_us));
    }
    if (attempt.deadline_us && attempt.wake.present) {
      KeepLarger(current_.worst_wake, attempt,
                 PositiveLateness(attempt.wake.steady_us, attempt.deadline_us),
                 PositiveLateness(current_.worst_wake.attempt.wake.steady_us,
                                  current_.worst_wake.attempt.deadline_us));
    }
    if (attempt.callback_begin.present && attempt.callback_end.present) {
      KeepLarger(current_.worst_callback, attempt, CallbackDuration(attempt),
                 CallbackDuration(current_.worst_callback.attempt));
    }
    if (attempt.deadline_us && attempt.wake.present &&
        attempt.outcome == Outcome::kTokenTimeout) {
      KeepLarger(
          current_.worst_token_timeout, attempt,
          PositiveLateness(attempt.wake.steady_us, attempt.deadline_us),
          PositiveLateness(
              current_.worst_token_timeout.attempt.wake.steady_us,
              current_.worst_token_timeout.attempt.deadline_us));
    }
  }
  Report current_{};
  uint32_t reports_emitted_ = 0;
  bool active_ = false;
  bool exhausted_ = false;
};

}  // namespace xe::apu::pump_observer
#endif  // XENIA_APU_AUDIO_PUMP_OBSERVER_H_
