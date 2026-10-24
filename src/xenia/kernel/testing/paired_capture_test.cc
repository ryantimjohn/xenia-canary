// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic: never upstream SDK-derived capture observations.
#include "xenia/kernel/xam/paired_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "third_party/catch/include/catch.hpp"

namespace xe::kernel::xam::paired_capture::test {
namespace {
using namespace std::chrono_literals;

class TempSidecar {
 public:
  TempSidecar() {
    static std::atomic<uint64_t> next{0};
    path_ = std::filesystem::temp_directory_path() /
            ("xenia-paired-capture-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++next) + ".bin");
  }
  ~TempSidecar() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};
}  // namespace

TEST_CASE("Paired scalar codec has stable byte layout",
          "[nui][paired_capture]") {
  const auto header = EncodeHeader(10000000, 0x0102030405060708ull, 8, 10);
  CHECK(std::string(reinterpret_cast<const char*>(header.data()), 8) ==
        "XPAIR001");
  CHECK(header[8] == 1);
  CHECK(header[12] == 64);
  CHECK(header[16] == 112);
  CHECK(header[32] == 0x80);
  CHECK(header[40] == 8);
  CHECK(header[47] == 1);
  CHECK(FloatBits(1.5f) == 0x3fc00000u);

  Record record;
  record.kind = Kind::kDepthReturn;
  record.key = 3;
  record.qpc = 0x0102030405060708ull;
  record.words[0] = 0x12345678u;
  const auto bytes = EncodeRecord(record);
  CHECK(bytes[0] == uint8_t(Kind::kDepthReturn));
  CHECK(bytes[4] == 3);
  CHECK(bytes[8] == 8);
  CHECK(bytes[15] == 1);
  CHECK(bytes[16] == 0x78);
  CHECK(bytes[19] == 0x12);
  CHECK(bytes.back() == 0);
}

TEST_CASE("Paired pending queue is bounded FIFO", "[nui][paired_capture]") {
  PendingRing ring(2);
  Record first, second, out;
  first.key = 1;
  second.key = 2;
  CHECK(ring.Push(first));
  CHECK(ring.Push(second));
  CHECK_FALSE(ring.Push(first));
  CHECK(ring.size() == 2);
  REQUIRE(ring.Pop(&out));
  CHECK(out.key == 1);
  REQUIRE(ring.Pop(&out));
  CHECK(out.key == 2);
  CHECK_FALSE(ring.Pop(&out));
}

TEST_CASE("Paired sidecar closes cleanly and requires its footer",
          "[nui][paired_capture]") {
  TempSidecar file;
  std::atomic<uint64_t> tick{100};
  auto clock = [&tick] { return ++tick; };
  auto session = Session::Create(file.path(), 10000000, clock, {2, 4});
  REQUIRE(session);
  CHECK_FALSE(Session::Create(file.path(), 10000000, clock, {2, 4}));
  Words words{};
  words[0] = 69;
  words[2] = 123456;
  session->RecordEvent(Kind::kDepthSource, 0, words);
  session->RecordEvent(Kind::kDepthReturn, 0, words);
  REQUIRE(session->Close(2s));
  FileSummary summary;
  REQUIRE(ValidateFileStructure(file.path(), &summary));
  CHECK(summary.counts.accepted == 2);
  CHECK(summary.counts.written == 2);
  CloseAcknowledgement acknowledgement{
      true, session->start_qpc(), session->cutoff_qpc(), session->counts()};
  CHECK(MatchesCloseAcknowledgement(summary, acknowledgement));
  acknowledgement.complete = false;
  CHECK_FALSE(MatchesCloseAcknowledgement(summary, acknowledgement));
  acknowledgement.complete = true;
  ++acknowledgement.counts.write_errors;
  CHECK_FALSE(MatchesCloseAcknowledgement(summary, acknowledgement));
  CHECK(std::filesystem::file_size(file.path()) ==
        kHeaderBytes + 2 * kRecordBytes + kFooterBytes);
  std::filesystem::resize_file(file.path(), kHeaderBytes + 2 * kRecordBytes);
  CHECK_FALSE(ValidateFileStructure(file.path()));
}

TEST_CASE(
    "Paired controller coalescing preserves route changes and fails overflow",
    "[nui][paired_capture]") {
  TempSidecar file;
  std::atomic<uint64_t> tick{200};
  auto clock = [&tick] { return ++tick; };
  auto session = Session::Create(file.path(), 10000000, clock, {2, 2});
  REQUIRE(session);
  Words words{};
  words[0] = 0;
  words[1] = 1;  // Guest-visible routing result.
  words[5] = 1;  // Packet number alone does not make a transition.
  session->RecordEvent(Kind::kInputState, 0, words, true);
  words[5] = 2;
  session->RecordEvent(Kind::kInputState, 0, words, true);
  words[1] = 2;
  session->RecordEvent(Kind::kInputState, 0, words, true);
  words[6] = 0x1000;  // Third transition exceeds the explicit file cap.
  session->RecordEvent(Kind::kInputState, 0, words, true);
  CHECK_FALSE(session->Close(2s));
  const Counts counts = session->counts();
  CHECK(counts.accepted == 2);
  CHECK(counts.written == 2);
  CHECK(counts.coalesced == 1);
  CHECK(counts.dropped_limit == 1);
  CHECK_FALSE(ValidateFileStructure(file.path()));
}

TEST_CASE("Paired close cuts off concurrent producer admission",
          "[nui][paired_capture]") {
  TempSidecar file;
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool producer_in_tick = false;
  bool release_tick = false;
  bool allow_second = false;
  std::atomic<uint64_t> tick{100};
  auto clock = [&] {
    const uint64_t value = ++tick;
    if (value == 102) {
      std::unique_lock<std::mutex> lock(gate_mutex);
      producer_in_tick = true;
      gate_cv.notify_all();
      gate_cv.wait(lock, [&] { return release_tick; });
    }
    return value;
  };
  auto session = Session::Create(file.path(), 10000000, clock, {8, 8});
  REQUIRE(session);
  Words words{};
  words[0] = 1;
  std::thread producer([&] {
    session->RecordEvent(Kind::kInputState, 0, words);
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      gate_cv.wait(lock, [&] { return allow_second; });
    }
    words[0] = 2;
    session->RecordEvent(Kind::kInputState, 0, words);
  });
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_cv.wait(lock, [&] { return producer_in_tick; });
  }
  bool clean = false;
  std::thread closer([&] { clean = session->Close(2s); });
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_tick = true;
  }
  gate_cv.notify_all();
  closer.join();
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    allow_second = true;
  }
  gate_cv.notify_all();
  producer.join();
  CHECK(clean);
  const Counts counts = session->counts();
  CHECK(counts.accepted == 1);
  CHECK(counts.written == 1);
  FileSummary summary;
  REQUIRE(ValidateFileStructure(file.path(), &summary));
  CHECK(summary.start_qpc == session->start_qpc());
  CHECK(summary.cutoff_qpc == session->cutoff_qpc());
  CHECK(summary.cutoff_qpc >= summary.start_qpc);
  std::ifstream recorded(file.path(), std::ios::binary);
  recorded.seekg(kHeaderBytes + 8);
  uint8_t qpc_bytes[8]{};
  recorded.read(reinterpret_cast<char*>(qpc_bytes), sizeof(qpc_bytes));
  REQUIRE(recorded.good());
  uint64_t record_qpc = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    record_qpc |= uint64_t(qpc_bytes[i]) << (8 * i);
  }
  CHECK(record_qpc <= summary.cutoff_qpc);
}

TEST_CASE("Paired footer gate timeout is an unacknowledged close",
          "[nui][paired_capture]") {
  TempSidecar file;
  std::atomic<uint64_t> tick{100};
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool entered = false;
  bool release = false;
  auto before_footer = [&] {
    std::unique_lock<std::mutex> lock(gate_mutex);
    entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lock, [&] { return release; });
  };
  auto session = Session::Create(
      file.path(), 10000000, [&tick] { return ++tick; }, {2, 2}, before_footer);
  REQUIRE(session);
  bool clean = true;
  std::thread closer([&] { clean = session->Close(100ms); });
  bool saw_gate = false;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    saw_gate = gate_cv.wait_for(lock, 1s, [&] { return entered; });
  }
  closer.join();
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_cv.notify_all();
  CHECK_FALSE(session->Close(2s));  // A late drain cannot reverse the timeout.
  session.reset();
  CHECK(saw_gate);
  CHECK_FALSE(clean);
  CHECK_FALSE(ValidateFileStructure(file.path()));
}

}  // namespace xe::kernel::xam::paired_capture::test
