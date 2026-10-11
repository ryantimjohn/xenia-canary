/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_tilt_gate.h"

#include <chrono>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

namespace {

using namespace std::chrono_literals;

// An arbitrary moment to count from; the gate only ever compares two times.
const TiltRequestGate::Clock::time_point kStart =
    TiltRequestGate::Clock::time_point() + 1000s;

}  // namespace

TEST_CASE("The first tilt request always reaches the motor", "[nui][tilt]") {
  TiltRequestGate gate;
  CHECK(gate.ShouldApply(27, kStart));
  CHECK(gate.ShouldApply(-25, kStart));
}

TEST_CASE("A repeat of the last target is dropped while the motor settles",
          "[nui][tilt]") {
  // The guest's auto-tilt driver asks again whenever the measured angle is
  // more than two degrees from its target. A head that cannot get there,
  // because its base stands at a slant, would otherwise have the motor
  // commanded, and the tilt thread blocked for about a second with the
  // accelerometer unread, every time.
  TiltRequestGate gate;
  gate.OnSetAngleReturned(27, kStart);
  CHECK(TiltRequestGate::kSettleWindow == 3000ms);
  CHECK_FALSE(gate.ShouldApply(27, kStart));
  CHECK_FALSE(gate.ShouldApply(27, kStart + 2999ms));
  CHECK(gate.ShouldApply(27, kStart + 3000ms));
  CHECK(gate.ShouldApply(27, kStart + 60s));
}

TEST_CASE("A new target reaches the motor at once", "[nui][tilt]") {
  TiltRequestGate gate;
  gate.OnSetAngleReturned(27, kStart);
  CHECK(gate.ShouldApply(0, kStart));
  CHECK(gate.ShouldApply(-25, kStart + 1ms));
}

TEST_CASE("The settle window runs from the latest return", "[nui][tilt]") {
  TiltRequestGate gate;
  gate.OnSetAngleReturned(27, kStart);
  gate.OnSetAngleReturned(8, kStart + 1s);
  // The last target is now 8, so 27 is new again...
  CHECK(gate.ShouldApply(27, kStart + 1s));
  // ...and 8 settles for three seconds from its own return.
  CHECK_FALSE(gate.ShouldApply(8, kStart + 3500ms));
  CHECK(gate.ShouldApply(8, kStart + 4s));
}

TEST_CASE("A motor that keeps refusing is reported once per settle window",
          "[nui][tilt]") {
  // The driver rate limits the motor, so a refusal is expected now and then;
  // saying so every time a title asks is what the window stops.
  TiltRequestGate gate;
  CHECK(gate.ShouldReportRefusal(kStart));
  CHECK_FALSE(gate.ShouldReportRefusal(kStart + 1s));
  CHECK_FALSE(gate.ShouldReportRefusal(kStart + 2999ms));
  CHECK(gate.ShouldReportRefusal(kStart + 3000ms));
  CHECK_FALSE(gate.ShouldReportRefusal(kStart + 4s));
}

}  // namespace test
}  // namespace nui
}  // namespace xe
