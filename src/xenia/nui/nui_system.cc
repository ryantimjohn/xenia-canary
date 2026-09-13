/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_system.h"

#include <cstddef>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/nui/nui_flags.h"

namespace xe {
namespace nui {

NuiSystem::NuiSystem() = default;

NuiSystem::~NuiSystem() = default;

X_STATUS NuiSystem::Setup(kernel::KernelState* kernel_state) {
  kernel_state_ = kernel_state;
  // Reading a flag here is also what keeps the linker from dropping
  // nui_flags.cc, and with it the registration of every Kinect cvar, while no
  // backend reads the flags yet.
  if (!cvars::nui_allow_tilt) {
    XELOGI("NUI: tilt motor control is disabled by the nui_allow_tilt cvar.");
  }
  return X_STATUS_SUCCESS;
}

void NuiSystem::Shutdown() {
  // The emulator destroys us right after this returns, so nobody may still be
  // parked on our condition variable or holding a callback into us by then.
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    shutting_down_ = true;
  }
  frame_published_.notify_all();
  std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
  device_status_callback_ = nullptr;
}

std::shared_ptr<const DepthFrame> NuiSystem::AcquireLatestDepthFrame() const {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  return latest_frame_;
}

bool NuiSystem::WaitForFrame(uint64_t after_sequence,
                             std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(frame_mutex_);
  frame_published_.wait_for(lock, timeout, [this, after_sequence]() {
    return shutting_down_ ||
           latest_sequence_.load(std::memory_order_relaxed) > after_sequence;
  });
  // A shutdown wakes waiters without having a frame to hand them.
  return latest_sequence_.load(std::memory_order_relaxed) > after_sequence;
}

void NuiSystem::SetDeviceStatusCallback(DeviceStatusCallback callback) {
  std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
  device_status_callback_ = std::move(callback);
}

void NuiSystem::PublishFrame(std::shared_ptr<DepthFrame> frame) {
  assert_not_null(frame);
  if (!frame) {
    return;
  }
  assert_true(frame->width == DepthFrame::kWidth &&
              frame->height == DepthFrame::kHeight);
  assert_true(frame->depth_mm.size() ==
              static_cast<size_t>(frame->width) * frame->height);
  {
    // Both stamps are taken under the lock so that they cannot disagree about
    // the order in which two publishers got here.
    std::lock_guard<std::mutex> lock(frame_mutex_);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    frame->timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    const uint64_t sequence =
        latest_sequence_.load(std::memory_order_relaxed) + 1;
    frame->sequence = sequence;
    latest_frame_ = std::move(frame);
    latest_sequence_.store(sequence, std::memory_order_release);
  }
  frame_published_.notify_all();
}

void NuiSystem::SetDeviceStatus(DeviceStatus status) {
  if (device_status_.exchange(status, std::memory_order_acq_rel) == status) {
    return;
  }
  // Copy the callback out so that it is invoked without any of our locks held.
  DeviceStatusCallback callback;
  {
    std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
    callback = device_status_callback_;
  }
  if (callback) {
    callback(status);
  }
}

}  // namespace nui
}  // namespace xe
