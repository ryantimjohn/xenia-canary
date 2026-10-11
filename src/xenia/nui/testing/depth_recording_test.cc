/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/depth_recording.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/nui/depth_frame.h"
#include "xenia/nui/nui_flags.h"
#include "xenia/nui/nui_system.h"
#include "xenia/nui/replay/replay_nui_system.h"

#include "third_party/catch/include/catch.hpp"

DECLARE_int32(log_level);

namespace xe {
namespace nui {
namespace test {

namespace {

using namespace std::chrono_literals;

// How many samples one 640x480 depth frame holds, and how many bytes one frame
// takes up in a recording.
constexpr size_t kSampleCount =
    static_cast<size_t>(DepthFrame::kWidth) * DepthFrame::kHeight;
constexpr size_t kRecordedFrameBytes =
    sizeof(DepthRecordingFrameHeader) + kSampleCount * sizeof(uint16_t);

// How many frames the recording made by MakeRecording() holds.
constexpr uint64_t kRecordedFrameCount = 3;
// How far apart in time those frames are published, so that the recorded
// timestamps carry a pace a replay can be watched keeping to.
constexpr std::chrono::milliseconds kRecordedFrameInterval{60};

// A concrete NuiSystem with no backend behind it, so that the recording the
// base class does on every publish can be driven straight from a test.
// PublishFrame is protected on the base class; the using-declaration below
// re-exposes it to this file only.
class RecordingNuiSystem : public NuiSystem {
 public:
  std::string name() const override { return "Test"; }

  bool GetTiltDegrees(int32_t* out_degrees) override { return false; }
  bool SetTiltDegrees(int32_t degrees) override { return false; }

  using NuiSystem::PublishFrame;
};

// A file in the host's temporary directory that is taken away again when the
// test that asked for it ends.
class ScopedTempFile {
 public:
  explicit ScopedTempFile(const std::string& file_name) {
    std::error_code error_code;
    path_ = std::filesystem::temp_directory_path(error_code) / file_name;
    std::filesystem::remove(path_, error_code);
  }

  ~ScopedTempFile() {
    std::error_code error_code;
    std::filesystem::remove(path_, error_code);
  }

  const std::filesystem::path& path() const { return path_; }
  std::string utf8_path() const { return xe::path_to_utf8(path_); }

 private:
  std::filesystem::path path_;
};

// The recorder and the replay announce themselves in the log, and two of the
// tests below hand the replay files it is meant to complain about. None of
// that belongs in the test suite's output.
class ScopedSilentLog {
 public:
  ScopedSilentLog() : saved_log_level_(cvars::log_level) {
    cvars::log_level = static_cast<int32_t>(LogLevel::Disabled);
  }

  ~ScopedSilentLog() { cvars::log_level = saved_log_level_; }

 private:
  int32_t saved_log_level_;
};

// Puts the two recording cvars back the way the test found them.
class ScopedRecordingCvars {
 public:
  ScopedRecordingCvars()
      : record_(cvars::nui_record), replay_(cvars::nui_replay) {}

  ~ScopedRecordingCvars() {
    cvars::nui_record = record_;
    cvars::nui_replay = replay_;
  }

