/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_system.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "xenia/nui/depth_frame.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

// A minimal concrete NuiSystem with no backend and no KernelState, so the
// lifecycle behaviour inherited from the base class can be exercised
// directly. PublishFrame and SetDeviceStatus are protected on the base
// class; the using-declarations below re-expose them to this test file only.
class TestNuiSystem : public NuiSystem {
 public:
  std::string name() const override { return "Test"; }

  bool GetTiltDegrees(int32_t* out_degrees) override { return false; }
  bool SetTiltDegrees(int32_t degrees) override { return false; }

  using NuiSystem::PublishFrame;
  using NuiSystem::SetDeviceStatus;
};

namespace {

// A valid, if blank, 640x480 depth frame ready to hand to PublishFrame.
std::shared_ptr<DepthFrame> MakeFrame() {
  auto frame = std::make_shared<DepthFrame>();
  frame->depth_mm.assign(static_cast<size_t>(frame->width) * frame->height, 0);
  return frame;
}

}  // namespace

TEST_CASE("NuiSystem publishes frames with increasing sequences",
          "[nui][nui_system]") {
  TestNuiSystem system;
  CHECK(system.latest_sequence() == 0);
  CHECK(system.AcquireLatestDepthFrame() == nullptr);

  system.PublishFrame(MakeFrame());
  CHECK(system.latest_sequence() == 1);
  const auto first_frame = system.AcquireLatestDepthFrame();
  REQUIRE(first_frame != nullptr);
  CHECK(first_frame->sequence == 1);

  system.PublishFrame(MakeFrame());
  CHECK(system.latest_sequence() == 2);
  const auto second_frame = system.AcquireLatestDepthFrame();
  REQUIRE(second_frame != nullptr);
  CHECK(second_frame->sequence == 2);
  CHECK(second_frame != first_frame);
  CHECK(second_frame->timestamp_us >= first_frame->timestamp_us);
}

TEST_CASE("NuiSystem::WaitForFrame reports new frames and timeouts",
          "[nui][nui_system]") {
  using namespace std::chrono_literals;
  TestNuiSystem system;

  // Nothing has been published yet, so this must time out and return false.
  CHECK_FALSE(system.WaitForFrame(0, 50ms));

  system.PublishFrame(MakeFrame());
  CHECK(system.WaitForFrame(0, 50ms));
}

TEST_CASE("NuiSystem::Shutdown wakes a thread blocked in WaitForFrame",
          "[nui][nui_system]") {
  using namespace std::chrono_literals;
  TestNuiSystem system;

  std::atomic<bool> wait_result{true};
  std::thread waiter(
      [&system, &wait_result] { wait_result = system.WaitForFrame(0, 300ms); });

  // Give the waiter time to actually park inside WaitForFrame before
  // shutting down, so this exercises notify_all() waking a blocked waiter
  // rather than racing Shutdown() against the waiter thread's startup.
  std::this_thread::sleep_for(30ms);

  const auto shutdown_start = std::chrono::steady_clock::now();
  system.Shutdown();
  waiter.join();
  const auto shutdown_duration =
      std::chrono::steady_clock::now() - shutdown_start;

  CHECK_FALSE(wait_result.load());
  // A wake from Shutdown() should be near-instant. If this took anywhere
  // close to the full 300ms timeout, notify_all() did not reach the waiter
  // and it only returned because the timeout itself elapsed.
  CHECK(shutdown_duration < 250ms);
}

TEST_CASE("NuiSystem::Shutdown clears the device status callback",
          "[nui][nui_system]") {
  TestNuiSystem system;

  int callback_invocations = 0;
  system.SetDeviceStatusCallback(
      [&callback_invocations](NuiSystem::DeviceStatus) {
        ++callback_invocations;
      });

  system.SetDeviceStatus(NuiSystem::DeviceStatus::kReady);
  CHECK(callback_invocations == 1);

  system.Shutdown();
  system.SetDeviceStatus(NuiSystem::DeviceStatus::kError);
  // The callback was cleared by Shutdown(), so this second status change
  // must not reach it.
  CHECK(callback_invocations == 1);
}

}  // namespace test
}  // namespace nui
}  // namespace xe
