/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_tilt_request_slot.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

TEST_CASE("An empty tilt request slot has nothing to apply", "[nui][tilt]") {
  TiltRequestSlot slot;
  int32_t degrees = 99;
  CHECK_FALSE(slot.IsMoving());
  CHECK_FALSE(slot.Take(&degrees));
  CHECK(degrees == 99);
  // Looking for work must not make the motor look busy, even for a moment.
  CHECK_FALSE(slot.IsMoving());
}

TEST_CASE("A tilt request counts as moving from post to finish",
          "[nui][tilt]") {
  // The guest's driver reads "moving" out of the block the whole time a
  // request is on its way: waiting to be taken, then being applied. There is
  // no step at which it could read "stopped short".
  TiltRequestSlot slot;
  slot.Post(-25);
  CHECK(slot.IsMoving());
  int32_t degrees = 0;
  REQUIRE(slot.Take(&degrees));
  CHECK(degrees == -25);
  CHECK(slot.IsMoving());
  slot.Finish();
  CHECK_FALSE(slot.IsMoving());
  CHECK_FALSE(slot.Take(&degrees));
}

TEST_CASE("Only the latest tilt request is applied", "[nui][tilt]") {
  TiltRequestSlot slot;
  slot.Post(-25);
  slot.Post(0);
  slot.Post(27);
  int32_t degrees = 0;
  REQUIRE(slot.Take(&degrees));
  CHECK(degrees == 27);
  slot.Finish();
  CHECK_FALSE(slot.Take(&degrees));
}

TEST_CASE("A tilt request posted while another is applied waits its turn",
          "[nui][tilt]") {
  TiltRequestSlot slot;
  slot.Post(-25);
  int32_t degrees = 0;
  REQUIRE(slot.Take(&degrees));
  slot.Post(13);
  slot.Finish();
  // Still moving: the second request has not been applied yet.
  CHECK(slot.IsMoving());
  REQUIRE(slot.Take(&degrees));
  CHECK(degrees == 13);
  slot.Finish();
  CHECK_FALSE(slot.IsMoving());
}

TEST_CASE("A cleared tilt request slot drops what it held", "[nui][tilt]") {
  // What the backend does once the thread that applies requests is gone:
  // nothing waiting is applied, and nothing is reported as moving.
  TiltRequestSlot slot;
  slot.Post(-25);
  int32_t degrees = 0;
  REQUIRE(slot.Take(&degrees));
  slot.Post(4);
  slot.Clear();
  CHECK_FALSE(slot.IsMoving());
  CHECK_FALSE(slot.Take(&degrees));
}

TEST_CASE("A tilt request posted while another is being taken is never lost",
          "[nui][tilt]") {
  // Posts race takes from another thread. Whatever interleaving the two
  // threads fall into, the last angle posted has to be applied in the end:
  // a take that clears "waiting" after a newer post has set it would drop
  // that post for good. The race is nanoseconds wide, so this guards the
  // property rather than reliably catching its loss.
  constexpr int32_t kLast = 100000;
  TiltRequestSlot slot;
  std::atomic<bool> done = {false};
  std::thread poster([&slot, &done]() {
    for (int32_t degrees = 1; degrees <= kLast; ++degrees) {
      slot.Post(degrees);
    }
    done.store(true, std::memory_order_release);
  });
  // Nothing below may throw before the join, so the checks wait until then.
  int32_t last_taken = 0;
  bool in_order = true;
  while (true) {
    int32_t degrees = 0;
    if (slot.Take(&degrees)) {
      in_order = in_order && degrees >= last_taken;
      last_taken = degrees;
      slot.Finish();
      continue;
    }
    if (done.load(std::memory_order_acquire) && !slot.IsMoving()) {
      break;
    }
  }
  poster.join();
  CHECK(in_order);
  CHECK(last_taken == kLast);
}

}  // namespace test
}  // namespace nui
}  // namespace xe
