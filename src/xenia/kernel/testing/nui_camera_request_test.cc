/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_camera_request.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

TEST_CASE("NuiCameraRequest fields sit where the NUI runtime writes them",
          "[nui][camera]") {
  // Offsets come from the runtime's request builders (docs/nui.md).
  CHECK(offsetof(NuiCameraRequest, code) == 0);
  CHECK(offsetof(NuiCameraRequest, arg0) == 4);
  CHECK(offsetof(NuiCameraRequest, context) == 16);
  CHECK(offsetof(NuiCameraRequest, completion) == 20);
  CHECK(offsetof(NuiCameraRequest, image_type) == 24);
  CHECK(offsetof(NuiCameraRequest, buffer) == 28);
  CHECK(offsetof(NuiCameraRequest, length) == 32);
  CHECK(sizeof(NuiCameraRequest) == 36);
  // A state query ends right after the field that receives the state.
  CHECK(kNuiCameraQueryStateRequestSize ==
        offsetof(NuiCameraRequest, arg0) + sizeof(uint32_t));
}

TEST_CASE("NuiCameraRequest parses a big-endian queue-buffer request",
          "[nui][camera]") {
  // A depth ring slot as 4D5308ED queues it: code 5, image type 4,
  // a 0x96000-byte buffer at 0xA0000000.
  const uint8_t guest_bytes[36] = {
      0x00, 0x00, 0x00, 0x05,  // code
      0x00, 0x00, 0x00, 0x00,  // arg0
      0x00, 0x00, 0x00, 0x00,  // arg1
      0x00, 0x00, 0x00, 0x00,  // arg2
      0x83, 0x89, 0x65, 0x90,  // context (the runtime's descriptor)
      0x82, 0xD2, 0x43, 0x60,  // completion routine
      0x00, 0x00, 0x00, 0x04,  // image type: depth
      0xA0, 0x00, 0x00, 0x00,  // buffer
      0x00, 0x09, 0x60, 0x00,  // length 0x96000
  };

  NuiCameraRequest request;
  std::memcpy(&request, guest_bytes, sizeof(request));

  CHECK(request.code == kNuiCameraRequestQueueBuffer);
  CHECK(request.context == 0x83896590u);
  CHECK(request.completion == 0x82D24360u);
  CHECK(request.image_type == 4u);
  CHECK(request.buffer == 0xA0000000u);
  CHECK(request.length == 0x96000u);
}

TEST_CASE("The per-code request blocks match the runtime's builders",
          "[nui][camera]") {
  // Every offset below was read off the function that builds the block; the
  // addresses are in docs/nui.md next to each request code.
  CHECK(offsetof(NuiCameraGetVersionRequest, major) == 4);
  CHECK(offsetof(NuiCameraGetVersionRequest, minor) == 5);
  CHECK(sizeof(NuiCameraGetVersionRequest) == 8);

  CHECK(offsetof(NuiCameraGetRawToDepthTableRequest, buffer) == 8);
  CHECK(offsetof(NuiCameraGetRawToDepthTableRequest, entry_count) == 12);
  CHECK(sizeof(NuiCameraGetRawToDepthTableRequest) == 16);

  CHECK(offsetof(NuiCameraGetTimingRequest, values) == 4);
  CHECK(sizeof(NuiCameraGetTimingRequest) == 60);
  // The frame period is the only one of the fourteen values the handler
  // answers, and nui_camera.cc writes it at kTimingFramePeriodIndex, which has
  // to be byte 36 of the block - the word the runtime reads it back from. The
  // two rows above pin the array's start and the block's size but not that.
  CHECK(offsetof(NuiCameraGetTimingRequest, values) +
            kTimingFramePeriodIndex * sizeof(uint32_t) ==
        36);

  CHECK(offsetof(NuiCameraSetTimingRequest, context) == 16);
  CHECK(offsetof(NuiCameraSetTimingRequest, completion) == 20);
  CHECK(sizeof(NuiCameraSetTimingRequest) == 44);

  CHECK(offsetof(NuiCameraDepthParametersRequest, b) == 4232);
  CHECK(offsetof(NuiCameraDepthParametersRequest, a) == 4240);
  CHECK(offsetof(NuiCameraDepthParametersRequest, w) == 4248);
  CHECK(offsetof(NuiCameraDepthParametersRequest, h) == 4250);
  CHECK(offsetof(NuiCameraDepthParametersRequest, c) == 4256);
  CHECK(offsetof(NuiCameraDepthParametersRequest, d) == 4264);
  CHECK(sizeof(NuiCameraDepthParametersRequest) == 4272);

  // Code 36: both senders (0x82d28508, 0x82d28844) set only the code and the
  // completion of a 144-byte block on their stacks and read the answer back
  // from the 92-byte payload code 35 sends, which starts at +28.
  CHECK(offsetof(NuiCameraGetExposureRequest, completion) == 20);
  CHECK(offsetof(NuiCameraGetExposureRequest, exposure) == 28);
  CHECK(sizeof(NuiCameraGetExposureRequest) == 144);
  CHECK(sizeof(NuiCameraExposure) == 92);
  // The words the first sender copies back: the valid bits at +28 and the
  // four values at +76..+88; and the one word the second reads, at +92.
  constexpr size_t exposure = offsetof(NuiCameraGetExposureRequest, exposure);
  CHECK(exposure + offsetof(NuiCameraExposure, flags) == 28);
  CHECK(exposure + offsetof(NuiCameraExposure, resolution) == 32);
  CHECK(exposure + offsetof(NuiCameraExposure, limits) == 60);
  CHECK(exposure + offsetof(NuiCameraExposure, values) == 76);
  CHECK(exposure + offsetof(NuiCameraExposure, readback) == 92);
}

