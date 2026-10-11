/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_REPLAY_REPLAY_NUI_SYSTEM_H_
#define XENIA_NUI_REPLAY_REPLAY_NUI_SYSTEM_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "xenia/base/threading.h"
#include "xenia/nui/depth_frame_pool.h"
#include "xenia/nui/nui_system.h"

namespace xe {
namespace nui {
namespace replay {

// A NUI system that publishes the depth frames of a recording made with the
// nui_record cvar instead of the ones a sensor would hand over. It is what
// makes a Kinect session repeatable: record once with somebody standing in
// front of the sensor, then run the title over and over with nobody there.
//
// The recording is streamed from disk a frame at a time - a minute of depth is
// well over a gigabyte - on one worker thread, which also does the waiting
// that paces the frames at the speed they were captured at.
class ReplayNuiSystem : public NuiSystem {
 public:
  ReplayNuiSystem();
  ~ReplayNuiSystem() override;

  // True only when the nui_replay cvar names a file that exists, so that
  // --nui=any can never pick this backend without a recording to play.
  static bool IsAvailable();

  std::string name() const override { return "Depth replay"; }

  X_STATUS Setup(kernel::KernelState* kernel_state) override;
  void Shutdown() override;

  // Asks the worker to start the recording over, so that the title's first
  // frames are the recording's first frames.
  void OnGuestStreamStarted() override;

  bool GetTiltDegrees(int32_t* out_degrees) override;
  bool SetTiltDegrees(int32_t degrees) override;

 private:
  // How often the worker says that it is still alive, in frames.
  static constexpr uint64_t kFrameLogInterval = 300;

  // Reads and validates the recording's file header and counts the complete
  // frames behind it, leaving the read position on the first one. Returns an
  // error status, having logged why, when the file is not one we can play.
  X_STATUS OpenRecording();
  // Closes the recording. Safe to call when it was never opened.
  void CloseRecording();
  // Puts the read position back on the recording's first frame. Returns false
  // when the file can no longer be seeked, which is the end of the replay.
  // Called on whichever thread owns the recording.
  bool SeekToFirstFrame();

  void WorkerThreadMain();
  // Waits for the given duration unless the worker is asked to stop first.
  // Returns false when it is time to leave the worker loop.
  bool SleepWhileRunning(std::chrono::milliseconds duration);

  // Reads the next frame of the recording into frame, going back to the first
  // frame at the end of the file. Returns false when the recording can no
  // longer be read at all.
  bool ReadNextFrame(DepthFrame* frame, uint64_t* out_recorded_timestamp_us);

  std::filesystem::path path_;
  // The recording, owned by whichever thread has it open: Setup() and
  // Shutdown() before and after the worker runs, the worker in between.
  FILE* file_ = nullptr;
  uint64_t frame_count_ = 0;

  std::unique_ptr<threading::Thread> worker_thread_;
  std::atomic<bool> worker_running_ = {false};
  // Set by OnGuestStreamStarted() and cleared by the worker once it has gone
  // back to the recording's first frame. The seek itself belongs to the
  // worker: it is the only thread that owns the read position.
  std::atomic<bool> restart_requested_ = {false};
  // Signalled by Shutdown() to get the worker out of its waits at once.
  std::unique_ptr<threading::Event> shutdown_event_;

  // Touched by the worker thread only.
  uint64_t published_frame_count_ = 0;
  DepthFramePool frame_pool_;

  // The angle a title last asked the motor for. There is no motor behind a
  // recording, so this is also the angle the title reads back.
  std::atomic<int32_t> tilt_degrees_ = {0};
};

}  // namespace replay
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_REPLAY_REPLAY_NUI_SYSTEM_H_
