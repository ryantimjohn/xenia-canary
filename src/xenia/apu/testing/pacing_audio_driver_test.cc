/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/pacing_audio_driver.h"

#include <chrono>
#include <cstdint>
#include <thread>

#include "xenia/apu/audio_system.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

#define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#include "third_party/catch/include/catch.hpp"

DECLARE_int32(log_level);

namespace xe {
namespace apu {
namespace test {

namespace {

using Clock = std::chrono::steady_clock;

// One frame at the driver's defaults: 256 samples at 48000 Hz.
constexpr auto kFrame = std::chrono::nanoseconds(5333333);

// Allowance for scheduler delays.
constexpr auto kMargin = std::chrono::milliseconds(50);

// The driver announces itself in the log when it starts. That does not belong
// in the test suite's output.
class ScopedSilentLog {
 public:
  ScopedSilentLog() : saved_log_level_(cvars::log_level) {
    cvars::log_level = static_cast<int32_t>(LogLevel::Disabled);
  }

  ~ScopedSilentLog() { cvars::log_level = saved_log_level_; }

 private:
  int32_t saved_log_level_;
};

void SubmitFrames(PacingAudioDriver* driver, int count) {
  static float silence[AudioDriver::kFrameSamplesMax] = {};
  for (int i = 0; i < count; ++i) {
    driver->SubmitFrame(silence);
  }
}

// Takes every count the driver has released so far off the semaphore, and
// returns how many there were.
int TakeReleased(threading::Semaphore* semaphore) {
  int taken = 0;
  while (threading::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         threading::WaitResult::kSuccess) {
    ++taken;
  }
  return taken;
}

int64_t MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               start)
      .count();
}

}  // namespace

TEST_CASE("Frames submitted at once are released at the real-time rate",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  PacingAudioDriver driver(semaphore.get());
  REQUIRE(driver.Initialize());

  constexpr int kFrames = 20;
  const Clock::time_point start = Clock::now();
  SubmitFrames(&driver, kFrames);

  for (int i = 0; i < kFrames; ++i) {
    REQUIRE(threading::Wait(semaphore.get(), false,
                            std::chrono::milliseconds(1000)) ==
            threading::WaitResult::kSuccess);
  }
  CHECK(Clock::now() - start >= (kFrames - 1) * kFrame);
  CHECK(TakeReleased(semaphore.get()) == 0);

  driver.Shutdown();
}

TEST_CASE("A frame submitted after a long idle is released promptly",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  PacingAudioDriver driver(semaphore.get());
  REQUIRE(driver.Initialize());

  // A first frame, so that the driver's last deadline lies long in the past
  // when the next one arrives.
  SubmitFrames(&driver, 1);
  REQUIRE(threading::Wait(semaphore.get(), false,
                          std::chrono::milliseconds(1000)) ==
          threading::WaitResult::kSuccess);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const Clock::time_point submitted = Clock::now();
  SubmitFrames(&driver, 1);
  const threading::WaitResult result =
      threading::Wait(semaphore.get(), false, std::chrono::milliseconds(1000));
  CHECK(result == threading::WaitResult::kSuccess);
  CHECK(Clock::now() - submitted <= kFrame + kMargin);

  driver.Shutdown();
}

TEST_CASE("Pause holds releases and Resume lets them continue",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  PacingAudioDriver driver(semaphore.get());
  REQUIRE(driver.Initialize());

  constexpr int kFrames = 20;
  driver.Pause();
  SubmitFrames(&driver, kFrames);

  // Wait beyond every pending deadline while paused.
  std::this_thread::sleep_for(kFrames * kFrame + kMargin);
  CHECK(TakeReleased(semaphore.get()) == 0);

  const Clock::time_point resumed = Clock::now();
  driver.Resume();
  for (int i = 0; i < kFrames; ++i) {
    REQUIRE(threading::Wait(semaphore.get(), false,
                            std::chrono::milliseconds(1000)) ==
            threading::WaitResult::kSuccess);
  }
  CHECK(Clock::now() - resumed >= (kFrames - 1) * kFrame);
  CHECK(TakeReleased(semaphore.get()) == 0);

  driver.Shutdown();
}

TEST_CASE("Shutdown drops pending frames and returns promptly",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  // A one-second frame separates prompt shutdown from waiting for a deadline.
  constexpr auto kSlowFrame = std::chrono::seconds(1);
  PacingAudioDriver driver(semaphore.get(), 48000, 48000);
  REQUIRE(driver.Initialize());

  // The driver does release at this rate while it runs.
  SubmitFrames(&driver, 1);
  REQUIRE(threading::Wait(semaphore.get(), false,
                          std::chrono::milliseconds(2000)) ==
          threading::WaitResult::kSuccess);

  constexpr int kFrames = 1;
  SubmitFrames(&driver, kFrames);
  const Clock::time_point shutdown_started = Clock::now();
  driver.Shutdown();
  CHECK(MillisecondsSince(shutdown_started) <= 250);
  // A frame that fell due before the shutdown, had the test thread stalled
  // after submitting, was released rightly. Only what comes after counts.
  TakeReleased(semaphore.get());

  // Every dropped frame would have been due by now.
  std::this_thread::sleep_for(kFrames * kSlowFrame + kMargin);
  CHECK(TakeReleased(semaphore.get()) == 0);
}

TEST_CASE("Pacing rejects invalid frame durations", "[apu][pacing]") {
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  uint32_t frequency = 48000;
  uint32_t channel_samples = 1;
  SECTION("Zero frequency") { frequency = 0; }
  SECTION("Zero samples") { channel_samples = 0; }
  SECTION("Zero frequency and samples") {
    frequency = 0;
    channel_samples = 0;
  }
  SECTION("Duration rounds to zero") { frequency = UINT32_MAX; }
  PacingAudioDriver driver(semaphore.get(), frequency, channel_samples);
  CHECK_FALSE(driver.Initialize());
  SubmitFrames(&driver, 1);
  CHECK(TakeReleased(semaphore.get()) == 0);
  driver.Shutdown();
}

}  // namespace test
}  // namespace apu
}  // namespace xe
