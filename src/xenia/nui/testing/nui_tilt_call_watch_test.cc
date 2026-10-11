/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_tilt_call_watch.h"

#include <chrono>
#include <cstdint>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

namespace {

using namespace std::chrono_literals;

// How long the backend gives an accelerometer read and a motor command.
constexpr int64_t kReadTimeoutMs = kTiltReadDue.count();
constexpr int64_t kSetAngleTimeoutMs = kTiltCommandDue.count();

}  // namespace

TEST_CASE("The sensor calls are given the time the ruling set", "[nui][tilt]") {
  // A read comes back in milliseconds. A motor command blocks for a second
  // or so by design, and beside the pump's frame fetches it has been seen
  // past three seconds; abandoning it costs the accelerometer for the whole
  // session, so it gets ten before it counts as stuck, and is only reported
  // as slow past two.
  CHECK(kTiltReadDue == 2000ms);
  CHECK(kTiltCommandDue == 10000ms);
  CHECK(kTiltSlowCommand == 2000ms);
  CHECK(kTiltSlowCommand < kTiltCommandDue);
}

TEST_CASE("A call that returns in time is never taken over", "[nui][tilt]") {
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kReadTimeoutMs));
  CHECK(watch.CallDueBy() == 3000);
  CHECK_FALSE(watch.TakeOverIfOverdue(2999));
  CHECK(watch.EndCall());
  CHECK(watch.CallDueBy() == TiltCallWatch::kIdle);
  // Idle is never overdue, however long it lasts.
  CHECK_FALSE(watch.TakeOverIfOverdue(100000));
  CHECK_FALSE(watch.IsTakenOver());
  CHECK(watch.BeginCall(100001 + kReadTimeoutMs));
}

TEST_CASE("A call still in flight past its due time is taken over",
          "[nui][tilt]") {
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kReadTimeoutMs));
  CHECK(watch.TakeOverIfOverdue(3000));
  CHECK(watch.IsTakenOver());
  // When the stuck call does return, the thread learns it has been given up
  // on, and may make no other call.
  CHECK_FALSE(watch.EndCall());
  CHECK_FALSE(watch.BeginCall(3500 + kReadTimeoutMs));
  CHECK(watch.IsTakenOver());
}

TEST_CASE("Each call into the sensor carries its own due time", "[nui][tilt]") {
  // A motor command blocks for about a second by design, so it is given
  // longer than a read; a watcher that only knew one timeout would give up
  // on a command that was merely doing its job.
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kSetAngleTimeoutMs));
  CHECK_FALSE(watch.TakeOverIfOverdue(1000 + kReadTimeoutMs));
  // A command still running three seconds in, past what a read is given, is
  // still in time.
  CHECK_FALSE(watch.TakeOverIfOverdue(1000 + 3000));
  CHECK_FALSE(watch.TakeOverIfOverdue(10999));
  CHECK(watch.TakeOverIfOverdue(11000));
}

TEST_CASE("A call that returns as the pump gives up keeps its thread",
          "[nui][tilt]") {
  // The pump reads the due time of a call that is overdue...
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kReadTimeoutMs));
  const int64_t observed = watch.CallDueBy();
  REQUIRE(observed == 3000);
  // ...the call returns before the pump acts on what it read...
  REQUIRE(watch.EndCall());
  // ...so the take-over fails, and the thread goes on as if nothing had
  // happened. This is the race a bare flag set after the fact lost.
  CHECK_FALSE(watch.TryTakeOver(observed));
  CHECK_FALSE(watch.IsTakenOver());
  CHECK(watch.BeginCall(3250 + kReadTimeoutMs));
}

TEST_CASE("A stale observation cannot take over the thread's next call",
          "[nui][tilt]") {
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kReadTimeoutMs));
  const int64_t observed = watch.CallDueBy();
  REQUIRE(watch.EndCall());
  // The thread has already started its next call when the pump acts on the
  // due time of the one before.
  REQUIRE(watch.BeginCall(3250 + kReadTimeoutMs));
  CHECK_FALSE(watch.TryTakeOver(observed));
  CHECK_FALSE(watch.IsTakenOver());
  CHECK(watch.EndCall());
}

TEST_CASE("Taking over a thread between calls stops its next call",
          "[nui][tilt]") {
  TiltCallWatch watch;
  // No call in flight: the take-over says so, and the thread's next call is
  // refused before it reaches the sensor.
  CHECK_FALSE(watch.TakeOver());
  CHECK(watch.IsTakenOver());
  CHECK_FALSE(watch.BeginCall(1000 + kReadTimeoutMs));
}

TEST_CASE("Taking over a thread inside a call reports the call in flight",
          "[nui][tilt]") {
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kSetAngleTimeoutMs));
  CHECK(watch.TakeOver());
  CHECK_FALSE(watch.EndCall());
  CHECK_FALSE(watch.BeginCall(1500 + kReadTimeoutMs));
}

TEST_CASE("A reset watch is ready for a new thread", "[nui][tilt]") {
  TiltCallWatch watch;
  REQUIRE(watch.BeginCall(1000 + kReadTimeoutMs));
  REQUIRE(watch.EndCall());
  watch.Reset();
  CHECK(watch.CallDueBy() == TiltCallWatch::kIdle);
  CHECK(watch.BeginCall(2000 + kReadTimeoutMs));
  CHECK(watch.EndCall());
}

}  // namespace test
}  // namespace nui
}  // namespace xe
