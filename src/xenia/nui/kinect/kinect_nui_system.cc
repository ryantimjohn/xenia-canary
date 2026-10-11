/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/kinect/kinect_nui_system.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/nui/kinect/kinect_api.h"
#include "xenia/nui/nui_accelerometer.h"
#include "xenia/nui/nui_flags.h"

namespace xe {
namespace nui {
namespace kinect {

// Name of the runtime that carries the flat NUI API.
static const wchar_t* const kRuntimeLibraryName = L"Kinect10.dll";

// How long the pump waits for a frame before it looks at its other work.
static constexpr DWORD kFrameWaitMilliseconds = 100;
// How long to wait before probing again for a sensor that was not there.
static constexpr std::chrono::milliseconds kProbeInterval{1000};
// How long to wait before picking a broken sensor back up.
static constexpr std::chrono::milliseconds kErrorInterval{2000};
// How long Shutdown() waits for the pump before it gives up on it.
static constexpr std::chrono::milliseconds kWorkerJoinTimeout{5000};
// How many identical failures to open the sensor to swallow between reports.
static constexpr uint32_t kOpenFailureLogInterval = 30;
// How often the accelerometer is read while the sensor is ready.
static constexpr std::chrono::milliseconds kAccelerometerInterval{250};
// How often the tilt thread looks for a tilt request between readings.
static constexpr std::chrono::milliseconds kTiltPollInterval{50};
// How long each call into the sensor may take before it counts as stuck is
// kTiltReadDue and kTiltCommandDue (nui_tilt_call_watch.h). Closing the sensor
// does not wait that long for a motor command, though: the pump's own join is
// bounded by kWorkerJoinTimeout, so a close gives a thread inside a call this
// long before it gives it up, and a thread taken over between calls this much
// more to leave.
static constexpr std::chrono::milliseconds kTiltThreadStopTimeout{3000};
static constexpr std::chrono::milliseconds kTiltThreadExitTimeout{1000};
// An export that only a runtime new enough to have the sensor interface's
// depth-filter methods carries. It is looked up, never called: see
// ResolveExports.
static const char* const kDepthFilterExportName = "NuiCreateDepthFilter";

// Now, in milliseconds on the steady clock, and always positive, as the tilt
// thread's call stamps must be (TiltCallWatch).
static int64_t SteadyMilliseconds() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
  return std::max<int64_t>(1, now.count());
}

std::unique_ptr<NuiSystem> KinectNuiSystem::Create() {
  return std::make_unique<KinectNuiSystem>();
}

bool KinectNuiSystem::IsAvailable() {
  HMODULE library = LoadLibraryW(kRuntimeLibraryName);
  if (!library) {
    return false;
  }
  FreeLibrary(library);
  return true;
}

KinectNuiSystem::KinectNuiSystem() = default;

KinectNuiSystem::~KinectNuiSystem() {
  if (!IsSafeToDestroy()) {
    // A pump or a tilt thread that is stuck in the driver still uses the
    // events and the library, so they are deliberately left behind rather
    // than pulled out from under it.
    return;
  }
  // Shutdown() is what tears the sensor down. This only catches an instance
  // that was dropped without one, such as a Setup() that failed early.
  ReleaseRuntime();
}

X_STATUS KinectNuiSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = NuiSystem::Setup(kernel_state);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }

  library_ = LoadLibraryW(kRuntimeLibraryName);
  if (!library_) {
    XELOGE(
        "NUI: Kinect10.dll could not be loaded. Install the Kinect for "
        "Windows runtime, or pick another backend with --nui.");
    return X_STATUS_UNSUCCESSFUL;
  }
  exports_ = std::make_unique<api::Exports>();
  if (!ResolveExports()) {
    ReleaseRuntime();
    return X_STATUS_UNSUCCESSFUL;
  }

  // The runtime signals this event whenever a depth frame is waiting for us.
  // It has to be manual reset; the pump resets it once the frame is read.
  next_frame_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  shutdown_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // Signalled to get the tilt thread out of its wait between readings.
  tilt_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!next_frame_event_ || !shutdown_event_ || !tilt_stop_event_) {
    XELOGE("NUI: failed to create the Kinect pump's events.");
    ReleaseRuntime();
    return X_STATUS_UNSUCCESSFUL;
  }

  worker_running_.store(true, std::memory_order_release);
  worker_thread_ =
      threading::Thread::Create({}, [this]() { WorkerThreadMain(); });
  if (!worker_thread_) {
    XELOGE("NUI: failed to start the Kinect pump thread.");
    worker_running_.store(false, std::memory_order_release);
    ReleaseRuntime();
    return X_STATUS_UNSUCCESSFUL;
  }
  worker_thread_->set_name("NUI Kinect Pump");
  return X_STATUS_SUCCESS;
}

