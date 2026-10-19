/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_QUEUE_TRACE_H_
#define XENIA_APU_AUDIO_QUEUE_TRACE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#include "xenia/apu/apu_flags.h"

namespace xe::apu {

inline bool AudioQueueTraceEnabled() {
  static const bool enabled = cvars::audio_queue_trace;
  return enabled;
}

// Raised by AudioSystem pause and resume. Registered drivers use the epoch to
// exclude paused time and treat the first post-resume submission as priming.
inline std::atomic<uint64_t>& AudioQueuePauseEpoch() {
  static std::atomic<uint64_t> epoch{0};
  return epoch;
}

class AudioQueueLogBudget {
 public:
  AudioQueueLogBudget(uint32_t rows = 600, uint32_t summaries = 16)
      : rows_left_(rows), summaries_left_(summaries) {}
  bool TakeRow() { return Take(rows_left_); }
  bool TakeSummary() { return Take(summaries_left_); }

 private:
  static bool Take(std::atomic<uint32_t>& left) {
    uint32_t value = left.load(std::memory_order_relaxed);
    while (value) {
      if (left.compare_exchange_weak(value, value - 1,
                                     std::memory_order_relaxed))
        return true;
    }
    return false;
  }
  std::atomic<uint32_t> rows_left_;
  std::atomic<uint32_t> summaries_left_;
};

class AudioQueueCheckpointBudget {
 public:
  explicit AudioQueueCheckpointBudget(uint32_t checkpoints = 96)
      : left_(checkpoints) {}
  bool Take() {
    uint32_t value = left_.load(std::memory_order_relaxed);
    while (value) {
      if (left_.compare_exchange_weak(value, value - 1,
                                      std::memory_order_relaxed))
        return true;
    }
    return false;
  }

 private:
  std::atomic<uint32_t> left_;
};

// Per-driver collector. Only the submit/pause thread takes the mutex. XAudio2
// callbacks increment atomics and never timestamp, allocate, or log.
class AudioQueueTrace {
 public:
  static constexpr size_t kSeconds = 600;
  static constexpr size_t kBinCount = kSeconds + 1;  // Last bin is overflow.
  static constexpr size_t kRowsPerShutdown = 8;
  static constexpr uint32_t kCheckpointsPerDriver = 12;
  static constexpr uint64_t kCheckpointPeriodUs = 30000000;
  static constexpr uint32_t kNoQueueSample =
      std::numeric_limits<uint32_t>::max();

  struct Bin {
    uint64_t first_us = 0;
    uint64_t last_us = 0;
    uint64_t attempts = 0;
    uint64_t submitted = 0;
    uint64_t submit_errors = 0;
    uint64_t ratio_errors = 0;
    uint64_t queue_zero = 0;
    uint64_t queue_one = 0;
    uint32_t min_queue_before = kNoQueueSample;
    uint64_t max_success_gap_us = 0;
    uint64_t buffer_ends_sampled = 0;
    uint64_t voice_errors_sampled = 0;
  };

  struct Selection {
    std::array<size_t, kRowsPerShutdown> indexes{};
    size_t count = 0;
    size_t occupied = 0;
    int64_t max_success_gap_bin = -1;
    uint64_t max_success_gap_us = 0;
  };

  struct Checkpoint {
    Bin total;
    uint64_t sample_us = 0;
    size_t sample_bin = 0;
    uint32_t ordinal = 0;
    int64_t max_success_gap_bin = -1;
  };

  AudioQueueTrace(uint64_t driver_id, int32_t client_index, uint64_t start_us,
                  uint64_t pause_epoch)
      : driver_id_(driver_id),
        client_index_(client_index),
        created_us_(start_us),
        pause_epoch_(pause_epoch) {}

  uint64_t driver_id() const { return driver_id_; }
  int32_t client_index() const { return client_index_; }
  uint64_t start_us() const {
    return bin_origin_us_ ? bin_origin_us_ : created_us_;
  }

  void BufferEnded() { buffer_ends_.fetch_add(1, std::memory_order_relaxed); }
  void VoiceErrored() { voice_errors_.fetch_add(1, std::memory_order_relaxed); }

