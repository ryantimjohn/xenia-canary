// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork-only timing diagnostic; never upstream SDK-derived ancestry.
#include "xenia/base/frame_timing.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include <windows.h>
#endif

namespace xe::frame_timing::test {
namespace {
using namespace std::chrono_literals;

struct TempSidecar {
  std::filesystem::path path;
  TempSidecar() {
    static std::atomic<uint64_t> next{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-frame-timing-test-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++next) + ".bin");
  }
  ~TempSidecar() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

void PatchLittleEndian(const std::filesystem::path& path, uint64_t offset,
                       uint64_t value, unsigned bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  for (unsigned i = 0; i < bytes; ++i) {
    const char byte = char(value >> (8 * i));
    file.write(&byte, 1);
  }
  file.flush();
}
}  // namespace

#if XE_PLATFORM_WIN32
TEST_CASE("Frame timing validates ownership, intervals, and sequence",
          "[frame_timing]") {
  TempSidecar lifecycle;
  LARGE_INTEGER frequency;
  REQUIRE(QueryPerformanceFrequency(&frequency));
  const uint64_t live_start = RawQpc();
  std::mutex first_wait_gate;
  std::condition_variable first_wait_changed;
  bool writer_parked = false;
  bool release_writer = false;
  auto before_first_wait = [&] {
    std::unique_lock<std::mutex> lock(first_wait_gate);
    writer_parked = true;
    first_wait_changed.notify_all();
    first_wait_changed.wait(lock, [&] { return release_writer; });
  };
  auto session = Session::Create(lifecycle.path, uint64_t(frequency.QuadPart),
                                 live_start, {}, before_first_wait);
  REQUIRE(session != nullptr);
  auto duplicate =
      Session::Create(lifecycle.path, uint64_t(frequency.QuadPart), RawQpc());
  bool parked;
  {
    std::unique_lock<std::mutex> lock(first_wait_gate);
    parked =
        first_wait_changed.wait_for(lock, 2s, [&] { return writer_parked; });
  }
  const uint64_t live_event = RawQpc();
  if (parked) {
    session->RecordEvent({Kind::kGuestSwap, 0, 0, live_event, live_event});
  }
  {
    std::lock_guard<std::mutex> lock(first_wait_gate);
    release_writer = true;
  }
  first_wait_changed.notify_all();
  REQUIRE(duplicate == nullptr);
  REQUIRE(parked);
  REQUIRE(session->Close(2s));
  session.reset();
  FileSummary live_summary;
  REQUIRE(ValidateFileStructure(lifecycle.path, &live_summary));
  REQUIRE(live_summary.counts.accepted == 1);

  // Construct a fixed three-record fixture so writer/producer mutex timing
  // cannot turn this format-validation test into a contention-loss test.
  TempSidecar original;
  const uint64_t start = RawQpc();
  const uint64_t guest1 = start + 1;
  const uint64_t guest2 = start + 2;
  const uint64_t present_start = start + 3;
  Counts fixture_counts;
  fixture_counts.accepted = fixture_counts.written = 3;
  {
    std::ofstream fixture(original.path, std::ios::binary);
    REQUIRE(fixture.good());
    auto write = [&](const auto& bytes) {
      fixture.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    write(EncodeHeader(uint64_t(frequency.QuadPart), start));
    write(EncodeRecord({Kind::kGuestSwap, 0, 1, guest1, guest1}));
    write(EncodeRecord({Kind::kGuestSwap, 0, 2, guest2, guest2}));
    write(EncodeRecord(
        {Kind::kD3D12Present, 0, 1, present_start, present_start}));
    write(EncodeFooter(fixture_counts, start + 4));
    REQUIRE(fixture.good());
  }
  FileSummary summary;
  REQUIRE(ValidateFileStructure(original.path, &summary));
  REQUIRE(summary.counts.accepted == 3);
  REQUIRE(summary.counts.written == 3);
  REQUIRE(MatchesCloseAcknowledgement(
      summary, {true, summary.start_qpc, summary.cutoff_qpc, summary.counts}));
  auto wrong_counts = summary.counts;
  ++wrong_counts.written;
  REQUIRE_FALSE(MatchesCloseAcknowledgement(
      summary, {true, summary.start_qpc, summary.cutoff_qpc, wrong_counts}));
  REQUIRE_FALSE(MatchesCloseAcknowledgement(
      summary, {false, summary.start_qpc, summary.cutoff_qpc, summary.counts}));
  REQUIRE_FALSE(MatchesCloseAcknowledgement(
      summary,
      {true, summary.start_qpc, summary.cutoff_qpc + 1, summary.counts}));

  TempSidecar bad_sequence;
  std::filesystem::copy_file(original.path, bad_sequence.path);
  PatchLittleEndian(bad_sequence.path, kHeaderBytes + kRecordBytes + 8, 1,
                    8);  // Second guest sequence duplicates first.
  REQUIRE_FALSE(ValidateFileStructure(bad_sequence.path));

  TempSidecar bad_interval;
  std::filesystem::copy_file(original.path, bad_interval.path);
  PatchLittleEndian(bad_interval.path, kHeaderBytes + kRecordBytes + 16,
                    summary.cutoff_qpc + 1, 8);
  PatchLittleEndian(bad_interval.path, kHeaderBytes + kRecordBytes + 24,
                    summary.cutoff_qpc + 1, 8);
  REQUIRE_FALSE(ValidateFileStructure(bad_interval.path));

  TempSidecar retrograde_interval;
  std::filesystem::copy_file(original.path, retrograde_interval.path);
  REQUIRE(guest1 > summary.start_qpc);
  PatchLittleEndian(retrograde_interval.path, kHeaderBytes + kRecordBytes + 16,
                    guest1 - 1, 8);
  PatchLittleEndian(retrograde_interval.path, kHeaderBytes + kRecordBytes + 24,
                    guest1 - 1, 8);
  REQUIRE_FALSE(ValidateFileStructure(retrograde_interval.path));

  TempSidecar reported_loss;
  std::filesystem::copy_file(original.path, reported_loss.path);
  PatchLittleEndian(
      reported_loss.path,
      std::filesystem::file_size(reported_loss.path) - kFooterBytes + 40, 1, 8);
  REQUIRE_FALSE(ValidateFileStructure(reported_loss.path));

  TempSidecar bad_limit;
  std::filesystem::copy_file(original.path, bad_limit.path);
  PatchLittleEndian(bad_limit.path, 24, kRecordLimit + 1, 4);
  REQUIRE_FALSE(ValidateFileStructure(bad_limit.path));

  TempSidecar bad_cutoff;
  std::filesystem::copy_file(original.path, bad_cutoff.path);
  PatchLittleEndian(bad_cutoff.path,
                    std::filesystem::file_size(bad_cutoff.path) - 8,
                    summary.start_qpc - 1, 8);
  REQUIRE_FALSE(ValidateFileStructure(bad_cutoff.path));

  TempSidecar truncated;
  std::filesystem::copy_file(original.path, truncated.path);
  std::filesystem::resize_file(
      truncated.path,
      std::filesystem::file_size(truncated.path) - kFooterBytes);
  REQUIRE_FALSE(ValidateFileStructure(truncated.path));
}

TEST_CASE("Frame timing latches a timed-out close as unqualified",
          "[frame_timing]") {
  TempSidecar output;
  LARGE_INTEGER frequency;
  REQUIRE(QueryPerformanceFrequency(&frequency));
  std::mutex gate;
  std::condition_variable changed;
  bool footer_reached = false;
  bool release_footer = false;
  auto before_footer = [&] {
    std::unique_lock<std::mutex> lock(gate);
    footer_reached = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release_footer; });
  };
  auto session = Session::Create(output.path, uint64_t(frequency.QuadPart),
                                 RawQpc(), before_footer);
  REQUIRE(session != nullptr);
  const uint64_t qpc = RawQpc();
  session->RecordEvent({Kind::kGuestSwap, 0, 0, qpc, qpc});
  bool clean = true;
  std::thread closer([&] { clean = session->Close(100ms); });
  {
    std::unique_lock<std::mutex> lock(gate);
    changed.wait_for(lock, 1s, [&] { return footer_reached; });
  }
  closer.join();
  {
    std::lock_guard<std::mutex> lock(gate);
    release_footer = true;
  }
  changed.notify_all();
  session.reset();
  REQUIRE_FALSE(clean);
  REQUIRE_FALSE(ValidateFileStructure(output.path));
}

TEST_CASE("Frame timing close excludes concurrent producer tail",
          "[frame_timing]") {
  TempSidecar output;
  LARGE_INTEGER frequency;
  REQUIRE(QueryPerformanceFrequency(&frequency));
  auto session =
      Session::Create(output.path, uint64_t(frequency.QuadPart), RawQpc());
  REQUIRE(session != nullptr);
  std::atomic<bool> producing{false};
  std::thread producer([&] {
    producing.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 1000; ++i) {
      const uint64_t qpc = RawQpc();
      session->RecordEvent({Kind::kGuestSwap, 0, 0, qpc, qpc});
    }
  });
  while (!producing.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const bool clean = session->Close(2s);
  producer.join();
  const Counts counts = session->counts();
  const bool timed_out = session->timed_out();
  REQUIRE(counts.accepted <= 1000);
  if (clean) {
    REQUIRE(counts.written == counts.accepted);
    REQUIRE(ValidateFileStructure(output.path));
  } else {
    REQUIRE(counts.contention_drops || counts.closing_exclusions ||
            counts.queue_drops || counts.limit_drops || counts.write_errors ||
            timed_out);
  }
}
#endif  // XE_PLATFORM_WIN32

}  // namespace xe::frame_timing::test