void KinectNuiSystem::Shutdown() {
  // Stop the worker before anything else: once it has been joined it can no
  // longer publish a frame or fire a device status callback into a base class
  // that is halfway through its own teardown.
  worker_running_.store(false, std::memory_order_release);
  if (tilt_stop_event_) {
    // The pump stops the tilt thread on its way out. This is for a pump that
    // never gets there, so that its tilt thread at least stops reading.
    SetEvent(static_cast<HANDLE>(tilt_stop_event_));
  }
  if (worker_thread_) {
    if (shutdown_event_) {
      SetEvent(static_cast<HANDLE>(shutdown_event_));
    }
    // The join is bounded because the sensor's driver can block a call for
    // good. Closing the emulator has to work even then, so a pump that will
    // not come back is abandoned rather than waited on forever.
    if (threading::Wait(worker_thread_.get(), false, kWorkerJoinTimeout) !=
        threading::WaitResult::kSuccess) {
      worker_abandoned_.store(true, std::memory_order_release);
      XELOGE(
          "NUI: the Kinect pump did not stop within {} ms and is stuck in the "
          "sensor's driver. Leaving Kinect10.dll loaded so that it cannot "
          "fault on its way out.",
          kWorkerJoinTimeout.count());
    }
    worker_thread_.reset();
  }
  if (!worker_abandoned_.load(std::memory_order_acquire)) {
    // The worker closes the sensor on its way out, so this only has work to do
    // when the thread never ran.
    CloseSensor();
    // A tilt thread left inside the driver keeps the library and the events,
    // just as a stuck pump does.
    if (!tilt_abandoned_.load(std::memory_order_acquire)) {
      ReleaseRuntime();
    }
  }
  NuiSystem::Shutdown();
}

bool KinectNuiSystem::ResolveExports() {
  bool resolved = true;
  auto resolve = [this, &resolved](const char* symbol) -> FARPROC {
    FARPROC address = GetProcAddress(static_cast<HMODULE>(library_), symbol);
    if (!address) {
      XELOGE("NUI: Kinect10.dll does not export {}.", symbol);
      resolved = false;
    }
    return address;
  };
  exports_->NuiInitialize =
      reinterpret_cast<api::PFNNuiInitialize>(resolve("NuiInitialize"));
  exports_->NuiShutdown =
      reinterpret_cast<api::PFNNuiShutdown>(resolve("NuiShutdown"));
  exports_->NuiGetSensorCount =
      reinterpret_cast<api::PFNNuiGetSensorCount>(resolve("NuiGetSensorCount"));
  exports_->NuiImageStreamOpen = reinterpret_cast<api::PFNNuiImageStreamOpen>(
      resolve("NuiImageStreamOpen"));
  exports_->NuiImageStreamGetNextFrame =
      reinterpret_cast<api::PFNNuiImageStreamGetNextFrame>(
          resolve("NuiImageStreamGetNextFrame"));
  exports_->NuiImageStreamReleaseFrame =
      reinterpret_cast<api::PFNNuiImageStreamReleaseFrame>(
          resolve("NuiImageStreamReleaseFrame"));
  exports_->NuiCameraElevationGetAngle =
      reinterpret_cast<api::PFNNuiCameraElevationGetAngle>(
          resolve("NuiCameraElevationGetAngle"));
  exports_->NuiCameraElevationSetAngle =
      reinterpret_cast<api::PFNNuiCameraElevationSetAngle>(
          resolve("NuiCameraElevationSetAngle"));

  // The ninth is optional. The accelerometer is only reachable through the
  // sensor's own interface, and its slot is only there on a runtime whose
  // interface goes on past it: one that exports the depth-filter entry point
  // has the depth-filter methods, which the interface lays out after the
  // accelerometer. On an older runtime the slot might not exist at all, so
  // the backend goes on reporting the angle it asked the motor for.
  auto* library = static_cast<HMODULE>(library_);
  auto create_sensor = reinterpret_cast<api::PFNNuiCreateSensorByIndex>(
      GetProcAddress(library, "NuiCreateSensorByIndex"));
  if (create_sensor && GetProcAddress(library, kDepthFilterExportName)) {
    exports_->NuiCreateSensorByIndex = create_sensor;
  } else {
    XELOGI(
        "NUI: this Kinect runtime cannot read the sensor's accelerometer; the "
        "tilt angle reported is the one the motor last accepted.");
  }
  return resolved;
}

void KinectNuiSystem::ReleaseRuntime() {
  if (next_frame_event_) {
    CloseHandle(static_cast<HANDLE>(next_frame_event_));
    next_frame_event_ = nullptr;
  }
  if (shutdown_event_) {
    CloseHandle(static_cast<HANDLE>(shutdown_event_));
    shutdown_event_ = nullptr;
  }
  if (tilt_stop_event_) {
    CloseHandle(static_cast<HANDLE>(tilt_stop_event_));
    tilt_stop_event_ = nullptr;
  }
  exports_.reset();
  if (library_) {
    FreeLibrary(static_cast<HMODULE>(library_));
    library_ = nullptr;
  }
}