  void Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = true;
    primed_ = false;
    last_success_us_ = 0;
  }
  void Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = false;
    primed_ = false;
    last_success_us_ = 0;
  }

  // now_us is sampled once before the existing conversion and API submit.
  // queued_before is the existing GetState value. A failure has no successful
  // submission gap; ratio_error is meaningful only on successful submits.
  void Submission(uint64_t now_us, uint32_t queued_before, bool success,
                  bool ratio_error, uint64_t pause_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bin_origin_us_) bin_origin_us_ = now_us ? now_us : 1;
    if (client_index_ >= 0 && pause_epoch != pause_epoch_) {
      pause_epoch_ = pause_epoch;
      primed_ = false;
      last_success_us_ = 0;
    }
    Bin& bin = bins_[BinIndex(now_us)];
    Touch(bin, now_us);
    Touch(total_, now_us);
    auto count = [&](Bin& b) {
      ++b.attempts;
      if (success) ++b.submitted;
      else ++b.submit_errors;
      if (ratio_error) ++b.ratio_errors;
      if (primed_ && !paused_) {
        b.min_queue_before = std::min(b.min_queue_before, queued_before);
        b.queue_zero += queued_before == 0;
        b.queue_one += queued_before == 1;
        if (success && last_success_us_ && now_us >= last_success_us_)
          b.max_success_gap_us = std::max(b.max_success_gap_us,
                                         now_us - last_success_us_);
      }
    };
    count(bin);
    count(total_);
    SampleCallbacks(bin, now_us);
    if (success && !paused_) {
      last_success_us_ = now_us;
      primed_ = true;
    }
  }

  // Call after the XAudio2 voice is destroyed, when no callback remains.
  void Finish(uint64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bin_origin_us_) bin_origin_us_ = now_us ? now_us : 1;
    Touch(total_, now_us);
    Bin& bin = bins_[BinIndex(now_us)];
    SampleCallbacks(bin, now_us);
  }

  Bin GetBin(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bins_[index];
  }
  Bin GetTotal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_;
  }

  // Called after the original submit path has finished. The supplied time is
  // the same pre-conversion sample used by Submission, so non-due calls need
  // no second clock read. No logging occurs while the collector mutex is held.
  bool TakeCheckpoint(uint64_t sample_us, AudioQueueCheckpointBudget& budget,
                      Checkpoint* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out || !bin_origin_us_ || paused_ ||
        checkpoint_count_ >= kCheckpointsPerDriver)
      return false;
    const uint64_t since = last_checkpoint_sample_us_
                               ? last_checkpoint_sample_us_
                               : bin_origin_us_;
    if (sample_us < since || sample_us - since < kCheckpointPeriodUs ||
        !budget.Take())
      return false;
    last_checkpoint_sample_us_ = sample_us;
    out->ordinal = ++checkpoint_count_;
    out->sample_us = sample_us;
    out->sample_bin = BinIndex(sample_us);
    out->total = total_;
    out->max_success_gap_bin = -1;
    uint64_t largest_gap = 0;
    for (size_t i = 0; i < bins_.size(); ++i) {
      const uint64_t gap = bins_[i].max_success_gap_us;
      if (gap && (gap > largest_gap ||
                  (gap == largest_gap &&
                   i > static_cast<size_t>(out->max_success_gap_bin)))) {
        largest_gap = gap;
        out->max_success_gap_bin = static_cast<int64_t>(i);
      }
    }
    return true;
  }

  // Fixed-size top-eight selection across the entire lifetime, rather than
  // eight early healthy bins. Ties prefer the later bin. Call after Finish.
  Selection SelectWorstBins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Selection selection;
    for (size_t i = 0; i < bins_.size(); ++i) {
      if (!Occupied(bins_[i])) continue;
      ++selection.occupied;
      if (bins_[i].max_success_gap_us &&
          (bins_[i].max_success_gap_us > selection.max_success_gap_us ||
           (bins_[i].max_success_gap_us == selection.max_success_gap_us &&
            i > static_cast<size_t>(selection.max_success_gap_bin)))) {
        selection.max_success_gap_us = bins_[i].max_success_gap_us;
        selection.max_success_gap_bin = static_cast<int64_t>(i);
      }
      size_t position = 0;
      while (position < selection.count &&
             !Worse(bins_[i], i, bins_[selection.indexes[position]],
                    selection.indexes[position]))
        ++position;
      if (position >= kRowsPerShutdown) continue;
      if (selection.count < kRowsPerShutdown) ++selection.count;
      for (size_t j = selection.count - 1; j > position; --j)
        selection.indexes[j] = selection.indexes[j - 1];
      selection.indexes[position] = i;
    }
    return selection;
  }

 private:
  static bool Occupied(const Bin& bin) {
    return bin.attempts || bin.buffer_ends_sampled ||
           bin.voice_errors_sampled;
  }
  static bool Worse(const Bin& a, size_t a_index, const Bin& b,
                    size_t b_index) {
    // Queue empty after priming and API errors are direct risk signals. A
    // >2-pump gap is also anomalous; smaller gaps rank within healthy bins.
    const bool a_anomaly = a.queue_zero || a.submit_errors || a.ratio_errors ||
                           a.voice_errors_sampled ||
                           a.max_success_gap_us > 10666;
    const bool b_anomaly = b.queue_zero || b.submit_errors || b.ratio_errors ||
                           b.voice_errors_sampled ||
                           b.max_success_gap_us > 10666;
    if (a_anomaly != b_anomaly) return a_anomaly;
    if (a.queue_zero != b.queue_zero) return a.queue_zero > b.queue_zero;
    if (a.submit_errors != b.submit_errors)
      return a.submit_errors > b.submit_errors;
    if (a.voice_errors_sampled != b.voice_errors_sampled)
      return a.voice_errors_sampled > b.voice_errors_sampled;
    if (a.max_success_gap_us != b.max_success_gap_us)
      return a.max_success_gap_us > b.max_success_gap_us;
    if (a.ratio_errors != b.ratio_errors)
      return a.ratio_errors > b.ratio_errors;
    if (a.queue_one != b.queue_one) return a.queue_one > b.queue_one;
    return a_index > b_index;
  }
  size_t BinIndex(uint64_t now_us) const {
    return now_us >= bin_origin_us_
               ? std::min<uint64_t>((now_us - bin_origin_us_) / 1000000, kSeconds)
               : 0;
  }
  static void Touch(Bin& bin, uint64_t now_us) {
    if (!bin.first_us) bin.first_us = now_us;
    bin.last_us = now_us;
  }
  void SampleCallbacks(Bin& bin, uint64_t now_us) {
    const uint64_t ends = buffer_ends_.load(std::memory_order_relaxed);
    const uint64_t errors = voice_errors_.load(std::memory_order_relaxed);
    const uint64_t new_ends = ends - last_buffer_ends_;
    const uint64_t new_errors = errors - last_voice_errors_;
    if (new_ends || new_errors) {
      Touch(bin, now_us);
      Touch(total_, now_us);
    }
    bin.buffer_ends_sampled += new_ends;
    bin.voice_errors_sampled += new_errors;
    total_.buffer_ends_sampled += new_ends;
    total_.voice_errors_sampled += new_errors;
    last_buffer_ends_ = ends;
    last_voice_errors_ = errors;
  }

  const uint64_t driver_id_;
  const int32_t client_index_;
  const uint64_t created_us_;
  uint64_t bin_origin_us_ = 0;
  mutable std::mutex mutex_;
  std::array<Bin, kBinCount> bins_{};
  Bin total_{};
  bool paused_ = false;
  bool primed_ = false;
  uint64_t last_success_us_ = 0;
  uint64_t pause_epoch_ = 0;
  uint64_t last_checkpoint_sample_us_ = 0;
  uint32_t checkpoint_count_ = 0;
  std::atomic<uint64_t> buffer_ends_{0};
  std::atomic<uint64_t> voice_errors_{0};
  uint64_t last_buffer_ends_ = 0;
  uint64_t last_voice_errors_ = 0;
};

}  // namespace xe::apu

#endif  // XENIA_APU_AUDIO_QUEUE_TRACE_H_
