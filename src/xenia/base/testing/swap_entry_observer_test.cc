// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private SDK-derived fork ancestry; never upstream.
#include "xenia/base/swap_entry_observer.h"
#include <atomic>
#include <fstream>
#include <future>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
namespace xe::swap_entry::test {
namespace {
using namespace std::chrono_literals;
struct Temp {
  std::filesystem::path path;
  Temp() {
    static std::atomic<unsigned> next{0};
    path = std::filesystem::temp_directory_path() /
        ("xenia-swap-entry-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++next) + ".bin");
  }
  ~Temp() { std::error_code ec; std::filesystem::remove(path, ec); }
};
void Patch(const std::filesystem::path& path, uint64_t offset, uint8_t value) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(std::streamoff(offset)); file.put(char(value)); file.flush();
}
}  // namespace
TEST_CASE("Swap entry disabled lookup does not start a session", "[swap-entry]") {
  REQUIRE(Current() == nullptr);
  REQUIRE(StartFromFlag());  // Default empty flag; no fixture path or writer.
  REQUIRE(Current() == nullptr);
  REQUIRE(CloseForExit(1ms));
}
#if XE_PLATFORM_WIN32
TEST_CASE("Swap entry real writer preserves scalars and close proof", "[swap-entry]") {
  Temp file;
  auto session = Session::Create(file.path, 10000000, RawQpc());
  REQUIRE(session);
  REQUIRE_FALSE(Session::Create(file.path, 10000000, RawQpc()));
  session->Observe(26, 123, 0xFFFFFFFF82869F18ull, true);
  session->Observe(0, 123, 0xFFFFFFFFFFFFFFFFull, false);
  REQUIRE(session->Close(2s));
  Summary wire{};
  REQUIRE(ValidateFile(file.path, &wire));
  REQUIRE(wire.counts.attempted == 2);
  REQUIRE(wire.counts.accepted == 2);
  REQUIRE(wire.counts.written == 2);
  REQUIRE(MatchesAcknowledgement(wire, session->summary(), true));
  REQUIRE_FALSE(MatchesAcknowledgement(wire, session->summary(), false));
  std::ifstream raw(file.path, std::ios::binary);
  raw.seekg(kHeaderBytes);
  std::array<uint8_t, kRecordBytes> first{};
  raw.read(reinterpret_cast<char*>(first.data()), first.size());
  REQUIRE(first[0] == 1);
  REQUIRE(first[16] == 0x18);
  REQUIRE(first[19] == 0x82);
  REQUIRE(first[23] == 0xFF);
  REQUIRE(first[24] == 26);
  REQUIRE(first[28] == 123);
  REQUIRE(first[32] == 1);
  raw.close();
  Patch(file.path, kHeaderBytes, 2);  // Sequence cannot masquerade as complete.
  REQUIRE_FALSE(ValidateFile(file.path));
  Patch(file.path, kHeaderBytes, 1);
  Patch(file.path, kHeaderBytes + 2 * kRecordBytes + 8, 3);  // Bad attempted.
  REQUIRE_FALSE(ValidateFile(file.path));
}
TEST_CASE("Swap entry full queue records loss without producer blocking", "[swap-entry]") {
  Temp file;
  std::promise<void> release;
  auto gate = release.get_future().share();
  auto session = Session::Create(file.path, 10000000, RawQpc(), [gate] { gate.wait(); });
  if (!session) release.set_value();
  REQUIRE(session);
  for (uint32_t i = 0; i < kQueueLimit + 7; ++i) session->Observe(1, 2, 0, true);
  release.set_value();  // Release before any failing assertion / destructor join.
  REQUIRE(session->Close(2s));
  Summary s{};
  REQUIRE(ValidateFile(file.path, &s));
  REQUIRE(s.counts.attempted == kQueueLimit + 7);
  REQUIRE(s.counts.accepted == kQueueLimit);
  REQUIRE(s.counts.queue_drops == 7);
  REQUIRE(s.counts.contention_drops == 0);
}
TEST_CASE("Swap entry concurrent producers retain exact admitted accounting", "[swap-entry]") {
  Temp file;
  auto session = Session::Create(file.path, 10000000, RawQpc());
  REQUIRE(session);
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  for (uint32_t t = 1; t <= 4; ++t) {
    workers.emplace_back([&, t] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned i = 0; i < 500; ++i) session->Observe(t, t + 100, t, true);
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();
  REQUIRE(session->Close(2s));
  Summary s{};
  REQUIRE(ValidateFile(file.path, &s));
  REQUIRE(s.counts.attempted == 2000);
  REQUIRE(s.counts.accepted > 0);
  REQUIRE(s.counts.attempted == s.counts.accepted + s.counts.queue_drops +
      s.counts.limit_drops + s.counts.contention_drops);
  REQUIRE(s.counts.written == s.counts.accepted);
}
TEST_CASE("Swap entry cutoff excludes late calls without changing frozen proof", "[swap-entry]") {
  Temp file;
  auto session = Session::Create(file.path, 10000000, RawQpc());
  REQUIRE(session);
  session->Observe(1, 2, 3, true);
  REQUIRE(session->Close(2s));
  auto ack = session->summary();
  session->Observe(9, 10, 11, true);
  REQUIRE(session->Close(2s));
  Summary wire{};
  REQUIRE(ValidateFile(file.path, &wire));
  REQUIRE(wire.counts.attempted == 1);
  REQUIRE(wire.counts.excluded == 0);  // Sample frozen before the late call.
  REQUIRE(MatchesAcknowledgement(wire, ack, true));
  REQUIRE(MatchesAcknowledgement(wire, session->summary(), true));
}
TEST_CASE("Swap entry writer timeout never acknowledges successful close", "[swap-entry]") {
  Temp file;
  std::promise<void> entered, release;
  auto entered_future = entered.get_future();
  auto gate = release.get_future().share();
  auto session = Session::Create(file.path, 10000000, RawQpc(), {}, [&] {
    entered.set_value(); gate.wait();
  });
  if (!session) release.set_value();
  REQUIRE(session);
  session->Observe(1, 2, 3, true);
  auto close = std::async(std::launch::async, [&] { return session->Close(20ms); });
  bool reached = entered_future.wait_for(1s) == std::future_status::ready;
  auto closed = close.wait_for(1s);
  release.set_value();  // Always release before any assertion or join.
  bool first_result = close.get();
  REQUIRE(reached);
  REQUIRE(closed == std::future_status::ready);
  REQUIRE_FALSE(first_result);
  REQUIRE(session->timed_out());
  REQUIRE_FALSE(session->Close(2s));  // Sticky failed attempt, no retry success.
  Summary wire{};
  REQUIRE(ValidateFile(file.path, &wire));
  REQUIRE_FALSE(MatchesAcknowledgement(wire, session->summary(), first_result));
}
#endif
}  // namespace xe::swap_entry::test
