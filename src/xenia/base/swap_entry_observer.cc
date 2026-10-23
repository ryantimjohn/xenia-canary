// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private SDK-derived fork ancestry; never upstream.
#include "xenia/base/swap_entry_observer.h"
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <utility>
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#if XE_PLATFORM_WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
DEFINE_string(swap_entry_observer, "",
              "Private Windows VdSwap construction-entry XSWAP001 sidecar; "
              "fresh absolute path, empty disables.", "GPU");
namespace xe::swap_entry {
namespace {
constexpr uint32_t kClosed = 0x80000000u;
std::atomic<Session*> active{nullptr};
Session* owned = nullptr;  // Process-retained, including timeout and late calls.
void Put(uint8_t* p, uint64_t value, unsigned n = 8) {
  for (unsigned i = 0; i < n; ++i) p[i] = uint8_t(value >> (8 * i));
}
uint64_t Get(const uint8_t* p, unsigned n = 8) {
  uint64_t value = 0;
  for (unsigned i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8 * i);
  return value;
}
template <size_t N>
bool Write(FILE* file, const std::array<uint8_t, N>& bytes) {
  return fwrite(bytes.data(), 1, N, file) == N;
}
FILE* CreateNew(const std::filesystem::path& path) {
  if (!path.is_absolute()) return nullptr;
#if XE_PLATFORM_WIN32
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY | _O_WRONLY);
  if (fd < 0) { CloseHandle(h); return nullptr; }
  FILE* file = _fdopen(fd, "wb");
  if (!file) _close(fd);
#else
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) return nullptr;
  FILE* file = fdopen(fd, "wb");
  if (!file) close(fd);
#endif
  return file;
}
}  // namespace
std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf, uint64_t start) {
  std::array<uint8_t, kHeaderBytes> b{};
  memcpy(b.data(), "XSWAP001", 8);
  const uint32_t fields[] = {1, kHeaderBytes, kRecordBytes, kQueueLimit, kRecordLimit};
  for (unsigned i = 0; i < 5; ++i) Put(b.data() + 8 + i * 4, fields[i], 4);
  Put(b.data() + 32, qpf);
  Put(b.data() + 40, start);
  return b;
}
std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& r) {
  std::array<uint8_t, kRecordBytes> b{};
  Put(b.data(), r.sequence); Put(b.data() + 8, r.qpc);
  Put(b.data() + 16, r.caller_lr);
  Put(b.data() + 24, r.guest_tid, 4); Put(b.data() + 28, r.native_tid, 4);
  Put(b.data() + 32, r.flags, 4); Put(b.data() + 36, r.reserved, 4);
  return b;
}
std::array<uint8_t, kFooterBytes> EncodeFooter(const Summary& s) {
  std::array<uint8_t, kFooterBytes> b{};
  memcpy(b.data(), "XSWAPEND", 8);
  const auto& c = s.counts;
  const uint64_t values[] = {c.attempted, c.accepted, c.written, c.queue_drops,
      c.limit_drops, c.contention_drops, c.excluded, c.write_errors, s.cutoff};
  for (unsigned i = 0; i < 9; ++i) Put(b.data() + 8 + 8 * i, values[i]);
  return b;
}
bool ValidateFile(const std::filesystem::path& path, Summary* summary) {
  std::error_code ec;
  uint64_t bytes = std::filesystem::file_size(path, ec);
  if (ec || bytes < kHeaderBytes + kFooterBytes ||
      bytes > kHeaderBytes + uint64_t(kRecordLimit) * kRecordBytes + kFooterBytes ||
      (bytes - kHeaderBytes - kFooterBytes) % kRecordBytes) return false;
  std::ifstream file(path, std::ios::binary);
  std::array<uint8_t, kHeaderBytes> h{};
  file.read(reinterpret_cast<char*>(h.data()), h.size());
  Summary s{};
  s.qpf = Get(h.data() + 32); s.start = Get(h.data() + 40);
  if (!file || !s.qpf || !s.start || h != EncodeHeader(s.qpf, s.start)) return false;
  uint64_t n = (bytes - kHeaderBytes - kFooterBytes) / kRecordBytes;
  uint64_t previous = s.start;
  for (uint64_t i = 0; i < n; ++i) {
    std::array<uint8_t, kRecordBytes> r{};
    file.read(reinterpret_cast<char*>(r.data()), r.size());
    uint64_t qpc = Get(r.data() + 8);
    uint32_t flags = uint32_t(Get(r.data() + 32, 4));
    if (!file || Get(r.data()) != i + 1 || qpc < previous ||
        !Get(r.data() + 28, 4) || flags > 1 ||
        (!flags && Get(r.data() + 16)) || Get(r.data() + 36, 4)) return false;
    previous = qpc;
  }
  std::array<uint8_t, kFooterBytes> f{};
  file.read(reinterpret_cast<char*>(f.data()), f.size());
  if (!file || memcmp(f.data(), "XSWAPEND", 8)) return false;
  auto& c = s.counts;
  c.attempted = Get(f.data() + 8); c.accepted = Get(f.data() + 16);
  c.written = Get(f.data() + 24); c.queue_drops = Get(f.data() + 32);
  c.limit_drops = Get(f.data() + 40); c.contention_drops = Get(f.data() + 48);
  c.excluded = Get(f.data() + 56); c.write_errors = Get(f.data() + 64);
  s.cutoff = Get(f.data() + 72);
  uint64_t remaining = c.attempted;
  for (uint64_t part : {c.accepted, c.queue_drops, c.limit_drops, c.contention_drops}) {
    if (part > remaining) return false;
    remaining -= part;
  }
  if (remaining || c.accepted != n || c.written != n || c.write_errors ||
      s.cutoff < previous || s.cutoff < s.start) return false;
  if (summary) *summary = s;
  return true;
}
bool MatchesAcknowledgement(const Summary& f, const Summary& a, bool complete) {
  return complete && f.qpf == a.qpf && f.start == a.start &&
      EncodeFooter(f) == EncodeFooter(a);
}
Session::Session(FILE* f, const Summary& initial, std::function<void()> drain,
                 std::function<void()> footer)
    : file_(f), frozen_(initial), before_drain_(std::move(drain)),
      before_footer_(std::move(footer)) {}
