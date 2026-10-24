// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private SDK-derived fork ancestry; never upstream.
#ifndef XENIA_BASE_SWAP_ENTRY_OBSERVER_H_
#define XENIA_BASE_SWAP_ENTRY_OBSERVER_H_
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

namespace xe::swap_entry {
constexpr uint32_t kHeaderBytes = 64, kRecordBytes = 40, kFooterBytes = 80;
constexpr uint32_t kQueueLimit = 1024, kRecordLimit = 32768;
struct Record {
  uint64_t sequence = 0, qpc = 0, caller_lr = 0;
  uint32_t guest_tid = 0, native_tid = 0, flags = 0, reserved = 0;
};
struct Counts {
  // Only open-gate admissions: attempted = accepted + queue + limit +
  // contention.
  uint64_t attempted = 0, accepted = 0, written = 0;
  uint64_t queue_drops = 0, limit_drops = 0, contention_drops = 0;
  // Sampled at cutoff, NOT a final count of calls arriving after close.
  uint64_t excluded = 0, write_errors = 0;
};
struct Summary {
  uint64_t qpf = 0, start = 0, cutoff = 0;
  Counts counts{};
};
std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf, uint64_t start);
std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& record);
std::array<uint8_t, kFooterBytes> EncodeFooter(const Summary& summary);
bool ValidateFile(const std::filesystem::path& path,
                  Summary* summary = nullptr);
bool MatchesAcknowledgement(const Summary& file, const Summary& ack,
                            bool complete);
uint64_t RawQpc();
class Session {
 public:
  // Writer gates are isolated-test seams; production supplies neither.
  static std::unique_ptr<Session> Create(
      const std::filesystem::path& path, uint64_t qpf, uint64_t start,
      std::function<void()> before_drain = {},
      std::function<void()> before_footer = {});
  ~Session();
  void Observe(uint32_t guest_tid, uint32_t native_tid, uint64_t caller_lr,
               bool caller_lr_read);
  // One shutdown caller, concurrent Observe allowed. No admission spans guest
  // work.
  bool Close(std::chrono::milliseconds timeout);
  bool timed_out() const { return timed_out_.load(); }
  Summary summary() const;  // Only after Close finished without a timeout.
 private:
  Session(FILE* file, const Summary& initial, std::function<void()> drain,
          std::function<void()> footer);
  bool Enter();
  void Leave();
  void Writer();
  FILE* file_;
  Summary frozen_{};
  std::array<Record, kQueueLimit> ring_{};
  std::atomic<uint32_t> head_{0}, tail_{0}, admissions_{0};
  std::atomic_flag producer_ = ATOMIC_FLAG_INIT;
  std::atomic<uint64_t> attempted_{0}, accepted_{0}, queue_drops_{0};
  std::atomic<uint64_t> limit_drops_{0}, contention_drops_{0}, excluded_{0};
  std::atomic<bool> stopping_{false}, done_{false}, timed_out_{false};
  bool flushed_ = false;
  std::mutex wait_mutex_;
  std::condition_variable changed_;
  std::thread writer_;
  std::function<void()> before_drain_, before_footer_;
};
// Atomic load only when disabled; no thread identity, context, QPC or I/O.
Session* Current();
bool StartFromFlag();
bool CloseForExit(std::chrono::milliseconds timeout);
}  // namespace xe::swap_entry
#endif
