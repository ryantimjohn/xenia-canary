// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork-only timing diagnostic; never upstream SDK-derived ancestry.
#include "xenia/base/frame_timing.h"

#include <atomic>
#include <cstring>
#include <fstream>

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

DEFINE_string(frame_timing, "",
              "Private fork-only guest-swap and D3D12 host-present QPC sidecar. "
              "Must name a fresh absolute file; empty disables capture.",
              "GPU");

namespace xe::frame_timing {
namespace {
std::atomic<Session*> active{nullptr};
Session* owned_session = nullptr;  // Retained across quick_exit.
std::atomic<uint64_t> guest_sequence{0};
std::atomic<uint64_t> host_sequence{0};

void Put32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i != 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
void Put64(uint8_t* p, uint64_t value) {
  for (unsigned i = 0; i != 8; ++i) p[i] = uint8_t(value >> (8 * i));
}
uint32_t Get32(const uint8_t* p) {
  uint32_t value = 0;
  for (unsigned i = 0; i != 4; ++i) value |= uint32_t(p[i]) << (8 * i);
  return value;
}
uint64_t Get64(const uint8_t* p) {
  uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i) value |= uint64_t(p[i]) << (8 * i);
  return value;
}
template <size_t N>
bool Write(FILE* file, const std::array<uint8_t, N>& bytes) {
  return fwrite(bytes.data(), 1, N, file) == N;
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
                                               uint64_t start_qpc) {
  std::array<uint8_t, kHeaderBytes> bytes{};
  std::memcpy(bytes.data(), "XFRAM001", 8);
  Put32(bytes.data() + 8, 1);
  Put32(bytes.data() + 12, kHeaderBytes);
  Put32(bytes.data() + 16, kRecordBytes);
  Put32(bytes.data() + 20, kQueueLimit);
  Put32(bytes.data() + 24, kRecordLimit);
  Put64(bytes.data() + 32, qpf);
  Put64(bytes.data() + 40, start_qpc);
  return bytes;
}

std::array<uint8_t, kRecordBytes> EncodeRecord(const Record& record) {
  std::array<uint8_t, kRecordBytes> bytes{};
  Put32(bytes.data(), uint32_t(record.kind));
  Put32(bytes.data() + 4, record.code);
  Put64(bytes.data() + 8, record.sequence);
  Put64(bytes.data() + 16, record.begin_qpc);
  Put64(bytes.data() + 24, record.end_qpc);
  return bytes;
}

std::array<uint8_t, kFooterBytes> EncodeFooter(const Counts& counts,
                                               uint64_t cutoff_qpc) {
  std::array<uint8_t, kFooterBytes> bytes{};
  std::memcpy(bytes.data(), "XFRAME01", 8);
  Put64(bytes.data() + 8, counts.accepted);
  Put64(bytes.data() + 16, counts.written);
  Put64(bytes.data() + 24, counts.queue_drops);
  Put64(bytes.data() + 32, counts.limit_drops);
  Put64(bytes.data() + 40, counts.write_errors);
  Put64(bytes.data() + 48, cutoff_qpc);
  return bytes;
}

bool ValidateFileStructure(const std::filesystem::path& path,
                           FileSummary* summary) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error || size < kHeaderBytes + kFooterBytes ||
      size > kHeaderBytes + uint64_t(kRecordLimit) * kRecordBytes +
                 kFooterBytes)
    return false;
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::array<uint8_t, kHeaderBytes> header{};
  std::array<uint8_t, kFooterBytes> footer{};
  file.read(reinterpret_cast<char*>(header.data()), header.size());
  if (!file) return false;
  file.seekg(static_cast<std::streamoff>(size - kFooterBytes));
  file.read(reinterpret_cast<char*>(footer.data()), footer.size());
  if (!file || std::memcmp(header.data(), "XFRAM001", 8) ||
      std::memcmp(footer.data(), "XFRAME01", 8) ||
      Get32(header.data() + 8) != 1 ||
      Get32(header.data() + 12) != kHeaderBytes ||
      Get32(header.data() + 16) != kRecordBytes ||
      Get32(header.data() + 20) != kQueueLimit ||
      Get32(header.data() + 24) != kRecordLimit ||
      !Get64(header.data() + 32))
    return false;
  const uint64_t start_qpc = Get64(header.data() + 40);
  const uint64_t cutoff_qpc = Get64(footer.data() + 48);
  Counts counts{Get64(footer.data() + 8), Get64(footer.data() + 16),
                Get64(footer.data() + 24), Get64(footer.data() + 32),
                Get64(footer.data() + 40)};
  if (!start_qpc || cutoff_qpc < start_qpc || counts.queue_drops ||
      counts.limit_drops || counts.write_errors ||
      counts.accepted != counts.written ||
      counts.written > kRecordLimit ||
      size != kHeaderBytes + counts.written * kRecordBytes + kFooterBytes)
    return false;
  file.clear();
  file.seekg(kHeaderBytes);
  for (uint64_t i = 0; i < counts.written; ++i) {
    std::array<uint8_t, kRecordBytes> record{};
    file.read(reinterpret_cast<char*>(record.data()), record.size());
    const uint32_t kind = Get32(record.data());
    const uint64_t begin = Get64(record.data() + 16);
    const uint64_t end = Get64(record.data() + 24);
    if (!file || (kind != uint32_t(Kind::kGuestSwap) &&
                  kind != uint32_t(Kind::kD3D12Present)) ||
        !Get64(record.data() + 8) || begin < start_qpc ||
        end < begin || end > cutoff_qpc ||
        (kind == uint32_t(Kind::kGuestSwap) &&
         (Get32(record.data() + 4) || begin != end)))
      return false;
  }
  if (summary)
    *summary = {Get64(header.data() + 32), start_qpc, cutoff_qpc, counts};
  return true;
}

