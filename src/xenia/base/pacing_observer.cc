// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private SDK-derived fork ancestry; never upstream.
#include "xenia/base/pacing_observer.h"
#include <cstring>
#include <fstream>
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
DEFINE_string(pacing_observer, "",
              "Private Windows vblank/NT-wait XPACE001 sidecar; fresh absolute "
              "path, empty disables.",
              "GPU");
namespace xe::pacing {
namespace {
constexpr uint32_t kClosed = 0x80000000u;
std::atomic<Session*> active{nullptr};
Session* owned = nullptr;
TimerMetadata timer_metadata{};  // Startup thread only, before publication.
void Put(uint8_t* p, uint64_t x, unsigned n = 8) {
  for (unsigned i = 0; i < n; ++i) {
    p[i] = uint8_t(x >> (8 * i));
  }
}
uint64_t Get(const uint8_t* p, unsigned n = 8) {
  uint64_t x = 0;
  for (unsigned i = 0; i < n; ++i) {
    x |= uint64_t(p[i]) << (8 * i);
  }
  return x;
}
template <size_t N>
bool Write(FILE* f, const std::array<uint8_t, N>& bytes) {
  return fwrite(bytes.data(), 1, N, f) == N;
}
FILE* CreateNew(const std::filesystem::path& path) {
  if (!path.is_absolute()) {
    return nullptr;
  }
#if XE_PLATFORM_WIN32
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  int fd =
      _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY | _O_WRONLY);
  if (fd < 0) {
    CloseHandle(h);
    return nullptr;
  }
  FILE* f = _fdopen(fd, "wb");
  if (!f) {
    _close(fd);
  }
#else
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) {
    return nullptr;
  }
  FILE* f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
  }
