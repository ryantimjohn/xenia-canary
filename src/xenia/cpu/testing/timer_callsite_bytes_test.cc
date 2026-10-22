// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#include "xenia/cpu/ppc/timer_callsite_bytes.h"

#include <atomic>
#include <limits>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::timer_callsite_bytes::test {
TEST_CASE("Callsite bytes select exact undefined timer import and full LR",
          "[timer_callsite_bytes]") {
  CHECK(Selected(0xA6, "KeSetTimer", true, false, 0x82CE8A6C, 0x8286EE20));
  CHECK_FALSE(Selected(0xA7, "KeSetTimerEx", true, false, kImport, kCallerLR));
  CHECK_FALSE(Selected(0xA6, "KeSetTimerSuffix", true, false, kImport, kCallerLR));
  CHECK_FALSE(Selected(0xA6, "kesettimer", true, false, kImport, kCallerLR));
  CHECK_FALSE(Selected(0xA6, "KeSetTimer", false, false, kImport, kCallerLR));
  CHECK_FALSE(Selected(0xA6, "KeSetTimer", true, true, kImport, kCallerLR));
  CHECK_FALSE(Selected(0xA6, "KeSetTimer", true, false, kImport + 4, kCallerLR));
  CHECK_FALSE(Selected(0xA6, "KeSetTimer", true, false, kImport, kCallerLR + 4));
  CHECK_FALSE(Selected(0xA6, "KeSetTimer", true, false, kImport, 0x100000000ULL | kCallerLR));
}

TEST_CASE("Callsite host window and readable region reject overflow and protection",
          "[timer_callsite_bytes]") {
  uintptr_t host = 77;
  CHECK_FALSE(HostWindow(0, host));
  CHECK(host == 77);
  CHECK(HostWindow(0x100000000ULL, host));
  CHECK(host == 0x18286ECA0ULL);
  const auto max = std::numeric_limits<uintptr_t>::max();
  CHECK_FALSE(HostWindow(max - kGuestStart - kBytes + 1, host));
  CHECK(host == 0x18286ECA0ULL);
  CHECK(HostWindow(max - kGuestStart - kBytes, host));
  CHECK(host == max - kBytes);
  for (uint32_t protection : {0x02u, 0x04u, 0x08u, 0x20u, 0x40u, 0x80u})
    CHECK(ReadableRegion(0x1200, 0x1000, 0x400, 0x1000, protection));
  CHECK(ReadableRegion(0x1000, 0x1000, 512, 0x1000, 0x04));
  CHECK_FALSE(ReadableRegion(0x1000, 0x1000, 511, 0x1000, 0x04));
  CHECK_FALSE(ReadableRegion(0x1201, 0x1000, 0x400, 0x1000, 0x04));
  CHECK_FALSE(ReadableRegion(0x0FFF, 0x1000, 0x400, 0x1000, 0x04));
  CHECK_FALSE(ReadableRegion(max - 510, max - 510, 512, 0x1000, 0x04));
  CHECK_FALSE(ReadableRegion(0x1000, 0x1000, 512, 0x2000, 0x04));
  for (uint32_t protection : {0u, 0x01u, 0x10u, 0x104u, 0x101u})
    CHECK_FALSE(ReadableRegion(0x1000, 0x1000, 512, 0x1000, protection));
}

TEST_CASE("Callsite attempt is consumed once despite concurrent or failed work",
          "[timer_callsite_bytes]") {
  OneShot once;
  std::atomic<unsigned> ready{0}, wins{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> workers;
  REQUIRE(once.Available());
  for (unsigned n = 0; n < 8; ++n) workers.emplace_back([&] {
    ++ready;
    while (!go.load()) std::this_thread::yield();
    for (unsigned n = 0; n < 64; ++n) if (once.TryClaim()) ++wins;
  });
  while (ready.load() != 8) std::this_thread::yield();
  go = true;
  for (auto& worker : workers) worker.join();
  CHECK(wins.load() == 1);
  CHECK_FALSE(once.Available());
  CHECK_FALSE(once.TryClaim());
  OneShot failed;
  REQUIRE(failed.TryClaim());  // A rejected region/read never refunds admission.
  CHECK_FALSE(ReadableRegion(0x1000, 0x1000, 512, 0x1000, 0x104));
  CHECK_FALSE(failed.Available());
  CHECK_FALSE(failed.TryClaim());
}

TEST_CASE("Callsite opwords preserve copied byte order at both buffer ends",
          "[timer_callsite_bytes]") {
  std::array<uint8_t, kBytes> local{};
  local[0] = 0x01; local[1] = 0x23; local[2] = 0x45; local[3] = 0x67;
  local[508] = 0xFE; local[509] = 0xDC; local[510] = 0xBA; local[511] = 0x98;
  const auto before = local;
  CHECK(Word(local, 0) == 0x01234567u);
  CHECK(Word(local, 1) == 0);
  CHECK(Word(local, kWords - 1) == 0xFEDCBA98u);
  CHECK(local == before);
}
}  // namespace xe::cpu::ppc::timer_callsite_bytes::test
