// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#include "xenia/kernel/xam/poll_counters.h"

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/xthread.h"
#if XE_PLATFORM_WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

DEFINE_string(poll_counters, "",
              "Private aggregate XNotifyGetNext/KeWaitMultiple snapshot; "
              "fresh absolute sidecar only, empty disables.",
              "Kernel");

namespace xe::kernel::xam::poll_counters {
namespace {
std::atomic<Session*> active{nullptr};
Session* owned_session = nullptr;
// Bounded contention is reported, never retried indefinitely in a guest call.
void AtomicCount(std::atomic<uint64_t>& value,
                 std::atomic<uint64_t>& saturation) {
  uint64_t old = value.load(std::memory_order_relaxed);
  for (unsigned n = 0; n != 8; ++n) {
    if (old == UINT64_MAX) {
      break;
    }
    if (value.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) {
      return;
    }
  }
  saturation.store(1, std::memory_order_relaxed);
}
FILE* CreateNew(const std::filesystem::path& path) {
  if (!path.is_absolute()) {
    return nullptr;
  }
#if XE_PLATFORM_WIN32
  HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                           _O_BINARY | _O_WRONLY);
  if (fd < 0) {
    CloseHandle(handle);
    return nullptr;
  }
  FILE* file = _fdopen(fd, "wb");
  if (!file) {
    _close(fd);
  }
#else
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) {
    return nullptr;
  }
  FILE* file = fdopen(fd, "wb");
  if (!file) {
    close(fd);
  }
#endif
  return file;
}
void AddElapsed(Elapsed& into, uint64_t ticks, uint64_t& saturation) {
  SaturatingAdd(into.samples, 1, saturation);
  into.ticks.Add(ticks);
  SaturatingAdd(into.sum, ticks, saturation);
}
unsigned ResultClass(uint32_t result) {
  if (result < 64) {
    return 0;
  }
  if (result == 0x102) {
    return 1;
  }
  if (result == 0xC0) {
    return 2;
  }
  if (result >= 0x80 && result < 0xC0) {
    return 3;
  }
  if (result == 0xC000000D || result == 0xC0000001) {
    return 4;
  }
  return 5;
}
}  // namespace