void KinectNuiSystem::WorkerThreadMain() {
  while (worker_running_.load(std::memory_order_acquire)) {
    if (!stream_handle_ && !OpenSensor()) {
      continue;
    }

    // Only with the sensor ready and depth flowing, and only once a tilt call
    // has asked for it, is the tilt thread started. It is where the motor is
    // moved and the accelerometer read, on a thread of its own: a motor
    // command blocks for a second or so, and has been seen to take over three
    // beside the frame fetches, which the depth frames cannot spare. A title
    // that makes no tilt call never gets the thread, and all it sees of the
    // tilt is the angle read when the sensor opened. One that asked before the
    // sensor was ready gets it as soon as the sensor is, and once asked for,
    // it starts again each time the sensor opens, whichever title is loaded.
    if (!tilt_thread_ && tilt_wanted_.load(std::memory_order_acquire)) {
      StartTiltThread();
    }

    // The pump never waits for the tilt thread; it only looks at it.
    WatchTiltThread();

    HANDLE wait_handles[] = {static_cast<HANDLE>(next_frame_event_),
                             static_cast<HANDLE>(shutdown_event_)};
    const DWORD wait_result =
        WaitForMultipleObjects(static_cast<DWORD>(std::size(wait_handles)),
                               wait_handles, FALSE, kFrameWaitMilliseconds);
    if (wait_result == WAIT_TIMEOUT) {
      continue;
    }
    if (wait_result == WAIT_OBJECT_0 + 1) {
      break;
    }
    if (wait_result != WAIT_OBJECT_0) {
      XELOGE("NUI: waiting for a depth frame failed with {:08X}.",
             GetLastError());
      CloseSensor();
      SetDeviceStatus(DeviceStatus::kError);
      if (!SleepWhileRunning(kErrorInterval)) {
        break;
      }
      continue;
    }

    ResetEvent(static_cast<HANDLE>(next_frame_event_));
    if (!PumpFrame()) {
      // The sensor was most likely unplugged. Drop everything and go back to
      // probing for one.
      CloseSensor();
      SetDeviceStatus(DeviceStatus::kError);
      if (!SleepWhileRunning(kErrorInterval)) {
        break;
      }
    }
  }
  CloseSensor();
}

bool KinectNuiSystem::SleepWhileRunning(std::chrono::milliseconds duration) {
  if (!worker_running_.load(std::memory_order_acquire)) {
    return false;
  }
  const DWORD wait_result =
      WaitForSingleObject(static_cast<HANDLE>(shutdown_event_),
                          static_cast<DWORD>(duration.count()));
  return wait_result == WAIT_TIMEOUT &&
         worker_running_.load(std::memory_order_acquire);
}

void KinectNuiSystem::LogOpenFailure(const char* call, int32_t result) {
  // A sensor that another process has claimed, or that is simply broken, fails
  // the same way every couple of seconds for as long as the emulator runs, so
  // saying so once is worth more than saying so hundreds of times.
  if (result == last_open_failure_ &&
      ++repeated_open_failures_ % kOpenFailureLogInterval != 0) {
    return;
  }
  if (result != last_open_failure_) {
    last_open_failure_ = result;
    repeated_open_failures_ = 0;
  }
  XELOGE("NUI: {} failed with {:08X}.", call, static_cast<uint32_t>(result));
}

bool KinectNuiSystem::OpenSensor() {
  if (tilt_abandoned_.load(std::memory_order_acquire)) {
    // A call of the tilt thread's is still inside this sensor's runtime, so
    // the runtime is never initialized again in this process; see
    // AbandonTiltThread.
    SetDeviceStatus(DeviceStatus::kError);
    SleepWhileRunning(kErrorInterval);
    return false;
  }

  int sensor_count = 0;
  HRESULT result = exports_->NuiGetSensorCount(&sensor_count);
  if (FAILED(result) || sensor_count <= 0) {
    if (FAILED(result)) {
      LogOpenFailure("NuiGetSensorCount", result);
    }
    SetDeviceStatus(DeviceStatus::kNotPresent);
    SleepWhileRunning(kProbeInterval);
    return false;
  }

  SetDeviceStatus(DeviceStatus::kInitializing);
  result = exports_->NuiInitialize(api::kInitializeUsesDepth);
  if (FAILED(result)) {
    LogOpenFailure("NuiInitialize", result);
    SetDeviceStatus(DeviceStatus::kError);
    SleepWhileRunning(kErrorInterval);
    return false;
  }
  runtime_initialized_ = true;

  HANDLE stream_handle = nullptr;
  result = exports_->NuiImageStreamOpen(
      api::kImageTypeDepth, api::kResolution640x480, 0, api::kDepthFrameLimit,
      static_cast<HANDLE>(next_frame_event_), &stream_handle);
  if (FAILED(result) || !stream_handle) {
    LogOpenFailure("NuiImageStreamOpen", result);
    CloseSensor();
    SetDeviceStatus(DeviceStatus::kError);
    SleepWhileRunning(kErrorInterval);
    return false;
  }
  stream_handle_ = stream_handle;
  // The sensor is up, so the next thing to go wrong deserves to be heard.
  last_open_failure_ = 0;
  repeated_open_failures_ = 0;

  // Read the angle so that a title asking for it before the motor has been
  // touched gets the sensor's real position. This is the only
  // NuiCameraElevationGetAngle call the backend makes: the second and later
  // calls of a session have been seen to block inside the motor driver and
  // never return, so every angle after this one is the angle we asked the
  // motor for rather than one read back from it - unless the accelerometer
  // can be read, in which case what it measures is reported instead. The
  // accelerometer is left alone here: the tilt thread reads it once the
  // sensor is ready and a tilt call has asked for it.
  ReadTiltAngle();

  SetDeviceStatus(DeviceStatus::kReady);
  XELOGI("NUI: Kinect sensor ready");
  return true;
}

void KinectNuiSystem::CloseSensor() {
  // The tilt thread lets go of the sensor's interface on its way out, so it
  // is stopped first, while the runtime is still up.
  StopTiltThread();
  // The runtime owns the stream handle; shutting it down closes the stream.
  stream_handle_ = nullptr;
  if (runtime_initialized_) {
    // Never under a call the tilt thread was left inside: the runtime is then
    // left exactly as it is, as it would be under a stuck pump. See
    // AbandonTiltThread.
    if (!tilt_abandoned_.load(std::memory_order_acquire)) {
      exports_->NuiShutdown();
    }
    runtime_initialized_ = false;
  }
  if (next_frame_event_) {
    ResetEvent(static_cast<HANDLE>(next_frame_event_));
  }
  has_tilt_degrees_.store(false, std::memory_order_release);
}

