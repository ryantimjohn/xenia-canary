/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_media_player.h"

#include <chrono>
#include <cstdint>
#include <memory>

#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"

#define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace apu {
namespace test {

class AudioMediaPlayerTestAccess {
 public:
  static bool SetupDriver(AudioMediaPlayer& player, uint32_t frequency,
                          uint32_t channels) {
    return player.SetupDriver(frequency, channels);
  }
  static void DeleteDriver(AudioMediaPlayer& player) { player.DeleteDriver(); }
  static AudioDriver* driver(AudioMediaPlayer& player) {
    return player.driver_.get();
  }
  static threading::Semaphore* semaphore(AudioMediaPlayer& player) {
    return player.driver_semaphore_.get();
  }
};

namespace {

int TakeReleased(threading::Semaphore* semaphore) {
  int taken = 0;
  while (threading::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         threading::WaitResult::kSuccess) {
    ++taken;
  }
  return taken;
}

struct DriverState {
  int initialized = 0;
  int shut_down = 0;
  bool destroyed = false;
  bool shutdown_released = false;
  bool destructor_released = false;
};

class FakeBackendDriver final : public AudioDriver {
 public:
  FakeBackendDriver(threading::Semaphore* semaphore, bool initialize_success,
                    DriverState* state)
      : semaphore_(semaphore),
        initialize_success_(initialize_success),
        state_(state) {}
  ~FakeBackendDriver() override {
    state_->destroyed = true;
    state_->destructor_released = semaphore_->Release(1, nullptr);
  }

  bool Initialize() override {
    ++state_->initialized;
    return initialize_success_;
  }
  void Shutdown() override {
    ++state_->shut_down;
    state_->shutdown_released = semaphore_->Release(1, nullptr);
  }
  void SubmitFrame(float* samples) override {}
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override {}

 private:
  threading::Semaphore* semaphore_;
  bool initialize_success_;
  DriverState* state_;
};

class FakeAudioSystem final : public AudioSystem {
 public:
  explicit FakeAudioSystem(cpu::Processor* processor)
      : AudioSystem(processor) {}

  using AudioSystem::CreateClientDriver;
  using AudioSystem::DestroyDriver;

  std::string name() const override { return "test"; }

  X_STATUS CreateDriver(size_t index, threading::Semaphore* semaphore,
                        AudioDriver** out_driver) override {
    *out_driver = CreateDriver(semaphore, 48000, 6, false);
    return creation_status;
  }
  AudioDriver* CreateDriver(threading::Semaphore* semaphore, uint32_t frequency,
                            uint32_t channels,
                            bool need_format_conversion) override {
    ++created;
    if (return_null) {
      return nullptr;
    }
    last_driver = new FakeBackendDriver(semaphore, initialize_success, &state);
    return last_driver;
  }

