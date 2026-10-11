/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_CAMERA_REQUEST_H_
#define XENIA_KERNEL_XAM_NUI_CAMERA_REQUEST_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace xam {

// The NUI runtime that is statically linked into Kinect titles talks to the
// depth camera through the xboxkrnl export PsCamDeviceRequest, which takes a
// single pointer to a request block. Only the header below is common to every
// request; what follows it depends on the request code. See docs/nui.md for
// the codes and layouts recovered from 4D5308ED.
struct NuiCameraRequest {
  // One of the NuiCameraRequestCode values.
  xe::be<uint32_t> code;
  // Code 0 receives the device state here (NuiCameraDeviceState).
  xe::be<uint32_t> arg0;
  xe::be<uint32_t> arg1;
  xe::be<uint32_t> arg2;
  // Opaque to the kernel; the runtime keeps its own descriptor pointer here.
  xe::be<uint32_t> context;
  // Guest routine(request, status, bytes_transferred) that is called when a
  // request that returned X_STATUS_PENDING finishes.
  xe::be<uint32_t> completion;
  // Code 5 only: 4 = depth, 2 = colour (YUV).
  xe::be<uint32_t> image_type;
  // Code 5 only: the physically contiguous buffer the frame lands in.
  xe::be<uint32_t> buffer;
  // Code 5 only: 0x96000 for a 640x480 16-bit depth frame.
  xe::be<uint32_t> length;
};
static_assert(sizeof(NuiCameraRequest) == 36, "Size matters");

// A state query (code 0) is only the code and the state that comes back in
// arg0; the runtime builds it on its stack with nothing after those 8 bytes.
constexpr uint32_t kNuiCameraQueryStateRequestSize = 8;

// A queued frame request (code 5) carries the result of the frame that was
// delivered into it, past the end of the common header, in 16 bytes:
//
//   +64 u32 the frame's sequence number;
//   +68 u32 not written by the kernel, so it keeps what the runtime left;
//   +72 u64 the frame's timestamp, in milliseconds.
//
// The runtime's completion routine reads the sequence number and counts
// dropped frames from the gaps. Its frame processing (0x82d2324c-0x82d2325c)
// makes the sequence number the skeleton frame's dwFrameNumber and the
// timestamp its liTimeStamp; the image frames take their timestamps from the
// same word of their own blocks.
constexpr uint32_t kNuiCameraFrameSequenceOffset = 64;
constexpr uint32_t kNuiCameraFrameTimestampOffset = 72;

// The timestamp a delivered frame carries, given the guest's uptime in
// milliseconds. Only the differences between stamps matter to the title, but
// a stamp of 0 is not one it can use: its avatar poser feeds a frame to its
// avateering engine only when the stamp differs from the last one fed, which
// starts at 0, and both it and the engine take a previous stamp of 0 to mean
// that there was no previous frame (0x82b342cc, 0x82c0d6dc). So a clock that
// reads 0 gives a stamp of 1.
inline uint64_t NuiCameraFrameTimestampMillis(uint64_t guest_uptime_ms) {
  return std::max<uint64_t>(guest_uptime_ms, 1);
}

// Writes the result of a delivered frame into a queued frame request, given
// the host address of the block's first byte: the sequence number at +64 and
// the timestamp at +72, both big-endian. Nothing else in the block changes.
inline void WriteNuiCameraFrameResult(uint8_t* block, uint32_t sequence,
                                      uint64_t timestamp_ms) {
  xe::store_and_swap<uint32_t>(block + kNuiCameraFrameSequenceOffset, sequence);
  xe::store_and_swap<uint64_t>(block + kNuiCameraFrameTimestampOffset,
                               timestamp_ms);
}

// Image types a queued frame request (code 5) asks for.
enum NuiCameraImageType : uint32_t {
  kNuiCameraImageColour = 2,
  kNuiCameraImageDepth = 4,
};

// The streams a configure request (code 27) turns on.
enum NuiCameraStreamFlags : uint32_t {
  kNuiCameraStreamDepth = 1 << 0,
  kNuiCameraStreamColour = 1 << 1,
};

// Bytes one 640x480 16-bit depth frame occupies, which is also the size of
// one 640x480 YUV 4:2:2 colour frame.
constexpr uint32_t kNuiCameraDepthFrameSize = 0x96000;

