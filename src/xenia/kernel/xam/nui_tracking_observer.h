/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_TRACKING_OBSERVER_H_
#define XENIA_KERNEL_XAM_NUI_TRACKING_OBSERVER_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "xenia/kernel/xam/nui_messages.h"

namespace xe::kernel::xam {

// Private diagnostic data. No guest pointer, position, or tracking ID enters
// the aggregate returned to the logger. Times are steady-clock microseconds,
// except guest_timestamp_ms, which is only compared with the delivered stamp.
class NuiTrackingObserver {
 public:
  struct HudSample {
    uint64_t guest_timestamp_ms = 0;
    std::array<uint32_t, 6> states{};
    std::array<uint32_t, 6> ids{};
    // Filled from the local Guide decision after the handler finishes its
    // original output handling. The message's engaged ID fields are [out].
    uint32_t guide_selected_id = 0;
    bool guide_took = false;
    std::array<uint32_t, 4> joint_states{};  // 0, 1, 2, invalid
    uint32_t quality_nonzero = 0;
    uint32_t nonfinite_vectors = 0;
    uint32_t zero_vectors = 0;
  };

  struct Stats {
    uint64_t source_frames = 0;
    uint64_t sessions = 0;
    uint64_t source_first = 0;
    uint64_t source_last = 0;
    uint64_t source_skipped = 0;
    uint64_t source_resets = 0;
    uint64_t no_buffer = 0;
    uint64_t invalid_buffer = 0;
    uint64_t copied_stamped = 0;
    uint64_t completion_called = 0;
    uint64_t callback_returned = 0;
    uint64_t host_age_unknown = 0;
    uint64_t host_age_max_ms = 0;
    uint64_t delivery_gap_max_ms = 0;
    uint64_t hud_frames = 0;
    std::array<uint64_t, 4> slot_states{};
    std::array<uint64_t, 4> joint_states{};
    uint64_t id_appeared = 0;
    uint64_t id_disappeared = 0;
    uint64_t id_replaced = 0;
    uint64_t tracked_set_changed = 0;
    uint64_t same_set_slot_reassigned = 0;
    uint64_t guide_not_selected = 0;
    uint64_t guide_selected_matched = 0;
    uint64_t guide_selected_unmatched = 0;
    uint64_t guide_selected_ambiguous = 0;
    uint64_t timestamp_exact = 0;
    uint64_t timestamp_ambiguous = 0;
    uint64_t timestamp_unmatched = 0;
    uint64_t hud_lag_max_ms = 0;
    uint64_t hud_lag_unknown = 0;
    uint64_t quality_nonzero = 0;
    uint64_t nonfinite_vectors = 0;
    uint64_t zero_vectors = 0;
  };

  struct Snapshot {
    Stats stats{};
    uint64_t start_us = 0;
    uint64_t end_us = 0;
    uint64_t row_index = 0;  // Zero identifies the first-feed-end total.
  };

  explicit NuiTrackingObserver(bool enabled) : enabled_(enabled) {}
  bool enabled() const { return enabled_; }

  void SessionStarted(uint64_t now_us) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    StartWindow(now_us);
    Count([](Stats& s) { ++s.sessions; });
    last_source_sequence_ = 0;
    last_delivery_us_ = 0;
    stamps_ = {};
    next_stamp_ = 0;
    last_ids_ = {};
    last_tracked_set_ = {};
    last_tracked_slots_ = {};
    have_tracked_set_ = false;
  }