bool KinectNuiSystem::PumpFrame() {
  const api::ImageFrame* image_frame = nullptr;
  // The frame event says that the stream wants servicing, not that a frame has
  // already landed: asking for one with no patience at all only ever gets
  // "nothing yet" out of the 1.8 runtime. Giving the read the same budget as
  // the event wait is what actually hands over a frame, and it costs nothing
  // when one is already queued.
  HRESULT result = exports_->NuiImageStreamGetNextFrame(
      static_cast<HANDLE>(stream_handle_), kFrameWaitMilliseconds,
      &image_frame);
  if (result == api::kErrorFrameNoData) {
    // The sensor is fine, it just has not finished a frame yet. Go back to
    // waiting for the event instead of tearing the stream down.
    return true;
  }
  if (FAILED(result)) {
    XELOGE("NUI: NuiImageStreamGetNextFrame failed with {:08X}.",
           static_cast<uint32_t>(result));
    return false;
  }
  if (!image_frame) {
    XELOGE("NUI: the runtime announced a depth frame but handed us nothing.");
    return false;
  }
  if (!image_frame->texture) {
    XELOGE("NUI: the runtime handed us a depth frame without a texture.");
    exports_->NuiImageStreamReleaseFrame(static_cast<HANDLE>(stream_handle_),
                                         image_frame);
    return false;
  }

  api::LockedRect locked_rect = {};
  result = image_frame->texture->LockRect(0, &locked_rect, nullptr, 0);
  std::shared_ptr<DepthFrame> frame;
  if (FAILED(result)) {
    XELOGE("NUI: locking a depth frame failed with {:08X}.",
           static_cast<uint32_t>(result));
  } else {
    // The copy below reads pitch bytes from each of 480 rows, so a frame whose
    // geometry is not what we asked for is dropped rather than read past the
    // end of the mapping and published half filled.
    const size_t row_bytes = size_t(DepthFrame::kWidth) * sizeof(uint16_t);
    const size_t mapped_bytes = size_t(locked_rect.pitch) * DepthFrame::kHeight;
    if (image_frame->resolution != api::kResolution640x480) {
      XELOGE("NUI: a depth frame arrived at resolution {}, not 640x480.",
             image_frame->resolution);
    } else if (!locked_rect.bits ||
               locked_rect.pitch < static_cast<INT>(row_bytes) ||
               locked_rect.size < 0 ||
               static_cast<size_t>(locked_rect.size) < mapped_bytes) {
      XELOGE(
          "NUI: a depth frame arrived with an unusable pitch of {} and size of "
          "{}.",
          locked_rect.pitch, locked_rect.size);
    } else {
      frame = frame_pool_.Acquire();
      const auto* source = static_cast<const uint8_t*>(locked_rect.bits);
      uint16_t* target = frame->depth_mm.data();
      for (uint32_t y = 0; y < DepthFrame::kHeight; ++y) {
        const auto* source_row = reinterpret_cast<const uint16_t*>(
            source + size_t(y) * size_t(locked_rect.pitch));
        for (uint32_t x = 0; x < DepthFrame::kWidth; ++x) {
          uint16_t millimetres = static_cast<uint16_t>(
              source_row[x] >> api::kDepthPixelDistanceShift);
          if (millimetres >= api::kDepthDistanceUnknown) {
            // Too far away, too close, or in shadow: no reading at all.
            millimetres = 0;
          }
          target[x] = millimetres;
        }
        target += DepthFrame::kWidth;
      }
    }
    const HRESULT unlock_result = image_frame->texture->UnlockRect(0);
    if (FAILED(unlock_result)) {
      XELOGW("NUI: unlocking a depth frame failed with {:08X}.",
             static_cast<uint32_t>(unlock_result));
    }
  }

  const HRESULT release_result = exports_->NuiImageStreamReleaseFrame(
      static_cast<HANDLE>(stream_handle_), image_frame);
  if (FAILED(release_result)) {
    XELOGE("NUI: NuiImageStreamReleaseFrame failed with {:08X}.",
           static_cast<uint32_t>(release_result));
    return false;
  }
  if (!frame) {
    return false;
  }

  PublishFrame(frame);
  ++published_frame_count_;
  if ((published_frame_count_ % kFrameLogInterval) == 0) {
    XELOGI("NUI: {} depth frames published", published_frame_count_);
  }
  return true;
}

void KinectNuiSystem::ReadTiltAngle() {
  LONG degrees = 0;
  const HRESULT result = exports_->NuiCameraElevationGetAngle(&degrees);
  if (FAILED(result)) {
    XELOGW("NUI: NuiCameraElevationGetAngle failed with {:08X}.",
           static_cast<uint32_t>(result));
    return;
  }
  tilt_degrees_.store(static_cast<int32_t>(degrees), std::memory_order_release);
  has_tilt_degrees_.store(true, std::memory_order_release);
}