// The most of a request block a log line dumps: enough for the common header
// and the sequence number a queued frame carries, and not the four kilobytes
// of a calibration block.
constexpr uint32_t kNuiCameraRequestDumpMax = 68;

// Code 13: the camera's firmware version. The runtime asks for the depth
// calibration as parameters (code 14) when major is at least 5 and minor at
// least 1, and as a table (code 11) otherwise.
struct NuiCameraGetVersionRequest {
  xe::be<uint32_t> code;
  uint8_t major;
  uint8_t minor;
  // The runtime reads neither of these, and does not clear the block before
  // the call, so the kernel zeroes them.
  uint8_t unknown[2];
};
static_assert(sizeof(NuiCameraGetVersionRequest) == 8, "Size matters");

// Code 24: fill an 8192-entry table that maps a raw depth value to its depth
// in millimetres. The skeletal tracker loads an entry unsigned and uses it as
// its metric depth, and the runtime treats 0 and anything from 4000 up as "no
// reading". It pre-fills the identity ramp and keeps it if the request fails,
// which suits a camera whose raw value already is a depth. It is the table the
// title's own shaders call g_SamplerShiftToDepth, and it is not the depth
// calibration of code 11 or code 14, which is a different thing altogether.
struct NuiCameraGetRawToDepthTableRequest {
  xe::be<uint32_t> code;
  // Always 1 in the only caller.
  xe::be<uint32_t> arg0;
  // Guest address of entry_count big-endian 16-bit entries.
  xe::be<uint32_t> buffer;
  xe::be<uint32_t> entry_count;
};
static_assert(sizeof(NuiCameraGetRawToDepthTableRequest) == 16, "Size matters");

// Code 25: the camera's timing parameters. The caller multiplies each value by
// 50 and keeps them as its timing state; the first and the sixth also go back
// to the camera with code 15, divided by 50 again.
struct NuiCameraGetTimingRequest {
  xe::be<uint32_t> code;
  xe::be<uint32_t> values[14];
};
static_assert(sizeof(NuiCameraGetTimingRequest) == 60, "Size matters");

// The ninth of the fourteen code-25 values is the only one the runtime tests:
// a zero there makes it skip the tail of its configuration - the code-15 "set
// timing" request included - the whole body of its per-frame graphics
// callback, its exposure scheduler and one branch of its colour completion.
// The other thirteen are only added, subtracted, compared or handed straight
// back with code 15; not one of them is a divisor, a modulus or a loop count,
// so a zero in them changes no control flow and the camera answers only what
// the kernel can account for.
constexpr size_t kTimingFramePeriodIndex = 8;

// Code 15: set the timing, asynchronously. The runtime leaves its "a timing
// change is in flight" flag set until the completion routine runs, so this
// request has to be completed even though nothing acts on the values.
struct NuiCameraSetTimingRequest {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown0[3];
  xe::be<uint32_t> context;
  xe::be<uint32_t> completion;
  xe::be<uint16_t> value0;
  xe::be<uint16_t> unknown1;
  xe::be<uint32_t> value1;
  xe::be<uint16_t> value2;
  xe::be<uint16_t> unknown2;
  xe::be<uint32_t> value3;
  xe::be<uint16_t> value4;
  xe::be<uint16_t> unknown3;
};
static_assert(sizeof(NuiCameraSetTimingRequest) == 44, "Size matters");

// Code 14: the depth calibration, as the parameters of the curve the runtime
// builds its lookup table from (see xe::nui::LutCalibration and docs/nui.md
// "Calibration source"). The runtime also turns the integers in the unknown
// region into shader constants for its depth-to-colour registration; nothing
// is known about them, so the kernel zeroes them.
struct NuiCameraDepthParametersRequest {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown[1057];
  xe::be<double> b;
  xe::be<double> a;
  xe::be<uint16_t> w;
  xe::be<uint16_t> h;
  xe::be<uint32_t> unknown_tail;
  xe::be<double> c;
  xe::be<double> d;
};
static_assert(sizeof(NuiCameraDepthParametersRequest) == 4272, "Size matters");

