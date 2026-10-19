// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic. SDK-derived ancestry stays in this fork; do not
// upstream this capture format or its observations.
#include "xenia/kernel/xam/paired_capture.h"

#include <algorithm>
#include <atomic>
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

DEFINE_string(paired_capture, "",
              "Private fork-only guest-consumed scalar input/NUI sidecar. "
              "Must name a fresh absolute file; empty disables capture.",
              "NUI");

namespace xe::kernel::xam::paired_capture {
namespace {
std::atomic<Session*> active{nullptr};
Session* owned_session = nullptr;  // UI thread owns startup and close.

void Put32(uint8_t* p, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
void Put64(uint8_t* p, uint64_t value) {
  for (uint32_t i = 0; i < 8; ++i) p[i] = uint8_t(value >> (8 * i));
}
uint32_t Get32(const uint8_t* p) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8 * i);
  return value;
}
uint64_t Get64(const uint8_t* p) {
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (8 * i);
  return value;
}
template <size_t N>
bool Write(FILE* file, const std::array<uint8_t, N>& bytes) {
  return fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
}

FILE* CreateNew(const std::filesystem::path& path) {
  if (!path.is_absolute()) return nullptr;
#if XE_PLATFORM_WIN32
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return nullptr;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                          _O_BINARY | _O_WRONLY);
  if (fd < 0) {
    CloseHandle(handle);
    return nullptr;
  }
  FILE* file = _fdopen(fd, "wb");
  if (!file) _close(fd);
  return file;
#else
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) return nullptr;
  FILE* file = fdopen(fd, "wb");
  if (!file) close(fd);
  return file;
#endif
}
}  // namespace

std::array<uint8_t, kHeaderBytes> EncodeHeader(uint64_t qpf,
                                                uint64_t start_qpc,
                                                uint32_t queue_limit,
                                                uint32_t record_limit) {
  std::array<uint8_t, kHeaderBytes> bytes{};
  std::memcpy(bytes.data(), "XPAIR001", 8);
  Put32(bytes.data() + 8, 1);
  Put32(bytes.data() + 12, kHeaderBytes);
  Put32(bytes.data() + 16, kRecordBytes);
  Put32(bytes.data() + 20, queue_limit);
  Put32(bytes.data() + 24, record_limit);
  Put64(bytes.data() + 32, qpf);
  Put64(bytes.data() + 40, start_qpc);
  return bytes;
}

std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& record) {
  std::array<uint8_t, kRecordBytes> bytes{};
  Put32(bytes.data(), uint32_t(record.kind));
  Put32(bytes.data() + 4, record.key);
  Put64(bytes.data() + 8, record.qpc);
  for (size_t i = 0; i < record.words.size(); ++i)
    Put32(bytes.data() + 16 + 4 * i, record.words[i]);
  return bytes;
}

std::array<uint8_t, kFooterBytes> EncodeFooter(const Counts& counts,
                                                uint64_t end_qpc) {
  std::array<uint8_t, kFooterBytes> bytes{};
  std::memcpy(bytes.data(), "XPAIREND", 8);
  Put64(bytes.data() + 8, counts.accepted);
  Put64(bytes.data() + 16, counts.written);
  Put64(bytes.data() + 24, counts.coalesced);
  Put64(bytes.data() + 32, counts.dropped_queue);
  Put64(bytes.data() + 40, counts.dropped_limit);
  Put64(bytes.data() + 48, counts.write_errors);
  Put64(bytes.data() + 56, end_qpc);
  return bytes;
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

bool ValidateFile(const std::filesystem::path& path, Counts* counts) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error || size < kHeaderBytes + kFooterBytes) return false;
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::array<uint8_t, kHeaderBytes> header{};
  std::array<uint8_t, kFooterBytes> footer{};
  file.read(reinterpret_cast<char*>(header.data()), header.size());
  if (!file) return false;
  file.seekg(static_cast<std::streamoff>(size - kFooterBytes));
  file.read(reinterpret_cast<char*>(footer.data()), footer.size());
  if (!file || std::memcmp(header.data(), "XPAIR001", 8) ||
      std::memcmp(footer.data(), "XPAIREND", 8) || Get32(header.data() + 8) != 1 ||
      Get32(header.data() + 12) != kHeaderBytes ||
      Get32(header.data() + 16) != kRecordBytes ||
      !Get32(header.data() + 20) ||
      Get32(header.data() + 20) > kHardQueueLimit ||
      !Get32(header.data() + 24) ||
      Get32(header.data() + 24) > kHardRecordLimit ||
      !Get64(header.data() + 32))
    return false;
  Counts found{Get64(footer.data() + 8), Get64(footer.data() + 16),
               Get64(footer.data() + 24), Get64(footer.data() + 32),
               Get64(footer.data() + 40), Get64(footer.data() + 48)};
  if (found.dropped_queue || found.dropped_limit || found.write_errors ||
      found.accepted != found.written ||
      found.written > Get32(header.data() + 24) ||
      size != kHeaderBytes + found.written * kRecordBytes + kFooterBytes)
    return false;
  if (counts) *counts = found;
  return true;
}

PendingRing::PendingRing(uint32_t capacity)
    : capacity_(std::min(capacity, kHardQueueLimit)) {}
bool PendingRing::Push(const Record& record) {
  if (size_ >= capacity_) return false;
  entries_[(head_ + size_) % capacity_] = record;
  ++size_;
  return true;
}
bool PendingRing::Pop(Record* record) {
  if (!size_ || !record) return false;
  *record = entries_[head_];
  head_ = (head_ + 1) % capacity_;
  --size_;
  return true;
}

