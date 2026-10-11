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
#include <memory>
#include <thread>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

#include "third_party/catch/include/catch.hpp"

DECLARE_int32(log_level);

namespace xe {
namespace apu {
namespace test {

namespace {

using Clock = std::chrono::steady_clock;

// The audio system's client semaphores hold at most this many counts
// (AudioSystem::kMaximumQueuedFrames).
constexpr int kSemaphoreMaximum = 64;

// One frame at the driver's defaults: 256 samples at 48000 Hz.
constexpr auto kFrame = std::chrono::nanoseconds(5333333);

// How long past a deadline the pacing thread may take to wake up and release,
// and the test thread to look. Loose, so that the tests never flake.
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

// What became of a FakeBackendDriver.
struct FakeBackendDriverFate {
  bool shut_down = false;
  bool destroyed = false;
};

// A driver a backend hands back uninitialised, as XAudio2's and SDL's are,
// whose Initialize succeeds only while the host's device is up.
class FakeBackendDriver final : public AudioDriver {
 public:
  FakeBackendDriver(bool device_up, FakeBackendDriverFate* fate)
      : device_up_(device_up), fate_(fate) {}
  ~FakeBackendDriver() override { fate_->destroyed = true; }

  bool Initialize() override { return device_up_; }
  void Shutdown() override { fate_->shut_down = true; }
  void SubmitFrame(float* samples) override {}
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override {}