  static HudSample ReadHudSample(const NuiMsg2B002& frame) {
    HudSample sample;
    sample.guest_timestamp_ms = frame.timestamp;
    for (size_t i = 0; i < kNuiSkeletonCount; ++i) {
      const auto& slot = frame.skeletons[i];
      sample.states[i] = slot.tracking_state;
      sample.ids[i] = slot.tracking_id;
      if (sample.states[i] != 2) continue;
      sample.quality_nonzero += slot.quality_flags != 0;
      for (size_t j = 0; j < kNuiSkeletonPositionCount; ++j) {
        const uint32_t joint_state = slot.position_tracking_state[j];
        ++sample.joint_states[std::min<uint32_t>(joint_state, 3)];
        const auto& point = slot.skeleton_positions[j];
        const float x = point.x, y = point.y, z = point.z;
        sample.nonfinite_vectors +=
            !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z);
        sample.zero_vectors += x == 0 && y == 0 && z == 0;
      }
    }
    return sample;
  }

  void SourceFrame(uint64_t sequence, uint64_t now_us) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Count([&](Stats& s) {
      if (!s.source_frames) s.source_first = sequence;
      ++s.source_frames;
      s.source_last = sequence;
      if (last_source_sequence_ && sequence > last_source_sequence_ + 1)
        s.source_skipped += sequence - last_source_sequence_ - 1;
      if (last_source_sequence_ && sequence <= last_source_sequence_)
        ++s.source_resets;
    });
    last_source_sequence_ = sequence;
    StartWindow(now_us);
  }

  enum class Delivery {
    kNoBuffer, kInvalidBuffer, kCopied, kCompletionCalled, kReturned
  };
  void DepthDelivery(Delivery kind, uint64_t now_us, uint64_t host_stamp_us = 0,
                     uint64_t guest_stamp_ms = 0) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    StartWindow(now_us);
    Count([&](Stats& s) {
      switch (kind) {
        case Delivery::kNoBuffer: ++s.no_buffer; break;
        case Delivery::kInvalidBuffer: ++s.invalid_buffer; break;
        case Delivery::kCopied:
          ++s.copied_stamped;
          if (!host_stamp_us || host_stamp_us > now_us) {
            ++s.host_age_unknown;
          } else {
            s.host_age_max_ms =
                std::max(s.host_age_max_ms, (now_us - host_stamp_us) / 1000);
          }
          if (last_delivery_us_ && now_us >= last_delivery_us_)
            s.delivery_gap_max_ms = std::max(
                s.delivery_gap_max_ms, (now_us - last_delivery_us_) / 1000);
          break;
        case Delivery::kCompletionCalled: ++s.completion_called; break;
        case Delivery::kReturned: ++s.callback_returned; break;
      }
    });
    if (kind == Delivery::kCopied) {
      last_delivery_us_ = now_us;
      // A nested HUD call can arrive before the completion callback returns.
      stamps_[next_stamp_++ % stamps_.size()] = {guest_stamp_ms, now_us};
    }
  }

  void Hud(const HudSample& sample, uint64_t now_us) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    StartWindow(now_us);
    uint32_t matches = 0;
    uint64_t copied_us = 0;
    for (const auto& stamp : stamps_) {
      if (stamp.guest_ms && stamp.guest_ms == sample.guest_timestamp_ms) {
        ++matches;
        copied_us = stamp.copied_us;
      }
    }
    std::array<uint32_t, 6> tracked_slots{};
    std::array<uint32_t, 6> tracked_set{};
    size_t tracked_count = 0;
    for (size_t i = 0; i < sample.ids.size(); ++i) {
      if (sample.states[i] != 2 || !sample.ids[i]) continue;
      tracked_slots[i] = sample.ids[i];
      tracked_set[tracked_count++] = sample.ids[i];
    }
    std::sort(tracked_set.begin(), tracked_set.begin() + tracked_count);
    tracked_count = static_cast<size_t>(
        std::unique(tracked_set.begin(), tracked_set.begin() + tracked_count) -
        tracked_set.begin());
    // Clear duplicates left by unique, so the fixed arrays can be compared.
    std::fill(tracked_set.begin() + tracked_count, tracked_set.end(), 0);
    const bool set_changed =
        have_tracked_set_ && tracked_set != last_tracked_set_;
    const bool slots_reassigned = have_tracked_set_ && !set_changed &&
                                  tracked_slots != last_tracked_slots_;
    Count([&](Stats& s) {
      ++s.hud_frames;
      if (matches == 1) ++s.timestamp_exact;
      else if (matches > 1) ++s.timestamp_ambiguous;
      else ++s.timestamp_unmatched;
      if (matches == 1 && now_us >= copied_us)
        s.hud_lag_max_ms =
            std::max(s.hud_lag_max_ms, (now_us - copied_us) / 1000);
      else
        ++s.hud_lag_unknown;
      for (size_t i = 0; i < sample.states.size(); ++i) {
        ++s.slot_states[std::min<uint32_t>(sample.states[i], 3)];
        const uint32_t before = last_ids_[i], after = sample.ids[i];
        if (!before && after) ++s.id_appeared;
        else if (before && !after) ++s.id_disappeared;
        else if (before && after && before != after) ++s.id_replaced;
      }
      for (size_t i = 0; i < sample.joint_states.size(); ++i)
        s.joint_states[i] += sample.joint_states[i];
      s.tracked_set_changed += set_changed;
      s.same_set_slot_reassigned += slots_reassigned;
      if (!sample.guide_took) {
        ++s.guide_not_selected;
      } else {
        uint32_t slot_matches = 0;
        for (size_t i = 0; i < sample.ids.size(); ++i)
          if (sample.guide_selected_id &&
              sample.ids[i] == sample.guide_selected_id &&
              sample.states[i] == 2)
            ++slot_matches;
        if (slot_matches == 1) ++s.guide_selected_matched;
        else if (slot_matches > 1) ++s.guide_selected_ambiguous;
        else ++s.guide_selected_unmatched;
      }
      s.quality_nonzero += sample.quality_nonzero;
      s.nonfinite_vectors += sample.nonfinite_vectors;
      s.zero_vectors += sample.zero_vectors;
    });
    last_ids_ = sample.ids;
    last_tracked_set_ = tracked_set;
    last_tracked_slots_ = tracked_slots;
    have_tracked_set_ = true;
  }

  // Called only by the feed thread, after any guest callback has returned.
  // The copy leaves the observer mutex before formatting or logging.
  std::optional<Snapshot> TakeRow(uint64_t now_us) {
    if (!enabled_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!window_start_us_ || now_us < window_start_us_ + 1000000)
      return std::nullopt;
    const uint64_t start_us = window_start_us_;
    window_start_us_ = now_us;
    if (rows_ >= 360) {
      row_ = {};
      return std::nullopt;
    }
    ++rows_;
    Snapshot result{row_, start_us, now_us, rows_};
    row_ = {};
    return result;
  }

  // Only the first feed end prints a cumulative snapshot. All later session
  // counters remain available through Summary and the bounded periodic rows.
  std::optional<Snapshot> TakeFirstFeedEnd(uint64_t now_us) {
    if (!enabled_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    if (first_end_logged_) return std::nullopt;
    first_end_logged_ = true;
    return Snapshot{total_, observer_start_us_, now_us, 0};
  }

  Stats Summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_;
  }

 private:
  template <typename F> void Count(F&& f) { f(row_); f(total_); }
  void StartWindow(uint64_t now_us) {
    if (!window_start_us_) window_start_us_ = now_us ? now_us : 1;
    if (!observer_start_us_) observer_start_us_ = window_start_us_;
  }

  const bool enabled_;
  mutable std::mutex mutex_;
  Stats row_{};
  Stats total_{};
  uint64_t window_start_us_ = 0;
  uint64_t observer_start_us_ = 0;
  uint64_t rows_ = 0;
  bool first_end_logged_ = false;
  uint64_t last_source_sequence_ = 0;
  uint64_t last_delivery_us_ = 0;
  struct Stamp { uint64_t guest_ms = 0; uint64_t copied_us = 0; };
  std::array<Stamp, 16> stamps_{};
  size_t next_stamp_ = 0;
  std::array<uint32_t, 6> last_ids_{};
  std::array<uint32_t, 6> last_tracked_set_{};
  std::array<uint32_t, 6> last_tracked_slots_{};
  bool have_tracked_set_ = false;
};

// The switch is captured once for the process lifetime; no public object or
// guest ABI gains a field. This instance is shared by the camera and HUD TUs.
NuiTrackingObserver& GetNuiTrackingObserver();

}  // namespace xe::kernel::xam

#endif  // XENIA_KERNEL_XAM_NUI_TRACKING_OBSERVER_H_
