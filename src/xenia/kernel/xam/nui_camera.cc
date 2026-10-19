/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_camera.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/kernel/xam/nui_tracking_observer.h"
#include "xenia/nui/depth_frame.h"
#include "xenia/nui/nui_depth_convert.h"
#include "xenia/nui/nui_flags.h"
#include "xenia/nui/nui_system.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

uint64_t TrackingNowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void LogTrackingStats(const NuiTrackingObserver::Stats& s,
                      const char* scope) {
  XELOGI(
      "NUI tracking observer {}: sessions={} source={} seq=[{},{}] skipped={} "
      "seq_resets={} no_buffer={} "
      "invalid_buffer={} copied_stamped={} completion_called={} "
      "callback_returned={} host_age_unknown={} host_age_max_ms={} "
      "delivery_gap_max_ms={} hud={} slots=[{},{},{},{}] "
      "joints=[{},{},{},{}] ids=[{},{},{}] engaged=[{},{},{}] "
      "timestamp=[{},{},{}] hud_lag_max_ms={} hud_lag_unknown={} "
      "quality_nonzero={} nonfinite={} zero={}",
      scope, s.sessions, s.source_frames, s.source_first, s.source_last,
      s.source_skipped, s.source_resets, s.no_buffer,
      s.invalid_buffer, s.copied_stamped, s.completion_called,
      s.callback_returned, s.host_age_unknown, s.host_age_max_ms,
      s.delivery_gap_max_ms, s.hud_frames, s.slot_states[0],
      s.slot_states[1], s.slot_states[2], s.slot_states[3],
      s.joint_states[0], s.joint_states[1], s.joint_states[2],
      s.joint_states[3], s.id_appeared, s.id_disappeared, s.id_replaced,
      s.engaged_matched, s.engaged_unmatched, s.engaged_ambiguous,
      s.timestamp_exact, s.timestamp_ambiguous, s.timestamp_unmatched,
      s.hud_lag_max_ms, s.hud_lag_unknown, s.quality_nonzero,
      s.nonfinite_vectors, s.zero_vectors);
}

void FlushTrackingRow(uint64_t now_us) {
  auto row = GetNuiTrackingObserver().TakeRow(now_us);
  if (row) LogTrackingStats(*row, "one-second row");
}

// Switch (read off the texture the runtime binds over the depth ring: format
// word 0x1A22005A, whose endianness field is 1 = 8-in-16, so the GPU swaps
// every halfword on its way in and the ring itself therefore holds
// big-endian values). If the tracker only ever sees garbage, false writes the
// ring host-endian instead.
constexpr bool kRingIsBigEndian = true;

// The firmware version the kernel reports for the camera. Major 5 with minor
// 1 is what makes the runtime ask for the depth calibration as parameters
// (code 14), which is the path the kernel can answer.
constexpr uint8_t kCameraFirmwareMajor = 5;
constexpr uint8_t kCameraFirmwareMinor = 1;

// The raw-value-to-millimetre depth table the runtime asks for with code 24
// has this many entries, one per 13-bit raw value; anything else means the
// request is not the one this kernel knows.
constexpr uint32_t kRawToDepthTableEntries = 8192;

// One YUY2 pixel pair is Y U Y V, and black is a luma of 16 with both chroma
// channels at 128. Written as bytes, so the pattern does not depend on the
// host's byte order.
constexpr uint8_t kColourBlackLuma = 0x10;
constexpr uint8_t kColourBlackChroma = 0x80;

// One frame of a 30 Hz depth stream, in microseconds. The runtime multiplies
// every code-25 value by 50 to reach the Xenon timebase of 49.875 MHz, so this
// arrives as 1,666,650 ticks, which is the frame period its own hard-coded
// modulus of 1,666,666 ticks already describes.
constexpr uint32_t kDepthFramePeriodUs = 33333;

// The exposure settings code 36 reads back, as the title's own code-35 block
// sends them (docs/nui.md, "Request codes"): its 0x80000000 stream flag with
// the four "value valid" bits set, the 640x480 resolution, the four limits it
// sends, and 1.0 in 16.16 fixed point for each of the four values.
constexpr uint32_t kExposureFlagsAllValid = 0x8000000F;
constexpr uint32_t kExposureResolution640x480 = 2;
constexpr uint32_t kExposureLimits[] = {0x1999, 0x1999, 0xCCCC, 0xCCCC};
constexpr uint32_t kExposureValueOne = 0x00010000;
static_assert(xe::countof(kExposureLimits) ==
                  std::extent_v<decltype(NuiCameraExposure::limits)>,
              "one limit per exposure limit word");

}  // namespace

NuiCamera::NuiCamera(NuiState* nui_state, KernelState* kernel_state)
    : nui_state_(nui_state), kernel_state_(kernel_state) {}

NuiCamera::~NuiCamera() { Shutdown(); }

xe::nui::NuiSystem* NuiCamera::nui_system() const {
  auto* emulator = kernel_state_->emulator();
  return emulator ? emulator->nui_system() : nullptr;
}

