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
#include <cstdio>
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
  // Releases the waiters and the status callback so that nothing can re-enter
  // this object while it is being torn down. Overrides must call the base.
  virtual void Shutdown();

  // Returns false when a backend thread could still touch this object after
  // Shutdown(); the owner must then leak the object instead of destroying it.
  virtual bool IsSafeToDestroy() const { return true; }

  // The title has started consuming frames; a backend that replays a
  // recording restarts it here so that the recording's first frames are the
  // first the title sees. Called from the guest thread that starts the
  // stream, and once for every stream the title starts, so an override must
  // return at once rather than do the work itself. Does nothing by default.
  virtual void OnGuestStreamStarted();

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
  // The same reading, for a caller that only shows it, such as a debug
  // overlay, and so is no title's tilt call. A backend that starts its tilt
  // machinery when a title first asks for the angle must not start it for this
  // call. By default it is GetTiltDegrees, which is right for a backend with
  // nothing to start.
  virtual bool PeekTiltDegrees(int32_t* out_degrees) {
    return GetTiltDegrees(out_degrees);
  }
  // Moves the sensor's tilt motor, clamping the angle to the [-27, 27] degrees
  // the hardware can reach. Returns false when there is no device or when the
  // user has forbidden tilting with the nui_allow_tilt cvar.
  virtual bool SetTiltDegrees(int32_t degrees) = 0;
  // Reads the gravity vector the sensor's accelerometer last measured into
  // xyz[0..2], in g along the sensor's own axes (see nui_accelerometer.h: a
  // level sensor reads (0, -1, 0)). Returns false, leaving xyz alone, when
  // there is no measured reading - no device, no accelerometer, nothing read
  // yet, or nui_tilt_override in force - and the tilt angle is then all there
  // is to go on. A backend without an accelerometer need not override this.
  virtual bool GetGravityVector(float* xyz) { return false; }
  // True while the tilt motor is on its way somewhere: a move a title asked
  // for has not been handed to the motor yet, or the motor has not finished
  // it. A backend without a motor need not override this.
  virtual bool IsTiltMotorMoving() { return false; }

  // Sequence number of the most recently published frame, 0 before the first
  // one arrives.
  uint64_t latest_sequence() const {
    return latest_sequence_.load(std::memory_order_acquire);
  }

  // The most recently published frame, or null before the first one arrives.
  std::shared_ptr<const DepthFrame> AcquireLatestDepthFrame() const;

  // Blocks until a frame newer than after_sequence has been published. Returns
  // true when latest_sequence() > after_sequence, false when the timeout
  // elapsed or Shutdown() released the waiters first.
  bool WaitForFrame(uint64_t after_sequence, std::chrono::milliseconds timeout);

  // Registers the callback invoked on every status change, replacing whatever
  // was registered before. Once this returns, no invocation of the callback it
  // replaced is running or can start, so the caller is free to destroy
  // whatever that callback captured. Shutdown() gives the same guarantee.
  // Precondition: a callback must never call this itself; it would wait for
  // its own invocation to finish.
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
  // Where the depth recording asked for by the nui_record cvar stands. The
  // decision is taken once, on the first published frame, and kStopped is
  // final: a recording that was finished, refused or never wanted can never be
  // reopened, so a frame that turns up after Shutdown() - a Kinect pump that
  // was abandoned inside the driver, say - cannot truncate a finished file.
  enum class RecordingState : uint32_t {
    kUndecided,
    kRecording,
    kStopped,
  };

  // Appends one published frame to the recording, opening the file named by
  // the nui_record cvar and writing its header on the first frame. Called with
  // record_mutex_ held and frame_mutex_ released.
  void RecordFrame(const DepthFrame& frame);
  // Reports that the recording cannot be written any further and closes it.
  // Called with record_mutex_ held.
  void AbandonRecording();
  // Flushes and closes the recording, if one is open, and stops this system
  // ever recording again.
  void StopRecording();

  mutable std::mutex frame_mutex_;
  std::condition_variable frame_published_;
  std::shared_ptr<const DepthFrame> latest_frame_;
  std::atomic<uint64_t> latest_sequence_ = {0};
  // Set by Shutdown() so that waiters leave instead of parking on an object
  // that is about to be destroyed. Guarded by frame_mutex_.
  bool shutting_down_ = false;

  // Guards the recording below. It is held across a whole publish, so that the
  // frames reach the file in the same order as the sequence numbers they were
  // stamped with; it is taken before frame_mutex_, never after, and the write
  // itself happens with frame_mutex_ let go of so that it cannot hold up a
  // thread waiting in WaitForFrame().
  std::mutex record_mutex_;
  // Read without record_mutex_ to keep a publish free when nothing is being
  // recorded; every change to it is made with the mutex held.
  std::atomic<RecordingState> recording_state_ = {RecordingState::kUndecided};
  FILE* record_file_ = nullptr;
  // The nui_record cvar as it read on the first published frame. Latched so
  // that the recording cannot move to another file halfway through.
  std::string record_path_;
  uint64_t recorded_frame_count_ = 0;

  std::atomic<DeviceStatus> device_status_ = {DeviceStatus::kNotPresent};

  // Held only while reading or replacing the callback, never while invoking it.
  mutable std::mutex device_status_callback_mutex_;
  DeviceStatusCallback device_status_callback_;
  // Number of SetDeviceStatus() calls currently inside the callback, and the
  // variable a thread replacing the callback waits on for them to come out.
  // Guarded by device_status_callback_mutex_.
  uint32_t device_status_callbacks_in_flight_ = 0;
  std::condition_variable device_status_callback_drained_;
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_SYSTEM_H_