Session::Session(FILE* file, uint64_t, Tick tick, Limits limits)
    : file_(file), tick_(std::move(tick)), limits_(limits), queue_(limits.queue) {}

std::unique_ptr<Session> Session::Create(const std::filesystem::path& path,
                                          uint64_t qpf, Tick tick,
                                          Limits limits) {
  if (!qpf || !tick || !limits.queue || !limits.records ||
      limits.queue > kHardQueueLimit || limits.records > kHardRecordLimit)
    return nullptr;
  FILE* file = CreateNew(path);
  if (!file) return nullptr;
  const auto header = EncodeHeader(qpf, tick(), limits.queue, limits.records);
  if (!Write(file, header) || fflush(file)) {
    fclose(file);  // Preserve the incomplete exclusive-create output.
    return nullptr;
  }
  std::unique_ptr<Session> session(new Session(file, qpf, std::move(tick), limits));
  try {
    session->writer_ = std::thread(&Session::WriterMain, session.get());
  } catch (...) {
    fclose(file);
    session->file_ = nullptr;
    return nullptr;
  }
  return session;
}

Session::~Session() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  pending_.notify_one();
  if (writer_.joinable()) writer_.join();
  // Create's thread-start failure already closed file_.
}

void Session::RecordEvent(Kind kind, uint32_t key, const Words& words,
                          bool coalesce) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  Words compared = words;
  if (kind == Kind::kInputState) {
    // Guest packet/flags churn alone is not a controller transition. The
    // emitted event still carries their exact values at that observation.
    compared[3] = 0;
    compared[5] = 0;
  }
  if (coalesce) {
    auto& last = last_[uint32_t(kind) * 5 + std::min(key, 4u)];
    if (last.valid && last.kind == kind && last.key == key &&
        last.words == compared) {
      ++counts_.coalesced;
      return;
    }
  }
  if (counts_.accepted >= limits_.records) {
    ++counts_.dropped_limit;
    return;
  }
  Record record{kind, key, tick_(), words};
  if (!queue_.Push(record)) {
    ++counts_.dropped_queue;
    return;
  }
  if (coalesce) last_[uint32_t(kind) * 5 + std::min(key, 4u)] =
                    {true, kind, key, compared};
  ++counts_.accepted;
  pending_.notify_one();
}

void Session::WriterMain() {
  bool writable = true;
  for (;;) {
    Record record;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      pending_.wait(lock, [this] { return stopping_ || queue_.size(); });
      if (!queue_.Pop(&record)) {
        if (stopping_) break;
        continue;
      }
    }
    if (writable && Write(file_, EncodeRecord(record))) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counts_.written;
    } else if (writable) {
      writable = false;
      std::lock_guard<std::mutex> lock(mutex_);
      ++counts_.write_errors;
    }
  }
  Counts final_counts;
  bool timed_out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    final_counts = counts_;
    timed_out = timed_out_;
  }
  // On a write failure a footer could falsely bless a truncated event stream.
  bool clean = writable && !timed_out &&
               Write(file_, EncodeFooter(final_counts, tick_())) &&
               fflush(file_) == 0;
  if (fclose(file_) != 0) clean = false;
  file_ = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clean) ++counts_.write_errors;
    clean_close_ = clean;
    done_ = true;
  }
  finished_.notify_all();
}

bool Session::Close(std::chrono::milliseconds timeout) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  pending_.notify_one();
  std::unique_lock<std::mutex> lock(mutex_);
  if (!finished_.wait_for(lock, timeout, [this] { return done_; })) {
    timed_out_ = true;
    return false;
  }
  const bool clean = clean_close_ && !counts_.dropped_queue &&
                     !counts_.dropped_limit && !counts_.write_errors &&
                     counts_.accepted == counts_.written;
  lock.unlock();
  if (writer_.joinable()) writer_.join();
  return clean;
}

Counts Session::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

uint64_t RawQpc() {
#if XE_PLATFORM_WIN32
  LARGE_INTEGER value;
  return QueryPerformanceCounter(&value) ? uint64_t(value.QuadPart) : 0;
#else
  return 0;
#endif
}

bool StartFromFlag() {
  if (cvars::paired_capture.empty()) return true;
  if (owned_session) return false;
#if XE_PLATFORM_WIN32
  LARGE_INTEGER frequency;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    return false;
  auto session = Session::Create(xe::to_path(cvars::paired_capture),
                                 uint64_t(frequency.QuadPart), RawQpc,
                                 Session::Limits{});
  if (!session) {
    XELOGE("Paired capture: exclusive create-new sidecar failed");
    return false;
  }
  owned_session = session.release();
  active.store(owned_session, std::memory_order_release);
  XELOGI("Paired capture: private scalar sidecar opened");
  return true;
#else
  XELOGE("Paired capture: raw QPC capture is Windows-only");
  return false;
#endif
}

Session* Active() { return active.load(std::memory_order_acquire); }

bool CloseForExit(std::chrono::milliseconds timeout) {
  Session* session = active.exchange(nullptr, std::memory_order_acq_rel);
  if (!session) return true;
  const bool clean = session->Close(timeout);
  const Counts counts = session->counts();
  XELOGI("Paired capture close: complete={} accepted={} written={} "
         "coalesced={} queue_drops={} limit_drops={} write_errors={}",
         clean, counts.accepted, counts.written, counts.coalesced,
         counts.dropped_queue, counts.dropped_limit, counts.write_errors);
  // The process quick_exits immediately. A timeout deliberately retains the
  // object and leaves a missing footer rather than waiting indefinitely.
  return clean;
}

}  // namespace xe::kernel::xam::paired_capture