 private:
  std::string record_;
  std::string replay_;
};

// A frame whose samples differ from those of every other seed, so that a frame
// read back out of a recording can be told apart from its neighbours.
std::vector<uint16_t> MakePattern(uint16_t seed) {
  std::vector<uint16_t> samples(kSampleCount);
  for (size_t i = 0; i < kSampleCount; ++i) {
    samples[i] = static_cast<uint16_t>(i * 3 + seed * 7 + 1);
  }
  return samples;
}

std::shared_ptr<DepthFrame> MakeFrame(const std::vector<uint16_t>& samples) {
  auto frame = std::make_shared<DepthFrame>();
  frame->depth_mm = samples;
  return frame;
}

// What one frame was published as, so that the bytes on disk can be held
// against it.
struct PublishedFrame {
  uint64_t sequence;
  uint64_t timestamp_us;
  std::vector<uint16_t> samples;
};

// Publishes kRecordedFrameCount frames through a NuiSystem, each with its own
// pattern, and returns what was published. The caller points cvars::nui_record
// at the file they are to be recorded to first.
std::vector<PublishedFrame> MakeRecording() {
  std::vector<PublishedFrame> published;
  RecordingNuiSystem system;
  for (uint64_t i = 0; i < kRecordedFrameCount; ++i) {
    if (i != 0) {
      std::this_thread::sleep_for(kRecordedFrameInterval);
    }
    PublishedFrame published_frame = {};
    published_frame.samples = MakePattern(static_cast<uint16_t>(i));
    system.PublishFrame(MakeFrame(published_frame.samples));

    const auto latest = system.AcquireLatestDepthFrame();
    REQUIRE(latest != nullptr);
    published_frame.sequence = latest->sequence;
    published_frame.timestamp_us = latest->timestamp_us;
    published.push_back(std::move(published_frame));
  }
  // Shutdown() is what flushes and closes the recording.
  system.Shutdown();
  return published;
}

// Reads a whole file, or an empty vector when it cannot be read.
std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return bytes;
  }
  if (xe::filesystem::Seek(file, 0, SEEK_END)) {
    const int64_t size = xe::filesystem::Tell(file);
    if (size > 0 && xe::filesystem::Seek(file, 0, SEEK_SET)) {
      bytes.resize(static_cast<size_t>(size));
      if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        bytes.clear();
      }
    }
  }
  fclose(file);
  return bytes;
}

// Writes a file whole, returning false when it could not be written.
bool WriteWholeFile(const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes) {
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    return false;
  }
  const bool written =
      fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  fclose(file);
  return written;
}

// Adds bytes to the end of a file, returning false when they could not all be
// written.
bool AppendToFile(const std::filesystem::path& path,
                  const std::vector<uint8_t>& bytes) {
  FILE* file = xe::filesystem::OpenFile(path, "ab");
  if (!file) {
    return false;
  }
  const bool written =
      fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  fclose(file);
  return written;
}

// Reads frames off a running replay until the recording has come round at
// least once. The frame published as sequence n is always the recording's
// frame (n - 1) % kRecordedFrameCount, which is order and looping in one
// invariant and stays true even when the scheduler drops a frame between the
// wait and the read.
void CheckReplayLoops(NuiSystem* system,
                      const std::vector<PublishedFrame>& published) {
  uint64_t sequence = 0;
  int reads = 0;
  while (sequence <= kRecordedFrameCount && reads < 20) {
    REQUIRE(system->WaitForFrame(sequence, 5000ms));
    const auto frame = system->AcquireLatestDepthFrame();
    REQUIRE(frame != nullptr);
    sequence = frame->sequence;
    INFO("replayed frame " << sequence);
    CHECK(frame->depth_mm ==
          published[(sequence - 1) % kRecordedFrameCount].samples);
    ++reads;
  }
  CHECK(sequence > kRecordedFrameCount);
}

// The bytes of a recording holding frame_count frames of nothing but a header,
// with the given magic in front of it.
std::vector<uint8_t> MakeRecordingBytes(const char magic[8],
                                        size_t frame_count) {
  std::vector<uint8_t> bytes(sizeof(DepthRecordingHeader) +
                             frame_count * kRecordedFrameBytes);
  DepthRecordingHeader header = {};
  std::memcpy(header.magic, magic, sizeof(header.magic));
  header.width = DepthFrame::kWidth;
  header.height = DepthFrame::kHeight;
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

// Which frame of a recording these samples are, or the number of frames in it
// when they are none of them. Held against each other as indices, two frames
// that should have matched print as two numbers rather than as two lists of
// 307,200 samples.
size_t FrameIndexOf(const std::vector<uint16_t>& samples,
                    const std::vector<PublishedFrame>& published) {
  for (size_t i = 0; i < published.size(); ++i) {
    if (published[i].samples == samples) {
      return i;
    }
  }
  return published.size();
}

// Writes a recording of kRecordedFrameCount frames, each holding the pattern
// MakePattern() gives out for it, whose recorded timestamps sit interval_us
// apart. Unlike MakeRecording() this lets a test choose how long the replay
// waits between two frames. Returns what was written, in the same shape.
std::vector<PublishedFrame> WritePacedRecording(
    const std::filesystem::path& path, uint64_t interval_us) {
  std::vector<PublishedFrame> published;
  std::vector<uint8_t> bytes = MakeRecordingBytes(kDepthRecordingMagic, 0);
  for (uint64_t i = 0; i < kRecordedFrameCount; ++i) {
    PublishedFrame published_frame = {};
    published_frame.sequence = i + 1;
    published_frame.timestamp_us = i * interval_us;
    published_frame.samples = MakePattern(static_cast<uint16_t>(i));

    DepthRecordingFrameHeader header = {};
    header.sequence = published_frame.sequence;
    header.timestamp_us = published_frame.timestamp_us;
    const auto* header_bytes = reinterpret_cast<const uint8_t*>(&header);
    bytes.insert(bytes.end(), header_bytes, header_bytes + sizeof(header));
    const auto* sample_bytes =
        reinterpret_cast<const uint8_t*>(published_frame.samples.data());
    bytes.insert(
        bytes.end(), sample_bytes,
        sample_bytes + published_frame.samples.size() * sizeof(uint16_t));
    published.push_back(std::move(published_frame));
  }
  REQUIRE(WriteWholeFile(path, bytes));
  return published;
}

}  // namespace

