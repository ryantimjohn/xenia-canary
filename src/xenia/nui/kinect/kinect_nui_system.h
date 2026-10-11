/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_KINECT_KINECT_NUI_SYSTEM_H_
#define XENIA_NUI_KINECT_KINECT_NUI_SYSTEM_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/base/threading.h"
#include "xenia/nui/depth_frame_pool.h"
#include "xenia/nui/nui_system.h"
#include "xenia/nui/nui_tilt_call_watch.h"
#include "xenia/nui/nui_tilt_gate.h"
#include "xenia/nui/nui_tilt_request_slot.h"

namespace xe {
namespace nui {
namespace kinect {

namespace api {
struct Exports;
struct ISensor;
}  // namespace api

// A NUI system backed by a real Kinect sensor through the Kinect for Windows
// runtime. The runtime is loaded by name at startup, so a host without it - or
// without a sensor plugged in - simply reports that no device is present.
//
// Nothing that talks to the runtime happens on a guest thread. The depth
// pump opens the sensor, which takes seconds, reads the frames, which block,
// and makes the session's one NuiCameraElevationGetAngle call. Everything the
// tilt motor does is on a second thread, the tilt thread: a motor command
// blocks for a second or so whether or not the head moves, and has been seen
// to take over three beside the frame fetches, and the accelerometer sits on
// the same device, so neither may hold up a depth frame. The pump starts the
// tilt thread once the sensor is ready and a tilt call has asked for it, never
// waits on it, and leaves it behind if a call into the sensor stops returning.
// Until a tilt call asks, there is no tilt thread, so the motor is not moved
// and the accelerometer is not read: a title that makes no tilt call - any
// title but a Kinect one - gets only the angle read when the sensor opens. The
// request lasts for the process, so a title opened after one that asked finds
// the thread running. The title's tilt calls ask; PeekTiltDegrees, used by the
// depth preview and play frame log, does not.
class KinectNuiSystem : public NuiSystem {
 public:
  KinectNuiSystem();
  ~KinectNuiSystem() override;

  // True when the Kinect for Windows runtime is installed. A sensor does not
  // have to be attached: the backend keeps probing for one while it runs.
  static bool IsAvailable();

  static std::unique_ptr<NuiSystem> Create();

  std::string name() const override { return "Kinect for Windows SDK"; }

  X_STATUS Setup(kernel::KernelState* kernel_state) override;
  void Shutdown() override;

  // False once the pump or the tilt thread has been abandoned: that thread is
  // still inside the driver and will come back to touch this object if the
  // call ever returns.
  bool IsSafeToDestroy() const override {
    return !worker_abandoned_.load(std::memory_order_acquire) &&
           !tilt_abandoned_.load(std::memory_order_acquire);
  }

  // The angle the accelerometer measures once it has been read, and the angle
  // the motor last accepted (or the one read back when the sensor opened)
  // until then. PeekTiltDegrees gives the same angle without asking for the
  // tilt thread.
  bool GetTiltDegrees(int32_t* out_degrees) override;
  bool PeekTiltDegrees(int32_t* out_degrees) override;
  bool SetTiltDegrees(int32_t degrees) override;
  bool GetGravityVector(float* xyz) override;
  bool IsTiltMotorMoving() override;

 protected:
  // Whether a tilt call has asked for the tilt thread (tilt_wanted_). Only a
  // test needs it: without a sensor there is no thread to see start.
  bool tilt_wanted() const {
    return tilt_wanted_.load(std::memory_order_acquire);
  }

 private:
  // How often the pump says that it is still alive, in frames.
  static constexpr uint64_t kFrameLogInterval = 300;

  bool ResolveExports();
  void ReleaseRuntime();

  // Reports a failure to bring the sensor up, quietly when it is the same
  // failure we have already reported.
  void LogOpenFailure(const char* call, int32_t result);

  void WorkerThreadMain();
  // Waits for the given duration unless the worker is asked to stop first.
  // Returns false when it is time to leave the worker loop.
  bool SleepWhileRunning(std::chrono::milliseconds duration);

  // Probes for a sensor and opens its depth stream. Returns false when there
  // is nothing to open yet; the device status is updated either way.
  bool OpenSensor();
  // Closes the depth stream and shuts the runtime down. Safe to call when the
  // sensor was never opened.
  void CloseSensor();
  // Reads one frame from the open stream and publishes it. Returns false when
  // the sensor should be torn down and probed for again.
  bool PumpFrame();