X_STATUS NuiCamera::HandleRequest(uint32_t request_ptr) {
  // The code comes first, and how much of the block exists depends on it.
  auto* memory = kernel_state_->memory();
  if (!nui_state_->IsGuestRangeReadable(request_ptr, sizeof(uint32_t))) {
    XELOGW("NUI: camera request at {:08X} is not readable", request_ptr);
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint32_t code =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(request_ptr));

  X_STATUS result = X_STATUS_DEVICE_NOT_CONNECTED;
  bool log = cvars::nui_log_messages;
  if (code == kNuiCameraRequestQueryState) {
    // State 4 means "present, configure me" and state 2 means "configured and
    // streaming": the runtime expects to see 4 once and 2 from then on. Its
    // completion routines re-probe the state while frames flow, and in their
    // state tables anything but 2 - 4 included - means the stream is not
    // live, so answering 4 forever makes the first colour completion revoke
    // the streaming flag and tear the pipeline down. Answering 2 from the
    // start is just as wrong: the runtime would skip the configuration and
    // never build its depth lookup table.
    uint32_t state = kNuiCameraDeviceNotPresent;
    if (nui_state_->IsDevicePresent()) {
      state = configured_.load(std::memory_order_acquire)
                  ? kNuiCameraDeviceReady
                  : kNuiCameraDeviceNeedsConfiguration;
    } else {
      // A camera that goes away has to be configured again when it returns.
      configured_.store(false, std::memory_order_release);
    }
    result = HandleQueryState(request_ptr, state);
    // The runtime polls this every 30 ms and re-probes it from inside its
    // completion routines, so the Info line is only worth printing when the
    // answer changes - but every probe is visible at Debug, because a re-probe
    // that nothing logged is what hid this bug.
    XELOGD("NUI: camera state query at {:08X} = {}", request_ptr, state);
    log = log && last_logged_state_.exchange(
                     state, std::memory_order_relaxed) != state;
  } else if (!nui_state_->IsDevicePresent()) {
    // Without a sensor the camera answers nothing but the state query, which
    // is what the runtime expects of a console with no Kinect plugged in.
  } else {
    switch (code) {
      case kNuiCameraRequestGetVersion: {
        result = HandleGetVersion(request_ptr);
      } break;
      case kNuiCameraRequestGetRawToDepthTable: {
        result = HandleGetRawToDepthTable(request_ptr);
      } break;
      case kNuiCameraRequestGetTiming: {
        result = HandleGetTiming(request_ptr);
        if (XSUCCEEDED(result)) {
          // Answering code 25 is what configures the camera, so the transition
          // belongs here beside the ones a configure and a stop make, not
          // inside a handler whose name only promises to fill a block.
          MarkConfigured();
        }
      } break;
      case kNuiCameraRequestGetDepthParameters: {
        result = HandleGetDepthParameters(request_ptr);
      } break;
      case kNuiCameraRequestGetExposure: {
        result = HandleGetExposure(request_ptr);
      } break;
      case kNuiCameraRequestUnpackFrame: {
        result = HandleUnpackFrame(request_ptr);
      } break;
      case kNuiCameraRequestQueueBuffer: {
        result = HandleQueueBuffer(request_ptr);
      } break;
      case kNuiCameraRequestSetTiming:
      case kNuiCameraRequestSetExposure: {
        result = HandleAsyncRequest(request_ptr);
      } break;
      case kNuiCameraRequestConfigure: {
        // The stream bits live where a queued frame keeps its buffer: bit 0
        // is depth, bit 1 is 640x480 colour.
        if (!nui_state_->IsGuestRangeReadable(request_ptr,
                                              sizeof(NuiCameraRequest))) {
          result = X_STATUS_INVALID_PARAMETER;
          break;
        }
        const auto* request =
            memory->TranslateVirtual<const NuiCameraRequest*>(request_ptr);
        const uint32_t flags = request->buffer;
        stream_flags_.store(flags, std::memory_order_relaxed);
        // A configure starts the sequence that code 25 ends; until it does,
        // the state query has to keep asking for configuration.
        configured_.store(false, std::memory_order_release);
        // A stream that has just been turned off gets its queued buffers
        // back; nothing may be written into them any more.
        if (!(flags & kNuiCameraStreamDepth)) {
          CancelQueuedFrames(&depth_requests_);
        }
        if (!(flags & kNuiCameraStreamColour)) {
          CancelQueuedFrames(&colour_requests_);
        }
        result = X_STATUS_SUCCESS;
      } break;
      case kNuiCameraRequestStop: {
        // The camera is no longer configured, so a restart gets state 4 and
        // the whole configuration sequence again.
        configured_.store(false, std::memory_order_release);
        // A stop takes both rings back. Leaving them queued would have the
        // feed writing frames into slots the title considers released, and
        // would fill the queues when it starts again.
        CancelQueuedFrames(&depth_requests_);
        CancelQueuedFrames(&colour_requests_);
        result = X_STATUS_SUCCESS;
      } break;
      case kNuiCameraRequestSetMode: {
        // Nothing to configure on this side, and no output the runtime reads
        // back.
        result = X_STATUS_SUCCESS;
      } break;
      default: {
        // Anything else is a request no run has produced. It is answered
        // "this camera does not do that" rather than with the initial
        // X_STATUS_DEVICE_NOT_CONNECTED, which the runtime reads as "the
        // camera went away" (docs/nui.md, "Frame transport") and would act
        // on by tearing down a device that is sitting right there.
        result = X_STATUS_NOT_SUPPORTED;
      } break;
    }
  }

  if (log) {
    const uint32_t request_size = NuiCameraRequestSize(code);
    XELOGI("NUI: camera request {} ({}) at {:08X} = {:08X} [{}]", code,
           NuiCameraRequestCodeName(code), request_ptr, result,
           nui_state_->DumpGuestBytes(
               request_ptr, std::min(request_size, kNuiCameraRequestDumpMax)));
  }
  return result;
}