TEST_CASE("NuiSystem records every published depth frame",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;
  ScopedTempFile recording("xenia-nui-record-test.xdepth");
  cvars::nui_record = recording.utf8_path();

  const std::vector<PublishedFrame> published = MakeRecording();
  REQUIRE(published.size() == kRecordedFrameCount);

  const std::vector<uint8_t> bytes = ReadWholeFile(recording.path());
  REQUIRE(bytes.size() == sizeof(DepthRecordingHeader) +
                              kRecordedFrameCount * kRecordedFrameBytes);

  DepthRecordingHeader header = {};
  std::memcpy(&header, bytes.data(), sizeof(header));
  CHECK(std::memcmp(header.magic, kDepthRecordingMagic, sizeof(header.magic)) ==
        0);
  CHECK(header.width == DepthFrame::kWidth);
  CHECK(header.height == DepthFrame::kHeight);
  CHECK(header.reserved[0] == 0);
  CHECK(header.reserved[1] == 0);

  for (size_t i = 0; i < published.size(); ++i) {
    INFO("recorded frame " << i);
    const size_t offset =
        sizeof(DepthRecordingHeader) + i * kRecordedFrameBytes;
    DepthRecordingFrameHeader frame_header = {};
    std::memcpy(&frame_header, bytes.data() + offset, sizeof(frame_header));
    CHECK(frame_header.sequence == published[i].sequence);
    CHECK(frame_header.timestamp_us == published[i].timestamp_us);
    CHECK(std::memcmp(bytes.data() + offset + sizeof(frame_header),
                      published[i].samples.data(),
                      kSampleCount * sizeof(uint16_t)) == 0);
  }
}

TEST_CASE("A frame published after Shutdown cannot reopen the recording",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;
  ScopedTempFile recording("xenia-nui-late-publish-test.xdepth");
  cvars::nui_record = recording.utf8_path();

  RecordingNuiSystem system;
  system.PublishFrame(MakeFrame(MakePattern(0)));
  system.PublishFrame(MakeFrame(MakePattern(1)));
  system.Shutdown();
  const size_t finished_size = ReadWholeFile(recording.path()).size();
  REQUIRE(finished_size ==
          sizeof(DepthRecordingHeader) + 2 * kRecordedFrameBytes);

  // A Kinect pump that was abandoned inside the sensor's driver publishes the
  // frame it was holding once that call finally returns, long after Shutdown()
  // closed the recording. Reopening the file for it would truncate a finished
  // recording of gigabytes down to a single frame.
  system.PublishFrame(MakeFrame(MakePattern(2)));

  const std::vector<uint8_t> bytes = ReadWholeFile(recording.path());
  REQUIRE(bytes.size() == finished_size);
  const std::vector<uint16_t> first_pattern = MakePattern(0);
  CHECK(std::memcmp(bytes.data() + sizeof(DepthRecordingHeader) +
                        sizeof(DepthRecordingFrameHeader),
                    first_pattern.data(),
                    kSampleCount * sizeof(uint16_t)) == 0);
}