  void ReadTiltAngle();

  // Pump side of the tilt thread. StartTiltThread starts it once the sensor
  // is ready and a tilt call has asked for it (tilt_wanted_), unless the
  // motor is unavailable for the session or a stop has been asked for; it
  // decides then whether the thread reads the accelerometer too, and logs
  // the first start. StopTiltThread stops it before the sensor closes,
  // waiting at most 3 s for a call in flight and 1 s more for a thread taken
  // over between calls - well short of a motor command's 10 s due time,
  // because the pump's own join is bounded at 5 s, so a command still in
  // flight at 3 s is given up on there. It releases an interface a thread
  // taken over between calls left behind. WatchTiltThread gives up on a
  // thread whose call into the sensor is overdue; it never waits.
  // Both give up only through a take-over they win (TiltCallWatch), and then
  // through AbandonTiltThread, which also settles what happens to the sensor.
  void StartTiltThread();
  void StopTiltThread();
  void WatchTiltThread();
  void AbandonTiltThread(const std::string& reason);

  // Tilt thread side. TiltThreadMain opens the sensor's interface when it
  // reads the accelerometer, then applies tilt requests as they come and
  // reads the accelerometer every interval until it is asked to stop.
  // ApplyPendingTilt hands the latest request to the motor unless it repeats
  // the target the motor was just given (TiltRequestGate), and with the
  // sensor's interface open takes a fresh reading before it finishes. Every
  // call into the sensor sits between BeginTiltCall and EndTiltCall, which is
  // what the pump watches; each call gives its own time limit and a name for
  // the Warning. BeginTiltCall returns false, and the call is not made, once
  // the pump has taken the thread over; EndTiltCall returns false when that
  // happened during the call. In both cases the thread leaves at once, and
  // every function below that makes a call returns false; on the way out it
  // touches nothing but the log, if the call it was given up in comes back
  // after all, and, for a motor command, its request's finish in the slot.
  // OpenSensorInterface otherwise returns true with or without an interface: a
  // sensor without a usable one still has a motor.
  void TiltThreadMain();
  bool OpenSensorInterface(api::ISensor** out_sensor);
  bool ApplyPendingTilt(api::ISensor* sensor,
                        std::chrono::steady_clock::time_point* next_read);
  bool ReadAccelerometer(api::ISensor* sensor);
  bool ReleaseSensorInterface(api::ISensor* sensor);
  bool BeginTiltCall(const char* call, std::chrono::milliseconds timeout);
  bool EndTiltCall();

  // The last plausible reading and the whole-degree angle held for it, under
  // gravity_mutex_. LoadGravity returns false when there is none; either
  // pointer may be null.
  bool LoadGravity(float* xyz, int32_t* held_degrees) const;
  void StoreGravity(const float* xyz, int32_t held_degrees);
  void ClearGravity();
  // Forgets the reading and stops any later one from being stored, for the
  // rest of the session.
  void TurnAccelerometerOff();

  // Asks the pump for the tilt thread (tilt_wanted_). Every tilt call makes it
  // first, whatever it goes on to answer.
  void WantTiltThread() { tilt_wanted_.store(true, std::memory_order_release); }

  // Kinect10.dll, as an HMODULE. Kept opaque so that this header does not drag
  // <windows.h> into everything that creates a backend.
  void* library_ = nullptr;
  std::unique_ptr<api::Exports> exports_;

  std::unique_ptr<threading::Thread> worker_thread_;
  std::atomic<bool> worker_running_ = {false};
  // Signalled by Shutdown() to get the worker out of its waits at once.
  void* shutdown_event_ = nullptr;
  // Set when the worker would not stop in time and was left running. Nothing
  // it might still be using may be freed after that, this object included.
  // The thread that owns this object is the only one that touches it:
  // Shutdown() writes it, and Shutdown(), the destructor and
  // IsSafeToDestroy() read it. The abandoned worker never looks at it - it is
  // stuck in a driver call, and nothing it does on the way out depends on
  // this. Atomic so that an owner which shuts down on one thread and destroys
  // on another still sees the store.
  std::atomic<bool> worker_abandoned_ = {false};

