/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/pacing_audio_driver.h"

#include <algorithm>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"

namespace xe {
namespace apu {

namespace {

// A frame of channel_samples samples per channel lasts channel_samples /
// frequency seconds. This is the only place that rate is worked out.
std::chrono::steady_clock::duration FrameDuration(uint32_t frequency,
                                                  uint32_t channel_samples) {
  // There is no rate to keep without a frequency; rather than divide by zero,
  // such frames take no time.
  assert_not_zero(frequency);
  if (!frequency) {
    return std::chrono::steady_clock::duration::zero();
  }
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::nanoseconds(uint64_t(channel_samples) * 1000000000u /
                               frequency));
}

}  // namespace

PacingAudioDriver::PacingAudioDriver(xe::threading::Semaphore* semaphore,
                                     uint32_t frequency,
                                     uint32_t channel_samples)
    : semaphore_(semaphore),
      frequency_(frequency),
      channel_samples_(channel_samples),
      frame_duration_(FrameDuration(frequency, channel_samples)),
      wake_event_(xe::threading::Event::CreateAutoResetEvent(false)) {
  assert_not_null(semaphore_);
}

PacingAudioDriver::~PacingAudioDriver() { Shutdown(); }

bool PacingAudioDriver::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_) {
    return true;
  }
  if (!wake_event_) {
    return false;
  }
  running_ = true;
  // The thread takes mutex_ first thing, so it waits for this to return.
  thread_ = xe::threading::Thread::Create({}, [this]() { ThreadMain(); });
  if (!thread_) {
    running_ = false;
    return false;
  }
  thread_->set_name("APU Pacing Driver");
  XELOGI(
      "APU: pacing client without a host device at {} Hz, {} samples per "
      "frame",
      frequency_, channel_samples_);
  return true;
}

void PacingAudioDriver::Shutdown() {
  std::unique_ptr<xe::threading::Thread> thread;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    deadlines_.clear();
    thread = std::move(thread_);
  }
  if (thread) {
    wake_event_->Set();
    xe::threading::Wait(thread.get(), false);
  }
}

void PacingAudioDriver::SubmitFrame(float* samples) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_) {
    return;
  }
  next_deadline_ = std::max(ClockNowLocked(), next_deadline_) + frame_duration_;
  deadlines_.push_back(next_deadline_);
  // Deadlines only ever grow, so the thread's wake-up time changes only when
  // this frame is the only one pending.
  if (deadlines_.size() == 1) {
    wake_event_->Set();
  }
}

void PacingAudioDriver::Pause() {
  // The thread looks at paused_ before it releases anything, so it need not
  // be woken: it finds the driver paused at its next deadline and then sleeps
  // until Resume wakes it.
  std::lock_guard<std::mutex> lock(mutex_);
  if (paused_) {
    return;
  }
  paused_ = true;
  paused_at_ = Clock::now();
}

void PacingAudioDriver::Resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!paused_) {
    return;
  }
  const Clock::duration paused_for = Clock::now() - paused_at_;
  for (Clock::time_point& deadline : deadlines_) {
    deadline += paused_for;
  }
  next_deadline_ += paused_for;
  paused_ = false;
  if (running_) {
    wake_event_->Set();
  }
}

PacingAudioDriver::Clock::time_point PacingAudioDriver::ClockNowLocked() const {
  return paused_ ? paused_at_ : Clock::now();
}

void PacingAudioDriver::ThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
    if (!paused_) {
      const Clock::time_point now = Clock::now();
      while (!deadlines_.empty() && deadlines_.front() <= now) {
        deadlines_.pop_front();
        // Released with mutex_ held, so that once Pause or Shutdown returns
        // no frame is released behind its back.
        auto released = semaphore_->Release(1, nullptr);
        assert_true(released);
      }
      if (!deadlines_.empty()) {
        timeout = std::chrono::ceil<std::chrono::milliseconds>(
            deadlines_.front() - now);
      }
    }
    lock.unlock();
    xe::threading::Wait(wake_event_.get(), false, timeout);
    lock.lock();
  }
}

std::unique_ptr<AudioDriver> InitializeDriverOrPace(
    std::unique_ptr<AudioDriver> driver, xe::threading::Semaphore* semaphore,
    uint32_t frequency, uint32_t channels, bool* out_is_fallback) {
  *out_is_fallback = false;
  if (driver) {
    if (driver->Initialize()) {
      return driver;
    }
    driver->Shutdown();
    driver.reset();
  }

  // A frame holds kFrameSamplesMax samples whatever its channel count, as the
  // XAudio2 and SDL drivers lay it out: 256 per channel for 5.1, 768 for
  // stereo.
  const uint32_t channel_samples =
      channels ? AudioDriver::kFrameSamplesMax / channels
               : AudioDriver::kChannelSamplesDefault;
  auto pacing_driver = std::make_unique<PacingAudioDriver>(semaphore, frequency,
                                                           channel_samples);
  if (!pacing_driver->Initialize()) {
    return nullptr;
  }
  *out_is_fallback = true;
  return pacing_driver;
}

}  // namespace apu
}  // namespace xe