TEST_CASE("ReplayNuiSystem publishes a recording in order and loops",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;
  ScopedTempFile recording("xenia-nui-replay-test.xdepth");
  cvars::nui_record = recording.utf8_path();
  const std::vector<PublishedFrame> published = MakeRecording();
  REQUIRE(published.size() == kRecordedFrameCount);

  // Stop recording before replaying, or the replay would record over the very
  // file it is reading from.
  cvars::nui_record.clear();
  cvars::nui_replay = recording.utf8_path();
  CHECK(replay::ReplayNuiSystem::IsAvailable());

  replay::ReplayNuiSystem system;
  REQUIRE(system.Setup(nullptr) == X_STATUS_SUCCESS);

  const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
  while (system.device_status() != NuiSystem::DeviceStatus::kReady &&
         std::chrono::steady_clock::now() < ready_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(system.device_status() == NuiSystem::DeviceStatus::kReady);

  CheckReplayLoops(&system, published);

  // A title's tilt request has no motor to reach, but it has to look as if it
  // took effect all the same.
  CHECK(system.SetTiltDegrees(19));
  int32_t tilt_degrees = 0;
  CHECK(system.GetTiltDegrees(&tilt_degrees));
  CHECK(tilt_degrees == 19);
  CHECK(system.SetTiltDegrees(100));
  CHECK(system.GetTiltDegrees(&tilt_degrees));
  CHECK(tilt_degrees == 27);
  // Nor does a recording carry an accelerometer, so the tilt status block
  // keeps being made from the angle the title asked for.
  float gravity[3] = {};
  CHECK_FALSE(system.GetGravityVector(gravity));
  // And with no motor, nothing is ever on its way anywhere.
  CHECK_FALSE(system.IsTiltMotorMoving());

  const auto shutdown_start = std::chrono::steady_clock::now();
  system.Shutdown();
  CHECK(std::chrono::steady_clock::now() - shutdown_start < 2000ms);
}

TEST_CASE("ReplayNuiSystem restarts its recording when the guest stream begins",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;
  ScopedTempFile recording("xenia-nui-restart-test.xdepth");
  // A fifth of a second between frames, the longest gap the replay plays back
  // as a gap. It is what makes this test's timing comfortable: the worker
  // reads a frame and then waits that long before publishing it, so the only
  // frame of the old position that can still reach the test after the hook is
  // the one frame the worker was already holding.
  const std::vector<PublishedFrame> published =
      WritePacedRecording(recording.path(), 200000);
  REQUIRE(published.size() == kRecordedFrameCount);

  cvars::nui_record.clear();
  cvars::nui_replay = recording.utf8_path();
  replay::ReplayNuiSystem system;
  REQUIRE(system.Setup(nullptr) == X_STATUS_SUCCESS);

  // The replay starts at the recording's first frame; the loop test above
  // checks the rest of the order.
  uint64_t sequence = 0;
  REQUIRE(system.WaitForFrame(sequence, 5000ms));
  auto frame = system.AcquireLatestDepthFrame();
  REQUIRE(frame != nullptr);
  sequence = frame->sequence;
  REQUIRE(FrameIndexOf(frame->depth_mm, published) == 0);

  // The title has started consuming frames, so the recording has to start
  // over rather than let the title join it wherever it had reached.
  system.OnGuestStreamStarted();

  struct ReplayedFrame {
    uint64_t sequence;
    std::vector<uint16_t> samples;
  };
  std::vector<ReplayedFrame> replayed;
  for (int reads = 0; reads < 4 && replayed.size() < 3; ++reads) {
    REQUIRE(system.WaitForFrame(sequence, 5000ms));
    frame = system.AcquireLatestDepthFrame();
    REQUIRE(frame != nullptr);
    sequence = frame->sequence;
    replayed.push_back({sequence, frame->depth_mm});
  }
  REQUIRE(replayed.size() == 3);

  // The worker had already read the recording's second frame when the hook
  // was called and publishes that one before it looks at the flag, so the
  // restart shows up either in the first frame read here or in the second.
  // Three frames of the old position would mean the file merely looped.
  const size_t restart =
      FrameIndexOf(replayed[0].samples, published) == 0 ? 0 : 1;
  INFO("the restart was expected at read " << restart);
  CHECK(FrameIndexOf(replayed[restart].samples, published) == 0);
  CHECK(FrameIndexOf(replayed[restart + 1].samples, published) == 1);

  // A restart that is still pending must not keep the worker from stopping.
  system.OnGuestStreamStarted();
  const auto shutdown_start = std::chrono::steady_clock::now();
  system.Shutdown();
  CHECK(std::chrono::steady_clock::now() - shutdown_start < 2000ms);
}

TEST_CASE("ReplayNuiSystem plays a recording that ends mid-frame",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;
  ScopedTempFile recording("xenia-nui-cut-short-test.xdepth");
  cvars::nui_record = recording.utf8_path();
  const std::vector<PublishedFrame> published = MakeRecording();
  REQUIRE(published.size() == kRecordedFrameCount);
  cvars::nui_record.clear();

  // A recording run ends by killing the emulator outright, so a real recording
  // stops in the middle of whatever frame was being written. Those bytes are
  // not a frame and must not be played as one.
  REQUIRE(AppendToFile(
      recording.path(),
      std::vector<uint8_t>(sizeof(DepthRecordingFrameHeader), 0xCD)));

  cvars::nui_replay = recording.utf8_path();
  replay::ReplayNuiSystem system;
  REQUIRE(system.Setup(nullptr) == X_STATUS_SUCCESS);
  CheckReplayLoops(&system, published);
  system.Shutdown();
}

TEST_CASE("ReplayNuiSystem refuses to record over what it is replaying",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;
  ScopedTempFile recording("xenia-nui-self-record-test.xdepth");
  cvars::nui_record = recording.utf8_path();
  const std::vector<PublishedFrame> published = MakeRecording();
  REQUIRE(published.size() == kRecordedFrameCount);
  const size_t recorded_size = ReadWholeFile(recording.path()).size();
  REQUIRE(recorded_size == sizeof(DepthRecordingHeader) +
                               kRecordedFrameCount * kRecordedFrameBytes);

  // Both flags naming one file is an easy copy-paste out of the two documented
  // command lines, and it would have the recorder truncate the very file the
  // replay is reading.
  cvars::nui_replay = recording.utf8_path();
  {
    replay::ReplayNuiSystem system;
    CHECK(system.Setup(nullptr) == X_STATUS_UNSUCCESSFUL);
    system.Shutdown();
  }
  CHECK(ReadWholeFile(recording.path()).size() == recorded_size);

  // The same file spelled another way is still the same file.
  cvars::nui_record = xe::path_to_utf8(recording.path().parent_path() / "." /
                                       recording.path().filename());
  {
    replay::ReplayNuiSystem system;
    CHECK(system.Setup(nullptr) == X_STATUS_UNSUCCESSFUL);
    system.Shutdown();
  }
  CHECK(ReadWholeFile(recording.path()).size() == recorded_size);
}

TEST_CASE("ReplayNuiSystem is unavailable without a recording",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;

  cvars::nui_replay = "";
  CHECK_FALSE(replay::ReplayNuiSystem::IsAvailable());

  ScopedTempFile missing("xenia-nui-missing-test.xdepth");
  cvars::nui_replay = missing.utf8_path();
  CHECK_FALSE(replay::ReplayNuiSystem::IsAvailable());
}

TEST_CASE("ReplayNuiSystem refuses a file it cannot play",
          "[nui][depth_recording]") {
  ScopedSilentLog silent_log;
  ScopedRecordingCvars scoped_cvars;

  ScopedTempFile bad_magic("xenia-nui-bad-magic-test.xdepth");
  REQUIRE(WriteWholeFile(bad_magic.path(), MakeRecordingBytes("NOTADEPT", 1)));
  cvars::nui_replay = bad_magic.utf8_path();
  CHECK(replay::ReplayNuiSystem::IsAvailable());
  {
    replay::ReplayNuiSystem system;
    CHECK(system.Setup(nullptr) == X_STATUS_UNSUCCESSFUL);
    system.Shutdown();
  }

  // A recording an emulator was killed in the middle of the first frame of has
  // nothing whole to play.
  ScopedTempFile no_frames("xenia-nui-no-frames-test.xdepth");
  std::vector<uint8_t> bytes = MakeRecordingBytes(kDepthRecordingMagic, 0);
  bytes.resize(sizeof(DepthRecordingHeader) + sizeof(DepthRecordingFrameHeader),
               0);
  REQUIRE(WriteWholeFile(no_frames.path(), bytes));
  cvars::nui_replay = no_frames.utf8_path();
  {
    replay::ReplayNuiSystem system;
    CHECK(system.Setup(nullptr) == X_STATUS_UNSUCCESSFUL);
    system.Shutdown();
  }
}

}  // namespace test
}  // namespace nui
}  // namespace xe