 private:
  bool device_up_;
  FakeBackendDriverFate* fate_;
};

}  // namespace

TEST_CASE("Frames submitted at once are released at the real-time rate",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
  REQUIRE(semaphore);
  PacingAudioDriver driver(semaphore.get());
  REQUIRE(driver.Initialize());

  constexpr int kFrames = 20;
  const Clock::time_point start = Clock::now();
  SubmitFrames(&driver, kFrames);

  // Halfway through, about half of them have played: not none, and not all.
  std::this_thread::sleep_until(start + kFrames * kFrame / 2);
  int released = TakeReleased(semaphore.get());
  CHECK(released >= 5);
  CHECK(released <= 15);

  std::this_thread::sleep_until(start + kFrames * kFrame + kMargin);
  released += TakeReleased(semaphore.get());
  CHECK(released == kFrames);

  driver.Shutdown();
}

TEST_CASE("A frame submitted after a long idle is released promptly",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
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
  const int64_t waited_ms = MillisecondsSince(submitted);
  CHECK(result == threading::WaitResult::kSuccess);
  CHECK(waited_ms <= 20);

  driver.Shutdown();
}

TEST_CASE("Pause holds releases and Resume lets them continue",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
  REQUIRE(semaphore);
  PacingAudioDriver driver(semaphore.get());
  REQUIRE(driver.Initialize());

  constexpr int kFrames = 20;
  const Clock::time_point start = Clock::now();
  SubmitFrames(&driver, kFrames);
  std::this_thread::sleep_until(start + 5 * kFrame);
  driver.Pause();
  const int remaining = kFrames - TakeReleased(semaphore.get());
  REQUIRE(remaining > 0);

  // Paused for longer than every remaining frame would have taken to play.
  std::this_thread::sleep_for(kFrames * kFrame + kMargin);
  CHECK(TakeReleased(semaphore.get()) == 0);

  const Clock::time_point resumed = Clock::now();
  driver.Resume();
  // The clock stood still while paused, so the rest still come at the
  // real-time rate rather than all at once.
  std::this_thread::sleep_until(resumed + remaining * kFrame / 2);
  int released = TakeReleased(semaphore.get());
  CHECK(released < remaining);

  std::this_thread::sleep_until(resumed + remaining * kFrame + kMargin);
  released += TakeReleased(semaphore.get());
  CHECK(released == remaining);

  driver.Shutdown();
}

TEST_CASE("Shutdown drops pending frames and returns promptly",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
  REQUIRE(semaphore);
  // 2400 samples at 48000 Hz: 50 ms frames, so that none falls due while the
  // test shuts the driver down.
  constexpr auto kSlowFrame = std::chrono::milliseconds(50);
  PacingAudioDriver driver(semaphore.get(), 48000, 2400);
  REQUIRE(driver.Initialize());

  // The driver does release at this rate while it runs.
  SubmitFrames(&driver, 1);
  REQUIRE(threading::Wait(semaphore.get(), false,
                          std::chrono::milliseconds(1000)) ==
          threading::WaitResult::kSuccess);

  constexpr int kFrames = 5;
  SubmitFrames(&driver, kFrames);
  const Clock::time_point shutdown_started = Clock::now();
  driver.Shutdown();
  // Well inside the first pending frame's 50 ms: the thread is woken, not
  // left to sleep out its deadline.
  CHECK(MillisecondsSince(shutdown_started) <= 25);
  // A frame that fell due before the shutdown, had the test thread stalled
  // after submitting, was released rightly. Only what comes after counts.
  TakeReleased(semaphore.get());

  // Every dropped frame would have been due by now.
  std::this_thread::sleep_for(kFrames * kSlowFrame + kMargin);
  CHECK(TakeReleased(semaphore.get()) == 0);
}

TEST_CASE("A backend driver that initialises is kept", "[apu][pacing]") {
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
  REQUIRE(semaphore);
  FakeBackendDriverFate fate;
  auto backend_driver = std::make_unique<FakeBackendDriver>(true, &fate);
  const AudioDriver* backend_driver_ptr = backend_driver.get();

  bool is_fallback = true;
  std::unique_ptr<AudioDriver> driver = InitializeDriverOrPace(
      std::move(backend_driver), semaphore.get(), 48000, 2, &is_fallback);
  CHECK(driver.get() == backend_driver_ptr);
  CHECK_FALSE(is_fallback);
  CHECK_FALSE(fate.shut_down);
  CHECK_FALSE(fate.destroyed);
}

TEST_CASE("A backend driver without a device gives way to a pacing driver",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
  REQUIRE(semaphore);
  FakeBackendDriverFate fate;
  // Mono at 8000 Hz: a frame of kFrameSamplesMax samples is 1536 of them, so
  // 192 ms, where one of the driver's default 256 would take 32 ms.
  constexpr auto kMonoFrame = std::chrono::milliseconds(192);

  bool is_fallback = false;
  std::unique_ptr<AudioDriver> driver =
      InitializeDriverOrPace(std::make_unique<FakeBackendDriver>(false, &fate),
                             semaphore.get(), 8000, 1, &is_fallback);
  REQUIRE(driver);
  CHECK(is_fallback);
  CHECK(fate.shut_down);
  CHECK(fate.destroyed);

  // The pacing driver runs, at the stream's own frame length.
  static float silence[AudioDriver::kFrameSamplesMax] = {};
  const Clock::time_point start = Clock::now();
  driver->SubmitFrame(silence);
  std::this_thread::sleep_until(start + kMonoFrame / 2);
  CHECK(TakeReleased(semaphore.get()) == 0);
  std::this_thread::sleep_until(start + kMonoFrame + kMargin);
  CHECK(TakeReleased(semaphore.get()) == 1);

  driver->Shutdown();
}

TEST_CASE("A pacing driver stands in when the backend makes no driver",
          "[apu][pacing]") {
  ScopedSilentLog silent_log;
  auto semaphore = threading::Semaphore::Create(0, kSemaphoreMaximum);
  REQUIRE(semaphore);

  bool is_fallback = false;
  std::unique_ptr<AudioDriver> driver =
      InitializeDriverOrPace(nullptr, semaphore.get(), 48000, 2, &is_fallback);
  REQUIRE(driver);
  CHECK(is_fallback);

  static float silence[AudioDriver::kFrameSamplesMax] = {};
  driver->SubmitFrame(silence);
  CHECK(threading::Wait(semaphore.get(), false,
                        std::chrono::milliseconds(1000)) ==
        threading::WaitResult::kSuccess);

  driver->Shutdown();
}

}  // namespace test
}  // namespace apu
}  // namespace xe