TEST_CASE("NuiCameraRequestSize covers the blocks the kernel writes",
          "[nui][camera]") {
  // The six rows that compare against a sizeof pin the mapping from request
  // code to struct, not the sizes: they fail when a code is answered with the
  // wrong block or stops being answered at all, and the case above is what
  // pins the layouts those sizes come from.
  CHECK(NuiCameraRequestSize(kNuiCameraRequestQueryState) ==
        kNuiCameraQueryStateRequestSize);
  CHECK(NuiCameraRequestSize(kNuiCameraRequestGetVersion) ==
        sizeof(NuiCameraGetVersionRequest));
  CHECK(NuiCameraRequestSize(kNuiCameraRequestGetRawToDepthTable) ==
        sizeof(NuiCameraGetRawToDepthTableRequest));
  CHECK(NuiCameraRequestSize(kNuiCameraRequestGetTiming) ==
        sizeof(NuiCameraGetTimingRequest));
  CHECK(NuiCameraRequestSize(kNuiCameraRequestSetTiming) ==
        sizeof(NuiCameraSetTimingRequest));
  CHECK(NuiCameraRequestSize(kNuiCameraRequestGetDepthParameters) ==
        sizeof(NuiCameraDepthParametersRequest));
  CHECK(NuiCameraRequestSize(kNuiCameraRequestGetExposure) ==
        sizeof(NuiCameraGetExposureRequest));
  // A queued frame has to reach the end of the result the kernel writes past
  // the end of the common header, which is the timestamp.
  CHECK(NuiCameraRequestSize(kNuiCameraRequestQueueBuffer) ==
        kNuiCameraFrameTimestampOffset + sizeof(uint64_t));
  // Anything the kernel only reads is sized by the common header.
  CHECK(NuiCameraRequestSize(kNuiCameraRequestConfigure) ==
        sizeof(NuiCameraRequest));
}

TEST_CASE("A queued frame's result sits where the NUI runtime reads it",
          "[nui][camera]") {
  // The runtime's frame processing reads the sequence number from +64 as the
  // skeleton frame's dwFrameNumber and the timestamp from +72 as its
  // liTimeStamp (0x82d2324c-0x82d2325c, docs/nui.md, request code 5).
  CHECK(kNuiCameraFrameSequenceOffset == 64);
  CHECK(kNuiCameraFrameTimestampOffset == 72);
  // Both have to be inside the bytes the kernel checks are writable, when the
  // request is queued and again when a frame is delivered into it.
  CHECK(NuiCameraRequestSize(kNuiCameraRequestQueueBuffer) == 80);
}

TEST_CASE("A delivered frame's result is written big-endian and alone",
          "[nui][camera]") {
  // A block with a byte pattern everywhere, so that a stray write anywhere
  // shows, and 16 bytes past the result for a write that runs over.
  uint8_t block[96];
  for (size_t i = 0; i < sizeof(block); ++i) {
    block[i] = static_cast<uint8_t>(0xA0 + i);
  }
  uint8_t expected[sizeof(block)];
  std::memcpy(expected, block, sizeof(block));
  const uint8_t sequence_bytes[4] = {0x01, 0x02, 0x03, 0x04};
  const uint8_t timestamp_bytes[8] = {0x11, 0x22, 0x33, 0x44,
                                      0x55, 0x66, 0x77, 0x88};
  std::memcpy(expected + 64, sequence_bytes, sizeof(sequence_bytes));
  std::memcpy(expected + 72, timestamp_bytes, sizeof(timestamp_bytes));

  WriteNuiCameraFrameResult(block, 0x01020304u, 0x1122334455667788ull);

  // The sequence number at +64 and the timestamp at +72, most significant
  // byte first; +0..+63, the word at +68 the runtime owns, and everything
  // past +80 are as they were.
  for (size_t i = 0; i < sizeof(block); ++i) {
    CAPTURE(i);
    CHECK(static_cast<int>(block[i]) == static_cast<int>(expected[i]));
  }
}

