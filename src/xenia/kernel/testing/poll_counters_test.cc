// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#include "xenia/kernel/xam/poll_counters.h"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"

DECLARE_string(poll_counters);
namespace xe::kernel::xam::poll_counters::test {
namespace {
using namespace std::chrono_literals;
struct File {
  std::filesystem::path path;
  File() {
    static std::atomic<uint64_t> next{0};
    path = std::filesystem::temp_directory_path() /
        ("xenia-poll-test-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++next) + ".bin");
  }
  ~File() { std::error_code ec; std::filesystem::remove(path, ec); }
};
}
TEST_CASE("Poll counters disabled lifecycle performs no output", "[poll_counters]") {
  REQUIRE(cvars::poll_counters.empty());
  CHECK(StartFromFlag());
  auto op = Begin(Api::kNotify);
  CHECK_FALSE(bool(op));
  op.FinishNotify(Notify::kInvalidOutput, 0);
  CHECK(CloseForExit(100ms));
}
TEST_CASE("Poll snapshot aggregates ranges and distinguishes pending filter miss",
          "[poll_counters]") {
  File file;
  auto session = Session::Create(file.path, 10000000);
  REQUIRE(session);
  {
    auto op = session->Begin(Api::kNotify, 1, 2, 0x1234);
    op.FinishNotify(Notify::kFilteredPending, 17, 2, 2);
  }
  {
    auto op = session->Begin(Api::kNotify, 1, 2, 0x1234);
    op.FinishNotify(Notify::kFilteredDequeue, 20, 2, 1);
  }
  {
    auto op = session->Begin(Api::kWait, 1, 2, 0x5678);
    op.FinishWait(4, 1, true, true, true, uint64_t(-10000), true, 1, 0x102);
  }
  {
    auto op = session->Begin(Api::kWait, 1, 2, 0x5678);
    op.FinishWait(2, 0, false, false, false, 0, false, 0, 0xC000000D);
  }
  REQUIRE(session->Close(2s));
  const auto& snapshot = session->snapshot();
  const auto& slot = snapshot.slots[0];
  CHECK(slot.api[0].started == 2);
  CHECK(slot.api[0].completed == 2);
  CHECK(slot.api[0].outstanding_at_cutoff == 0);
  CHECK(slot.notify[6].before.min == 2);
  CHECK(slot.notify[6].after.max == 2);
  CHECK(slot.notify[4].after.min == 1);
  const auto& wait = slot.wait[(2 * 3 + 1) * 6 + 1];
  CHECK(wait.count == 1);
  CHECK(wait.host_ms.min == 1);
  CHECK((wait.raw_timeout_ordered.min ^ kClosed) == uint64_t(-10000));
  CHECK(wait.alertable == 1);
  CHECK(slot.wait[(4 * 3) * 6 + 4].host_ms.min == UINT64_MAX);
  CHECK(std::filesystem::file_size(file.path) == sizeof(Snapshot));
  std::ifstream input(file.path, std::ios::binary);
  auto decoded = std::make_unique<Snapshot>();
  input.read(reinterpret_cast<char*>(decoded.get()), sizeof(Snapshot));
  CHECK(input.gcount() == sizeof(Snapshot));
  CHECK(decoded->magic == snapshot.magic);
  CHECK(decoded->frozen == 1);
  CHECK(decoded->bytes < 1024 * 1024);
}
TEST_CASE("Poll close freezes while an unbounded operation remains outstanding",
          "[poll_counters]") {
  File file;
  auto session = Session::Create(file.path, 10000000);
  REQUIRE(session);
  std::mutex mutex;
  std::condition_variable condition;
  bool ready = false, release = false;
  std::thread blocked([&] {
    auto op = session->Begin(Api::kWait, 3, 4, 0x1111);
    { std::unique_lock<std::mutex> lock(mutex);
      ready = true; condition.notify_all();
      condition.wait(lock, [&] { return release; }); }
    op.FinishWait(1, 0, false, true, false, 0, true, UINT32_MAX, 0);
  });
  { std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return ready; }); }
  const bool closed = session->Close(2s);
  // Always release/join before assertions that can abort this test.
  { std::lock_guard<std::mutex> lock(mutex); release = true; }
  condition.notify_all(); blocked.join();
  REQUIRE(closed);
  const auto& stats = session->snapshot().slots[0].api[1];
  CHECK(stats.started == 1);
  CHECK(stats.completed == 0);
  CHECK(stats.outstanding_at_cutoff == 1);
  // The late completion has not raced or changed serialized aggregates.
  CHECK(session->snapshot().slots[0].wait[0].count == 0);
}
TEST_CASE("Poll concurrent admissions and close retain accounting relations",
          "[poll_counters]") {
  File file;
  auto session = Session::Create(file.path, 10000000);
  REQUIRE(session);
  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false}, stop{false};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != 8; ++i) workers.emplace_back([&, i] {
    auto first = session->Begin(Api::kNotify, 10 + i, 100 + i, 0x20);
    ++ready;
    while (!go.load()) std::this_thread::yield();
    first.FinishNotify(Notify::kAnyEmpty, 0, 0, 0);
    while (!stop.load()) {
      auto op = session->Begin(Api::kNotify, 10 + i, 100 + i, 0x20);
      op.FinishNotify(Notify::kAnyEmpty, 0, 0, 0);
    }
  });
  while (ready.load() != 8) std::this_thread::yield();
  go = true;
  // Close races entry AND completion, not a sleep-dependent quiescent test.
  bool closed = session->Close(2s);
  stop = true;
  for (auto& worker : workers) worker.join();
  REQUIRE(closed);
  uint64_t starts = 0;
  for (const auto& slot : session->snapshot().slots) {
    const auto& stats = slot.api[0];
    starts += stats.started;
    CHECK(stats.started == stats.completed + stats.abandoned +
                              stats.outstanding_at_cutoff);
    CHECK(stats.completed == slot.notify[3].count);
  }
  CHECK(starts >= 8);
}
TEST_CASE("Poll explicit limits do not overwrite slots callers or counters",
          "[poll_counters]") {
  File file;
  auto session = Session::Create(file.path, 10000000);
  REQUIRE(session);
  for (uint32_t i = 0; i != kSlots + 1; ++i) {
    auto op = session->Begin(Api::kNotify, i + 1, i + 1, 1);
    op.FinishNotify(Notify::kAnyEmpty, 0, 0, 0);
  }
  for (unsigned i = 2; i != 7; ++i) {
    auto op = session->Begin(Api::kNotify, 1, 1, i);
    op.FinishNotify(Notify(99), 0);
  }
  { auto abandoned = session->Begin(Api::kWait, 1, 1, 7); }
  REQUIRE(session->Close(2s));
  CHECK(session->snapshot().slot_overflow == 1);
  CHECK(session->snapshot().slots[0].api[0].caller_overflow == 2);
  CHECK(session->snapshot().slots[0].bucket_overflow == 5);
  CHECK(session->snapshot().slots[0].api[1].abandoned == 1);
  uint64_t value = UINT64_MAX - 1, saturated = 0;
  SaturatingAdd(value, 2, saturated);
  CHECK(value == UINT64_MAX);
  CHECK(saturated == 1);
}
TEST_CASE("Poll sidecars refuse overwrite and preserve create failures",
          "[poll_counters]") {
  File file;
  { std::ofstream existing(file.path); existing << "retain"; }
  CHECK_FALSE(Session::Create(file.path, 10000000));
  CHECK(std::filesystem::file_size(file.path) == 6);
  CHECK_FALSE(Session::Create(file.path / "missing-parent.bin", 10000000));
  CHECK_FALSE(Session::Create("relative-poll-test.bin", 10000000));
  CHECK_FALSE(Session::Create(file.path, 0));
}
TEST_CASE("Poll writer failure never acknowledges a complete snapshot",
          "[poll_counters]") {
  File file;
  auto session = Session::Create(file.path, 10000000,
      [](FILE*, const Snapshot&) { return false; });
  REQUIRE(session);
  { auto op = session->Begin(Api::kNotify, 1, 1, 1);
    op.FinishNotify(Notify::kAnyEmpty, 0); }
  CHECK_FALSE(session->Close(2s));
  CHECK(std::filesystem::file_size(file.path) < sizeof(Snapshot));
}
}  // namespace xe::kernel::xam::poll_counters::test