void KinectNuiSystem::StartTiltThread() {
  if (tilt_thread_ || tilt_motor_unavailable_.load(std::memory_order_acquire)) {
    return;
  }
  // Whether this run of the thread reads the accelerometer, decided once each
  // time the thread starts. Off, the sensor's interface is never even asked
  // for, and the thread only applies tilt requests.
  tilt_reads_accelerometer_ = false;
  if (exports_->NuiCreateSensorByIndex &&
      !accelerometer_off_.load(std::memory_order_acquire)) {
    if (cvars::nui_accelerometer) {
      tilt_reads_accelerometer_ = true;
    } else if (!accelerometer_cvar_reported_) {
      accelerometer_cvar_reported_ = true;
      XELOGI(
          "NUI: nui_accelerometer is off; tilt requests still reach the "
          "motor, and the tilt angle reported is the one it last accepted.");
    }
  }
  // Shutdown clears worker_running_ before it signals the stop event, so
  // checking it after the reset rather than before is what keeps this from
  // undoing a stop that has just been asked for: a stop that lands after the
  // check finds the event reset already and sets it again for the new thread.
  ResetEvent(static_cast<HANDLE>(tilt_stop_event_));
  if (!worker_running_.load(std::memory_order_acquire)) {
    return;
  }
  tilt_call_watch_.Reset();
  tilt_thread_ = threading::Thread::Create({}, [this]() { TiltThreadMain(); });
  if (!tilt_thread_) {
    XELOGW(
        "NUI: failed to start the Kinect tilt thread; the tilt motor and the "
        "accelerometer are left alone for the rest of the session.");
    tilt_motor_unavailable_.store(true, std::memory_order_release);
    tilt_requests_.Clear();
    TurnAccelerometerOff();
    return;
  }
  tilt_thread_->set_name("NUI Kinect Tilt");
  // Said once: a start after the sensor has closed and opened again is not
  // the title's doing.
  if (!tilt_start_logged_) {
    tilt_start_logged_ = true;
    XELOGI("NUI: tilt thread started on the title's first tilt call");
  }
}

void KinectNuiSystem::StopTiltThread() {
  if (!tilt_thread_) {
    return;
  }
  SetEvent(static_cast<HANDLE>(tilt_stop_event_));
  // Bounded for the same reason the pump's own join is: a call into the
  // sensor can block for good, and the sensor has to close regardless. A
  // thread between calls leaves within one poll; one inside a call leaves
  // when the call returns, which for a motor command is usually within a
  // second or two. The wait is shorter than a command's due time so that the
  // pump itself stays inside kWorkerJoinTimeout.
  if (threading::Wait(tilt_thread_.get(), false, kTiltThreadStopTimeout) !=
      threading::WaitResult::kSuccess) {
    if (tilt_call_watch_.TakeOver()) {
      // Inside a call that is taking too long: given up on.
      AbandonTiltThread(
          fmt::format("{} did not return within {} ms of the sensor closing",
                      tilt_call_name_.load(std::memory_order_acquire),
                      kTiltThreadStopTimeout.count()));
    } else if (threading::Wait(tilt_thread_.get(), false,
                               kTiltThreadExitTimeout) !=
               threading::WaitResult::kSuccess) {
      // Between calls, and now refused any other, yet still not gone. It
      // cannot be inside the sensor, but it still uses this object, so it is
      // treated as stuck all the same.
      AbandonTiltThread("the Kinect tilt thread would not stop");
    }
  }
  tilt_thread_.reset();
  ClearGravity();
  // A thread taken over between calls leaves without its Release, which the
  // take-over refuses. Once it has left, as it has unless it was abandoned,
  // nothing can race a Release, so the pump makes it here, before the
  // runtime is shut down. An abandoned thread keeps its reference: its call
  // may still be inside the driver.
  if (tilt_sensor_ && !tilt_abandoned_.load(std::memory_order_acquire)) {
    XELOGD(
        "NUI: releasing the Kinect sensor interface the tilt thread left "
        "behind when it was stopped between calls.");
    tilt_sensor_->Release();
    tilt_sensor_ = nullptr;
  }
}

void KinectNuiSystem::WatchTiltThread() {
  if (!tilt_thread_) {
    return;
  }
  // Only a take-over the pump wins counts: a call that returns just as its
  // time runs out keeps its thread.
  if (!tilt_call_watch_.TakeOverIfOverdue(SteadyMilliseconds())) {
    return;
  }
  AbandonTiltThread(
      fmt::format("{} has not returned within {} ms",
                  tilt_call_name_.load(std::memory_order_acquire),
                  tilt_call_timeout_ms_.load(std::memory_order_acquire)));
  tilt_thread_.reset();
}

void KinectNuiSystem::AbandonTiltThread(const std::string& reason) {
  // Called only once the pump has won the take-over (TiltCallWatch), so from
  // here on the tilt thread makes no other call into the sensor: if its call
  // ever returns, it sees the take-over and leaves, touching nothing on the
  // way but the log and its request's finish in the slot cleared below. It is
  // never joined, and the interface it holds is never released.
  //
  // The call it is stuck in is inside the same sensor the pump drives, so the
  // sensor session is treated exactly like one under a stuck pump. The depth
  // stream keeps running for as long as it does, but when the session ends -
  // a failed frame, or the emulator closing - the runtime is not shut down
  // under the stuck call, the device status goes to kError, and the sensor
  // is never opened again in this process. Tilt requests are refused from
  // now on, since there is no thread left to apply them and the pump makes no
  // motor call. Kinect10.dll, the events and this object stay alive
  // (IsSafeToDestroy), and the sensor stays claimed until it is unplugged and
  // plugged back in.
  tilt_abandoned_.store(true, std::memory_order_release);
  tilt_motor_unavailable_.store(true, std::memory_order_release);
  tilt_requests_.Clear();
  TurnAccelerometerOff();
  XELOGW(
      "NUI: {}. The Kinect tilt thread is left inside it with Kinect10.dll, "
      "its events and the backend kept alive; tilt requests are refused and "
      "the tilt angle reported is the last one the motor accepted. When this "
      "sensor session ends the sensor is neither shut down nor opened again, "
      "so it stays claimed until it is unplugged and plugged back in.",
      reason);
}

