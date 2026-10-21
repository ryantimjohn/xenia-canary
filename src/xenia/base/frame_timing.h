// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork-only timing diagnostic; never upstream SDK-derived ancestry.
#ifndef XENIA_BASE_FRAME_TIMING_H_
#define XENIA_BASE_FRAME_TIMING_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace xe::frame_timing {

constexpr uint32_t kQueueLimit = 4096;
constexpr uint32_t kRecordLimit = 524288;
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kRecordBytes = 32;
constexpr uint32_t kFooterBytes = 72;

enum class Kind : uint32_t { kGuestSwap = 1, kD3D12Present = 2 };

// A guest swap has equal begin/end QPC values and code zero. A host Present
// spans the call only; code is the raw HRESULT. Neither implies display time.
struct Record {
  Kind kind;
  uint32_t code;
  uint64_t sequence;
  uint64_t begin_qpc;
  uint64_t end_qpc;
};

struct Counts {
  uint64_t accepted = 0;
  uint64_t written = 0;
  uint64_t queue_drops = 0;
  uint64_t limit_drops = 0;
  uint64_t contention_drops = 0;
  uint64_t closing_exclusions = 0;
  uint64_t write_errors = 0;
};

struct FileSummary {
  uint64_t qpf = 0;
  uint64_t start_qpc = 0;
  uint64_t cutoff_qpc = 0;
  Counts counts{};
};
struct CloseAcknowledgement {
  bool complete = false;
  uint64_t start_qpc = 0;
  uint64_t cutoff_qpc = 0;
  Counts counts{};
};

// The file format is independent of XPAIR001 and capped at 16,777,352 bytes:
// 64-byte header, at most 524288 32-byte records, 72-byte footer. All fields
// are little endian. Header: XFRAM001, version=1, byte sizes, limits, QPF,
// start QPC, reserved zeros. Footer: XFRAME01, seven counts and cutoff QPC.
std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf,
                                               uint64_t start_qpc);
std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& record);
std::array<uint8_t, kFooterBytes> EncodeFooter(const Counts& counts,
                                               uint64_t cutoff_qpc);
// Structural validity must be paired with a positive close acknowledgement
// from the flushed log of the same owned Xenia process.
bool ValidateFileStructure(const std::filesystem::path& path,
                           FileSummary* summary = nullptr);
bool MatchesCloseAcknowledgement(const FileSummary& file,
                                 const CloseAcknowledgement& acknowledgement);

class Session {
 public:
  using BeforeFirstWait = std::function<void()>;  // Test-only writer gate.
  using BeforeFooter = std::function<void()>;     // Test-only writer gate.
  // Exclusive fresh absolute path. The parent directory must already exist.
  static std::unique_ptr<Session> Create(
      const std::filesystem::path& path, uint64_t qpf, uint64_t start_qpc,
      BeforeFooter before_footer = {}, BeforeFirstWait before_first_wait = {});
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Try-lock admission never waits for the writer or another producer. Every
  // event rejected while the gate is open is counted in the footer.
  void RecordEvent(const Record& record);
  bool Close(std::chrono::milliseconds timeout);
  Counts counts() const;
  uint64_t start_qpc() const { return start_qpc_; }
  uint64_t cutoff_qpc() const;
  bool timed_out() const { return timed_out_.load(std::memory_order_acquire); }

 private:
  Session(FILE* file, uint64_t start_qpc, BeforeFooter before_footer,
          BeforeFirstWait before_first_wait);
  void WriterMain();
  FILE* file_ = nullptr;
  BeforeFooter before_footer_;
  BeforeFirstWait before_first_wait_;
  uint64_t start_qpc_ = 0;
  uint64_t cutoff_qpc_ = 0;
  std::array<Record, kQueueLimit> ring_{};
  uint32_t head_ = 0;
  uint32_t size_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable pending_;
  std::condition_variable finished_;
  std::thread writer_;
  Counts counts_{};
  uint64_t kind_sequences_[2] = {};
  // High bit closes admission; low bits count producers admitted before close.
  std::atomic<uint32_t> admission_state_{0};
  std::atomic<uint64_t> contention_drops_{0};
  std::atomic<uint64_t> closing_exclusions_{0};
  bool stopping_ = false;
  bool done_ = false;
  bool clean_close_ = false;
  std::atomic<bool> timed_out_{false};
};

// Defaults off. The process-lifetime session is intentionally retained across
// quick_exit; CloseForExit is called on the normal UI shutdown path.
bool StartFromFlag();
bool CloseForExit(std::chrono::milliseconds timeout);
void GuestSwap();
uint64_t BeginD3D12Present();
void EndD3D12Present(uint64_t begin_qpc, uint32_t hresult);
uint64_t RawQpc();

}  // namespace xe::frame_timing
#endif  // XENIA_BASE_FRAME_TIMING_H_