#endif
  return f;
}
bool ValidTimer(const TimerMetadata& t) {
  return !(t.flags & ~15u) && (!(t.flags & 2) || (t.flags & 1)) &&
         (!(t.flags & 8) || (t.flags & 4)) &&
         ((t.flags & 1) || !t.query_status) &&
         ((t.flags & 4) || !t.set_status) &&
         (bool(t.flags & 2) ==
          (bool(t.flags & 1) && int32_t(t.query_status) >= 0)) &&
         (bool(t.flags & 8) ==
          (bool(t.flags & 4) && int32_t(t.set_status) >= 0)) &&
         ((t.flags & 2) ||
          !(t.minimum | t.maximum | t.query_current | t.requested)) &&
         ((t.flags & 8) || !t.set_current);
}
bool ValidRecord(const Record& r, uint64_t start, uint64_t cutoff) {
  return r.sequence && r.native_tid && r.flags == 1 && !r.reserved &&
         r.mark >= start && r.mark <= r.callback_begin &&
         r.callback_begin <= r.callback_end && r.callback_end <= r.wait_begin &&
         r.wait_begin <= r.wait_end && r.wait_end <= cutoff &&
         r.nt_interval == NtInterval(r.requested_ns);
}
}  // namespace
std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf, uint64_t start,
                                               const TimerMetadata& t) {
  std::array<uint8_t, kHeaderBytes> b{};
  memcpy(b.data(), "XPACE001", 8);
  Put(b.data() + 8, 1, 4);
  Put(b.data() + 12, kHeaderBytes, 4);
  Put(b.data() + 16, kRecordBytes, 4);
  Put(b.data() + 20, kQueueLimit, 4);
  Put(b.data() + 24, kRecordLimit, 4);
  Put(b.data() + 32, qpf);
  Put(b.data() + 40, start);
  const uint32_t values[] = {t.flags,       t.query_status, t.set_status,
                             t.minimum,     t.maximum,      t.query_current,
                             t.set_current, t.requested};
  for (unsigned i = 0; i < 8; ++i) {
    Put(b.data() + 48 + i * 4, values[i], 4);
  }
  return b;
}
std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& r) {
  std::array<uint8_t, kRecordBytes> b{};
  const uint64_t values[] = {r.sequence,
                             r.mark,
                             r.callback_begin,
                             r.callback_end,
                             r.wait_begin,
                             r.wait_end,
                             uint64_t(r.requested_ns),
                             uint64_t(r.nt_interval)};
  for (unsigned i = 0; i < 8; ++i) {
    Put(b.data() + i * 8, values[i]);
  }
  Put(b.data() + 64, r.native_tid, 4);
  Put(b.data() + 68, r.nt_status, 4);
  Put(b.data() + 72, r.flags, 4);
  Put(b.data() + 76, r.reserved, 4);
  return b;
}
std::array<uint8_t, kFooterBytes> EncodeFooter(const Counts& c,
                                               uint64_t cutoff) {
  std::array<uint8_t, kFooterBytes> b{};
  memcpy(b.data(), "XPACEEND", 8);
  const uint64_t values[] = {c.started,          c.completed,   c.accepted,
                             c.written,          c.queue_drops, c.limit_drops,
                             c.contention_drops, c.excluded,    c.outstanding,
                             c.write_errors,     cutoff};
  for (unsigned i = 0; i < 11; ++i) {
    Put(b.data() + 8 + i * 8, values[i]);
  }
  return b;
}
bool ValidateFile(const std::filesystem::path& path, Summary* summary) {
  std::error_code ec;
  uint64_t size = std::filesystem::file_size(path, ec);
  if (ec || size < kHeaderBytes + kFooterBytes ||
      size >
          kHeaderBytes + uint64_t(kRecordLimit) * kRecordBytes + kFooterBytes ||
      (size - kHeaderBytes - kFooterBytes) % kRecordBytes) {
    return false;
  }
  std::ifstream f(path, std::ios::binary);
  std::array<uint8_t, kHeaderBytes> h{};
  std::array<uint8_t, kFooterBytes> e{};
  f.read(reinterpret_cast<char*>(h.data()), h.size());
  f.seekg(std::streamoff(size - kFooterBytes));
  f.read(reinterpret_cast<char*>(e.data()), e.size());
  Summary s{};
  s.qpf = Get(h.data() + 32);
  s.start = Get(h.data() + 40);
  auto& t = s.timer;
  t.flags = uint32_t(Get(h.data() + 48, 4));
  t.query_status = uint32_t(Get(h.data() + 52, 4));
  t.set_status = uint32_t(Get(h.data() + 56, 4));
  t.minimum = uint32_t(Get(h.data() + 60, 4));
  t.maximum = uint32_t(Get(h.data() + 64, 4));
  t.query_current = uint32_t(Get(h.data() + 68, 4));
  t.set_current = uint32_t(Get(h.data() + 72, 4));
  t.requested = uint32_t(Get(h.data() + 76, 4));
  if (!f || !s.qpf || !s.start || !ValidTimer(t) ||
      h != EncodeHeader(s.qpf, s.start, t) || memcmp(e.data(), "XPACEEND", 8)) {
    return false;
  }
  auto& c = s.counts;
  c.started = Get(e.data() + 8);
  c.completed = Get(e.data() + 16);
  c.accepted = Get(e.data() + 24);
  c.written = Get(e.data() + 32);
  c.queue_drops = Get(e.data() + 40);
  c.limit_drops = Get(e.data() + 48);
  c.contention_drops = Get(e.data() + 56);
  c.excluded = Get(e.data() + 64);
  c.outstanding = Get(e.data() + 72);
  c.write_errors = Get(e.data() + 80);
  s.cutoff = Get(e.data() + 88);
  if (s.cutoff < s.start || c.started > kRecordLimit ||
      c.completed > c.started || c.outstanding != c.started - c.completed ||
      c.accepted > c.completed || c.written != c.accepted ||
      c.queue_drops > c.completed - c.accepted ||
      c.completed - c.accepted - c.queue_drops > c.contention_drops ||
      c.write_errors ||
      c.written != (size - kHeaderBytes - kFooterBytes) / kRecordBytes) {
    return false;
  }
  f.seekg(kHeaderBytes);
  uint64_t previous = 0, previous_end = s.start;
  for (uint64_t i = 0; i < c.written; ++i) {
    std::array<uint8_t, kRecordBytes> b{};
    f.read(reinterpret_cast<char*>(b.data()), b.size());
    Record r{};
    r.sequence = Get(b.data());
    r.mark = Get(b.data() + 8);
    r.callback_begin = Get(b.data() + 16);
    r.callback_end = Get(b.data() + 24);
    r.wait_begin = Get(b.data() + 32);
    r.wait_end = Get(b.data() + 40);
    r.requested_ns = int64_t(Get(b.data() + 48));
    r.nt_interval = int64_t(Get(b.data() + 56));
    r.native_tid = uint32_t(Get(b.data() + 64, 4));
    r.nt_status = uint32_t(Get(b.data() + 68, 4));
    r.flags = uint32_t(Get(b.data() + 72, 4));
    r.reserved = uint32_t(Get(b.data() + 76, 4));
    if (!f || !ValidRecord(r, s.start, s.cutoff) || r.sequence <= previous ||
        r.sequence > c.completed || r.mark < previous_end ||
        ((!c.queue_drops && !c.contention_drops) &&
         r.sequence != previous + 1)) {
      return false;
    }
    previous = r.sequence;
    previous_end = r.wait_end;
  }
  if (summary) {
    *summary = s;
  }
  return true;
}
bool MatchesAcknowledgement(const Summary& f, const Summary& a, bool complete) {
  return complete && f.qpf == a.qpf && f.start == a.start &&
         f.cutoff == a.cutoff &&
         EncodeHeader(f.qpf, f.start, f.timer) ==
             EncodeHeader(a.qpf, a.start, a.timer) &&
         EncodeFooter(f.counts, f.cutoff) == EncodeFooter(a.counts, a.cutoff);
}
Session::Session(FILE* f, const Summary& s, std::function<void()> drain,
                 std::function<void()> footer)
    : file_(f),
      frozen_(s),
      before_drain_(std::move(drain)),
      before_footer_(std::move(footer)) {}