void Range::Add(uint64_t value) {
  min = std::min(min, value);
  max = std::max(max, value);
}
void SaturatingAdd(uint64_t& field, uint64_t amount, uint64_t& saturation) {
  if (amount > UINT64_MAX - field) {
    field = UINT64_MAX;
    saturation = 1;
  } else {
    field += amount;
  }
}
uint64_t RawQpc() {
#if XE_PLATFORM_WIN32
  LARGE_INTEGER value;
  return QueryPerformanceCounter(&value) ? uint64_t(value.QuadPart) : 0;
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

Session::Session(FILE* file, uint64_t qpf, Write write)
    : file_(file), write_(write) {
  snapshot_.bytes = sizeof(Snapshot);
  snapshot_.qpf = qpf;
  snapshot_.start = RawQpc();
}
std::unique_ptr<Session> Session::Create(const std::filesystem::path& path,
                                         uint64_t qpf, Write write) {
  // This raw scalar format is qualified only on little-endian hosts.
  uint16_t endian = 1;
  if (!qpf || *reinterpret_cast<uint8_t*>(&endian) != 1) {
    return nullptr;
  }
  FILE* file = CreateNew(path);
  if (!file) {
    return nullptr;
  }
  auto session = std::unique_ptr<Session>(new Session(file, qpf, write));
  if (!session->snapshot_.start) {
    fclose(file);
    session->file_ = nullptr;
    return nullptr;
  }
  // An interrupted startup leaves an explicitly unfrozen partial header.
  constexpr size_t prefix = offsetof(Snapshot, slots);
  if (fwrite(&session->snapshot_, 1, prefix, file) != prefix || fflush(file)) {
    fclose(file);
    session->file_ = nullptr;
    return nullptr;
  }
  try {
    session->writer_ = std::thread(&Session::Writer, session.get());
  } catch (const std::system_error&) {
    fclose(file);
    session->file_ = nullptr;
    return nullptr;
  }
  return session;
}
Session::~Session() {
  // Production intentionally retains the session through process exit.
  // Test callers must finish all Operation lifetimes before destruction.
  if (writer_.joinable()) {
    Close(std::chrono::seconds(2));
    writer_.join();
  }
}
bool Session::Enter() {
  uint64_t old = gate_.load(std::memory_order_relaxed);
  for (unsigned n = 0; n != 8 && !(old & kClosed); ++n) {
    if (gate_.compare_exchange_weak(old, old + 1)) {
      return true;
    }
  }
  return false;
}
void Session::Leave() { gate_.fetch_sub(1); }

Operation Session::Begin(Api api, uint32_t guest, uint32_t native,
                         uint64_t caller) {
  if (!Enter()) {
    AtomicCount(excluded_entry_, global_saturation_);
    return Operation();
  }
  const uint64_t key = (uint64_t(guest) << 32) | native;
  size_t index = kSlots;
  // Permanent slots avoid deletion/reuse races. Thread-local cache does not
  // dereference an old session, and checks the currently claimed key again.
  struct Cache {
    Session* session = nullptr;
    uint64_t key = 0;
    size_t slot = 0;
  };
  thread_local Cache cache;
  if (key && cache.session == this && cache.key == key &&
      keys_[cache.slot].load() == key) {
    index = cache.slot;
  } else if (key) {
    for (size_t n = 0; n != kSlots; ++n) {
      uint64_t existing = keys_[n].load();
      if (existing == key ||
          (!existing && keys_[n].compare_exchange_strong(existing, key))) {
        index = n;
        cache = {this, key, n};
        break;
      }
    }
  }
  if (index == kSlots) {
    AtomicCount(slot_overflow_, global_saturation_);
    Leave();
    return Operation();
  }
  auto& slot = snapshot_.slots[index];
  slot.key = key;
  auto& stats = slot.api[size_t(api)];
  SaturatingAdd(stats.started, 1, slot.saturation);
  bool saved = false;
  for (auto& value : stats.callers) {
    if (!value.count || value.lr == caller) {
      value.lr = caller;
      SaturatingAdd(value.count, 1, slot.saturation);
      saved = true;
      break;
    }
  }
  if (!saved) {
    SaturatingAdd(stats.caller_overflow, 1, slot.saturation);
  }
  const bool sample = (stats.started & 1023) == 0;
  Leave();
  return Operation(this, index, api, sample);
}
bool Session::Commit(Operation& op) {
  if (Enter()) {
    return true;
  }
  AtomicCount(excluded_completion_, global_saturation_);
  op.session_ = nullptr;
  return false;
}
void Session::Abandon(Operation& op) {
  if (!Commit(op)) {
    return;
  }
  auto& slot = snapshot_.slots[op.slot_];
  SaturatingAdd(slot.api[size_t(op.api_)].abandoned, 1, slot.saturation);
  op.session_ = nullptr;
  Leave();
}
Operation::Operation(Session* session, size_t slot, Api api, bool sample)
    : session_(session), slot_(slot), api_(api), sample_(sample) {}
Operation::~Operation() {
  if (session_) {
    session_->Abandon(*this);
  }
}
void Operation::StartElapsed() {
  if (session_ && sample_) {
    begin_ = RawQpc();
  }
}
void Operation::EndElapsed() {
  if (session_ && sample_) {
    const auto end = RawQpc();
    timed_ = begin_ && end >= begin_;
    if (timed_) {
      elapsed_ = end - begin_;
    }
  }
}
void Operation::FinishNotify(Notify outcome, uint32_t filter, uint64_t before,
                             uint64_t after) {
  auto* session = session_;
  if (!session || !session->Commit(*this)) {
    return;
  }
  auto& slot = session->snapshot_.slots[slot_];
  SaturatingAdd(slot.api[size_t(api_)].completed, 1, slot.saturation);
  const size_t index = size_t(outcome);
  if (index >= kNotifyBuckets) {
    SaturatingAdd(slot.bucket_overflow, 1, slot.saturation);
  } else {
    auto& bucket = slot.notify[index];
    SaturatingAdd(bucket.count, 1, slot.saturation);
    bucket.filter.Add(filter);
    // Invalid-output/handle outcomes have no queue observation.
    if (index >= size_t(Notify::kAnyDequeue)) {
      bucket.before.Add(before);
      bucket.after.Add(after);
    }
    if (timed_) {
      AddElapsed(bucket.elapsed, elapsed_, slot.saturation);
    }
  }
  session_ = nullptr;
  session->Leave();
}
void Operation::FinishWait(uint32_t count, uint32_t type, bool alertable,
                           bool timeout_known, bool timeout_present,
                           uint64_t timeout, bool host_known, uint32_t host_ms,
                           uint32_t result) {
  auto* session = session_;
  if (!session || !session->Commit(*this)) {
    return;
  }
  auto& slot = session->snapshot_.slots[slot_];
  SaturatingAdd(slot.api[size_t(api_)].completed, 1, slot.saturation);
  // Class4 means timeout unavailable: preserve the existing invalid-object
  // early return without dereferencing a timeout that the API never read.
  unsigned tc = !timeout_known     ? 4
                : !timeout_present ? 0
                : !timeout         ? 1
                : (timeout >> 63)  ? 2
                                   : 3;
  unsigned wt = type < 2 ? type : 2;
  auto& bucket = slot.wait[(tc * 3 + wt) * 6 + ResultClass(result)];
  SaturatingAdd(bucket.count, 1, slot.saturation);
  bucket.object_count.Add(count);
  bucket.result.Add(result);
  if (alertable) {
    SaturatingAdd(bucket.alertable, 1, slot.saturation);
  }
  if (timeout_known && timeout_present) {
    SaturatingAdd(bucket.raw_present, 1, slot.saturation);
    bucket.raw_timeout_ordered.Add(timeout ^ kClosed);
  }
  if (host_known) {
    bucket.host_ms.Add(host_ms);
  }
  if (timed_) {
    AddElapsed(bucket.elapsed, elapsed_, slot.saturation);
  }
  session_ = nullptr;
  session->Leave();
}

void Session::Writer() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return requested_; });
  lock.unlock();
  // Only short collector mutations participate in this gate. No guest wait
  // holds it. The high bit prevents any later admission to bucket storage.
  gate_.fetch_or(kClosed);
  bool drained = false;
  while (!(drained = gate_.load() == kClosed) &&
         std::chrono::steady_clock::now() < deadline_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  bool clean = false;
  if (drained) {
    snapshot_.cutoff = RawQpc();
    snapshot_.slot_overflow = slot_overflow_.load();
    snapshot_.excluded_entry = excluded_entry_.load();
    snapshot_.excluded_completion = excluded_completion_.load();
    snapshot_.global_saturation = global_saturation_.load();
    snapshot_.exclusion_sample_end_qpc = RawQpc();
    for (auto& slot : snapshot_.slots) {
      for (auto& api : slot.api) {
        // Unless saturation is flagged: started = completed + abandoned +
        // outstanding. Late completion cannot change this frozen relation.
        if (api.completed <= api.started &&
            api.abandoned <= api.started - api.completed) {
          api.outstanding_at_cutoff =
              api.started - api.completed - api.abandoned;
        } else {
          slot.saturation = 1;
          api.outstanding_at_cutoff = UINT64_MAX;
        }
      }
    }
    snapshot_.frozen = 1;
    clean = snapshot_.cutoff && snapshot_.exclusion_sample_end_qpc &&
            fseek(file_, 0, SEEK_SET) == 0 &&
            (write_ ? write_(file_, snapshot_)
                    : fwrite(&snapshot_, 1, sizeof(snapshot_), file_) ==
                          sizeof(snapshot_)) &&
            fflush(file_) == 0;
  }
  if (fclose(file_)) {
    clean = false;
  }
  file_ = nullptr;
  lock.lock();
  clean_ = clean;
  done_ = true;
  lock.unlock();
  condition_.notify_all();
}
bool Session::Close(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!requested_) {
    deadline_ = std::chrono::steady_clock::now() + timeout;
    requested_ = true;
    condition_.notify_one();
  }
  if (!condition_.wait_for(lock, timeout, [this] { return done_; })) {
    timed_out_ = true;
    return false;
  }
  const bool clean = clean_ && !timed_out_;
  lock.unlock();
  if (writer_.joinable()) {
    writer_.join();
  }
  return clean;
}
Operation Begin(Api api) {
  auto* session = active.load(std::memory_order_acquire);
  if (!session) {
    return Operation();
  }
  auto* thread = XThread::GetCurrentThread();
  return session->Begin(api, thread ? thread->thread_id() : 0,
                        xe::threading::current_thread_system_id(),
                        thread ? thread->thread_state()->context()->lr : 0);
}
bool StartFromFlag() {
  if (cvars::poll_counters.empty()) {
    return true;
  }
  if (owned_session) {
    return false;
  }
#if XE_PLATFORM_WIN32
  LARGE_INTEGER frequency;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
    return false;
  }
  auto session = Session::Create(xe::to_path(cvars::poll_counters),
                                 uint64_t(frequency.QuadPart));
  if (!session) {
    XELOGE("Poll counters: fresh sidecar failed");
    return false;
  }
  owned_session = session.release();
  active.store(owned_session, std::memory_order_release);
  return true;
#else
  return false;
#endif
}
bool CloseForExit(std::chrono::milliseconds timeout) {
  auto* session = active.load(std::memory_order_acquire);
  if (!session) {
    return true;
  }
  bool clean = session->Close(timeout);
  active.store(nullptr, std::memory_order_release);
  // No snapshot reads on failed drain: short mutations may still exist.
  XELOGI(
      "Poll counters close: complete={} frozen_snapshot_only={} "
      "write_or_drain_failure={} start_qpc={} cutoff_qpc={} bytes={}",
      clean, clean, !clean, clean ? session->snapshot().start : 0,
      clean ? session->snapshot().cutoff : 0, sizeof(Snapshot));
  return clean;
}
static_assert(std::is_trivially_copyable<Snapshot>::value);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
}  // namespace xe::kernel::xam::poll_counters
