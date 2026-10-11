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
#include <cstring>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/nui/depth_recording.h"
#include "xenia/nui/nui_flags.h"

namespace xe {
namespace nui {

NuiSystem::NuiSystem() = default;

NuiSystem::~NuiSystem() {
  // Shutdown() is what normally closes a recording. This only catches an
  // instance that was dropped without one.
  StopRecording();
}

X_STATUS NuiSystem::Setup(kernel::KernelState* kernel_state) {
  kernel_state_ = kernel_state;
  // Reading a flag here is also what keeps the linker from dropping
  // nui_flags.cc, and with it the registration of every Kinect cvar, in a
  // build whose backends are all compiled out.
  if (!cvars::nui_allow_tilt) {
    XELOGI("NUI: tilt motor control is disabled by the nui_allow_tilt cvar.");
  }
  return X_STATUS_SUCCESS;
}

void NuiSystem::Shutdown() {
  // Close the recording for good. A backend normally stops its worker before
  // it calls us, but the Kinect one is allowed to give up on a pump stuck in
  // the sensor's driver, and that pump publishes one more frame if the driver
  // ever lets it go; StopRecording() latches so that frame cannot reopen the
  // file we just finished.
  StopRecording();
  // The emulator destroys us right after this returns, so nobody may still be
  // parked on our condition variable or holding a callback into us by then.
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    shutting_down_ = true;
  }
  frame_published_.notify_all();
  SetDeviceStatusCallback(nullptr);
}

void NuiSystem::OnGuestStreamStarted() {
  // A backend with a sensor behind it has nothing to restart: what it
  // publishes is whatever the sensor is looking at, and the title has just
  // arrived at the newest frame of it.
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
  std::unique_lock<std::mutex> lock(device_status_callback_mutex_);
  device_status_callback_ = std::move(callback);
  // Anyone who was already past the lock in SetDeviceStatus() is still running
  // the callback we just replaced, and our caller is about to destroy what
  // that callback captured. Wait for those invocations to come back out. The
  // wait is unbounded on purpose: a bounded one would hand back the object
  // while a thread is still inside it, which is the bug this guards against.
  device_status_callback_drained_.wait(
      lock, [this]() { return device_status_callbacks_in_flight_ == 0; });
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

  // Recording holds its lock across the stamping as well as the write, so that
  // two publishers cannot put their frames in the file the other way round
  // from their sequence numbers. Once the recording has stopped for good the
  // lock is not even taken, which is also what keeps a publish free on a run
  // that never asked for one.
  std::unique_lock<std::mutex> record_lock(record_mutex_, std::defer_lock);
  std::shared_ptr<const DepthFrame> frame_to_record;
  if (recording_state_.load(std::memory_order_acquire) !=
      RecordingState::kStopped) {
    record_lock.lock();
    frame_to_record = frame;
  }

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

  // 600 KiB of disk write per frame is far too much to make anyone waiting for
  // a frame sit through, so it happens out here with frame_mutex_ let go of.
  if (frame_to_record) {
    RecordFrame(*frame_to_record);
  }
}

void NuiSystem::RecordFrame(const DepthFrame& frame) {
  const RecordingState state = recording_state_.load(std::memory_order_relaxed);
  if (state == RecordingState::kStopped) {
    return;
  }
  if (state == RecordingState::kUndecided) {
    // Whether to record, and where to, is settled here and never looked at
    // again: a cvar that changes under a running recording must not move it to
    // another file or leave this one open with nothing writing to it.
    record_path_ = cvars::nui_record;
    if (record_path_.empty()) {
      recording_state_.store(RecordingState::kStopped,
                             std::memory_order_release);
      return;
    }
    record_file_ = xe::filesystem::OpenFile(xe::to_path(record_path_), "wb");
    if (!record_file_) {
      recording_state_.store(RecordingState::kStopped,
                             std::memory_order_release);
      XELOGE("NUI: could not open {} to record depth frames to.", record_path_);
      return;
    }
    recording_state_.store(RecordingState::kRecording,
                           std::memory_order_release);
    XELOGI("NUI: recording depth frames to {}", record_path_);
    DepthRecordingHeader header = {};
    std::memcpy(header.magic, kDepthRecordingMagic, sizeof(header.magic));
    header.width = frame.width;
    header.height = frame.height;
    if (fwrite(&header, sizeof(header), 1, record_file_) != 1) {
      AbandonRecording();
      return;
    }
  }

  DepthRecordingFrameHeader frame_header = {};
  frame_header.sequence = frame.sequence;
  frame_header.timestamp_us = frame.timestamp_us;
  const size_t sample_bytes = frame.depth_mm.size() * sizeof(uint16_t);
  if (fwrite(&frame_header, sizeof(frame_header), 1, record_file_) != 1 ||
      fwrite(frame.depth_mm.data(), 1, sample_bytes, record_file_) !=
          sample_bytes) {
    AbandonRecording();
    return;
  }
  ++recorded_frame_count_;
}

void NuiSystem::AbandonRecording() {
  XELOGE("NUI: writing to {} failed after {} depth frames; recording stopped.",
         record_path_, recorded_frame_count_);
  recording_state_.store(RecordingState::kStopped, std::memory_order_release);
  fclose(record_file_);
  record_file_ = nullptr;
}

void NuiSystem::StopRecording() {
  std::lock_guard<std::mutex> lock(record_mutex_);
  // Stopping is final. A backend can still publish a frame after this - the
  // Kinect pump abandoned inside the sensor's driver comes back out and
  // publishes the one it was holding - and reopening the file for it would
  // truncate a finished recording of gigabytes down to a single frame.
  const bool was_recording =
      recording_state_.exchange(RecordingState::kStopped,
                                std::memory_order_acq_rel) ==
      RecordingState::kRecording;
  if (!was_recording || !record_file_) {
    return;
  }
  fflush(record_file_);
  fclose(record_file_);
  record_file_ = nullptr;
  XELOGI("NUI: recorded {} depth frames to {}", recorded_frame_count_,
         record_path_);
}

void NuiSystem::SetDeviceStatus(DeviceStatus status) {
  if (device_status_.exchange(status, std::memory_order_acq_rel) == status) {
    return;
  }
  // Copy the callback out so that it is invoked without any of our locks held;
  // it reaches into the kernel, which has locks of its own, and holding ours
  // across that would put an ordering edge between them. Counting the
  // invocation in while the lock is held is what lets
  // SetDeviceStatusCallback() know it has to wait for us.
  DeviceStatusCallback callback;
  {
    std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
    callback = device_status_callback_;
    if (!callback) {
      return;
    }
    ++device_status_callbacks_in_flight_;
  }

  callback(status);

  {
    std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
    --device_status_callbacks_in_flight_;
  }
  device_status_callback_drained_.notify_all();
}

}  // namespace nui
}  // namespace xe
