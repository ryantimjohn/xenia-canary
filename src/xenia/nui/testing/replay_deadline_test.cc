/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/replay/replay_deadline.h"

#include <chrono>
#include <cstdint>
#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace replay {
namespace test {

namespace {

using namespace std::chrono_literals;

ReplayDeadline::Clock::time_point AtUs(int64_t microseconds) {
  return ReplayDeadline::Clock::time_point{} +
         std::chrono::microseconds(microseconds);
}

}  // namespace

TEST_CASE("Replay deadline retains fractional frame gaps across waits",
          "[nui][replay_deadline]") {
  ReplayDeadline pace;
  pace.Anchor(AtUs(0));

  // Three 33,333 us gaps should be due at 33,333, 66,666 and 99,999 us.
  // Rounding each gap to 33 ms would finish almost a millisecond early;
  // rounding each wait to 34 ms would drift three milliseconds late.
  CHECK(pace.WaitForNext(0, 33333, false, AtUs(0)) == 34ms);
  CHECK(pace.WaitForNext(33333, 66666, false, AtUs(34000)) == 33ms);
  CHECK(pace.WaitForNext(66666, 99999, false, AtUs(67000)) == 33ms);
}

TEST_CASE("Replay deadline subtracts reads and catches up after an overrun",
          "[nui][replay_deadline]") {
  ReplayDeadline pace;
  pace.Anchor(AtUs(0));

  CHECK(pace.WaitForNext(0, 10000, false, AtUs(7000)) == 3ms);
  // A long read or host stall makes one frame late. Publish it promptly,
  // then retain the original phase for the following frame.
  CHECK(pace.WaitForNext(10000, 20000, false, AtUs(22000)) == 0ms);
  CHECK(pace.WaitForNext(20000, 30000, false, AtUs(25000)) == 5ms);
}

TEST_CASE("Replay deadline restart begins a fresh phase",
          "[nui][replay_deadline]") {
  ReplayDeadline pace;
  pace.Anchor(AtUs(0));
  CHECK(pace.WaitForNext(0, 200000, false, AtUs(0)) == 200ms);

  // The first ready frame after rewind anchors again. Old deadlines cannot
  // make the restarted recording burst out immediately or wait too long.
  pace.Reset();
  pace.Anchor(AtUs(500000));
  CHECK(pace.WaitForNext(1000, 51000, false, AtUs(505000)) == 45ms);
}

TEST_CASE("Replay deadline handles invalid and EOF gaps including one frame",
          "[nui][replay_deadline]") {
  ReplayDeadline pace;
  pace.Anchor(AtUs(0));
  CHECK(pace.WaitForNext(0, std::numeric_limits<uint64_t>::max(), false,
                         AtUs(0)) == 200ms);
  CHECK(pace.WaitForNext(std::numeric_limits<uint64_t>::max(), 5, false,
                         AtUs(200000)) == 33ms);
  CHECK(pace.WaitForNext(5, 5, false, AtUs(233000)) == 33ms);
  CHECK(pace.WaitForNext(5, std::numeric_limits<uint64_t>::max(), true,
                         AtUs(266000)) == 33ms);

  pace.Reset();
  pace.Anchor(AtUs(0));
  CHECK(pace.WaitForNext(42, 42, true, AtUs(0)) == 33ms);
  CHECK(pace.WaitForNext(42, 42, true, AtUs(33000)) == 33ms);
}

TEST_CASE("Replay deadline retains the deliberate 200 ms cap",
          "[nui][replay_deadline]") {
  ReplayDeadline pace;
  pace.Anchor(AtUs(0));

  // These two positive source gaps total 530 ms. Their 200 ms caps discard
  // exactly 100 + 30 ms; an EOF wrap adds the existing 33 ms fallback.
  CHECK(pace.WaitForNext(0, 300000, false, AtUs(0)) == 200ms);
  CHECK(pace.WaitForNext(300000, 530000, false, AtUs(200000)) == 200ms);
  CHECK(pace.WaitForNext(530000, 0, true, AtUs(400000)) == 33ms);
  // The resulting first-frame-to-wrap target is 433 ms, not the uncapped
  // source span plus fallback (563 ms). This policy difference is intentional.
  // A recording's precise target must be calculated from raw microsecond
  // gaps. A millisecond-floor summary can include sub-ms remainders in its
  // reported cap loss, so it is not an exact deadline-mode reference.
}

}  // namespace test
}  // namespace replay
}  // namespace nui
}  // namespace xe
