// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#ifndef XENIA_KERNEL_XAM_POLL_COUNTERS_H_
#define XENIA_KERNEL_XAM_POLL_COUNTERS_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace xe::kernel::xam::poll_counters {
constexpr size_t kSlots = 64, kWaitBuckets = 90, kNotifyBuckets = 7;
constexpr uint64_t kClosed = uint64_t(1) << 63;
enum class Api : uint32_t { kNotify, kWait };
enum class Notify : uint32_t {
  kInvalidOutput,
  kInvalidHandle,
  kAnyDequeue,
  kAnyEmpty,
  kFilteredDequeue,
  kFilteredEmpty,
  kFilteredPending
};
struct Range {
  uint64_t min = UINT64_MAX, max = 0;
  void Add(uint64_t value);
};
struct Elapsed {
  uint64_t samples = 0;
  Range ticks;
  uint64_t sum = 0;
};
struct Caller {
  uint64_t lr = 0, count = 0;
};
struct ApiStats {
  uint64_t started = 0, completed = 0, abandoned = 0;
  uint64_t outstanding_at_cutoff = 0;
  std::array<Caller, 4> callers{};
  uint64_t caller_overflow = 0;
};
struct NotifyBucket {
  uint64_t count = 0;
  Range filter, before, after;
  Elapsed elapsed;
};
struct WaitBucket {
  uint64_t count = 0;
  Range object_count, raw_timeout_ordered, host_ms, result;
  Elapsed elapsed;
  uint64_t alertable = 0, raw_present = 0;
};
struct SlotData {
  uint64_t key = 0;
  std::array<ApiStats, 2> api{};
  std::array<NotifyBucket, kNotifyBuckets> notify{};
  std::array<WaitBucket, kWaitBuckets> wait{};
  uint64_t saturation = 0, bucket_overflow = 0;
};
struct Snapshot {
  // Fixed native little-endian u64 fields, no pointers/atomic objects/padding.
  std::array<char, 8> magic{'X', 'P', 'O', 'L', 'L', '0', '0', '1'};
  uint64_t version = 1, bytes = 0, qpf = 0, start = 0, cutoff = 0;
  uint64_t slot_limit = kSlots, wait_buckets = kWaitBuckets;
  uint64_t notify_buckets = kNotifyBuckets, sample_stride = 1024;
  uint64_t slot_overflow = 0, excluded_entry = 0, excluded_completion = 0;
  uint64_t global_saturation = 0;
  uint64_t exclusion_sample_end_qpc = 0;
  std::array<SlotData, kSlots> slots{};
  uint64_t frozen = 0;
};
static_assert(sizeof(Snapshot) < 1024 * 1024);
static_assert(sizeof(WaitBucket) == 15 * 8);
static_assert(sizeof(NotifyBucket) == 11 * 8);
static_assert(sizeof(Snapshot) == 745600);
uint64_t RawQpc();
void SaturatingAdd(uint64_t& field, uint64_t amount, uint64_t& saturation);

class Session;
class Operation {
 public:
  Operation() = default;
  Operation(Session* session, size_t slot, Api api, bool sample);
  ~Operation();
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
  explicit operator bool() const { return session_ != nullptr; }
  void StartElapsed();
  void EndElapsed();
  void FinishNotify(Notify outcome, uint32_t filter, uint64_t before = 0,
                    uint64_t after = 0);
  void FinishWait(uint32_t count, uint32_t type, bool alertable,
                  bool timeout_known, bool timeout_present, uint64_t timeout,
                  bool host_known, uint32_t host_ms, uint32_t result);

 private:
  friend class Session;
  Session* session_ = nullptr;
  size_t slot_ = 0;
  Api api_ = Api::kNotify;
  bool sample_ = false, timed_ = false;
  uint64_t begin_ = 0, elapsed_ = 0;
};
class Session {
 public:
  // Lifecycle-only injection for deterministic writer-error tests. Production
  // always uses the ordinary single snapshot fwrite path.
  using Write = bool (*)(FILE*, const Snapshot&);
  static std::unique_ptr<Session> Create(const std::filesystem::path& path,
                                         uint64_t qpf, Write write = nullptr);
  ~Session();
  Operation Begin(Api api, uint32_t guest, uint32_t native, uint64_t caller);
  bool Close(std::chrono::milliseconds timeout);
  // Only inspect after successful Close; late completions cannot alter data.
  const Snapshot& snapshot() const { return snapshot_; }

 private:
  friend class Operation;
  explicit Session(FILE* file, uint64_t qpf, Write write);
  bool Enter();
  void Leave();
  bool Commit(Operation& op);
  void Abandon(Operation& op);
  void Writer();
  FILE* file_;
  Write write_;
  Snapshot snapshot_{};
  std::array<std::atomic<uint64_t>, kSlots> keys_{};
  std::atomic<uint64_t> gate_{0}, excluded_entry_{0}, excluded_completion_{0};
  std::atomic<uint64_t> slot_overflow_{0}, global_saturation_{0};
  std::mutex mutex_;
  std::condition_variable condition_;
  bool requested_ = false, done_ = false, clean_ = false, timed_out_ = false;
  std::chrono::steady_clock::time_point deadline_;
  std::thread writer_;
};
Operation Begin(Api api);
bool StartFromFlag();
bool CloseForExit(std::chrono::milliseconds timeout);
}  // namespace xe::kernel::xam::poll_counters
#endif
