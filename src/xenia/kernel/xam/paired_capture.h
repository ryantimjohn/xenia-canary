// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic. SDK-derived ancestry stays in this fork; do not
// upstream this capture format or its observations.
#ifndef XENIA_KERNEL_XAM_PAIRED_CAPTURE_H_
#define XENIA_KERNEL_XAM_PAIRED_CAPTURE_H_

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace xe::kernel::xam::paired_capture {

// XPAIR001 is a separate scalar sidecar. Existing .xdepth and .xctrl files
// and their replay readers are intentionally unchanged. All integers on disk
// are little-endian, irrespective of the host or guest object layout.
constexpr size_t kHeaderBytes = 64;
constexpr size_t kRecordBytes = 112;
constexpr size_t kFooterBytes = 64;
constexpr uint32_t kHardQueueLimit = 2048;
constexpr uint32_t kHardRecordLimit = 131072;

enum class Kind : uint32_t {
  kInputState = 1,
  kKeystroke = 2,
  kStreamStart = 3,
  kDepthReturn = 4,
  kTiltStatus = 5,
  kElevationGet = 6,
  kElevationSet = 7,
  kRememberFloor = 8,
};

using Words = std::array<uint32_t, 24>;
struct Record {
  Kind kind = Kind::kInputState;
  uint32_t key = 0;
  uint64_t qpc = 0;
  Words words{};
};
struct Counts {
  uint64_t accepted = 0;
  uint64_t written = 0;
  uint64_t coalesced = 0;
  uint64_t dropped_queue = 0;
  uint64_t dropped_limit = 0;
  uint64_t write_errors = 0;
};

// Header: magic[8], version/u32, sizes and limits/u32, zero/u32,
// QPF/u64, start QPC/u64, reserved zeroes. Record: kind/u32, key/u32,
// QPC/u64, 24 scalar words/u32. Footer: magic[8], six counts/u64, end QPC/u64.
// A missing footer, count mismatch, drop or write error makes a capture
// incomplete. Float values use their IEEE-754 bit pattern in one scalar word.
std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf,
                                                uint64_t start_qpc,
                                                uint32_t queue_limit,
                                                uint32_t record_limit);
std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& record);
std::array<uint8_t, kFooterBytes> EncodeFooter(const Counts& counts,
                                                uint64_t end_qpc);
uint32_t FloatBits(float value);
// Offline qualification. Rejects absent/truncated footers, overflow, writer
// errors, or counts inconsistent with the exact fixed-record file length.
bool ValidateFile(const std::filesystem::path& path, Counts* counts = nullptr);

// Fixed ring, separated so full-queue behavior can be tested without racing
// a writer thread. The caller holds the session mutex in production.
class PendingRing {
 public:
  explicit PendingRing(uint32_t capacity);
  bool Push(const Record& record);
  bool Pop(Record* record);
  uint32_t size() const { return size_; }

 private:
  std::array<Record, kHardQueueLimit> entries_{};
  uint32_t capacity_ = 0;
  uint32_t head_ = 0;
  uint32_t size_ = 0;
};

class Session {
 public:
  using Tick = std::function<uint64_t()>;
  struct Limits {
    uint32_t queue = kHardQueueLimit;
    uint32_t records = kHardRecordLimit;
  };
  // Exclusive create-new output. No directory creation or overwrite.
  static std::unique_ptr<Session> Create(const std::filesystem::path& path,
                                          uint64_t qpf, Tick tick,
                                          Limits limits);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // No disk I/O or waits on the writer in guest-facing calls. Unchanged
  // state/tilt/floor polls use coalesce=true; depth deliveries never do.
  void RecordEvent(Kind kind, uint32_t key, const Words& words,
                   bool coalesce = false);
  bool Close(std::chrono::milliseconds timeout);
  Counts counts() const;
  uint64_t TickNow() const { return tick_(); }

 private:
  struct Last {
    bool valid = false;
    Kind kind = Kind::kInputState;
    uint32_t key = 0;
    Words words{};
  };
  Session(FILE* file, uint64_t qpf, Tick tick, Limits limits);
  void WriterMain();
  FILE* file_ = nullptr;
  Tick tick_;
  Limits limits_;
  PendingRing queue_;
  mutable std::mutex mutex_;
  std::condition_variable pending_;
  std::condition_variable finished_;
  std::thread writer_;
  std::array<Last, 64> last_{};
  Counts counts_{};
  bool stopping_ = false;
  bool done_ = false;
  bool clean_close_ = false;
  bool timed_out_ = false;
};

// A process-lifetime pointer is deliberately leaked because normal GUI exit
// calls quick_exit. CloseForExit is called explicitly before quick_exit.
bool StartFromFlag();
Session* Active();
bool CloseForExit(std::chrono::milliseconds timeout);
uint64_t RawQpc();

}  // namespace xe::kernel::xam::paired_capture
#endif  // XENIA_KERNEL_XAM_PAIRED_CAPTURE_H_
