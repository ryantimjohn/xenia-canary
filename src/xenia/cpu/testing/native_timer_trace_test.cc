// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#include "xenia/cpu/ppc/native_timer_trace.h"

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::native_timer_trace::test {

TEST_CASE("Native timer trace selects only exact undefined function candidates",
          "[native_timer_trace]") {
  CHECK(Select(0x54, "KeCancelTimer", true, false) == 0);
  CHECK(Select(0xA6, "KeSetTimer", true, false) == 1);
  CHECK(Select(0xA7, "KeSetTimerEx", true, false) == 2);
  CHECK(Select(0xA6, "KeSetTimer", false, false) == kNotSelected);
  CHECK(Select(0xA6, "KeSetTimer", true, true) == kNotSelected);
  CHECK(Select(0xA6, "KeSetTimerEx", true, false) == kNotSelected);
  CHECK(Select(0xA7, "KeSetTimer", true, false) == kNotSelected);
  CHECK(Select(0xA6, "KeSetTimerSuffix", true, false) == kNotSelected);
  CHECK(Select(0xA6, "kesettimer", true, false) == kNotSelected);
  CHECK(Select(0xA6, "", true, false) == kNotSelected);
  CHECK(Select(0x75, "KeInitializeTimerEx", true, false) == kNotSelected);
}

TEST_CASE("Native timer raw capture preserves full registers without interpretation",
          "[native_timer_trace]") {
  uint64_t gpr[32];
  for (uint64_t i = 0; i != 32; ++i) gpr[i] = (i << 56) | (i << 32) | (31 - i);
  uint64_t before[32];
  std::memcpy(before, gpr, sizeof(gpr));
  auto capture = Capture(gpr, 0xABCDEF0182869F18ULL);
  for (uint32_t n = 0; n != 6; ++n) CHECK(capture.r3_to_r8[n] == gpr[n + 3]);
  CHECK(capture.lr == 0xABCDEF0182869F18ULL);
  // High and low halves are retained, not presumed two distinct ABI arguments.
  CHECK(uint32_t(capture.r3_to_r8[1] >> 32) == 0x04000004);
  CHECK(uint32_t(capture.r3_to_r8[1]) == 27);
  capture.r3_to_r8[0] = 0;
  CHECK(std::memcmp(gpr, before, sizeof(gpr)) == 0);
}

TEST_CASE("Native timer per-export caps remain independent under concurrency",
          "[native_timer_trace]") {
  Budget budget;
  std::array<std::array<std::atomic<uint32_t>, 17>, 3> seen{};
  std::atomic<uint32_t> ready{0}, invalid{0};
  std::atomic<bool> go{false};
  // Exhausting Cancel must not consume either Set budget.
  for (uint32_t n = 0; n != 16; ++n) {
    const auto sequence = budget.TryClaim(0);
    REQUIRE(sequence == n + 1);
    ++seen[0][sequence];
  }
  REQUIRE_FALSE(budget.HasCapacity(0));
  REQUIRE(budget.HasCapacity(1));
  REQUIRE(budget.HasCapacity(2));
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != 8; ++i) workers.emplace_back([&] {
    ++ready;
    while (!go.load()) std::this_thread::yield();
    for (unsigned n = 0; n != 64; ++n) {
      for (uint32_t index = 0; index != 3; ++index) {
        const auto sequence = budget.TryClaim(index);
        if (!sequence) continue;
        if (sequence > 16) ++invalid;
        else ++seen[index][sequence];
      }
    }
  });
  while (ready.load() != 8) std::this_thread::yield();
  go = true;
  for (auto& worker : workers) worker.join();
  CHECK(invalid.load() == 0);
  for (uint32_t index = 0; index != 3; ++index) {
    for (uint32_t sequence = 1; sequence <= 16; ++sequence)
      CHECK(seen[index][sequence].load() == 1);
    CHECK_FALSE(budget.HasCapacity(index));
    CHECK(budget.TryClaim(index) == 0);
  }
  CHECK_FALSE(budget.HasCapacity(kNotSelected));
  CHECK(budget.TryClaim(kNotSelected) == 0);
}
}  // namespace xe::cpu::ppc::native_timer_trace::test