bool KinectNuiSystem::BeginTiltCall(const char* call,
                                    std::chrono::milliseconds timeout) {
  if (tilt_abandoned_.load(std::memory_order_acquire)) {
    return false;
  }
  // Named for the Warning, should the call never come back. Stored before
  // the stamp, which is what the pump synchronises on.
  tilt_call_name_.store(call, std::memory_order_release);
  tilt_call_timeout_ms_.store(timeout.count(), std::memory_order_release);
  tilt_call_started_ms_ = SteadyMilliseconds();
  return tilt_call_watch_.BeginCall(tilt_call_started_ms_ + timeout.count());
}

bool KinectNuiSystem::EndTiltCall() {
  if (tilt_call_watch_.EndCall() &&
      !tilt_abandoned_.load(std::memory_order_acquire)) {
    return true;
  }
  // The pump gave this thread up while the call was in flight, and the call
  // has come back after all. Said once, because the thread leaves now and
  // makes no other call; on the way out it touches nothing but the log and,
  // for a motor command, its request's finish. Only a call the pump took over
  // ends up here: a thread taken over between calls is stopped by
  // BeginTiltCall instead.
  XELOGW(
      "NUI: {} returned after {} ms, after the Kinect tilt thread had been "
      "given up on; the thread leaves without touching the sensor again.",
      tilt_call_name_.load(std::memory_order_acquire),
      SteadyMilliseconds() - tilt_call_started_ms_);
  return false;
}

void KinectNuiSystem::TiltThreadMain() {
  tilt_first_reading_logged_ = false;
  tilt_has_held_degrees_ = false;

  api::ISensor* sensor = nullptr;
  if (tilt_reads_accelerometer_ && !OpenSensorInterface(&sensor)) {
    return;
  }

  // Tilt requests are looked for every poll and applied at once; the
  // accelerometer is read every interval, the first time straight away, and
  // once more right after each command. A motor command in progress holds the
  // next reading back, which is harmless: the block says the motor is moving
  // until the command returns. A stop is looked for before each command as
  // well as after the wait, so a stop already asked for when the thread comes
  // round never starts another command; one that lands in the microseconds
  // between that check and the command still waits for the command, which the
  // close's own time limit covers.
  auto next_read = std::chrono::steady_clock::now();
  while (true) {
    if (WaitForSingleObject(static_cast<HANDLE>(tilt_stop_event_), 0) !=
        WAIT_TIMEOUT) {
      break;
    }
    if (!ApplyPendingTilt(sensor, &next_read)) {
      return;
    }
    if (sensor && std::chrono::steady_clock::now() >= next_read) {
      if (!ReadAccelerometer(sensor)) {
        return;
      }
      next_read = std::chrono::steady_clock::now() + kAccelerometerInterval;
    }
    if (WaitForSingleObject(static_cast<HANDLE>(tilt_stop_event_),
                            static_cast<DWORD>(kTiltPollInterval.count())) !=
        WAIT_TIMEOUT) {
      break;
    }
  }
  if (sensor) {
    ReleaseSensorInterface(sensor);
  }
}

bool KinectNuiSystem::OpenSensorInterface(api::ISensor** out_sensor) {
  *out_sensor = nullptr;
  api::ISensor* sensor = nullptr;
  if (!BeginTiltCall("NuiCreateSensorByIndex", kTiltReadDue)) {
    return false;
  }
  const HRESULT result =
      exports_->NuiCreateSensorByIndex(api::kDefaultSensorIndex, &sensor);
  if (!EndTiltCall()) {
    return false;
  }
  if (SUCCEEDED(result) && sensor) {
    // Kept where the pump can find it, should this thread be stopped between
    // calls and so leave without its Release (StopTiltThread).
    tilt_sensor_ = sensor;
  }
  if (FAILED(result) || !sensor) {
    XELOGW(
        "NUI: NuiCreateSensorByIndex({}) failed with {:08X}; the tilt angle "
        "reported is the one the motor last accepted.",
        api::kDefaultSensorIndex, static_cast<uint32_t>(result));
    TurnAccelerometerOff();
    return true;
  }

  // Two questions with known answers, asked before the accelerometer ever
  // is: which sensor this is, and the flags the flat NuiInitialize gave it.
  // They catch an instance that is not the sensor the flat entry points
  // drive - a separate, uninitialized one answers flags of 0 - and a table
  // whose slot 27 does not answer 0x20. They cannot prove the table's layout,
  // since they are called through that same layout; what makes these calls
  // and the accelerometer's safe is the slot order, checked against the 1.8
  // interface, and the depth-filter gate in ResolveExports, which ensures
  // the table reaches slot 33 at all.
  if (!BeginTiltCall("the sensor interface's self-check", kTiltReadDue)) {
    return false;
  }
  const int index = sensor->NuiInstanceIndex();
  const DWORD flags = sensor->NuiInitializationFlags();
  if (!EndTiltCall()) {
    return false;
  }
  if (index != api::kDefaultSensorIndex || flags != api::kInitializeUsesDepth) {
    XELOGW(
        "NUI: the Kinect sensor's interface says it is sensor {} initialized "
        "with {:08X}, not sensor {} with {:08X}; leaving its accelerometer "
        "unread, so the tilt angle reported is the one the motor last "
        "accepted.",
        index, static_cast<uint32_t>(flags), api::kDefaultSensorIndex,
        static_cast<uint32_t>(api::kInitializeUsesDepth));
    TurnAccelerometerOff();
    return ReleaseSensorInterface(sensor);
  }
  *out_sensor = sensor;
  return true;
}