// The colour exposure settings of one stream, as the runtime's exposure
// manager sends them with code 35 and reads them back with code 36 (see
// docs/nui.md, "Request codes"). A read-back keeps only the low four bits of
// flags and the four values, and one caller reads the word after them.
struct NuiCameraExposure {
  // 0x80000000 as the runtime sends it. In a read-back the low four bits say
  // which values are valid, and the runtime caches each valid one: bits 1 to
  // 3 mark values[0] to values[2] and bit 0 marks values[3].
  xe::be<uint32_t> flags;
  // 2 for 640x480, else 1.
  xe::be<uint32_t> resolution;
  xe::be<uint32_t> unknown0[6];
  // Sent as 0x1999, 0x1999, 0xCCCC and 0xCCCC, and not read back.
  xe::be<uint32_t> limits[4];
  // 16.16 fixed point.
  xe::be<uint32_t> values[4];
  // The one word the second sender of code 36 reads back.
  xe::be<uint32_t> readback;
  xe::be<uint32_t> unknown1[6];
};
static_assert(sizeof(NuiCameraExposure) == 92, "Size matters");

// Code 36: read one stream's exposure settings back. Both senders build the
// block on their stacks and set nothing but the code and the completion, so
// everything the runtime reads back has to be written by the kernel.
struct NuiCameraGetExposureRequest {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown0[4];
  xe::be<uint32_t> completion;
  xe::be<uint32_t> unknown1;
  NuiCameraExposure exposure;
  xe::be<uint32_t> unknown2[6];
};
static_assert(sizeof(NuiCameraGetExposureRequest) == 144, "Size matters");

// Request codes as issued by NUI runtime 2.0.11775; the names describe what
// the title does with each one, not what the kernel calls them.
enum NuiCameraRequestCode : uint32_t {
  kNuiCameraRequestQueryState = 0,
  kNuiCameraRequestStop = 2,
  kNuiCameraRequestQueueBuffer = 5,
  kNuiCameraRequestGetDepthTable = 11,
  kNuiCameraRequestGetVersion = 13,
  kNuiCameraRequestGetDepthParameters = 14,
  kNuiCameraRequestSetTiming = 15,
  kNuiCameraRequestGetRawToDepthTable = 24,
  kNuiCameraRequestGetTiming = 25,
  kNuiCameraRequestConfigure = 27,
  kNuiCameraRequestUnpackFrame = 30,
  kNuiCameraRequestSetMode = 33,
  kNuiCameraRequestSetExposure = 35,
  kNuiCameraRequestGetExposure = 36,
};

// Device states returned by kNuiCameraRequestQueryState in arg0, as the
// runtime interprets them: 1 keeps it polling, 2 starts streaming right away,
// 4 runs the configuration requests first, 3 and 5 count as present but
// unusable, 6 and 7 make it send a stop request.
enum NuiCameraDeviceState : uint32_t {
  kNuiCameraDeviceNotPresent = 1,
  kNuiCameraDeviceReady = 2,
  kNuiCameraDeviceNeedsConfiguration = 4,
};

inline const char* NuiCameraRequestCodeName(uint32_t code) {
  switch (code) {
    case kNuiCameraRequestQueryState:
      return "query state";
    case kNuiCameraRequestStop:
      return "stop";
    case kNuiCameraRequestQueueBuffer:
      return "queue buffer";
    case kNuiCameraRequestGetDepthTable:
      return "get depth table";
    case kNuiCameraRequestGetVersion:
      return "get version";
    case kNuiCameraRequestGetDepthParameters:
      return "get depth parameters";
    case kNuiCameraRequestSetTiming:
      return "set timing";
    case kNuiCameraRequestGetRawToDepthTable:
      return "get raw-to-depth table";
    case kNuiCameraRequestGetTiming:
      return "get timing";
    case kNuiCameraRequestConfigure:
      return "configure";
    case kNuiCameraRequestUnpackFrame:
      return "unpack frame";
    case kNuiCameraRequestSetMode:
      return "set mode";
    case kNuiCameraRequestSetExposure:
      return "set exposure";
    case kNuiCameraRequestGetExposure:
      return "get exposure";
    default:
      return "unknown";
  }
}