std::unique_ptr<Session> Session::Create(const std::filesystem::path& path,
                                         uint64_t qpf, uint64_t start,
                                         const TimerMetadata& timer,
                                         std::function<void()> drain,
                                         std::function<void()> footer) {
  if (!qpf || !start || !ValidTimer(timer)) {
    return nullptr;
  }
  FILE* f = CreateNew(path);
  if (!f) {
    return nullptr;
  }
  if (!Write(f, EncodeHeader(qpf, start, timer)) || fflush(f)) {
    fclose(f);
    return nullptr;
  }
  Summary s{};
  s.qpf = qpf;
  s.start = start;
  s.timer = timer;
  auto p = std::unique_ptr<Session>(
      new Session(f, s, std::move(drain), std::move(footer)));
  try {
    p->writer_ = std::thread(&Session::Writer, p.get());
  } catch (...) {
    fclose(f);
    p->file_ = nullptr;
    return nullptr;
  }
  return p;
}
bool Session::Enter() {
  uint32_t old = admissions_.fetch_add(1, std::memory_order_acq_rel);
  if (!(old & kClosed)) {
    return true;
  }
  Leave();
  return false;
}
void Session::Leave() { admissions_.fetch_sub(1, std::memory_order_acq_rel); }
uint64_t Session::Begin() {
  if (!Enter()) {
    return 0;
  }
  uint64_t seq = 0;
  if (producer_.test_and_set(std::memory_order_acquire)) {
    ++contention_drops_;
  } else {
    if (started_.load() < kRecordLimit) {
      seq = ++started_;
    } else {
      ++limit_drops_;
    }
    producer_.clear(std::memory_order_release);
  }
  Leave();
  return seq;
}
void Session::Complete(const Record& r) {
  if (!Enter()) {
    ++excluded_;
    return;
  }
  ++completed_;
  if (producer_.test_and_set(std::memory_order_acquire)) {
    ++contention_drops_;
  } else {
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == kQueueLimit) {
      ++queue_drops_;
    } else {
      ring_[tail % kQueueLimit] = r;
      tail_.store(tail + 1, std::memory_order_release);
      ++accepted_;
      changed_.notify_one();
    }
    producer_.clear(std::memory_order_release);
  }
  Leave();
}
void Session::Writer() {
  if (before_drain_) {
    before_drain_();
  }
  uint64_t written = 0, errors = 0;
  for (;;) {
    uint32_t head = head_.load(std::memory_order_relaxed);
    if (head != tail_.load(std::memory_order_acquire)) {
      Record r = ring_[head % kQueueLimit];
      head_.store(head + 1, std::memory_order_release);
      if (!errors) {
        if (Write(file_, EncodeRecord(r))) {
          ++written;
        } else {
          ++errors;
        }
      }
      continue;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      if (head_.load(std::memory_order_relaxed) ==
          tail_.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    std::unique_lock<std::mutex> lock(wait_mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(50), [this] {
      return stopping_.load(std::memory_order_acquire) ||
             head_.load() != tail_.load();
    });
  }
  if (before_footer_) {
    before_footer_();
  }
  frozen_.counts.written = written;
  frozen_.counts.write_errors = errors;
  bool good = !errors &&
              Write(file_, EncodeFooter(frozen_.counts, frozen_.cutoff)) &&
              fflush(file_) == 0;
  if (fclose(file_)) {
    good = false;
  }
  file_ = nullptr;
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    flushed_ = good;
    done_.store(true, std::memory_order_release);
  }
  changed_.notify_all();
}
bool Session::Close(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  admissions_.fetch_or(kClosed, std::memory_order_acq_rel);
  while (admissions_.load(std::memory_order_acquire) & ~kClosed) {
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out_ = true;
      return false;
    }
    std::this_thread::yield();
  }
  if (!stopping_.load()) {
    auto& c = frozen_.counts;
    frozen_.cutoff = RawQpc();
    c.started = started_.load();
    c.completed = completed_.load();
    c.accepted = accepted_.load();
    c.queue_drops = queue_drops_.load();
    c.limit_drops = limit_drops_.load();
    c.contention_drops = contention_drops_.load();
    c.excluded = excluded_.load();
    c.outstanding = c.started - c.completed;
    stopping_.store(true, std::memory_order_release);
    changed_.notify_all();
  }
  std::unique_lock<std::mutex> lock(wait_mutex_, std::defer_lock);
  while (!lock.try_lock()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out_ = true;
      return false;
    }
    std::this_thread::yield();
  }
  if (!changed_.wait_until(lock, deadline, [this] {
        return done_.load(std::memory_order_acquire);
      })) {
    timed_out_ = true;
    return false;
  }
  lock.unlock();
  if (writer_.joinable()) {
    writer_.join();
  }
  return flushed_ && !timed_out_;
}
Session::~Session() {
  // Production sessions are retained. Isolated tests must release writer gates
  // before destruction; destructor joins and is not the bounded exit protocol.
  if (writer_.joinable()) {
    Close(std::chrono::seconds(2));
    if (writer_.joinable()) {
      writer_.join();
    }
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
Tick BeginVblank() {
  Tick t;
  Session* s = active.load(std::memory_order_acquire);
  if (!s) {
    return t;
  }
  uint64_t seq = s->Begin();
  if (!seq) {
    return t;
  }
  t.session = s;
  t.record.sequence = seq;
  t.record.mark = RawQpc();
#if XE_PLATFORM_WIN32
  t.record.native_tid = GetCurrentThreadId();
#endif
  return t;
}
void CompleteVblank(Tick& t) {
  if (t.session) {
    t.session->Complete(t.record);
    t.session = nullptr;
  }
}
void SaveTimerMetadata(const TimerMetadata& t) { timer_metadata = t; }
bool StartFromFlag() {
  if (cvars::pacing_observer.empty()) {
    return true;
  }
  if (owned) {
    return false;
  }
#if XE_PLATFORM_WIN32
  LARGE_INTEGER f;
  if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
    return false;
  }
  auto s = Session::Create(to_path(cvars::pacing_observer),
                           uint64_t(f.QuadPart), RawQpc(), timer_metadata);
  if (!s) {
    return false;
  }
  owned = s.release();
  active.store(owned, std::memory_order_release);
  XELOGI("Pacing observer: private XPACE001 opened");
  return true;
#else
  return false;
#endif
}
bool CloseForExit(std::chrono::milliseconds timeout) {
  Session* s = active.exchange(nullptr, std::memory_order_acq_rel);
  if (!s) {
    return true;
  }
  bool complete = s->Close(timeout);
  if (s->timed_out()) {
    XELOGE("Pacing close: complete=false drain_timeout=true");
    return false;
  }
  auto f = s->summary();
  const auto& c = f.counts;
  XELOGI(
      "Pacing close: complete={} qpf={} start={} cutoff={} started={} "
      "completed={} accepted={} written={} queue_drops={} limit_drops={} "
      "contention_drops={} excluded_at_cutoff={} outstanding={} "
      "write_errors={} timer_flags={} query_status={} set_status={} minimum={} "
      "maximum={} query_current={} set_current={} requested={}",
      complete, f.qpf, f.start, f.cutoff, c.started, c.completed, c.accepted,
      c.written, c.queue_drops, c.limit_drops, c.contention_drops, c.excluded,
      c.outstanding, c.write_errors, f.timer.flags, f.timer.query_status,
      f.timer.set_status, f.timer.minimum, f.timer.maximum,
      f.timer.query_current, f.timer.set_current, f.timer.requested);
  return complete;
}
}  // namespace xe::pacing