TEST_CASE("A frame is never stamped 0", "[nui][camera]") {
  // A clock that reads 0 still gives a stamp the title counts as a new frame.
  CHECK(NuiCameraFrameTimestampMillis(0) == 1);
  // Anything else is the clock's own reading.
  CHECK(NuiCameraFrameTimestampMillis(1) == 1);
  CHECK(NuiCameraFrameTimestampMillis(33) == 33);
  CHECK(NuiCameraFrameTimestampMillis(0xFFFFFFFFull) == 0xFFFFFFFFull);

  // A clock that does not go backwards gives stamps that do not either.
  uint64_t previous = 0;
  for (uint64_t uptime_ms : {0ull, 0ull, 1ull, 2ull, 2ull, 35ull, 68ull}) {
    CAPTURE(uptime_ms);
    const uint64_t stamp = NuiCameraFrameTimestampMillis(uptime_ms);
    CHECK(stamp != 0);
    CHECK(stamp >= previous);
    previous = stamp;
  }
}

TEST_CASE("NuiPendingRequestQueue hands frames back oldest first",
          "[nui][camera]") {
  NuiPendingRequestQueue queue;
  NuiPendingRequest taken = {};

  CHECK(queue.size() == 0);
  CHECK_FALSE(queue.TakeOldest(&taken));

  CHECK(queue.Enqueue({0x1000, 4, 0xA0000000, 0x96000, 0x82D24360}));
  CHECK(queue.Enqueue({0x1080, 4, 0xA0096000, 0x96000, 0x82D24360}));
  CHECK(queue.size() == 2);

  REQUIRE(queue.TakeOldest(&taken));
  CHECK(taken.request == 0x1000u);
  CHECK(taken.image_type == 4u);
  CHECK(taken.buffer == 0xA0000000u);
  CHECK(taken.length == 0x96000u);
  CHECK(taken.completion == 0x82D24360u);

  REQUIRE(queue.TakeOldest(&taken));
  CHECK(taken.request == 0x1080u);
  CHECK(queue.size() == 0);
  CHECK_FALSE(queue.TakeOldest(&taken));
}

TEST_CASE("NuiPendingRequestQueue can hand over everything it holds",
          "[nui][camera]") {
  NuiPendingRequestQueue queue;
  std::vector<NuiPendingRequest> taken;

  // Nothing queued, nothing appended.
  queue.TakeAll(&taken);
  CHECK(taken.empty());

  REQUIRE(queue.Enqueue({0x1000, 4, 0xA0000000, 0x96000, 0x82D24360}));
  REQUIRE(queue.Enqueue({0x1080, 4, 0xA0096000, 0x96000, 0x82D24360}));
  queue.TakeAll(&taken);
  REQUIRE(taken.size() == 2);
  CHECK(taken[0].request == 0x1000u);
  CHECK(taken[1].request == 0x1080u);
  CHECK(taken[1].buffer == 0xA0096000u);
  CHECK(queue.size() == 0);

  // The requests are appended, so both rings can be emptied into one list,
  // and the queue is usable again afterwards.
  REQUIRE(queue.Enqueue({0x1100, 2, 0xA0200000, 0x96000, 0x82D24708}));
  queue.TakeAll(&taken);
  REQUIRE(taken.size() == 3);
  CHECK(taken[2].request == 0x1100u);
  CHECK(taken[2].image_type == 2u);
  CHECK(queue.size() == 0);
}

TEST_CASE("NuiPendingRequestQueue drains and closes when it stops",
          "[nui][camera]") {
  NuiPendingRequestQueue queue;
  REQUIRE(queue.Enqueue({0x1000, 4, 0xA0000000, 0x96000, 0x82D24360}));
  REQUIRE(queue.size() == 1);

  queue.Stop();

  // Nothing may reach the guest after a stop, so the queue both forgets what
  // it held and refuses anything new.
  CHECK(queue.stopped());
  CHECK(queue.size() == 0);
  NuiPendingRequest taken = {};
  CHECK_FALSE(queue.TakeOldest(&taken));
  CHECK_FALSE(queue.Enqueue({0x1080, 4, 0xA0096000, 0x96000, 0x82D24360}));
  CHECK(queue.size() == 0);
}

