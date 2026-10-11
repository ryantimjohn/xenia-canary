/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/replay/replay_nui_system.h"

#include <algorithm>
#include <cstring>
#include <system_error>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/nui/depth_recording.h"
#include "xenia/nui/nui_flags.h"

namespace xe {
namespace nui {
namespace replay {

// How long to wait between frames whose recorded timestamps say nothing: the
// sensor's own 30 Hz.
static constexpr std::chrono::milliseconds kDefaultFrameInterval{33};
// The longest gap in a recording that is played back as a gap. A longer one -
// a sensor that was unplugged for a while, or an emulator that was paused - is
// not worth making anyone sit through again.
static constexpr std::chrono::milliseconds kMaximumFrameInterval{200};

// How many bytes the file header and one frame take up in a recording.
static constexpr int64_t kRecordingHeaderBytes =
    static_cast<int64_t>(sizeof(DepthRecordingHeader));
static constexpr int64_t kRecordedFrameBytes =
    static_cast<int64_t>(sizeof(DepthRecordingFrameHeader)) +
    static_cast<int64_t>(DepthFrame::kWidth) * DepthFrame::kHeight *
        sizeof(uint16_t);

namespace {

// The path as the filesystem sees it, so that two spellings of one file can be
// told apart from two files. Falls back to the path as written when it cannot
// be resolved.
std::filesystem::path ResolvePath(const std::filesystem::path& path) {
  std::error_code error_code;
  std::filesystem::path resolved =
      std::filesystem::weakly_canonical(path, error_code);
  return error_code ? path.lexically_normal() : resolved;
}

}  // namespace

bool ReplayNuiSystem::IsAvailable() {
  if (cvars::nui_replay.empty()) {
    return false;
  }
  const auto info = xe::filesystem::GetInfo(xe::to_path(cvars::nui_replay));
  return info.has_value() &&
         info->type == xe::filesystem::FileInfo::Type::kFile;
}

ReplayNuiSystem::ReplayNuiSystem() = default;

ReplayNuiSystem::~ReplayNuiSystem() {
  // Shutdown() is what stops the worker and closes the recording. This only
  // catches an instance that was dropped without one, such as a Setup() that
  // failed before the worker started.
  CloseRecording();
}

X_STATUS ReplayNuiSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = NuiSystem::Setup(kernel_state);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }

  path_ = xe::to_path(cvars::nui_replay);
  if (!cvars::nui_record.empty() &&
      ResolvePath(xe::to_path(cvars::nui_record)) == ResolvePath(path_)) {
    // The recorder opens its file truncating, so letting both flags name one
    // file would destroy the recording this run is here to play. Setting both
    // is an easy copy-paste out of the two documented command lines.
    XELOGE(
        "NUI: nui_record and nui_replay both name {}. Recording over the "
        "recording being replayed would destroy it; drop one of the two.",
        xe::path_to_utf8(path_));
    return X_STATUS_UNSUCCESSFUL;
  }

  result = OpenRecording();
  if (result != X_STATUS_SUCCESS) {
    return result;
  }

  shutdown_event_ = threading::Event::CreateManualResetEvent(false);
  if (!shutdown_event_) {
    XELOGE("NUI: failed to create the depth replay's shutdown event.");
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }

