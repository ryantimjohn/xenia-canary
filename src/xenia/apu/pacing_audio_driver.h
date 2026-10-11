/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_PACING_AUDIO_DRIVER_H_
#define XENIA_APU_PACING_AUDIO_DRIVER_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {

// An audio driver with no host device behind it. It throws the samples away
// and gives the client's semaphore back one count per frame at the rate a
// device would have played the frame, so a title whose audio client needs a
// driver runs as it would with a working, muted device. The nop backend uses
// it for direct media playback; its registered clients are paced by the audio
// worker and use NopAudioDriver. AudioSystem also falls back to this driver
// when a backend cannot create a client driver, and AudioMediaPlayer when its
// music driver cannot start (InitializeDriverOrPace).
class PacingAudioDriver final : public AudioDriver {
 public:
  explicit PacingAudioDriver(xe::threading::Semaphore* semaphore,
                             uint32_t frequency = kFrameFrequencyDefault,
                             uint32_t channel_samples = kChannelSamplesDefault);
  ~PacingAudioDriver() override;

  // Starts the pacing thread. Starting a driver that already runs is a no-op:
  // AudioMediaPlayer initialises the driver its backend hands it, and the
  // backends differ on whether they already did.
  bool Initialize() override;
  // Stops and joins the pacing thread and drops the frames still pending
  // without releasing them. The audio system drains the semaphore itself.
  void Shutdown() override;

  // Ignores the samples. The frame is due one frame's duration after the later
  // of now and the previous frame's deadline, and its count is released then.
  void SubmitFrame(float* samples) override;
  // Stops the frames' clock: nothing is released while paused, and Resume
  // moves every pending deadline back by the time spent paused.
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override {}

 private:
  using Clock = std::chrono::steady_clock;

  // What the frames' clock reads: the moment of pausing while paused. Called
  // with mutex_ held.
  Clock::time_point ClockNowLocked() const;
  void ThreadMain();

  xe::threading::Semaphore* semaphore_ = nullptr;
  const uint32_t frequency_;
  const uint32_t channel_samples_;
  const Clock::duration frame_duration_;

  std::mutex mutex_;
  bool running_ = false;
  bool paused_ = false;
  Clock::time_point paused_at_;
  Clock::time_point next_deadline_;
  // One entry per submitted frame not yet released, in submission order, so
  // the earliest is at the front.
  std::deque<Clock::time_point> deadlines_;

  std::unique_ptr<xe::threading::Event> wake_event_;
  std::unique_ptr<xe::threading::Thread> thread_;
};

// Initialises a driver that a backend made for a player outside the audio
// system's clients, the way AudioMediaPlayer plays a title's music, and falls
// back as AudioSystem does for a client: when there is no driver, or it does
// not initialise because the host's audio device is unavailable, it is shut
// down and destroyed, and a PacingAudioDriver on the same semaphore takes its
// place, its frames as long as the stream's own, kFrameSamplesMax samples
// across its channels. *out_is_fallback says which of the two came back.
// Returns null only when the pacing driver cannot start either.
std::unique_ptr<AudioDriver> InitializeDriverOrPace(
    std::unique_ptr<AudioDriver> driver, xe::threading::Semaphore* semaphore,
    uint32_t frequency, uint32_t channels, bool* out_is_fallback);

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_PACING_AUDIO_DRIVER_H_