bool MatchesCloseAcknowledgement(const FileSummary& file,
                                 const CloseAcknowledgement& acknowledgement) {
  const Counts& a = file.counts;
  const Counts& b = acknowledgement.counts;
  return acknowledgement.complete &&
         file.start_qpc == acknowledgement.start_qpc &&
         file.cutoff_qpc == acknowledgement.cutoff_qpc &&
         a.accepted == b.accepted && a.written == b.written &&
         a.queue_drops == b.queue_drops &&
         a.limit_drops == b.limit_drops &&
         a.write_errors == b.write_errors;
}

Session::Session(FILE* file, uint64_t start_qpc)
    : file_(file), start_qpc_(start_qpc) {}

std::unique_ptr<Session> Session::Create(const std::filesystem::path& path,
                                         uint64_t qpf, uint64_t start_qpc) {
  if (!qpf || !start_qpc) return nullptr;
  FILE* file = CreateNew(path);
  if (!file) return nullptr;
  if (!Write(file, EncodeHeader(qpf, start_qpc)) || fflush(file)) {
    fclose(file);  // Keep the incomplete, exclusively created file.
    return nullptr;
  }
  std::unique_ptr<Session> session(new Session(file, start_qpc));
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
    if (!stopping_) {
      cutoff_qpc_ = RawQpc();
      stopping_ = true;
    }
  }
  pending_.notify_one();
  if (writer_.joinable()) writer_.join();
}

void Session::RecordEvent(const Record& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  if (counts_.accepted >= kRecordLimit) {
    ++counts_.limit_drops;
    return;
  }
  if (size_ == kQueueLimit) {
    ++counts_.queue_drops;
    return;
  }
  ring_[(head_ + size_) % kQueueLimit] = record;
  ++size_;
  ++counts_.accepted;
  pending_.notify_one();
}

void Session::WriterMain() {
  bool writable = true;
  for (;;) {
    Record record{};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      pending_.wait(lock, [this] { return stopping_ || size_; });
      if (!size_) break;
      record = ring_[head_];
      head_ = (head_ + 1) % kQueueLimit;
      --size_;
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
  uint64_t cutoff;
  bool timed_out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    final_counts = counts_;
    cutoff = cutoff_qpc_;
    timed_out = timed_out_;
  }
  bool clean = writable && !timed_out &&
               Write(file_, EncodeFooter(final_counts, cutoff)) &&
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
    if (!stopping_) {
      cutoff_qpc_ = RawQpc();
      stopping_ = true;
    }
  }
  pending_.notify_one();
  std::unique_lock<std::mutex> lock(mutex_);
  if (!finished_.wait_for(lock, timeout, [this] { return done_; })) {
    timed_out_ = true;
    return false;
  }
  const bool clean = clean_close_ && !timed_out_ &&
                     !counts_.queue_drops && !counts_.limit_drops &&
                     !counts_.write_errors &&
                     counts_.accepted == counts_.written;
  lock.unlock();
  if (writer_.joinable()) writer_.join();
  return clean;
}

Counts Session::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}
uint64_t Session::cutoff_qpc() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cutoff_qpc_;
}

uint64_t RawQpc() {
#if XE_PLATFORM_WIN32
  LARGE_INTEGER counter;
  return QueryPerformanceCounter(&counter) ? uint64_t(counter.QuadPart) : 0;
#else
  return 0;
#endif
}

bool StartFromFlag() {
  if (cvars::frame_timing.empty()) return true;
  if (owned_session) return false;
#if XE_PLATFORM_WIN32
  LARGE_INTEGER frequency;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    return false;
  auto session = Session::Create(xe::to_path(cvars::frame_timing),
                                 uint64_t(frequency.QuadPart), RawQpc());
  if (!session) {
    XELOGE("Frame timing: exclusive create-new sidecar failed");
    return false;
  }
  owned_session = session.release();
  active.store(owned_session, std::memory_order_release);
  XELOGI("Frame timing: private QPC sidecar opened");
  return true;
#else
  XELOGE("Frame timing: raw QPC capture is Windows-only");
  return false;
#endif
}

bool CloseForExit(std::chrono::milliseconds timeout) {
  Session* session = active.load(std::memory_order_acquire);
  if (!session) return true;
  const bool clean = session->Close(timeout);
  active.store(nullptr, std::memory_order_release);
  const Counts counts = session->counts();
  XELOGI("Frame timing close: start_qpc={} cutoff_qpc={} complete={} "
         "accepted={} written={} queue_drops={} limit_drops={} write_errors={}",
         session->start_qpc(), session->cutoff_qpc(), clean, counts.accepted,
         counts.written, counts.queue_drops, counts.limit_drops,
         counts.write_errors);
  return clean;
}

void GuestSwap() {
  Session* session = active.load(std::memory_order_acquire);
  if (!session) return;
  const uint64_t qpc = RawQpc();
  session->RecordEvent({Kind::kGuestSwap, 0,
                        guest_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
                        qpc, qpc});
}

uint64_t BeginD3D12Present() {
  return active.load(std::memory_order_acquire) ? RawQpc() : 0;
}

void EndD3D12Present(uint64_t begin_qpc, uint32_t hresult) {
  if (!begin_qpc) return;
  Session* session = active.load(std::memory_order_acquire);
  if (!session) return;
  session->RecordEvent({Kind::kD3D12Present, hresult,
                        host_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
                        begin_qpc, RawQpc()});
}

}  // namespace xe::frame_timing