  // Touched by the worker thread only.
  void* next_frame_event_ = nullptr;
  void* stream_handle_ = nullptr;
  bool runtime_initialized_ = false;
  uint64_t published_frame_count_ = 0;
  // The last failure to open the sensor and how many times it has repeated
  // since, so that a sensor that stays broken is not reported over and over.
  int32_t last_open_failure_ = 0;
  uint32_t repeated_open_failures_ = 0;
  DepthFramePool frame_pool_;

  // The title's tilt requests on their way to the motor, which also says
  // whether a move is in progress (IsTiltMotorMoving), and which of them
  // reach the motor. The gate is the tilt thread's alone.
  TiltRequestSlot tilt_requests_;
  TiltRequestGate tilt_gate_;
  // Set, for the rest of the session, once there is no tilt thread to apply
  // requests - it was abandoned, or could not be started. Tilt requests are
  // then refused rather than left waiting.
  std::atomic<bool> tilt_motor_unavailable_ = {false};
  // Set by the first tilt call - any of the four above, on whichever thread
  // makes it; PeekTiltDegrees is not one - and never cleared. The pump starts
  // the tilt thread once it is set and the sensor is ready, and again each
  // time the sensor opens after that. The calls only ask: they run on the
  // title's threads, and starting the thread, like watching it, is the
  // pump's. Until the thread's first reading they answer as they do without
  // an accelerometer, with the angle read when the sensor opened or the one
  // the motor last accepted.
  std::atomic<bool> tilt_wanted_ = {false};
  // Whether the first start of the tilt thread has been logged. The pump's
  // alone.
  bool tilt_start_logged_ = false;

  // The tilt thread, and the event that gets it out of its wait between
  // readings. The thread object is the pump's alone.
  std::unique_ptr<threading::Thread> tilt_thread_;
  void* tilt_stop_event_ = nullptr;
  // The hand-off between the tilt thread's calls into the sensor and the
  // pump that watches them, and the name and time limit of the call in
  // flight, for the Warning if it never returns.
  TiltCallWatch tilt_call_watch_;
  std::atomic<const char*> tilt_call_name_ = {""};
  std::atomic<int64_t> tilt_call_timeout_ms_ = {0};
  // When the tilt thread's current call began, for the line it logs if the
  // call comes back after the pump gave it up. The tilt thread's alone.
  int64_t tilt_call_started_ms_ = 0;
  // The sensor's interface while the tilt thread holds a reference to it.
  // Written by the tilt thread; read by the pump only once that thread has
  // left, to release a reference a take-over kept it from releasing.
  api::ISensor* tilt_sensor_ = nullptr;
  // Whether the current tilt thread reads the accelerometer. Written by the
  // pump before it starts the thread, read by the thread.
  bool tilt_reads_accelerometer_ = false;
  // Set, for the rest of the session, once the accelerometer failed to open
  // or its check, or a call into it hung; it is never read again after that.
  std::atomic<bool> accelerometer_off_ = {false};
  // Set when the tilt thread was left inside a call that never returned.
  // Like worker_abandoned_, it keeps the library, the events and this object
  // alive, and the sensor is then never shut down or opened again in this
  // process (AbandonTiltThread).
  std::atomic<bool> tilt_abandoned_ = {false};
  // Whether "nui_accelerometer is off" has been said. The pump's alone.
  bool accelerometer_cvar_reported_ = false;
  // Touched by the tilt thread only. Only one tilt thread runs at a time, and
  // one that has been given up on never touches them again.
  bool accelerometer_failure_reported_ = false;
  bool implausible_reading_reported_ = false;
  bool tilt_first_reading_logged_ = false;
  double tilt_logged_elevation_ = 0.0;
  bool tilt_has_held_degrees_ = false;
  int32_t tilt_held_degrees_ = 0;

  // The last plausible accelerometer reading, in g on the runtime's axes, and
  // the whole-degree angle reported for it, written by the tilt thread and
  // read by the guest's threads.
  mutable std::mutex gravity_mutex_;
  float gravity_[3] = {};
  int32_t held_degrees_ = 0;
  bool has_gravity_ = false;

  // The angle the motor last accepted, or the one read back from the sensor
  // when it opened.
  std::atomic<int32_t> tilt_degrees_ = {0};
  std::atomic<bool> has_tilt_degrees_ = {false};
};

}  // namespace kinect
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_KINECT_KINECT_NUI_SYSTEM_H_
