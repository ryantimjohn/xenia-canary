// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private SDK-derived fork ancestry; never upstream.
#include "xenia/base/pacing_observer.h"
#include <atomic>
#include <fstream>
#include <future>
#include <limits>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include <windows.h>
#endif
namespace xe::pacing::test {
namespace {
using namespace std::chrono_literals;
struct Temp {
  std::filesystem::path path;
  Temp() {
    static std::atomic<unsigned> next{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-pacing-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++next) + ".bin");
  }
  ~Temp() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};
Record Example(uint64_t seq, uint64_t q) {
  Record r{};
  r.sequence = seq;
  r.mark = q;
  r.callback_begin = q + 1;
  r.callback_end = q + 2;
  r.wait_begin = q + 3;
  r.wait_end = q + 4;
  r.requested_ns = 15000000;
  r.nt_interval = -150000;
  r.native_tid = 42;
  r.flags = 1;
  return r;
}
void Patch(const std::filesystem::path& p, uint64_t offset, uint8_t value) {
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  f.seekp(std::streamoff(offset));
  f.put(char(value));
  f.flush();
}
}  // namespace
TEST_CASE("Pacing preserves signed NT interval conversion", "[pacing]") {
  REQUIRE(NtInterval(0) == 0);
  REQUIRE(NtInterval(1) == -1);
  REQUIRE(NtInterval(99) == -1);
  REQUIRE(NtInterval(100) == -1);
  REQUIRE(NtInterval(199) == -1);
  REQUIRE(NtInterval(-1) == -1);  // Existing, deliberately unusual behavior.
  REQUIRE(NtInterval(-100) == 1);
  REQUIRE(NtInterval(std::numeric_limits<int64_t>::min()) ==
          92233720368547758LL);
  REQUIRE(NtInterval(std::numeric_limits<int64_t>::max()) ==
          -92233720368547758LL);
}
TEST_CASE("Pacing rejects malformed wire and missing close proof", "[pacing]") {
  Temp file;
  TimerMetadata timer{};
  timer.flags = 15;
  timer.minimum = 156250;
  timer.maximum = 5000;
  timer.query_current = 156250;
  timer.set_current = 5000;
  timer.requested = 5000;
  Counts c{};
  c.started = c.completed = c.accepted = c.written = 1;
  auto h = EncodeHeader(10000000, 10, timer);
  auto r = EncodeRecord(Example(1, 20));
  auto e = EncodeFooter(c, 30);
  {
    std::ofstream f(file.path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(h.data()), h.size());
    f.write(reinterpret_cast<const char*>(r.data()), r.size());
    f.write(reinterpret_cast<const char*>(e.data()), e.size());
  }
  Summary s;
  REQUIRE(ValidateFile(file.path, &s));
  REQUIRE(MatchesAcknowledgement(s, s, true));
  REQUIRE_FALSE(MatchesAcknowledgement(s, s, false));
  auto wrong = s;
  ++wrong.counts.outstanding;
  REQUIRE_FALSE(MatchesAcknowledgement(s, wrong, true));
  Patch(file.path, 80, 1);  // Reserved header byte.
  REQUIRE_FALSE(ValidateFile(file.path));
  Patch(file.path, 80, 0);
  Patch(file.path, kHeaderBytes + 72, 0);  // NT call-valid flag.
  REQUIRE_FALSE(ValidateFile(file.path));
  Patch(file.path, kHeaderBytes + 72, 1);
  Patch(file.path, kHeaderBytes + 56, 0);  // Exact rounded NT argument.
  REQUIRE_FALSE(ValidateFile(file.path));
}
#if XE_PLATFORM_WIN32
TEST_CASE("Pacing publishes a complete tick and binds frozen acknowledgement",
          "[pacing]") {
  Temp file;
  auto s = Session::Create(file.path, 10000000, RawQpc());
  REQUIRE(s != nullptr);
  const uint64_t seq = s->Begin();
  REQUIRE(seq == 1);
  Record r{};
  r.sequence = seq;
  r.mark = RawQpc();
  r.callback_begin = RawQpc();
  r.callback_end = RawQpc();
  r.wait_begin = RawQpc();
  r.wait_end = RawQpc();
  r.requested_ns = 15000000;
  r.nt_interval = -150000;
  r.native_tid = GetCurrentThreadId();
  r.flags =
      1;  // Isolated synthetic call outcome; no NT wait is performed here.
  s->Complete(r);
  REQUIRE(s->Close(2s));
  Summary file_summary;
  REQUIRE(ValidateFile(file.path, &file_summary));
  REQUIRE(file_summary.counts.started == 1);
  REQUIRE(file_summary.counts.completed == 1);
  REQUIRE(file_summary.counts.accepted == 1);
  REQUIRE(file_summary.counts.written == 1);
  REQUIRE(file_summary.counts.outstanding == 0);
  REQUIRE(MatchesAcknowledgement(file_summary, s->summary(), true));
  Patch(file.path, kHeaderBytes + kRecordBytes + 16, 0);
  REQUIRE_FALSE(ValidateFile(file.path));  // Completed < accepted.
  Patch(file.path, kHeaderBytes + kRecordBytes + 16, 1);
  Patch(file.path, kHeaderBytes, 2);
  REQUIRE_FALSE(
      ValidateFile(file.path));  // A lone record cannot claim start #2.
}
TEST_CASE("Pacing freezes outstanding callback without draining it",
          "[pacing]") {
  Temp file;
  auto s = Session::Create(file.path, 10000000, RawQpc());
  REQUIRE(s != nullptr);
  REQUIRE(Session::Create(file.path, 10000000, RawQpc()) == nullptr);
  uint64_t seq = s->Begin();
  REQUIRE(seq == 1);
  REQUIRE(s->Close(2s));  // No Complete: emulates a blocked guest callback.
  Summary at_cutoff = s->summary();
  REQUIRE(at_cutoff.counts.started == 1);
  REQUIRE(at_cutoff.counts.completed == 0);
  REQUIRE(at_cutoff.counts.outstanding == 1);
  auto late = Example(seq, RawQpc());
  s->Complete(late);
  Summary sealed;
  REQUIRE(ValidateFile(file.path, &sealed));
  REQUIRE(MatchesAcknowledgement(sealed, at_cutoff, true));
  REQUIRE(s->Begin() == 0);
}
TEST_CASE("Pacing reports bounded queue and begin cap losses", "[pacing]") {
  Temp file;
  std::promise<void> parked, release;
  auto release_future = release.get_future().share();
  auto parked_future = parked.get_future();
  auto gate = [&] {
    parked.set_value();
    release_future.wait();
  };
  auto s = Session::Create(file.path, 10000000, RawQpc(), {}, gate);
  if (!s) {
    release.set_value();
  }
  REQUIRE(s != nullptr);
  bool entered = parked_future.wait_for(2s) == std::future_status::ready;
  if (entered) {
    for (uint32_t i = 0; i < kRecordLimit; ++i) {
      uint64_t seq = s->Begin();
      auto r = Example(seq, RawQpc());
      // Synthetic event fields; this case checks admission/loss counts.
      s->Complete(r);
    }
  }
  uint64_t over = s->Begin();
  // Release before assertions/destruction; never strand a test writer.
  release.set_value();
  bool closed = s->Close(2s);
  REQUIRE(entered);
  REQUIRE(over == 0);
  REQUIRE(closed);
  auto c = s->summary().counts;
  REQUIRE(c.started == kRecordLimit);
  REQUIRE(c.completed == kRecordLimit);
  REQUIRE(c.accepted == kQueueLimit);
  REQUIRE(c.written == kQueueLimit);
  REQUIRE(c.queue_drops == kRecordLimit - kQueueLimit);
  REQUIRE(c.limit_drops == 1);
  REQUIRE(c.outstanding == 0);
}
TEST_CASE("Pacing close timeout never becomes positive acknowledgement",
          "[pacing]") {
  Temp file;
  std::promise<void> parked, release;
  auto released = release.get_future().share();
  auto entered = parked.get_future();
  auto gate = [&] {
    parked.set_value();
    released.wait();
  };
  auto s = Session::Create(file.path, 10000000, RawQpc(), {}, {}, gate);
  if (!s) {
    release.set_value();
  }
  REQUIRE(s != nullptr);
  auto close = std::async(std::launch::async, [&] { return s->Close(30ms); });
  bool parked_ok = entered.wait_for(2s) == std::future_status::ready;
  bool bounded = close.wait_for(2s) == std::future_status::ready;
  release.set_value();
  bool first = close.get();
  bool second = s->Close(2s);
  REQUIRE(parked_ok);
  REQUIRE(bounded);
  REQUIRE_FALSE(first);
  REQUIRE_FALSE(second);
  REQUIRE(s->timed_out());
}
TEST_CASE("Pacing disabled process path has no token", "[pacing]") {
  auto t = BeginVblank();
  REQUIRE(t.session == nullptr);
  REQUIRE(t.record.mark == 0);
  CompleteVblank(t);
  REQUIRE(CloseForExit(0ms));
}
#endif
}  // namespace xe::pacing::test
