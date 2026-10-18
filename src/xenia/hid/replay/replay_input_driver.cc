// Copyright 2026 Xenia Canary. Released under the BSD license - see LICENSE.
#include "xenia/hid/replay/replay_input_driver.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include "xenia/base/logging.h"

namespace xe::hid::replay {
namespace {
constexpr size_t kHeaderBytes = 24, kEventBytes = 32, kMaxEvents = 1000000;
constexpr uint64_t kMaxDurationUs = 86400000000ull;
constexpr uint8_t kMagic[] = {'X', 'E', 'C', 'T', 'R', 'L', '1', 0};
uint64_t ReadLE(const uint8_t* p, size_t n) {
  uint64_t result = 0;
  for (size_t i = 0; i < n; ++i) {
    result |= uint64_t(p[i]) << (8 * i);
  }
  return result;
}
// Avoid implementation-defined conversion of an out-of-range unsigned value.
int16_t ReadS16(const uint8_t* p) {
  int32_t v = int32_t(ReadLE(p, 2));
  return int16_t(v < 32768 ? v : v - 65536);
}
}  // namespace

bool ParseRecording(const std::vector<uint8_t>& bytes, Recording* out,
                    std::string* error) {
  auto fail = [&](const char* reason) {
    *error = reason;
    return false;
  };
  if (bytes.size() < kHeaderBytes ||
      bytes.size() > kHeaderBytes + kMaxEvents * kEventBytes)
    return fail("invalid recording size");
  if (!std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin()))
    return fail("invalid magic/version");
  Recording parsed;
  parsed.duration_us = ReadLE(bytes.data() + 8, 8);
  uint64_t count = ReadLE(bytes.data() + 16, 4);
  if (!parsed.duration_us || parsed.duration_us > kMaxDurationUs || !count ||
      count > kMaxEvents || ReadLE(bytes.data() + 20, 4) ||
      bytes.size() != kHeaderBytes + count * kEventBytes)
    return fail("invalid duration, count, reserved field or truncation");
  parsed.events.reserve(size_t(count));
  uint64_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = bytes.data() + kHeaderBytes + i * kEventBytes;
    Event e;
    e.time_us = ReadLE(p, 8);
    e.port = uint32_t(ReadLE(p + 8, 4));
    e.status = X_RESULT(ReadLE(p + 12, 4));
    if ((i == 0 && e.time_us != 0) || e.time_us < previous ||
        e.time_us >= parsed.duration_us || e.port >= 4 ||
        (e.status != X_ERROR_SUCCESS &&
         e.status != X_ERROR_DEVICE_NOT_CONNECTED))
      return fail("invalid event timestamp, port or status");
    e.state.packet_number = uint32_t(ReadLE(p + 16, 4));
    e.state.gamepad.buttons = uint16_t(ReadLE(p + 20, 2));
    e.state.gamepad.left_trigger = p[22];
    e.state.gamepad.right_trigger = p[23];
    e.state.gamepad.thumb_lx = ReadS16(p + 24);
    e.state.gamepad.thumb_ly = ReadS16(p + 26);
    e.state.gamepad.thumb_rx = ReadS16(p + 28);
    e.state.gamepad.thumb_ry = ReadS16(p + 30);
    if (e.status != X_ERROR_SUCCESS &&
        std::any_of(p + 16, p + 32, [](uint8_t v) { return v != 0; }))
      return fail("disconnected event must have zero state");
    previous = e.time_us;
    parsed.events.push_back(e);
  }
  *out = std::move(parsed);
  error->clear();
  return true;
}

ReplayInputDriver::ReplayInputDriver(ui::Window* window, size_t z_order,
                                     std::filesystem::path path, Clock clock)
    : InputDriver(window, z_order),
      path_(std::move(path)),
      clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = [] {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count());
    };
  }
}