  worker_running_.store(true, std::memory_order_release);
  worker_thread_ =
      threading::Thread::Create({}, [this]() { WorkerThreadMain(); });
  if (!worker_thread_) {
    XELOGE("NUI: failed to start the depth replay thread.");
    worker_running_.store(false, std::memory_order_release);
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }
  worker_thread_->set_name("NUI Replay");
  return X_STATUS_SUCCESS;
}

void ReplayNuiSystem::Shutdown() {
  // Stop the worker before anything else: once it has been joined it can no
  // longer publish a frame or fire a device status callback into a base class
  // that is halfway through its own teardown.
  worker_running_.store(false, std::memory_order_release);
  if (worker_thread_) {
    if (shutdown_event_) {
      shutdown_event_->Set();
    }
    // Unlike the Kinect pump, this worker only ever waits on that event and on
    // reads from a local file, both of which come back, so the join does not
    // have to be bounded. That does assume the recording is on local storage:
    // one on a network share that disconnects mid-read would wedge shutdown
    // here the way a stuck sensor driver wedges the Kinect pump.
    threading::Wait(worker_thread_.get(), false);
    worker_thread_.reset();
  }
  CloseRecording();
  NuiSystem::Shutdown();
}

void ReplayNuiSystem::OnGuestStreamStarted() {
  // Only a flag is raised here. This runs on the guest thread that started the
  // stream, and the recording's read position belongs to the worker thread
  // alone, so the seek happens there, at the top of its next turn round the
  // loop.
  restart_requested_.store(true, std::memory_order_release);
}

X_STATUS ReplayNuiSystem::OpenRecording() {
  const std::string path_utf8 = xe::path_to_utf8(path_);
  file_ = xe::filesystem::OpenFile(path_, "rb");
  if (!file_) {
    XELOGE("NUI: could not open the depth recording {}.", path_utf8);
    return X_STATUS_UNSUCCESSFUL;
  }

  DepthRecordingHeader header = {};
  if (fread(&header, sizeof(header), 1, file_) != 1 ||
      std::memcmp(header.magic, kDepthRecordingMagic, sizeof(header.magic)) !=
          0) {
    XELOGE("NUI: {} does not start with a depth recording header.", path_utf8);
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }
  if (header.width != DepthFrame::kWidth ||
      header.height != DepthFrame::kHeight) {
    XELOGE("NUI: the depth recording {} is {}x{}, not {}x{}.", path_utf8,
           header.width, header.height, DepthFrame::kWidth,
           DepthFrame::kHeight);
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }

  // The frames are all the same size, so the file's own size says how many it
  // holds. A trailing frame that an emulator killed mid-write left incomplete
  // is not one of them and is never played.
  if (!xe::filesystem::Seek(file_, 0, SEEK_END)) {
    XELOGE("NUI: could not measure the depth recording {}.", path_utf8);
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }
  const int64_t file_size = xe::filesystem::Tell(file_);
  const int64_t frame_bytes = file_size - kRecordingHeaderBytes;
  frame_count_ = frame_bytes > 0
                     ? static_cast<uint64_t>(frame_bytes / kRecordedFrameBytes)
                     : 0;
  if (!frame_count_) {
    XELOGE("NUI: the depth recording {} holds no complete frame.", path_utf8);
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!xe::filesystem::Seek(file_, kRecordingHeaderBytes, SEEK_SET)) {
    XELOGE("NUI: could not rewind the depth recording {}.", path_utf8);
    CloseRecording();
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

void ReplayNuiSystem::CloseRecording() {
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
}

bool ReplayNuiSystem::SeekToFirstFrame() {
  return xe::filesystem::Seek(file_, kRecordingHeaderBytes, SEEK_SET);
}

void ReplayNuiSystem::WorkerThreadMain() {
  SetDeviceStatus(DeviceStatus::kInitializing);
  SetDeviceStatus(DeviceStatus::kReady);
  XELOGI("NUI: replaying {} ({} frames)", xe::path_to_utf8(path_),
         frame_count_);

  bool have_previous_frame = false;
  uint64_t previous_timestamp_us = 0;
  while (worker_running_.load(std::memory_order_acquire)) {
    if (restart_requested_.exchange(false, std::memory_order_acq_rel)) {
      // The title has started consuming frames, so it gets the recording from
      // the beginning rather than from wherever the loop had reached.
      if (!SeekToFirstFrame()) {
        XELOGE("NUI: {} could not be rewound; the replay has stopped.",
               xe::path_to_utf8(path_));
        SetDeviceStatus(DeviceStatus::kError);
        break;
      }
      // The next frame is the first of the recording again, and there is
      // nothing left to pace it against, so it goes out as soon as it is read.
      have_previous_frame = false;
      XELOGI(
          "NUI: the title has started streaming; {} restarts at its first "
          "frame, {} frames in",
          xe::path_to_utf8(path_), published_frame_count_);
    }

    auto frame = frame_pool_.Acquire();
    uint64_t timestamp_us = 0;
    if (!ReadNextFrame(frame.get(), &timestamp_us)) {
      XELOGE("NUI: {} could not be read any further; the replay has stopped.",
             xe::path_to_utf8(path_));
      SetDeviceStatus(DeviceStatus::kError);
      break;
    }

    if (have_previous_frame) {
      // Hand the frames over at the speed they were captured at. A pair with
      // no usable gap between them - two frames of one burst, or the wrap back
      // to the start of the file - falls back to the sensor's frame rate.
      std::chrono::milliseconds interval = kDefaultFrameInterval;
      if (timestamp_us > previous_timestamp_us) {
        interval =
            std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::microseconds(timestamp_us -
                                                     previous_timestamp_us)),
                       std::chrono::milliseconds(0), kMaximumFrameInterval);
      }
      if (!SleepWhileRunning(interval)) {
        break;
      }
    }
    have_previous_frame = true;
    previous_timestamp_us = timestamp_us;

    PublishFrame(std::move(frame));
    ++published_frame_count_;
    if ((published_frame_count_ % kFrameLogInterval) == 0) {
      XELOGI("NUI: {} depth frames replayed", published_frame_count_);
    }
  }
}

bool ReplayNuiSystem::SleepWhileRunning(std::chrono::milliseconds duration) {
  if (!worker_running_.load(std::memory_order_acquire)) {
    return false;
  }
  return threading::Wait(shutdown_event_.get(), false, duration) ==
             threading::WaitResult::kTimeout &&
         worker_running_.load(std::memory_order_acquire);
}

bool ReplayNuiSystem::ReadNextFrame(DepthFrame* frame,
                                    uint64_t* out_recorded_timestamp_us) {
  const size_t sample_bytes = frame->depth_mm.size() * sizeof(uint16_t);
  // Two goes: one for the frame that is next, and one for the first frame of
  // the recording after the file has run out. Setup() made sure there is at
  // least one complete frame to go back to.
  for (int attempt = 0; attempt < 2; ++attempt) {
    DepthRecordingFrameHeader header = {};
    if (fread(&header, sizeof(header), 1, file_) == 1 &&
        fread(frame->depth_mm.data(), 1, sample_bytes, file_) == sample_bytes) {
      *out_recorded_timestamp_us = header.timestamp_us;
      return true;
    }
    // Either the file ended or its last frame was cut short by an emulator
    // that was killed mid-write. Start the recording over. The published
    // sequence numbers keep climbing: PublishFrame is what assigns them.
    if (ferror(file_) || !SeekToFirstFrame()) {
      return false;
    }
  }
  return false;
}

bool ReplayNuiSystem::GetTiltDegrees(int32_t* out_degrees) {
  if (!out_degrees) {
    return false;
  }
  // "False when there is no device" is the contract in nui_system.h, and it
  // is what the Kinect backend and this backend's own SetTiltDegrees answer.
  // The worker reaches kReady at its first instruction, so this changes no
  // run; the two backends disagreeing about the contract is the defect.
  if (!IsDevicePresent()) {
    return false;
  }
  if (GetTiltOverrideDegrees(out_degrees)) {
    return true;
  }
  // There is no motor to read an angle back from, so a title is told that the
  // one it last asked for is where the sensor now is, which is what a motor
  // that obeyed the request would have reported.
  *out_degrees = tilt_degrees_.load(std::memory_order_acquire);
  return true;
}

bool ReplayNuiSystem::SetTiltDegrees(int32_t degrees) {
  // A recording has no motor of its own, but nui_allow_tilt is still the
  // user's answer to whether a title may move one at all, and the request is
  // answered the way the hardware would answer it.
  if (!IsDevicePresent() || !cvars::nui_allow_tilt) {
    return false;
  }
  tilt_degrees_.store(
      std::clamp(degrees, kTiltMinimumDegrees, kTiltMaximumDegrees),
      std::memory_order_release);
  return true;
}

}  // namespace replay
}  // namespace nui
}  // namespace xe