bool KinectNuiSystem::ReleaseSensorInterface(api::ISensor* sensor) {
  // A thread the pump has taken over keeps its reference: nothing is
  // released under a call that may still be in the driver.
  if (!BeginTiltCall("the sensor interface's Release", kTiltReadDue)) {
    return false;
  }
  sensor->Release();
  tilt_sensor_ = nullptr;
  return EndTiltCall();
}

bool KinectNuiSystem::ApplyPendingTilt(
    api::ISensor* sensor, std::chrono::steady_clock::time_point* next_read) {
  int32_t degrees = 0;
  if (!tilt_requests_.Take(&degrees)) {
    return true;
  }
  if (!tilt_gate_.ShouldApply(degrees, TiltRequestGate::Clock::now())) {
    // The target the motor was just given, asked for again before it has had
    // time to get there - the guest's driver reading a head that is not
    // there yet, or cannot get there. Another command would only be refused
    // or change nothing.
    tilt_requests_.Finish();
    return true;
  }
  // Every way out below finishes the request, the two early ones included:
  // a request left taken would read as moving until the next one completed.
  // After an abandonment the pump has already cleared the slot and marked the
  // motor unavailable, so the finish is masked and nothing reads it. After
  // StopTiltThread takes the thread over between calls and it leaves in time,
  // nothing is abandoned or cleared: then the finish is what keeps the slot
  // from reading "moving" into the next sensor session, and the request it
  // took is dropped, which the title's own driver makes up for by asking
  // again.
  if (!BeginTiltCall("NuiCameraElevationSetAngle", kTiltCommandDue)) {
    tilt_requests_.Finish();
    return false;
  }
  const auto started = TiltRequestGate::Clock::now();
  const HRESULT result = exports_->NuiCameraElevationSetAngle(degrees);
  if (!EndTiltCall()) {
    tilt_requests_.Finish();
    return false;
  }
  const auto returned = TiltRequestGate::Clock::now();
  tilt_gate_.OnSetAngleReturned(degrees, returned);
  const auto took =
      std::chrono::duration_cast<std::chrono::milliseconds>(returned - started);
  XELOGD("NUI: NuiCameraElevationSetAngle({}) returned {:08X} after {} ms.",
         degrees, static_cast<uint32_t>(result), took.count());
  if (took > kTiltSlowCommand) {
    // Slow, not stuck: seen so that a command creeping toward its due time
    // shows up long before one is ever given up on.
    XELOGI("NUI: NuiCameraElevationSetAngle({}) took {} ms.", degrees,
           took.count());
  }
  if (FAILED(result)) {
    tilt_requests_.Finish();
    // The driver rate limits the motor, so a refused move is expected now and
    // then and is not a reason to tear the sensor down, nor to say so more
    // than once a settle window.
    if (tilt_gate_.ShouldReportRefusal(returned)) {
      XELOGW("NUI: NuiCameraElevationSetAngle({}) failed with {:08X}.", degrees,
             static_cast<uint32_t>(result));
    }
    return true;
  }
  // Report the angle we asked for instead of reading it back. Do not put a
  // NuiCameraElevationGetAngle call here: the second and later calls of a
  // session have been seen to block inside the motor driver and never return.
  // The angle is stored, and with the accelerometer open a fresh reading
  // taken, before the request is finished, so that a guest that sees the
  // motor stop also sees where it ended up rather than where it was before
  // the command.
  tilt_degrees_.store(degrees, std::memory_order_release);
  has_tilt_degrees_.store(true, std::memory_order_release);
  if (sensor) {
    if (!ReadAccelerometer(sensor)) {
      tilt_requests_.Finish();
      return false;
    }
    *next_read = std::chrono::steady_clock::now() + kAccelerometerInterval;
  }
  tilt_requests_.Finish();
  return true;
}

