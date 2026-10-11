/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_CAMERA_H_
#define XENIA_KERNEL_XAM_NUI_CAMERA_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "xenia/kernel/xam/nui_camera_request.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace nui {
struct DepthFrame;
class NuiSystem;
}  // namespace nui
}  // namespace xe

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class NuiState;

// What the depth preview overlay shows about the camera transport.
struct NuiCameraStats {
  bool streaming = false;
  bool keep_alive = false;
  // The streams the title enabled with request code 27: bit 0 is depth, bit 1
  // is 640x480 colour.
  uint32_t stream_flags = 0;
  size_t pending_depth_requests = 0;
  size_t pending_colour_requests = 0;
  uint64_t depth_frames_fed = 0;
  uint64_t depth_frames_dropped = 0;
  uint64_t colour_frames_fed = 0;
};

// What ending a title cost the camera, so that the event can be logged with
// numbers on it rather than as "some requests", and so that a title which
// never touched the camera can be told from one that did.
struct NuiCameraTermination {
  // Frame requests that were still queued and can never be answered.
  size_t depth = 0;
  size_t colour = 0;
  // Whether there was a frame feed to stop.
  bool feed_stopped = false;
};

// The depth and colour camera transport: everything a Kinect title's NUI
// runtime asks for through the xboxkrnl export PsCamDeviceRequest. The
// runtime hands the kernel a request block per operation, queues four buffers
// per ring and waits for each of them to be completed with a frame in it, so
// this class both answers the configuration requests and runs the thread that
// fills those buffers from the host sensor. See docs/nui.md.
class NuiCamera {
 public:
  NuiCamera(NuiState* nui_state, KernelState* kernel_state);
  ~NuiCamera();

  // Answers one PsCamDeviceRequest. request_ptr points at a NuiCameraRequest;
  // what follows the common header depends on the request code.
  X_STATUS HandleRequest(uint32_t request_ptr);

  // Stops the frame feed and drops every queued request. Nothing touches the
  // guest or the host NUI system afterwards, so this has to run before either
  // of them goes away.
  void Shutdown();

  // The title is ending and its threads are still running. Refuses every new
  // request from here on and refuses to start a feed, then stops the feed and
  // waits for it, so that a completion routine which is executing right now
  // finishes against a guest that is still whole. Returns whether there was a
  // feed to stop. Must run before the title's threads are killed; the camera
  // stays in this state until OnTitleTerminated() below leaves it.
  bool OnTitleTerminating();

  // The title's threads are gone. Drops what it left queued without completing
  // any of it, and puts the camera back the way a fresh one starts, so the
  // next title in this process configures it from scratch. Unlike Shutdown()
  // the camera stays usable: the queues reopen and a new feed may be started.
  // Returns what was dropped and whether a feed had been stopped.
  NuiCameraTermination OnTitleTerminated();

  NuiCameraStats GetStats() const;

 private:
  // How long the feed waits for a frame before looking at its queues again.
  static constexpr std::chrono::milliseconds kFrameWaitTimeout{100};

  xe::nui::NuiSystem* nui_system() const;

  // One handler per request code, each answering in the block the runtime
  // handed over. The block has already been checked for writability.
  X_STATUS HandleQueryState(uint32_t request_ptr, uint32_t state);
  X_STATUS HandleGetVersion(uint32_t request_ptr);
  X_STATUS HandleGetRawToDepthTable(uint32_t request_ptr);
  X_STATUS HandleGetTiming(uint32_t request_ptr);
  // Records that the runtime has finished its configuration sequence, which is
  // what turns the state query's answer from "configure me" into "streaming"
  // and arms the next stream-start announcement.
  void MarkConfigured();
  X_STATUS HandleGetDepthParameters(uint32_t request_ptr);
  X_STATUS HandleGetExposure(uint32_t request_ptr);
  X_STATUS HandleUnpackFrame(uint32_t request_ptr);
  X_STATUS HandleQueueBuffer(uint32_t request_ptr);
  // Codes 15 and 35: answered later by the feed thread, because the runtime
  // leaves a request in flight until its completion routine runs.
  X_STATUS HandleAsyncRequest(uint32_t request_ptr);

  // Starts the frame feed if it is not running yet. False when the thread
  // could not be created or must not be created, in which case nothing may be
  // left pending.
  bool EnsureFeedThread();

  // True when the remembered feed thread has really ended. Called with
  // feed_thread_mutex_ held.
  bool HasFeedThreadEnded();

  // Tells the feed to stop, waits for it to come back out of whatever it is
  // in, and forgets it. No lock of this class is held during the wait,
  // because what the feed is in may be a guest completion routine. Returns
  // whether there was a feed to stop.
  bool StopAndJoinFeedThread();