TEST_CASE("NuiPendingRequestQueue counts what it dropped and reopens",
          "[nui][camera]") {
  NuiPendingRequestQueue queue;

  // A queue nothing was ever put in drops nothing.
  CHECK(queue.Stop() == 0);
  CHECK(queue.stopped());

  queue.Reopen();
  CHECK_FALSE(queue.stopped());
  REQUIRE(queue.Enqueue({0x1000, 4, 0xA0000000, 0x96000, 0x82D24360}));
  REQUIRE(queue.Enqueue({0x1080, 4, 0xA0096000, 0x96000, 0x82D24360}));
  REQUIRE(queue.Enqueue({0x1100, 4, 0xA012C000, 0x96000, 0x82D24360}));

  // A title that is terminating leaves its queued frames behind. They are
  // counted and forgotten, never handed to anybody to complete: the
  // completion routine belongs to the module being unloaded, so calling it
  // would run code that is no longer there.
  CHECK(queue.Stop() == 3);
  CHECK(queue.size() == 0);
  NuiPendingRequest taken = {};
  CHECK_FALSE(queue.TakeOldest(&taken));
  CHECK_FALSE(queue.Enqueue({0x1180, 4, 0xA01C2000, 0x96000, 0x82D24360}));

  // Reopening is what lets the next title queue frames of its own, which a
  // queue that stayed stopped for the life of the process could not.
  queue.Reopen();
  CHECK_FALSE(queue.stopped());
  CHECK(queue.Enqueue({0x1180, 4, 0xA01C2000, 0x96000, 0x82D24360}));
  CHECK(queue.size() == 1);
}

TEST_CASE(
    "NuiPendingRequestQueue turns new requests away while a title is "
    "terminating",
    "[nui][camera]") {
  NuiPendingRequestQueue queue;
  REQUIRE(queue.Enqueue({0x1000, 4, 0xA0000000, 0x96000, 0x82D24360}));
  REQUIRE(queue.Enqueue({0x1080, 4, 0xA0096000, 0x96000, 0x82D24360}));

  // The first half of a title's termination runs while its threads are still
  // alive, so the queue must stop taking new work without throwing away the
  // work it already has: a frame the feed is about to deliver still has a live
  // guest to deliver it to.
  queue.BeginTerminating();
  CHECK(queue.terminating());
  CHECK_FALSE(queue.stopped());
  CHECK_FALSE(queue.Enqueue({0x1100, 4, 0xA012C000, 0x96000, 0x82D24360}));
  CHECK(queue.size() == 2);

  // What was queued before is still handed out, oldest first.
  NuiPendingRequest taken = {};
  REQUIRE(queue.TakeOldest(&taken));
  CHECK(taken.request == 0x1000);

  // The second half runs once the guest's threads are gone; what is left can
  // never be completed and is counted and forgotten.
  CHECK(queue.Stop() == 1);
  CHECK(queue.size() == 0);

  // Reopening clears both states, so the next title in this process queues
  // frames of its own.
  queue.Reopen();
  CHECK_FALSE(queue.terminating());
  CHECK_FALSE(queue.stopped());
  CHECK(queue.Enqueue({0x1180, 4, 0xA01C2000, 0x96000, 0x82D24360}));
  CHECK(queue.size() == 1);
}

TEST_CASE("NuiPendingRequestQueue refuses more than a ring's worth",
          "[nui][camera]") {
  NuiPendingRequestQueue queue;
  for (uint32_t i = 0; i < NuiPendingRequestQueue::kMaximumRequests; ++i) {
    CAPTURE(i);
    REQUIRE(
        queue.Enqueue({0x1000 + i * 0x80, 4, 0xA0000000, 0x96000, 0x82D24360}));
  }
  CHECK(queue.size() == NuiPendingRequestQueue::kMaximumRequests);
  CHECK_FALSE(queue.Enqueue({0x2000, 4, 0xA0000000, 0x96000, 0x82D24360}));
  CHECK(queue.size() == NuiPendingRequestQueue::kMaximumRequests);
}

TEST_CASE("Every known camera request code has a name", "[nui][camera]") {
  const uint32_t known_codes[] = {0,  2,  5,  11, 13, 14, 15,
                                  24, 25, 27, 30, 33, 35, 36};
  for (uint32_t code : known_codes) {
    CHECK(std::string(NuiCameraRequestCodeName(code)) != "unknown");
  }
  CHECK(std::string(NuiCameraRequestCodeName(36)) == "get exposure");
  CHECK(std::string(NuiCameraRequestCodeName(1)) == "unknown");
  CHECK(std::string(NuiCameraRequestCodeName(0xFFFFFFFFu)) == "unknown");
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