bool KinectNuiSystem::ReadAccelerometer(api::ISensor* sensor) {
  api::Vector4 reading = {};
  if (!BeginTiltCall("NuiAccelerometerGetCurrentReading", kTiltReadDue)) {
    return false;
  }
  const HRESULT result = sensor->NuiAccelerometerGetCurrentReading(&reading);
  if (!EndTiltCall()) {
    return false;
  }
  if (FAILED(result)) {
    // Said once a session: a sensor that cannot be read fails four times a
    // second for as long as it stays that way.
    if (!accelerometer_failure_reported_) {
      accelerometer_failure_reported_ = true;
      XELOGW(
          "NUI: reading the Kinect accelerometer failed with {:08X}; until a "
          "read works, the tilt angle reported is the one the motor last "
          "accepted.",
          static_cast<uint32_t>(result));
    }
    ClearGravity();
    return true;
  }
  const float xyz[3] = {reading.x, reading.y, reading.z};
  if (!IsPlausibleGravityReading(xyz)) {
    if (!implausible_reading_reported_) {
      implausible_reading_reported_ = true;
      XELOGW(
          "NUI: the Kinect accelerometer read ({:.3f}, {:.3f}, {:.3f}) g, "
          "which is not gravity on a sensor standing on its base; ignoring "
          "it.",
          xyz[0], xyz[1], xyz[2]);
    }
    ClearGravity();
    return true;
  }

  const double elevation = AccelerometerElevationDegrees(xyz);
  tilt_held_degrees_ = tilt_has_held_degrees_
                           ? HoldElevationDegrees(tilt_held_degrees_, elevation)
                           : static_cast<int32_t>(std::lround(elevation));
  tilt_has_held_degrees_ = true;
  StoreGravity(xyz, tilt_held_degrees_);

  if (!tilt_first_reading_logged_) {
    tilt_first_reading_logged_ = true;
    tilt_logged_elevation_ = elevation;
    XELOGI(
        "NUI: the Kinect accelerometer reads ({:.3f}, {:.3f}, {:.3f}) g, an "
        "elevation of {:.1f} degrees; reporting the measured angle from now "
        "on.",
        xyz[0], xyz[1], xyz[2], elevation);
  } else if (ElevationMovedEnoughToLog(tilt_logged_elevation_, elevation)) {
    tilt_logged_elevation_ = elevation;
    XELOGD("NUI: the Kinect accelerometer now reads {:.1f} degrees.",
           elevation);
  }
  return true;
}

bool KinectNuiSystem::LoadGravity(float* xyz, int32_t* held_degrees) const {
  std::lock_guard<std::mutex> lock(gravity_mutex_);
  if (!has_gravity_) {
    return false;
  }
  if (xyz) {
    std::copy(std::begin(gravity_), std::end(gravity_), xyz);
  }
  if (held_degrees) {
    *held_degrees = held_degrees_;
  }
  return true;
}

void KinectNuiSystem::StoreGravity(const float* xyz, int32_t held_degrees) {
  std::lock_guard<std::mutex> lock(gravity_mutex_);
  // A reading that was already on its way when the path was turned off does
  // not bring it back.
  if (accelerometer_off_.load(std::memory_order_acquire)) {
    return;
  }
  std::copy(xyz, xyz + std::size(gravity_), gravity_);
  held_degrees_ = held_degrees;
  has_gravity_ = true;
}

void KinectNuiSystem::ClearGravity() {
  std::lock_guard<std::mutex> lock(gravity_mutex_);
  has_gravity_ = false;
}

void KinectNuiSystem::TurnAccelerometerOff() {
  std::lock_guard<std::mutex> lock(gravity_mutex_);
  accelerometer_off_.store(true, std::memory_order_release);
  has_gravity_ = false;
}

bool KinectNuiSystem::GetTiltDegrees(int32_t* out_degrees) {
  WantTiltThread();
  return PeekTiltDegrees(out_degrees);
}

bool KinectNuiSystem::PeekTiltDegrees(int32_t* out_degrees) {
  if (!out_degrees) {
    return false;
  }
  if (GetTiltOverrideDegrees(out_degrees)) {
    return true;
  }
  if (!IsDevicePresent()) {
    return false;
  }
  // Where the sensor really points, once the accelerometer says so, held
  // against jitter (HoldElevationDegrees). It is not clamped to the motor's
  // travel: a base that stands at a slant puts the head beyond it, and the
  // title is better told the truth.
  int32_t held_degrees = 0;
  if (LoadGravity(nullptr, &held_degrees)) {
    *out_degrees = held_degrees;
    return true;
  }
  if (!has_tilt_degrees_.load(std::memory_order_acquire)) {
    return false;
  }
  *out_degrees = tilt_degrees_.load(std::memory_order_acquire);
  return true;
}

bool KinectNuiSystem::GetGravityVector(float* xyz) {
  WantTiltThread();
  if (!xyz) {
    return false;
  }
  // The override is an angle, so while it is set the block is made from
  // that angle rather than from what the sensor measures.
  int32_t override_degrees = 0;
  if (GetTiltOverrideDegrees(&override_degrees)) {
    return false;
  }
  if (!IsDevicePresent()) {
    return false;
  }
  return LoadGravity(xyz, nullptr);
}

bool KinectNuiSystem::IsTiltMotorMoving() {
  WantTiltThread();
  return IsDevicePresent() &&
         !tilt_motor_unavailable_.load(std::memory_order_acquire) &&
         tilt_requests_.IsMoving();
}

bool KinectNuiSystem::SetTiltDegrees(int32_t degrees) {
  WantTiltThread();
  if (!IsDevicePresent() || !cvars::nui_allow_tilt ||
      tilt_motor_unavailable_.load(std::memory_order_acquire)) {
    return false;
  }
  degrees = std::clamp(degrees, kTiltMinimumDegrees, kTiltMaximumDegrees);
  // Only recorded here; the tilt thread hands it to the motor.
  tilt_requests_.Post(degrees);
  return true;
}

}  // namespace kinect
}  // namespace nui
}  // namespace xe