std::unique_ptr<Session> Session::Create(const std::filesystem::path& path,
    uint64_t qpf, uint64_t start, std::function<void()> drain,
    std::function<void()> footer) {
  if (!qpf || !start) return nullptr;
  FILE* file = CreateNew(path);
  if (!file) return nullptr;
  if (!Write(file, EncodeHeader(qpf, start)) || fflush(file)) {
    fclose(file); return nullptr;
  }
  Summary s{}; s.qpf = qpf; s.start = start;
  auto result = std::unique_ptr<Session>(
      new Session(file, s, std::move(drain), std::move(footer)));
  try { result->writer_ = std::thread(&Session::Writer, result.get()); }
  catch (...) { fclose(file); result->file_ = nullptr; return nullptr; }
  return result;
}
bool Session::Enter() {
  uint32_t old = admissions_.fetch_add(1, std::memory_order_acq_rel);
  if (!(old & kClosed)) return true;
  Leave(); return false;
}
void Session::Leave() { admissions_.fetch_sub(1, std::memory_order_acq_rel); }
void Session::Observe(uint32_t guest, uint32_t native, uint64_t lr, bool lr_read) {
  if (!Enter()) { ++excluded_; return; }
  ++attempted_;
  if (producer_.test_and_set(std::memory_order_acquire)) {
    ++contention_drops_;
  } else {
    if (accepted_.load(std::memory_order_relaxed) == kRecordLimit) {
      ++limit_drops_;
    } else {
      uint32_t tail = tail_.load(std::memory_order_relaxed);
      if (tail - head_.load(std::memory_order_acquire) == kQueueLimit) {
        ++queue_drops_;
      } else {
        Record r{};
        r.sequence = accepted_.load(std::memory_order_relaxed) + 1;
        r.qpc = RawQpc(); r.guest_tid = guest; r.native_tid = native;
        r.caller_lr = lr_read ? lr : 0; r.flags = lr_read ? 1 : 0;
        ring_[tail % kQueueLimit] = r;
        tail_.store(tail + 1, std::memory_order_release);
        ++accepted_;
        changed_.notify_one();
      }
    }
    producer_.clear(std::memory_order_release);
  }
  Leave();
}
void Session::Writer() {
  if (before_drain_) before_drain_();
  uint64_t written = 0, errors = 0;
  for (;;) {
    uint32_t head = head_.load(std::memory_order_relaxed);
    if (head != tail_.load(std::memory_order_acquire)) {
      Record r = ring_[head % kQueueLimit];
      head_.store(head + 1, std::memory_order_release);
      if (!errors) { if (Write(file_, EncodeRecord(r))) ++written; else ++errors; }
      continue;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      if (head_.load() == tail_.load(std::memory_order_acquire)) break;
      continue;
    }
    std::unique_lock<std::mutex> lock(wait_mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(50), [this] {
      return stopping_.load(std::memory_order_acquire) || head_.load() != tail_.load();
    });
  }
  if (before_footer_) before_footer_();
  frozen_.counts.written = written; frozen_.counts.write_errors = errors;
  bool good = !errors && Write(file_, EncodeFooter(frozen_)) && fflush(file_) == 0;
  if (fclose(file_)) good = false;
  file_ = nullptr;
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    flushed_ = good; done_.store(true, std::memory_order_release);
  }
  changed_.notify_all();
}
bool Session::Close(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  admissions_.fetch_or(kClosed, std::memory_order_acq_rel);
  while (admissions_.load(std::memory_order_acquire) & ~kClosed) {
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out_ = true; return false;
    }
    std::this_thread::yield();
  }
  if (!stopping_.load()) {
    auto& c = frozen_.counts;
    frozen_.cutoff = RawQpc();
    c.attempted = attempted_.load(); c.accepted = accepted_.load();
    c.queue_drops = queue_drops_.load(); c.limit_drops = limit_drops_.load();
    c.contention_drops = contention_drops_.load(); c.excluded = excluded_.load();
    stopping_.store(true, std::memory_order_release); changed_.notify_all();
  }
  std::unique_lock<std::mutex> lock(wait_mutex_, std::defer_lock);
  while (!lock.try_lock()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out_ = true; return false;
    }
    std::this_thread::yield();
  }
  if (!changed_.wait_until(lock, deadline, [this] {
        return done_.load(std::memory_order_acquire);
      })) { timed_out_ = true; return false; }
  lock.unlock();
  if (writer_.joinable()) writer_.join();
  return flushed_ && !timed_out_;
}
Session::~Session() {
  // Test owners must release writer gates. Production retains sessions at exit.
  if (writer_.joinable()) {
    Close(std::chrono::seconds(2));
    if (writer_.joinable()) writer_.join();
  }
}
Summary Session::summary() const { return frozen_; }
uint64_t RawQpc() {
#if XE_PLATFORM_WIN32
  LARGE_INTEGER q;
  return QueryPerformanceCounter(&q) ? uint64_t(q.QuadPart) : 0;
#else
  return 0;
#endif
}
Session* Current() { return active.load(std::memory_order_acquire); }
bool StartFromFlag() {
  if (cvars::swap_entry_observer.empty()) return true;
  if (owned) return false;
#if XE_PLATFORM_WIN32
  LARGE_INTEGER f;
  if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return false;
  auto s = Session::Create(to_path(cvars::swap_entry_observer),
                           uint64_t(f.QuadPart), RawQpc());
  if (!s) return false;
  owned = s.release(); active.store(owned, std::memory_order_release);
  XELOGI("Swap entry observer: private XSWAP001 opened");
  return true;
#else
  return false;
#endif
}
bool CloseForExit(std::chrono::milliseconds timeout) {
  Session* s = active.exchange(nullptr, std::memory_order_acq_rel);
  if (!s) return true;
  bool complete = s->Close(timeout);
  if (s->timed_out()) {
    XELOGE("Swap entry close: complete=false drain_timeout=true");
    return false;
  }
  auto f = s->summary(); const auto& c = f.counts;
  XELOGI("Swap entry close: complete={} qpf={} start={} cutoff={} attempted={} "
         "accepted={} written={} queue_drops={} limit_drops={} contention_drops={} "
         "excluded_at_cutoff={} write_errors={}", complete, f.qpf, f.start,
         f.cutoff, c.attempted, c.accepted, c.written, c.queue_drops,
         c.limit_drops, c.contention_drops, c.excluded, c.write_errors);
  return complete;
}
}  // namespace xe::swap_entry
