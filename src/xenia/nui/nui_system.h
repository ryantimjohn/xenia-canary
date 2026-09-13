/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_SYSTEM_H_
#define XENIA_NUI_NUI_SYSTEM_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/nui/depth_frame.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace nui {

// Host side of the natural user interface (NUI), better known as the Kinect
// sensor. A backend owns the real device, if there is one, and feeds this class
// the sensor state and the depth frames that the guest gets to see. The kernel
// reaches an instance through Emulator::nui_system(), which may be null.
class NuiSystem {
 public:
  enum class DeviceStatus : uint32_t {
    kNotPresent,
    kInitializing,
    kReady,
    kError,
  };

  // Invoked on every status change, from the backend's thread.
  using DeviceStatusCallback = std::function<void(DeviceStatus)>;

  virtual ~NuiSystem();

  // Name of the backend, as accepted by the --nui option.
  virtual std::string name() const = 0;

  virtual X_STATUS Setup(kernel::KernelState* kernel_state);
  virtual void Shutdown();

  DeviceStatus device_status() const {
    return device_status_.load(std::memory_order_acquire);
  }

  // True when a sensor is attached and ready to be used by the guest.
  bool IsDevicePresent() const {
    return device_status() == DeviceStatus::kReady;
  }

  // Reads the current tilt angle of the sensor. Returns false when there is no
  // device to read it from.
  virtual bool GetTiltDegrees(int32_t* out_degrees) = 0;
  // Moves the sensor's tilt motor, clamping the angle to the [-27, 27] degrees
  // the hardware can reach. Returns false when there is no device or when the
  // user has forbidden tilting with the nui_allow_tilt cvar.
  virtual bool SetTiltDegrees(int32_t degrees) = 0;

  // Sequence number of the most recently published frame, 0 before the first
  // one arrives.
  uint64_t latest_sequence() const {
    return latest_sequence_.load(std::memory_order_acquire);
  }

  // The most recently published frame, or null before the first one arrives.
  std::shared_ptr<const DepthFrame> AcquireLatestDepthFrame() const;

  // Blocks until a frame newer than after_sequence has been published. Returns
  // true when latest_sequence() > after_sequence, false when the timeout
  // elapsed first.
  bool WaitForFrame(uint64_t after_sequence, std::chrono::milliseconds timeout);

  void SetDeviceStatusCallback(DeviceStatusCallback callback);

 protected:
  NuiSystem();

  // Stamps the frame with the next sequence number and the current time, makes
  // it the latest frame and wakes everyone waiting in WaitForFrame(). The
  // backend must not touch the frame again after handing it over.
  void PublishFrame(std::shared_ptr<DepthFrame> frame);

  // Records the new device status, invoking the status callback if the value
  // actually changed.
  void SetDeviceStatus(DeviceStatus status);

  kernel::KernelState* kernel_state_ = nullptr;

 private:
  mutable std::mutex frame_mutex_;
  std::condition_variable frame_published_;
  std::shared_ptr<const DepthFrame> latest_frame_;
  std::atomic<uint64_t> latest_sequence_ = {0};

  std::atomic<DeviceStatus> device_status_ = {DeviceStatus::kNotPresent};

  // Held only while reading or replacing the callback, never while invoking it.
  mutable std::mutex device_status_callback_mutex_;
  DeviceStatusCallback device_status_callback_;
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_SYSTEM_H_