X_STATUS NuiCamera::HandleQueryState(uint32_t request_ptr, uint32_t state) {
  // The answer goes back into the block, so it has to be writable.
  if (!nui_state_->IsGuestRangeWritable(request_ptr,
                                        kNuiCameraQueryStateRequestSize)) {
    XELOGW("NUI: camera state request at {:08X} is not writable", request_ptr);
    return X_STATUS_INVALID_PARAMETER;
  }
  xe::store_and_swap<uint32_t>(
      kernel_state_->memory()->TranslateVirtual(
          request_ptr + offsetof(NuiCameraRequest, arg0)),
      state);
  return X_STATUS_SUCCESS;
}

X_STATUS NuiCamera::HandleGetVersion(uint32_t request_ptr) {
  if (!nui_state_->IsGuestRangeWritable(request_ptr,
                                        sizeof(NuiCameraGetVersionRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto* block =
      kernel_state_->memory()->TranslateVirtual<NuiCameraGetVersionRequest*>(
          request_ptr);
  block->major = kCameraFirmwareMajor;
  block->minor = kCameraFirmwareMinor;
  block->unknown[0] = 0;
  block->unknown[1] = 0;
  return X_STATUS_SUCCESS;
}

X_STATUS NuiCamera::HandleGetRawToDepthTable(uint32_t request_ptr) {
  if (!nui_state_->IsGuestRangeReadable(
          request_ptr, sizeof(NuiCameraGetRawToDepthTableRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto* memory = kernel_state_->memory();
  const auto* block =
      memory->TranslateVirtual<const NuiCameraGetRawToDepthTableRequest*>(
          request_ptr);
  const uint32_t table_ptr = block->buffer;
  const uint32_t entry_count = block->entry_count;
  const uint32_t table_bytes = entry_count * uint32_t(sizeof(uint16_t));
  if (entry_count != kRawToDepthTableEntries ||
      !nui_state_->IsGuestRangeWritable(table_ptr, table_bytes)) {
    XELOGW("NUI: raw-to-depth table of {} entries at {:08X} cannot be filled",
           entry_count, table_ptr);
    return X_STATUS_INVALID_PARAMETER;
  }
  // The depth in millimetres each raw ring value stands for. The title's
  // skeletal tracker reads this table with an unsigned halfword load and uses
  // the entry as its metric depth with no scaling of any kind, so the table
  // is the sensor's raw-value-to-millimetre conversion, which on real
  // hardware is a property of the camera's firmware. Raw values the encoding
  // never produces read 0, which both the tracker and the runtime take as "no
  // reading". The runtime pre-fills the identity ramp and keeps it if this
  // request fails, which suits a camera whose raw value already is a depth.
  auto* table = memory->TranslateVirtual<uint8_t*>(table_ptr);
  for (uint32_t i = 0; i < entry_count; ++i) {
    xe::store_and_swap<uint16_t>(table + i * sizeof(uint16_t),
                                 xe::nui::LutDepthTableEntry(i));
  }
  return X_STATUS_SUCCESS;
}

X_STATUS NuiCamera::HandleGetTiming(uint32_t request_ptr) {
  if (!nui_state_->IsGuestRangeWritable(request_ptr,
                                        sizeof(NuiCameraGetTimingRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // The runtime multiplies each of the fourteen values by 50 and keeps them as
  // its timing state; the first and the sixth also go back to the camera with
  // code 15. Only the frame period is answered, for the reasons given with
  // kTimingFramePeriodIndex.
  auto* block =
      kernel_state_->memory()->TranslateVirtual<NuiCameraGetTimingRequest*>(
          request_ptr);
  for (size_t i = 0; i < xe::countof(block->values); ++i) {
    block->values[i] = i == kTimingFramePeriodIndex ? kDepthFramePeriodUs : 0u;
  }
  return X_STATUS_SUCCESS;
}

void NuiCamera::MarkConfigured() {
  // Code 25 is the last request of the runtime's configuration sequence
  // (27, 24, 33, 13, 14, 25), so answering it means the camera is configured
  // and every state query from here on says "streaming" rather than
  // "configure me".
  configured_.store(true, std::memory_order_release);
  // The stream this configuration opens is a new one, whether it is the
  // title's first or the one after a stop (code 2), so the host NUI system is
  // told about its first frame request too.
  stream_start_announced_.store(false, std::memory_order_release);
}

X_STATUS NuiCamera::HandleGetDepthParameters(uint32_t request_ptr) {
  if (!nui_state_->IsGuestRangeWritable(
          request_ptr, sizeof(NuiCameraDepthParametersRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto* block =
      kernel_state_->memory()
          ->TranslateVirtual<NuiCameraDepthParametersRequest*>(request_ptr);
  // The runtime does not clear the block before the call, so everything the
  // kernel has no answer for has to be zeroed here. Zero bytes need no
  // byte-swapping, which is why this is one memset rather than a field walk.
  std::memset(reinterpret_cast<uint8_t*>(block) + sizeof(uint32_t), 0,
              sizeof(*block) - sizeof(uint32_t));

  const auto& calibration = xe::nui::kLutCalibration;
  block->a = calibration.a;
  block->b = calibration.b;
  block->c = calibration.c;
  block->d = calibration.d;
  block->w = calibration.w;
  block->h = calibration.h;
  // Always said once per run: whether the far end of the depth window still
  // fits in an entry the guest can read back as a positive number.
  const uint16_t far_entry =
      xe::nui::LutEntryFromIndex(xe::nui::kLutIndexAtMaxDepth);
  XELOGI("NUI: depth table entry for {} mm is {}, ceiling {}{}",
         xe::nui::kLutDepthMaxMm, far_entry, xe::nui::kLutEntryMax,
         far_entry > xe::nui::kLutEntryMax ? " (NEGATIVE to the shader)" : "");
  if (cvars::nui_log_messages) {
    XELOGI(
        "NUI: depth calibration a={} b={} c={} d={} w={} h={}, raw {}..{} "
        "covers {}..{} mm",
        calibration.a, calibration.b, calibration.c, calibration.d,
        calibration.w, calibration.h, xe::nui::kLutIndexAtMaxDepth,
        xe::nui::kLutIndexMax,
        xe::nui::LutDepthMmFromIndex(xe::nui::kLutIndexMax),
        xe::nui::LutDepthMmFromIndex(xe::nui::kLutIndexAtMaxDepth));
  }
  return X_STATUS_SUCCESS;
}

X_STATUS NuiCamera::HandleGetExposure(uint32_t request_ptr) {
  if (!nui_state_->IsGuestRangeWritable(request_ptr,
                                        sizeof(NuiCameraGetExposureRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // The read-back of what code 35 set: the runtime's exposure getters issue it
  // until a stream's values are cached, and a getter whose read-back fails
  // hands the title an error instead of a value. The answer is the payload
  // the title's own code-35 block carries, with all four values marked valid
  // so that the getters stop asking. The last value is 1.0 where the title
  // sends 0, because a 0 there would read back as an exposure of nothing.
  auto& exposure =
      kernel_state_->memory()
          ->TranslateVirtual<NuiCameraGetExposureRequest*>(request_ptr)
          ->exposure;
  exposure.flags = kExposureFlagsAllValid;
  exposure.resolution = kExposureResolution640x480;
  for (auto& word : exposure.unknown0) {
    word = 0u;
  }
  for (size_t i = 0; i < xe::countof(exposure.limits); ++i) {
    exposure.limits[i] = kExposureLimits[i];
  }
  for (auto& value : exposure.values) {
    value = kExposureValueOne;
  }
  exposure.readback = 0u;
  return X_STATUS_SUCCESS;
}

X_STATUS NuiCamera::HandleUnpackFrame(uint32_t request_ptr) {
  // Frames are delivered unpacked, so the runtime never asks for this. If it
  // ever does, the honest answer is the size it would have unpacked to: the
  // 16-bit frame is already there.
  if (!nui_state_->IsGuestRangeReadable(request_ptr,
                                        sizeof(NuiCameraRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto* memory = kernel_state_->memory();
  const auto* request =
      memory->TranslateVirtual<const NuiCameraRequest*>(request_ptr);
  // The unpacked size goes back through a pointer that sits where every other
  // request keeps its context.
  const uint32_t out_ptr = request->context;
  if (!nui_state_->IsGuestRangeWritable(out_ptr, sizeof(uint32_t))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(out_ptr),
                               kNuiCameraDepthFrameSize);
  return X_STATUS_SUCCESS;
}

X_STATUS NuiCamera::HandleQueueBuffer(uint32_t request_ptr) {
  // The block has to reach the end of the frame's result, which the feed
  // writes past the end of the common header, and all of it is written.
  if (!nui_state_->IsGuestRangeWritable(
          request_ptr, NuiCameraRequestSize(kNuiCameraRequestQueueBuffer))) {
    XELOGW("NUI: queued frame request at {:08X} is not writable", request_ptr);
    return X_STATUS_INVALID_PARAMETER;
  }
  const auto* request =
      kernel_state_->memory()->TranslateVirtual<const NuiCameraRequest*>(
          request_ptr);
  const NuiPendingRequest pending = {request_ptr, request->image_type,
                                     request->buffer, request->length,
                                     request->completion};

  const bool is_depth = pending.image_type == kNuiCameraImageDepth;
  const bool is_colour = pending.image_type == kNuiCameraImageColour;
  const bool length_fits =
      is_depth ? pending.length == kNuiCameraDepthFrameSize
               : pending.length != 0 && pending.length % 2 == 0 &&
                     pending.length <= kNuiCameraDepthFrameSize;
  if ((!is_depth && !is_colour) || !length_fits || !pending.completion) {
    XELOGW(
        "NUI: queued frame request at {:08X} asks for image type {} of {} "
        "bytes, which is not a frame this camera produces",
        request_ptr, pending.image_type, pending.length);
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!nui_state_->IsGuestRangeWritable(pending.buffer, pending.length)) {
    XELOGW("NUI: frame buffer {:08X} of {} bytes is not writable",
           pending.buffer, pending.length);
    return X_STATUS_INVALID_PARAMETER;
  }
  if (pending.buffer % sizeof(uint16_t) != 0) {
    // A ring of 16-bit samples that does not start on a halfword is not a
    // ring; refusing it here costs one check per queued buffer and keeps the
    // feed from byte-swapping through a misaligned pointer.
    XELOGW("NUI: frame buffer {:08X} is not aligned to a 16-bit sample",
           pending.buffer);
    return X_STATUS_INVALID_PARAMETER;
  }

  if (shut_down_.load(std::memory_order_acquire) ||
      terminating_.load(std::memory_order_acquire) || !EnsureFeedThread()) {
    // A plain failure rather than X_STATUS_PENDING: the runtime waits without
    // a timeout for every request that went pending to be completed before it
    // closes the camera (0x82d264b8), and nothing will complete this one, so a
    // pending answer here is the one answer that hangs that loop.
    return X_STATUS_DEVICE_NOT_READY;
  }
  auto& queue = is_depth ? depth_requests_ : colour_requests_;
  if (!queue.Enqueue(pending)) {
    XELOGW("NUI: no room for another queued {} frame request",
           is_depth ? "depth" : "colour");
    return X_STATUS_DEVICE_NOT_READY;
  }

  // This is where the title's stream begins: the camera has been configured,
  // the feed is running, and this is the first buffer the title has offered
  // for a frame to be written into, so the next frame the host publishes is
  // the first one the title will ever see. Telling the host system here
  // rather than at the configuration (code 25) is what makes that true - the
  // configuration is answered several requests before any buffer is queued.
  // A backend that replays a recording starts it over on this, so that the
  // recording's first frames are the title's first frames; a stop (code 2)
  // followed by a reconfiguration arms it again.
  if (!stream_start_announced_.exchange(true, std::memory_order_acq_rel)) {
    auto* system = nui_system();
    if (system) {
      system->OnGuestStreamStarted();
    }
  }
  return X_STATUS_PENDING;
}

X_STATUS NuiCamera::HandleAsyncRequest(uint32_t request_ptr) {
  if (!nui_state_->IsGuestRangeReadable(request_ptr,
                                        sizeof(NuiCameraRequest))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const auto* request =
      kernel_state_->memory()->TranslateVirtual<const NuiCameraRequest*>(
          request_ptr);
  const NuiPendingRequest pending = {request_ptr, 0, 0, 0, request->completion};
  if (!pending.completion) {
    // There is nothing to call back, so the request is already finished. The
    // runtime treats any non-negative answer that is not X_STATUS_PENDING as
    // a synchronous success.
    return X_STATUS_SUCCESS;
  }
  if (shut_down_.load(std::memory_order_acquire) ||
      terminating_.load(std::memory_order_acquire) || !EnsureFeedThread() ||
      !async_requests_.Enqueue(pending)) {
    // Nothing will call the completion routine later, and saying "done" when
    // it was not would leave the runtime waiting for a call that never comes.
    return X_STATUS_DEVICE_NOT_READY;
  }
  return X_STATUS_PENDING;
}

void NuiCamera::CancelQueuedFrames(NuiPendingRequestQueue* queue) {
  std::vector<NuiPendingRequest> taken;
  queue->TakeAll(&taken);
  if (taken.empty()) {
    return;
  }
  // The runtime waits, without a timeout, for every request it has queued to
  // be completed before it closes the camera (0x82d264b8), so a request that
  // is taken back still has to reach its completion routine. That is guest
  // code, so the feed thread does it.
  if (!EnsureFeedThread()) {
    XELOGW("NUI: {} queued frame requests dropped with no feed to answer them",
           taken.size());
    return;
  }
  std::lock_guard<std::mutex> lock(cancelled_mutex_);
  if (shut_down_.load(std::memory_order_acquire)) {
    // Shutdown() empties this vector under this same lock, so a stop that
    // gets here after that clear would leave its requests waiting for a feed
    // that is already gone. A shutdown drops everything still queued without
    // completing it, because a completion routine is guest code and the guest
    // is on its way out; these are no different.
    XELOGD("NUI: {} queued frame requests dropped by the camera shutdown",
           taken.size());
    return;
  }
  cancelled_requests_.insert(cancelled_requests_.end(), taken.begin(),
                             taken.end());
}

bool NuiCamera::EnsureFeedThread() {
  std::lock_guard<std::mutex> lock(feed_thread_mutex_);
  if (feed_thread_) {
    if (!feed_thread_abandoned_ || !HasFeedThreadEnded()) {
      // A thread that has left its loop is not a feed any more, and nothing
      // may be queued for it. It is not started again either: every way out of
      // that loop, other than a stop this class ordered, means no frame will
      // ever arrive.
      return feed_running_.load(std::memory_order_acquire);
    }
    // This handle belongs to a feed that was stopped from inside itself, so it
    // could not be joined, and the thread has since really ended. Forgetting it
    // is what lets the next title get a feed at all: without this the camera
    // would answer X_STATUS_DEVICE_NOT_READY for the rest of the process.
    feed_thread_.reset();
    feed_thread_abandoned_ = false;
  }
  if (shut_down_.load(std::memory_order_acquire) ||
      terminating_.load(std::memory_order_acquire)) {
    // A camera that is closing, or a title that is on its way out, gets no new
    // feed: the thread runs the guest's completion routines and would outlive
    // the guest that owns them.
    return false;
  }
  feed_running_.store(true, std::memory_order_release);
  feed_thread_ = object_ref<XHostThread>(new XHostThread(
      kernel_state_, 128 * 1024, 0,
      [this]() {
        FeedThreadMain();
        return 0;
      },
      kernel_state_->GetSystemProcess()));
  // The feed runs the runtime's completion routines, which is guest code, so
  // the debugger has to be able to stop it.
  feed_thread_->set_can_debugger_suspend(true);
  feed_thread_->set_name("NUI Frame Feed");
  if (XFAILED(feed_thread_->Create())) {
    XELOGE("NUI: could not create the frame feed thread");
    feed_running_.store(false, std::memory_order_release);
    feed_thread_.reset();
    return false;
  }
  XELOGI("NUI: frame feed started");
  return true;
}

void NuiCamera::Shutdown() {
  shut_down_.store(true, std::memory_order_release);
  // Everything still queued is dropped without being completed: unlike a
  // stop, a shutdown means the guest is on its way out, and a completion
  // routine is guest code.
  depth_requests_.Stop();
  colour_requests_.Stop();
  async_requests_.Stop();
  {
    std::lock_guard<std::mutex> lock(cancelled_mutex_);
    cancelled_requests_.clear();
  }
  StopAndJoinFeedThread();
}

bool NuiCamera::OnTitleTerminating() {
  if (shut_down_.load(std::memory_order_acquire)) {
    // The camera is closed for good and the feed is already joined. A title
    // ending after that has nothing left to quiesce.
    return false;
  }

  // Everything that could start work is closed first, so that no guest thread
  // can put a request behind the stop below or start a second feed beside the
  // one being stopped. The queues keep what they already hold: the guest's
  // threads are still running at this point, so a frame the feed is part way
  // through delivering still has a live title to deliver it to, and the
  // request that carries it still has a completion routine worth calling.
  terminating_.store(true, std::memory_order_release);
  depth_requests_.BeginTerminating();
  colour_requests_.BeginTerminating();
  async_requests_.BeginTerminating();

  // Then the feed is stopped and waited for. This is the whole reason the
  // first half of the termination runs before the title's threads are killed:
  // the wait has no timeout, and a completion routine the feed is inside can
  // need a guest lock that a thread killed at a safe point would still own.
  // Waiting for it here means waiting for guest code that can still finish.
  const bool feed_stopped = StopAndJoinFeedThread();
  terminating_stopped_feed_.store(feed_stopped, std::memory_order_relaxed);
  return feed_stopped;
}

NuiCameraTermination NuiCamera::OnTitleTerminated() {
  NuiCameraTermination result;
  if (shut_down_.load(std::memory_order_acquire)) {
    // The camera is closed for good. A title ending after that has nothing
    // left to give back and nothing to reopen for.
    return result;
  }
  result.feed_stopped =
      terminating_stopped_feed_.exchange(false, std::memory_order_relaxed);

  // The feed is already stopped and joined, and the title's threads are gone,
  // so nothing here can be completed: a completion routine is guest code owned
  // by a thread that no longer exists, in a module that is about to be
  // unloaded. What is left is counted and forgotten.
  result.depth = depth_requests_.Stop();
  result.colour = colour_requests_.Stop();
  async_requests_.Stop();
  {
    std::lock_guard<std::mutex> lock(cancelled_mutex_);
    for (const NuiPendingRequest& request : cancelled_requests_) {
      if (request.image_type == kNuiCameraImageColour) {
        ++result.colour;
      } else {
        ++result.depth;
      }
    }
    cancelled_requests_.clear();
  }

  // What follows is everything a fresh NuiCamera holds. The next title in
  // this process has to find a camera that has never been configured: a
  // device state of 2 on its first probe makes a NUI runtime skip its
  // configuration sequence and never build its depth table, which is a title
  // that runs and tracks nobody (docs/nui.md, "Request codes").
  depth_requests_.Reopen();
  colour_requests_.Reopen();
  async_requests_.Reopen();
  stream_flags_.store(0, std::memory_order_relaxed);
  configured_.store(false, std::memory_order_release);
  stream_start_announced_.store(false, std::memory_order_release);
  last_logged_state_.store(0, std::memory_order_relaxed);
  unwritable_ring_logged_.store(false, std::memory_order_relaxed);
  depth_sequence_.store(0, std::memory_order_relaxed);
  colour_sequence_.store(0, std::memory_order_relaxed);
  depth_frames_dropped_.store(0, std::memory_order_relaxed);
  // Last of all, so that nothing above can be raced by a request from the next
  // title arriving between a queue reopening and the camera being reset.
  terminating_.store(false, std::memory_order_release);
  return result;
}

bool NuiCamera::HasFeedThreadEnded() {
  // XThread::is_running() is cleared as the thread leaves its entry point,
  // while it is still unwinding and still holds the reference to itself, so it
  // says "gone" of a thread that is not gone yet. Poll the native thread
  // handle, which is signalled only once the thread has really ended. These
  // host lifecycle waits may run on the UI thread, where XObject::Wait's
  // guest-thread wake bookkeeping is not valid.
  return xe::threading::Wait(feed_thread_->thread(), false,
                             std::chrono::milliseconds(0)) ==
         xe::threading::WaitResult::kSuccess;
}

bool NuiCamera::StopAndJoinFeedThread() {
  object_ref<XHostThread> thread;
  {
    // Under the same lock a starting feed takes, so that a thread created
    // just before this point is still stopped and waited for.
    std::lock_guard<std::mutex> lock(feed_thread_mutex_);
    feed_running_.store(false, std::memory_order_release);
    thread = feed_thread_;
  }
  if (!thread) {
    return false;
  }
  // The feed may be waiting for a frame or inside a completion routine; both
  // come back out on their own, and it must not be running when the host NUI
  // system, the kernel or the title's modules go away. Waiting for a thread
  // to leave guest code is why no lock of this class is held here.
  if (XThread::IsInThread(thread.get())) {
    // Being asked to join the feed from the feed itself can only mean a
    // completion routine ended the title. Waiting would deadlock, and
    // forgetting the handle here would let a second feed start beside this
    // one; the store above is enough to make this thread leave its loop as
    // soon as it returns. The handle is marked instead, so that
    // EnsureFeedThread can forget it once the thread has really ended and the
    // next title is not left with a camera that answers nothing.
    std::lock_guard<std::mutex> lock(feed_thread_mutex_);
    feed_thread_abandoned_ = true;
    return true;
  }
  // Join the retained native thread without XObject::Wait's guest-thread
  // wake bookkeeping: title close can reach here from the host UI thread.
  xe::threading::Wait(thread->thread(), false);
  // Clearing the handle is what lets EnsureFeedThread start a feed again: it
  // refuses to while one it has already created is still remembered, so a
  // second title would otherwise never get a frame.
  std::lock_guard<std::mutex> lock(feed_thread_mutex_);
  feed_thread_.reset();
  feed_thread_abandoned_ = false;
  return true;
}

NuiCameraStats NuiCamera::GetStats() const {
  NuiCameraStats stats;
  stats.streaming = feed_running_.load(std::memory_order_acquire);
  stats.keep_alive = nui_state_->IsCameraKeepAliveActive();
  stats.stream_flags = stream_flags_.load(std::memory_order_relaxed);
  stats.pending_depth_requests = depth_requests_.size();
  stats.pending_colour_requests = colour_requests_.size();
  stats.depth_frames_fed = depth_sequence_.load(std::memory_order_relaxed);
  stats.colour_frames_fed = colour_sequence_.load(std::memory_order_relaxed);
  stats.depth_frames_dropped =
      depth_frames_dropped_.load(std::memory_order_relaxed);
  return stats;
}

void NuiCamera::FeedThreadMain() {
  if (GetNuiTrackingObserver().enabled())
    GetNuiTrackingObserver().SessionStarted(TrackingNowUs());
  uint64_t last_sequence = 0;
  const char* stop_reason = "the camera was shut down";
  while (feed_running_.load(std::memory_order_acquire)) {
    auto* system = nui_system();
    if (!system) {
      stop_reason = "the host NUI system is gone";
      break;
    }
    const auto wait_started = std::chrono::steady_clock::now();
    const bool have_frame =
        system->WaitForFrame(last_sequence, kFrameWaitTimeout);
    if (!feed_running_.load(std::memory_order_acquire)) {
      break;
    }
    CompletePendingRequests();
    if (GetNuiTrackingObserver().enabled()) FlushTrackingRow(TrackingNowUs());
    if (!have_frame) {
      if (std::chrono::steady_clock::now() - wait_started <
          kFrameWaitTimeout / 2) {
        // A wait that comes back early with no frame means the host system is
        // releasing its waiters, and nothing will ever arrive again.
        stop_reason = "the host NUI system released its waiters";
        break;
      }
      continue;
    }
    std::shared_ptr<const xe::nui::DepthFrame> frame =
        system->AcquireLatestDepthFrame();
    if (!frame || frame->sequence <= last_sequence) {
      continue;
    }
    if (GetNuiTrackingObserver().enabled())
      GetNuiTrackingObserver().SourceFrame(frame->sequence, TrackingNowUs());
    last_sequence = frame->sequence;
    FeedDepthFrame(*frame);
    FeedColourFrame();
    if (GetNuiTrackingObserver().enabled()) FlushTrackingRow(TrackingNowUs());
  }
  // Whatever brought the loop here, the feed is over: say so where a stage
  // diagnosis will see it, and stop claiming to be streaming. Anything queued
  // from here on is refused rather than left for a thread that is gone.
  feed_running_.store(false, std::memory_order_release);
  if (GetNuiTrackingObserver().enabled()) {
    FlushTrackingRow(TrackingNowUs());
    LogTrackingStats(GetNuiTrackingObserver().Summary(), "feed-end total");
  }
  XELOGW(
      "NUI: frame feed stopped, {}, after {} depth and {} colour frames, {} "
      "dropped",
      stop_reason, depth_sequence_.load(std::memory_order_relaxed),
      colour_sequence_.load(std::memory_order_relaxed),
      depth_frames_dropped_.load(std::memory_order_relaxed));
}

void NuiCamera::FeedDepthFrame(const xe::nui::DepthFrame& frame) {
  NuiPendingRequest request = {};
  if (!depth_requests_.TakeOldest(&request)) {
    // The runtime only ever wants the newest frame, so one that arrives with
    // no buffer waiting for it is dropped rather than queued up.
    depth_frames_dropped_.fetch_add(1, std::memory_order_relaxed);
    if (GetNuiTrackingObserver().enabled())
      GetNuiTrackingObserver().DepthDelivery(
          NuiTrackingObserver::Delivery::kNoBuffer, TrackingNowUs());
    return;
  }

  const size_t pixel_count = static_cast<size_t>(frame.width) * frame.height;
  if (frame.depth_mm.size() < pixel_count ||
      pixel_count * sizeof(uint16_t) != request.length) {
    XELOGW("NUI: a {}x{} depth frame does not fit the guest's {} byte buffer",
           frame.width, frame.height, request.length);
    CompleteRequest(request, X_STATUS_UNSUCCESSFUL, 0);
    if (GetNuiTrackingObserver().enabled())
      GetNuiTrackingObserver().DepthDelivery(
          NuiTrackingObserver::Delivery::kInvalidBuffer, TrackingNowUs());
    return;
  }

  if (!IsRequestStillWritable(request)) {
    CompleteRequest(request, X_STATUS_UNSUCCESSFUL, 0);
    if (GetNuiTrackingObserver().enabled())
      GetNuiTrackingObserver().DepthDelivery(
          NuiTrackingObserver::Delivery::kInvalidBuffer, TrackingNowUs());
    return;
  }

  // The host frame keeps the orientation the Kinect for Windows runtime
  // delivers, and the title's runtime expects each row the other way round
  // (see DepthFrame). This conversion is the one place the guest gets its
  // depth from, so the rows are reversed here and nowhere else.
  staging_.resize(pixel_count);
  xe::nui::ConvertDepthFrame(frame, xe::nui::GuestDepthFormat::kLutIndex,
                             nullptr, staging_.data(),
                             /*mirror_columns=*/true);
  auto* memory = kernel_state_->memory();
  auto* slot = memory->TranslateVirtual<uint16_t*>(request.buffer);
  if constexpr (kRingIsBigEndian) {
    // copy_and_swap_16_aligned loads and stores whole SSE registers, so both
    // ends have to be 16-byte aligned. Every ring a title allocates is (the
    // slots are page-aligned), but the guest chooses the address, so a ring
    // that is not falls back to the scalar copy instead of faulting.
    const uintptr_t alignment_bits =
        reinterpret_cast<uintptr_t>(slot) |
        reinterpret_cast<uintptr_t>(staging_.data());
    if ((alignment_bits & 0xF) == 0) {
      xe::copy_and_swap_16_aligned(slot, staging_.data(), pixel_count);
    } else {
      xe::copy_and_swap_16_unaligned(slot, staging_.data(), pixel_count);
    }
  } else {
    std::memcpy(slot, staging_.data(), pixel_count * sizeof(uint16_t));
  }

  const uint32_t sequence =
      depth_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  // The runtime makes the timestamp the skeleton frame's liTimeStamp, and the
  // title's avatar poser feeds its avateering engine only a frame whose stamp
  // has changed: without one, the avatar's limbs never move (docs/nui.md,
  // "The avatar").
  const uint64_t guest_stamp_ms =
      NuiCameraFrameTimestampMillis(Clock::QueryGuestUptimeMillis());
  WriteNuiCameraFrameResult(
      memory->TranslateVirtual<uint8_t*>(request.request), sequence,
      guest_stamp_ms);
  if (GetNuiTrackingObserver().enabled())
    GetNuiTrackingObserver().DepthDelivery(
        NuiTrackingObserver::Delivery::kCopied, TrackingNowUs(),
        frame.timestamp_us, guest_stamp_ms);
  if (cvars::nui_log_messages) {
    XELOGD("NUI: depth frame {} (host {}) fed to request {:08X}", sequence,
           frame.sequence, request.request);
  }
  if (GetNuiTrackingObserver().enabled())
    GetNuiTrackingObserver().DepthDelivery(
        NuiTrackingObserver::Delivery::kCompletionCalled, TrackingNowUs());
  CompleteRequest(request, X_STATUS_SUCCESS, request.length);
  if (GetNuiTrackingObserver().enabled())
    GetNuiTrackingObserver().DepthDelivery(
        NuiTrackingObserver::Delivery::kReturned, TrackingNowUs());
}

void NuiCamera::FeedColourFrame() {
  NuiPendingRequest request = {};
  if (!colour_requests_.TakeOldest(&request)) {
    return;
  }
  if (!IsRequestStillWritable(request)) {
    CompleteRequest(request, X_STATUS_UNSUCCESSFUL, 0);
    return;
  }

  // There is no colour camera behind the host NUI system, and the tracker
  // only consumes depth; a constant black image keeps the runtime's colour
  // ring turning over instead of starving.
  auto* memory = kernel_state_->memory();
  auto* bytes = memory->TranslateVirtual<uint8_t*>(request.buffer);
  for (uint32_t i = 0; i + 1 < request.length; i += 2) {
    bytes[i] = kColourBlackLuma;
    bytes[i + 1] = kColourBlackChroma;
  }

  const uint32_t sequence =
      colour_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  // Stamped from the same clock as the depth frames, as a camera stamps both
  // of its streams.
  WriteNuiCameraFrameResult(
      memory->TranslateVirtual<uint8_t*>(request.request), sequence,
      NuiCameraFrameTimestampMillis(Clock::QueryGuestUptimeMillis()));
  CompleteRequest(request, X_STATUS_SUCCESS, request.length);
}

bool NuiCamera::IsRequestStillWritable(const NuiPendingRequest& request) {
  // Both ranges were checked when the request was queued, which is not when
  // they are used: the runtime takes its buffers back before it releases a
  // ring, but nothing in the kernel makes it, and a write into decommitted
  // guest pages faults the host rather than the guest. Two more
  // QueryRangeAccess calls per frame cost nothing at 30 Hz.
  const bool ring_writable =
      nui_state_->IsGuestRangeWritable(request.buffer, request.length);
  // The request block is a separate allocation from the ring and carries the
  // frame's result the runtime reads back, the sequence number and the
  // timestamp, so it can go away on its own and has to be checked on its own.
  const bool result_writable = nui_state_->IsGuestRangeWritable(
      request.request + kNuiCameraFrameSequenceOffset,
      NuiCameraRequestSize(kNuiCameraRequestQueueBuffer) -
          kNuiCameraFrameSequenceOffset);
  if (ring_writable && result_writable) {
    return true;
  }
  // Rate-limited to one line: a ring that has gone would otherwise report
  // itself thirty times a second for as long as the title runs.
  if (!unwritable_ring_logged_.exchange(true, std::memory_order_relaxed)) {
    XELOGW(
        "NUI: {} of queued frame request {:08X} is no longer writable; the "
        "frame is dropped and the request failed. Further occurrences are "
        "not logged.",
        ring_writable ? "the frame's result" : "the frame buffer",
        request.request);
  }
  return false;
}

void NuiCamera::CompletePendingRequests() {
  NuiPendingRequest request = {};
  while (async_requests_.TakeOldest(&request)) {
    CompleteRequest(request, X_STATUS_SUCCESS, 0);
  }

  std::vector<NuiPendingRequest> cancelled;
  {
    std::lock_guard<std::mutex> lock(cancelled_mutex_);
    cancelled.swap(cancelled_requests_);
  }
  for (const NuiPendingRequest& taken_back : cancelled) {
    CompleteRequest(taken_back, X_STATUS_CANCELLED, 0);
  }
}

void NuiCamera::CompleteRequest(const NuiPendingRequest& request,
                                X_STATUS status, uint32_t bytes) {
  uint64_t args[] = {request.request, static_cast<uint32_t>(status), bytes};
  kernel_state_->processor()->Execute(feed_thread_->thread_state(),
                                      request.completion, args,
                                      xe::countof(args));
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
