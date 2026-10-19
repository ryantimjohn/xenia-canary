// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic: never upstream SDK-derived capture observations.
#include "xenia/kernel/xam/paired_capture.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

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
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()) +
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

TEST_CASE("Paired scalar codec has stable byte layout", "[nui][paired_capture]") {
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
  Counts counts;
  REQUIRE(ValidateFile(file.path(), &counts));
  CHECK(counts.accepted == 2);
  CHECK(counts.written == 2);
  CHECK(std::filesystem::file_size(file.path()) ==
        kHeaderBytes + 2 * kRecordBytes + kFooterBytes);
  std::filesystem::resize_file(file.path(),
                               kHeaderBytes + 2 * kRecordBytes);
  CHECK_FALSE(ValidateFile(file.path()));
}

TEST_CASE("Paired controller coalescing preserves route changes and fails overflow",
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
  CHECK_FALSE(ValidateFile(file.path()));
}

}  // namespace xe::kernel::xam::paired_capture::test