// Bytes of the request block the kernel may touch for a request code. Blocks
// the kernel only reads are sized by the common header; the rest are sized by
// the last field the runtime reads back, because the block is usually a piece
// of a stack frame or of the runtime's state that has other things after it.
inline uint32_t NuiCameraRequestSize(uint32_t code) {
  switch (code) {
    case kNuiCameraRequestQueryState:
      return kNuiCameraQueryStateRequestSize;
    case kNuiCameraRequestQueueBuffer:
      // Through the end of the frame's result, the timestamp. The runtime
      // builds each block 8 bytes into a 128-byte descriptor (docs/nui.md,
      // "Buffers and rings"), so there is room for it. The kernel sees only
      // whether these bytes are writable, not where a block ends: a title
      // whose block were shorter would be refused only where the bytes past
      // its end are not writable, as with the 68 bytes checked before the
      // timestamp was written.
      return kNuiCameraFrameTimestampOffset + sizeof(uint64_t);
    case kNuiCameraRequestGetVersion:
      return static_cast<uint32_t>(sizeof(NuiCameraGetVersionRequest));
    case kNuiCameraRequestGetRawToDepthTable:
      return static_cast<uint32_t>(sizeof(NuiCameraGetRawToDepthTableRequest));
    case kNuiCameraRequestGetTiming:
      return static_cast<uint32_t>(sizeof(NuiCameraGetTimingRequest));
    case kNuiCameraRequestSetTiming:
      return static_cast<uint32_t>(sizeof(NuiCameraSetTimingRequest));
    case kNuiCameraRequestGetDepthParameters:
      return static_cast<uint32_t>(sizeof(NuiCameraDepthParametersRequest));
    case kNuiCameraRequestGetExposure:
      return static_cast<uint32_t>(sizeof(NuiCameraGetExposureRequest));
    default:
      return static_cast<uint32_t>(sizeof(NuiCameraRequest));
  }
}

// One queued frame request (code 5): where the frame goes and how to tell the
// runtime that it arrived.
struct NuiPendingRequest {
  // Guest address of the request block, which is also the completion
  // routine's first argument.
  uint32_t request;
  // NuiCameraImageType.
  uint32_t image_type;
  // Guest address of the ring slot the frame is written into.
  uint32_t buffer;
  // Bytes of that slot, as the runtime asked for them.
  uint32_t length;
  // Guest routine to call once the frame is in the slot.
  uint32_t completion;
};

// The frame requests one ring has queued and the kernel has not answered yet,
// oldest first. Guest threads queue; the frame feed takes. Once stopped the
// queue is empty and stays empty, which is what keeps the feed from calling
// into a guest that is going away.
class NuiPendingRequestQueue {
 public:
  // The runtime queues one request per ring slot and there are four slots per
  // ring; more than this means nothing is completing them.
  static constexpr size_t kMaximumRequests = 8;

  // Takes a request, unless the queue is stopped, its title is terminating or
  // it is already full.
  bool Enqueue(const NuiPendingRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || terminating_ || requests_.size() >= kMaximumRequests) {
      return false;
    }
    requests_.push_back(request);
    return true;
  }

  // Hands back the request that has waited longest. False when there is none.
  bool TakeOldest(NuiPendingRequest* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requests_.empty()) {
      return false;
    }
    *out = requests_.front();
    requests_.pop_front();
    return true;
  }

  // Appends everything queued, oldest first, and leaves the queue empty. The
  // queue stays open: this is how a stream that has been turned off gives its
  // requests back so they can be answered, rather than left in limbo.
  void TakeAll(std::vector<NuiPendingRequest>* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out->insert(out->end(), requests_.begin(), requests_.end());
    requests_.clear();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.size();
  }

  bool stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

  // Refuses anything new from here on but keeps what is already queued. This
  // is the first half of a title ending, which runs while the title's threads
  // are still alive: a frame the feed is about to deliver still has a live
  // guest to deliver it to, so only new work is turned away. Refusing inside
  // the same lock the enqueue takes is what leaves no window between a caller
  // seeing that the camera is quiescing and its request landing here anyway.
  void BeginTerminating() {
    std::lock_guard<std::mutex> lock(mutex_);
    terminating_ = true;
  }

  bool terminating() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminating_;
  }

  // Discards everything queued and refuses anything new from here on, and
  // says how many requests went undelivered. Nothing dropped here is ever
  // completed: a completion routine is guest code, and this runs when the
  // guest that owns it is going away.
  size_t Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    const size_t dropped = requests_.size();
    requests_.clear();
    return dropped;
  }

  // Takes requests again. A stop that came from a title ending has to be
  // undone before the next title can queue anything, because the queue is as
  // long-lived as the process and a title of its own is all a stopped queue
  // would ever refuse.
  void Reopen() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = false;
    terminating_ = false;
    requests_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<NuiPendingRequest> requests_;
  bool stopped_ = false;
  // Set for the window between the two halves of a title's termination, in
  // which the queue hands out what it holds but takes nothing new.
  bool terminating_ = false;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_CAMERA_REQUEST_H_