  bool return_null = false;
  bool initialize_success = true;
  X_STATUS creation_status = X_STATUS_SUCCESS;
  int created = 0;
  AudioDriver* last_driver = nullptr;
  DriverState state;
};

}  // namespace

TEST_CASE("Registered backend failures release slots immediately",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  system.return_null = true;
  SECTION("Failed status") { system.creation_status = X_STATUS_UNSUCCESSFUL; }
  SECTION("Positive error status") {
    system.creation_status = X_ERROR_NOT_FOUND;
  }
  SECTION("Success with no driver") {}
  SECTION("Failed status with a partial driver") {
    system.creation_status = X_STATUS_UNSUCCESSFUL;
    system.return_null = false;
  }
  SECTION("Positive error with a partial driver") {
    system.creation_status = X_ERROR_NOT_FOUND;
    system.return_null = false;
  }

  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  AudioDriver* driver = nullptr;
  REQUIRE(system.CreateClientDriver(0, semaphore.get(), &driver) ==
          X_STATUS_SUCCESS);
  REQUIRE(driver);
  if (!system.return_null) {
    CHECK(system.state.shut_down == 1);
    CHECK(system.state.destroyed);
    CHECK(system.state.shutdown_released);
    CHECK(system.state.destructor_released);
    TakeReleased(semaphore.get());
  }

  float silence[AudioDriver::kFrameSamplesMax] = {};
  for (size_t i = 0; i < 2 * AudioSystem::kMaximumQueuedFrames; ++i) {
    driver->SubmitFrame(silence);
    REQUIRE(
        threading::Wait(semaphore.get(), false, std::chrono::milliseconds(0)) ==
        threading::WaitResult::kSuccess);
  }
  system.DestroyDriver(driver);
}

TEST_CASE("Registered successful drivers use common teardown",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  AudioDriver* driver = nullptr;
  REQUIRE(system.CreateClientDriver(0, semaphore.get(), &driver) ==
          X_STATUS_SUCCESS);
  CHECK(driver == system.last_driver);
  CHECK_FALSE(system.state.destroyed);
  system.DestroyDriver(driver);
  CHECK(system.state.shut_down == 1);
  CHECK(system.state.destroyed);
  CHECK(system.state.shutdown_released);
  CHECK(system.state.destructor_released);
}

TEST_CASE("Media setup retains and initializes successful backends once",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  AudioMediaPlayer player(&system, nullptr);
  REQUIRE(AudioMediaPlayerTestAccess::SetupDriver(player, 48000, 2));
  CHECK(AudioMediaPlayerTestAccess::driver(player) == system.last_driver);
  CHECK(system.state.initialized == 1);
  CHECK(system.state.shut_down == 0);
  CHECK_FALSE(system.state.destroyed);
  CHECK(TakeReleased(AudioMediaPlayerTestAccess::semaphore(player)) ==
        AudioSystem::kMaximumQueuedFrames);

  AudioMediaPlayerTestAccess::DeleteDriver(player);
  CHECK(system.state.shut_down == 1);
  CHECK(system.state.destroyed);
  CHECK(system.state.shutdown_released);
  CHECK(system.state.destructor_released);
  CHECK_FALSE(AudioMediaPlayerTestAccess::driver(player));
  CHECK_FALSE(AudioMediaPlayerTestAccess::semaphore(player));
  AudioMediaPlayerTestAccess::DeleteDriver(player);
  CHECK(system.state.shut_down == 1);
}

TEST_CASE("Media setup paces failed and missing backends at the stream rate",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  system.initialize_success = false;
  SECTION("Failed initialization") {}
  SECTION("Missing backend") { system.return_null = true; }
  AudioMediaPlayer player(&system, nullptr);
  REQUIRE(AudioMediaPlayerTestAccess::SetupDriver(player, 8000, 1));
  if (!system.return_null) {
    CHECK(system.state.initialized == 1);
    CHECK(system.state.shut_down == 1);
    CHECK(system.state.destroyed);
  }
  auto semaphore = AudioMediaPlayerTestAccess::semaphore(player);
  CHECK(TakeReleased(semaphore) == AudioSystem::kMaximumQueuedFrames);
  float silence[AudioDriver::kFrameSamplesMax] = {};
  const auto submitted = std::chrono::steady_clock::now();
  AudioMediaPlayerTestAccess::driver(player)->SubmitFrame(silence);
  // Mono at 8000 Hz consumes 1536 samples in 192 ms.
  CHECK(threading::Wait(semaphore, false, std::chrono::milliseconds(1000)) ==
        threading::WaitResult::kSuccess);
  CHECK(std::chrono::steady_clock::now() - submitted >=
        std::chrono::milliseconds(191));
  AudioMediaPlayerTestAccess::DeleteDriver(player);
  CHECK_FALSE(AudioMediaPlayerTestAccess::driver(player));
  CHECK_FALSE(AudioMediaPlayerTestAccess::semaphore(player));
}

TEST_CASE("Media setup rejects invalid formats before backend creation",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  AudioMediaPlayer player(&system, nullptr);
  uint32_t frequency = 48000;
  uint32_t channels = 2;
  SECTION("Zero sample rate") { frequency = 0; }
  SECTION("Zero channels") { channels = 0; }
  SECTION("No samples per channel") {
    channels = AudioDriver::kFrameSamplesMax + 1;
  }
  REQUIRE_FALSE(
      AudioMediaPlayerTestAccess::SetupDriver(player, frequency, channels));
  CHECK(system.created == 0);
  CHECK_FALSE(AudioMediaPlayerTestAccess::driver(player));
  CHECK_FALSE(AudioMediaPlayerTestAccess::semaphore(player));
}

TEST_CASE("Media setup clears a fallback whose duration rounds to zero",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  system.return_null = true;
  AudioMediaPlayer player(&system, nullptr);
  CHECK_FALSE(AudioMediaPlayerTestAccess::SetupDriver(
      player, UINT32_MAX, AudioDriver::kFrameSamplesMax));
  CHECK_FALSE(AudioMediaPlayerTestAccess::driver(player));
  CHECK_FALSE(AudioMediaPlayerTestAccess::semaphore(player));
}

TEST_CASE(
    "Media setup shuts down the previous driver before rejecting a format",
    "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  FakeAudioSystem system(&processor);
  AudioMediaPlayer player(&system, nullptr);
  REQUIRE(AudioMediaPlayerTestAccess::SetupDriver(player, 48000, 2));
  TakeReleased(AudioMediaPlayerTestAccess::semaphore(player));
  CHECK_FALSE(AudioMediaPlayerTestAccess::SetupDriver(player, 48000, 0));
  CHECK(system.created == 1);
  CHECK(system.state.shut_down == 1);
  CHECK(system.state.destroyed);
  CHECK(system.state.shutdown_released);
  CHECK(system.state.destructor_released);
  CHECK_FALSE(AudioMediaPlayerTestAccess::driver(player));
  CHECK_FALSE(AudioMediaPlayerTestAccess::semaphore(player));
}

TEST_CASE("Nop media factory returns an uninitialized pacing driver",
          "[apu][driver-setup]") {
  cpu::Processor processor(nullptr, nullptr);
  nop::NopAudioSystem system(&processor);
  auto semaphore =
      threading::Semaphore::Create(0, AudioSystem::kMaximumQueuedFrames);
  REQUIRE(semaphore);
  std::unique_ptr<AudioDriver> driver(
      system.CreateDriver(semaphore.get(), 8000, 1, false));
  REQUIRE(driver);
  float silence[AudioDriver::kFrameSamplesMax] = {};
  driver->SubmitFrame(silence);
  CHECK(
      threading::Wait(semaphore.get(), false, std::chrono::milliseconds(250)) ==
      threading::WaitResult::kTimeout);
  REQUIRE(driver->Initialize());
  driver->SubmitFrame(silence);
  CHECK(threading::Wait(semaphore.get(), false,
                        std::chrono::milliseconds(1000)) ==
        threading::WaitResult::kSuccess);
  driver->Shutdown();
  CHECK_FALSE(system.CreateDriver(semaphore.get(), 48000, 0, false));
}

}  // namespace test
}  // namespace apu
}  // namespace xe
