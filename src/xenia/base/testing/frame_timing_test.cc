// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork-only timing diagnostic; never upstream SDK-derived ancestry.
#include "xenia/base/frame_timing.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "xenia/base/platform.h"
#include "third_party/catch/include/catch.hpp"

#if XE_PLATFORM_WIN32
#include <windows.h>
#endif

namespace xe::frame_timing::test {

#if XE_PLATFORM_WIN32
TEST_CASE("Frame timing retains an owned, lossless bounded capture",
          "[frame_timing]") {
  static std::atomic<uint64_t> next{0};
  const auto path = std::filesystem::temp_directory_path() /
                    ("xenia-frame-timing-test-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     "-" + std::to_string(++next) + ".bin");
  const uint64_t start = RawQpc();
  LARGE_INTEGER frequency;
  REQUIRE(QueryPerformanceFrequency(&frequency));
  auto session = Session::Create(path, uint64_t(frequency.QuadPart), start);
  REQUIRE(session != nullptr);
  REQUIRE(Session::Create(path, uint64_t(frequency.QuadPart), RawQpc()) ==
          nullptr);

  const uint64_t guest = RawQpc();
  session->RecordEvent({Kind::kGuestSwap, 0, 1, guest, guest});
  const uint64_t present_start = RawQpc();
  session->RecordEvent(
      {Kind::kD3D12Present, 0, 1, present_start, RawQpc()});
  REQUIRE(session->Close(std::chrono::seconds(2)));
  FileSummary summary;
  REQUIRE(ValidateFileStructure(path, &summary));
  REQUIRE(summary.counts.accepted == 2);
  REQUIRE(summary.counts.written == 2);
  REQUIRE(summary.counts.queue_drops == 0);
  REQUIRE(summary.counts.limit_drops == 0);
  REQUIRE(MatchesCloseAcknowledgement(
      summary, {true, summary.start_qpc, summary.cutoff_qpc,
                summary.counts}));
  REQUIRE_FALSE(MatchesCloseAcknowledgement(
      summary, {false, summary.start_qpc, summary.cutoff_qpc,
                summary.counts}));
  session.reset();
  // A partial output after abnormal exit must not qualify as frame timing.
  std::filesystem::resize_file(path,
                               std::filesystem::file_size(path) - kFooterBytes);
  REQUIRE_FALSE(ValidateFileStructure(path));
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
#endif  // XE_PLATFORM_WIN32

}  // namespace xe::frame_timing::test
