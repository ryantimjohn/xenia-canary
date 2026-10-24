// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private SDK-derived fork ancestry; never upstream.
#ifndef XENIA_BASE_PACING_OBSERVER_H_
#define XENIA_BASE_PACING_OBSERVER_H_
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

namespace xe::pacing {
constexpr uint32_t kHeaderBytes = 128, kRecordBytes = 80, kFooterBytes = 96;
constexpr uint32_t kQueueLimit = 1024, kRecordLimit = 32768;
// Flags: bit0=query attempted, bit1=query success, bit2=set attempted,
// bit3=set success. Invalid outputs MUST be zero. Values are 100ns units.
struct TimerMetadata {
  uint32_t flags = 0, query_status = 0, set_status = 0;
  uint32_t minimum = 0, maximum = 0, query_current = 0;
  uint32_t set_current = 0, requested = 0;
};
struct Record {
  uint64_t sequence = 0, mark = 0, callback_begin = 0, callback_end = 0;
  uint64_t wait_begin = 0, wait_end = 0;
  int64_t requested_ns = 0, nt_interval = 0;
  uint32_t native_tid = 0, nt_status = 0, flags = 0, reserved = 0;
};
// completed counts Complete admissions, including rejected queue records.
// started = completed + outstanding at cutoff, never a final process total.
// excluded is sampled during cutoff; later excluded completions are not totals.
struct Counts {
  uint64_t started = 0, completed = 0, accepted = 0, written = 0;
  uint64_t queue_drops = 0, limit_drops = 0, contention_drops = 0;
  uint64_t excluded = 0, outstanding = 0, write_errors = 0;
};
struct Summary {
  uint64_t qpf = 0, start = 0, cutoff = 0;
  TimerMetadata timer{};
  Counts counts{};
};
// Identical to existing Windows NanoSleep arithmetic, including negative input.
constexpr int64_t NtInterval(int64_t ns) {
  int64_t units = ns / 100LL;
  if (units == 0 && ns != 0) {
    units = 1;
  }
  return -units;
}
std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf, uint64_t start,
                                               const TimerMetadata& timer);
std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& record);
std::array<uint8_t, kFooterBytes> EncodeFooter(const Counts& counts,
                                               uint64_t cutoff);
bool ValidateFile(const std::filesystem::path& path,
                  Summary* summary = nullptr);
bool MatchesAcknowledgement(const Summary& file, const Summary& ack,
                            bool complete);
uint64_t RawQpc();

class Session {
 public:
  // Isolated test writer gates; never supplied by production.
  static std::unique_ptr<Session> Create(
      const std::filesystem::path& path, uint64_t qpf, uint64_t start,
      const TimerMetadata& timer = {}, std::function<void()> before_drain = {},
      std::function<void()> before_footer = {});
  ~Session();
  // One production limiter thread, ordered Begin -> Complete; no nested ticks.
  // Contending test/misuse producers are dropped, not a multi-producer
  // contract.
  uint64_t Begin();
  void Complete(const Record& record);
  bool Close(std::chrono::milliseconds timeout);
  bool timed_out() const { return timed_out_.load(); }
  Summary summary() const;  // Only after successful bounded Close.
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
  std::atomic<uint64_t> started_{0}, completed_{0}, accepted_{0};
  std::atomic<uint64_t> queue_drops_{0}, limit_drops_{0}, contention_drops_{0};
  std::atomic<uint64_t> excluded_{0};
  std::atomic<bool> stopping_{false}, done_{false}, timed_out_{false};
  bool flushed_ = false;
  std::mutex wait_mutex_;
  std::condition_variable changed_;
  std::thread writer_;
  std::function<void()> before_drain_, before_footer_;
};
// Token owns no drain admission across guest execution or OS wait. Session is
// process-retained; a late completion can never access freed session storage.
struct Tick {
  Session* session = nullptr;
  Record record{};
};
Tick BeginVblank();
void CompleteVblank(Tick& tick);
// Called only with results of the EXISTING main_win query/set calls, before
// Start.
void SaveTimerMetadata(const TimerMetadata& timer);
bool StartFromFlag();
bool CloseForExit(std::chrono::milliseconds timeout);
}  // namespace xe::pacing
#endif