  // Takes every frame request one ring has queued and hands them to the feed
  // thread to be completed as cancelled, so that nothing is written into a
  // slot the title has taken back and the runtime's in-flight counters still
  // reach zero.
  void CancelQueuedFrames(NuiPendingRequestQueue* queue);

  // Re-checks that both guest ranges a delivered frame writes - the ring slot
  // and the sequence number in the request block - can still be written, right
  // before the copy rather than when the request was queued. False means the
  // frame is dropped and the request failed; the first such request logs a
  // Warning and the rest are silent.
  bool IsRequestStillWritable(const NuiPendingRequest& request);

  void FeedThreadMain();
  // Hands the newest host frame to the oldest queued depth request, and a
  // constant black image to the oldest queued colour request. Frames with
  // nothing queued are dropped: the runtime only ever wants the newest one.
  void FeedDepthFrame(const xe::nui::DepthFrame& frame);
  void FeedColourFrame();
  // Answers the requests that carry no frame: the asynchronous ones with
  // success, and the ones a stop took back with X_STATUS_CANCELLED.
  void CompletePendingRequests();
  // Calls a request's completion routine on the feed thread, which is the
  // only thread here that may run guest code.
  void CompleteRequest(const NuiPendingRequest& request, X_STATUS status,
                       uint32_t bytes);

  NuiState* nui_state_;
  KernelState* kernel_state_;

  // The queued frame buffers of each ring, oldest first.
  NuiPendingRequestQueue depth_requests_;
  NuiPendingRequestQueue colour_requests_;
  // Requests that carry no frame but still have to be completed (codes 15 and
  // 35); only the request address and the completion routine are used.
  NuiPendingRequestQueue async_requests_;
  // Frame requests a stop took back, waiting to be completed as cancelled.
  // A plain list rather than a queue, because a stop can hand over both rings
  // at once and none of it may be turned away.
  mutable std::mutex cancelled_mutex_;
  std::vector<NuiPendingRequest> cancelled_requests_;

  // Guards feed_thread_ and feed_thread_abandoned_, which guest threads create
  // and Shutdown() clears.
  std::mutex feed_thread_mutex_;
  object_ref<XHostThread> feed_thread_;
  // Set when a stop could not join the feed because it was asked for from
  // inside the feed itself, which leaves a handle to a thread that is on its
  // way out. EnsureFeedThread forgets such a handle once the thread has really
  // ended, so that the next title gets a feed of its own; a handle that was
  // not abandoned is never forgotten, because a feed that left its loop on its
  // own is one no frame will ever reach again.
  bool feed_thread_abandoned_ = false;
  std::atomic<bool> feed_running_ = {false};
  // Set once Shutdown() has begun, so that no request goes pending again.
  std::atomic<bool> shut_down_ = {false};
  // Set between the two halves of a title's termination: the camera answers
  // every new request with a plain failure and starts no feed, but the feed
  // that is running may still finish what it is doing.
  std::atomic<bool> terminating_ = {false};
  // Whether the first half of the termination found a feed to stop, carried
  // to the second half so that the event can be logged with one line.
  std::atomic<bool> terminating_stopped_feed_ = {false};

  // Owned by the feed thread alone: the host-endian frame on its way to a
  // ring slot.
  std::vector<uint16_t> staging_;

  // The stream bits the runtime configured with request code 27.
  std::atomic<uint32_t> stream_flags_ = {0};

  // Set once the runtime has run its configuration sequence, which is what
  // turns the device state query's answer from "configure me" into
  // "streaming".
  std::atomic<bool> configured_ = {false};

  // Whether the host NUI system has been told that the title's stream has
  // begun. Cleared by each configuration, so that a stop and a reconfiguration
  // count as a new stream rather than as a continuation of the old one.
  std::atomic<bool> stream_start_announced_ = {false};

  // The sequence numbers the completion routines read back, one per ring, and
  // the counters the overlay shows.
  std::atomic<uint32_t> depth_sequence_ = {0};
  std::atomic<uint32_t> colour_sequence_ = {0};
  std::atomic<uint64_t> depth_frames_dropped_ = {0};

  // The device state last logged for a query-state request. The NUI runtime
  // polls that request every 30 ms while no camera is present, so it is only
  // logged when the answer changes.
  std::atomic<uint32_t> last_logged_state_ = {0};

  // Whether a ring slot that could no longer be written has been reported.
  // The re-check runs per frame, so the report is a one-shot.
  std::atomic<bool> unwritable_ring_logged_ = {false};
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_CAMERA_H_
