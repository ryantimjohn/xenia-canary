/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_keepalive.h"

#include <cstdint>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

TEST_CASE("A fresh keep-alive is inactive", "[nui][keepalive]") {
  NuiKeepAlive keep_alive;
  CHECK_FALSE(keep_alive.IsActive(0));
  CHECK(keep_alive.deadline_us() == 0);
}

TEST_CASE("A keep-alive request expires after its timeout",
          "[nui][keepalive]") {
  NuiKeepAlive keep_alive;
  keep_alive.Request(30000, 0);

  CHECK(keep_alive.deadline_us() == 30000000);
  CHECK(keep_alive.IsActive(0));
  CHECK(keep_alive.IsActive(29999999));
  CHECK_FALSE(keep_alive.IsActive(30000001));
}

TEST_CASE("A zero timeout releases the keep-alive", "[nui][keepalive]") {
  NuiKeepAlive keep_alive;
  keep_alive.Request(30000, 0);
  REQUIRE(keep_alive.IsActive(1000));

  keep_alive.Request(0, 1000);

  CHECK_FALSE(keep_alive.IsActive(1000));
  CHECK(keep_alive.deadline_us() == 0);
}

TEST_CASE("A later request extends the deadline", "[nui][keepalive]") {
  NuiKeepAlive keep_alive;
  keep_alive.Request(30000, 0);
  REQUIRE(keep_alive.deadline_us() == 30000000);

  keep_alive.Request(30000, 10000000);

  CHECK(keep_alive.deadline_us() == 40000000);
  CHECK(keep_alive.IsActive(30000001));
  CHECK_FALSE(keep_alive.IsActive(40000001));
}

TEST_CASE("Release cancels an outstanding request", "[nui][keepalive]") {
  NuiKeepAlive keep_alive;
  keep_alive.Request(30000, 0);
  REQUIRE(keep_alive.IsActive(0));

  keep_alive.Release();

  CHECK_FALSE(keep_alive.IsActive(0));
  CHECK(keep_alive.deadline_us() == 0);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