X_STATUS ReplayInputDriver::Setup() {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_ = started_ = completed_ = false;
  next_ = 0;
  states_ = {};
  recording_ = {};
  std::ifstream file(path_, std::ios::binary | std::ios::ate);
  std::streamoff size = file.tellg();
  if (!file || size < std::streamoff(kHeaderBytes) ||
      size > std::streamoff(kHeaderBytes + kMaxEvents * kEventBytes)) {
    XELOGE("HID replay: missing, truncated or oversized recording");
    return X_STATUS_UNSUCCESSFUL;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  std::string error;
  if (!file.read(reinterpret_cast<char*>(bytes.data()),
                 std::streamsize(bytes.size())) ||
      !ParseRecording(bytes, &recording_, &error)) {
    XELOGE("HID replay: recording rejected: {}", error);
    return X_STATUS_UNSUCCESSFUL;
  }
  loaded_ = true;
  Advance(false);  // Seed time zero; capability discovery must not start time.
  XELOGI("HID replay: loaded {} events; clock starts on first GetState; "
         "wall time continues through guest pauses; GetKeystroke not recorded",
         recording_.events.size());
  return X_STATUS_SUCCESS;
}

void ReplayInputDriver::Advance(bool start) {
  if (!loaded_ || completed_) return;
  if (start && !started_) {
    start_us_ = clock_();
    started_ = true;
  }
  uint64_t now = started_ ? clock_() : 0;
  uint64_t elapsed = started_ && now >= start_us_ ? now - start_us_ : 0;
  while (next_ < recording_.events.size() &&
         recording_.events[next_].time_us <= elapsed) {
    const auto& e = recording_.events[next_++];
    states_[e.port] = e;
  }
  if (elapsed >= recording_.duration_us) {
    for (auto& e : states_) {
      if (e.status == X_ERROR_SUCCESS) {
        uint32_t packet = e.state.packet_number;
        e.state = {};
        e.state.packet_number = packet + 1;
      }
    }
    completed_ = true;
    XELOGI("HID replay: completed; connected ports neutral, no loop");
  }
}

X_RESULT ReplayInputDriver::GetCapabilities(uint32_t user_index, uint32_t,
                                            X_INPUT_CAPABILITIES* out_caps) {
  std::lock_guard<std::mutex> lock(mutex_);
  Advance(false);
  if (!loaded_ || user_index >= 4 || states_[user_index].status != X_ERROR_SUCCESS)
    return X_ERROR_DEVICE_NOT_CONNECTED;
  if (!out_caps) return X_ERROR_INVALID_PARAMETER;
  *out_caps = {};
  out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->gamepad.buttons = 0xF7FF;
  out_caps->gamepad.left_trigger = out_caps->gamepad.right_trigger = 255;
  out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = 32767;
  out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = 32767;
  return X_ERROR_SUCCESS;
}
X_RESULT ReplayInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (user_index >= 4) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (!out_state) return X_ERROR_INVALID_PARAMETER;
  Advance(true);
  if (!loaded_ || states_[user_index].status != X_ERROR_SUCCESS)
    return X_ERROR_DEVICE_NOT_CONNECTED;
  *out_state = states_[user_index].state;
  return X_ERROR_SUCCESS;
}
X_RESULT ReplayInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION*) {
  std::lock_guard<std::mutex> lock(mutex_);
  Advance(false);
  // Virtual controller: acknowledge and discard vibration, never touch hardware.
  return loaded_ && user_index < 4 &&
                 states_[user_index].status == X_ERROR_SUCCESS
             ? X_ERROR_SUCCESS
             : X_ERROR_DEVICE_NOT_CONNECTED;
}
X_RESULT ReplayInputDriver::GetKeystroke(uint32_t user_index, uint32_t,
                                         X_INPUT_KEYSTROKE*) {
  std::lock_guard<std::mutex> lock(mutex_);
  Advance(false);
  if (loaded_ && user_index == XUserIndexAny) {
    for (const auto& e : states_) {
      if (e.status == X_ERROR_SUCCESS) return X_ERROR_EMPTY;
    }
  } else if (loaded_ && user_index < 4 &&
             states_[user_index].status == X_ERROR_SUCCESS) {
    return X_ERROR_EMPTY;  // No keystroke history exists in this recording format.
  }
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
}  // namespace xe::hid::replay
