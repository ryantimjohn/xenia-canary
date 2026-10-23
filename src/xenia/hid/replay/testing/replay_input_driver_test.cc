// Copyright 2026 Xenia Canary. Released under the BSD license - see LICENSE.
#include "xenia/hid/replay/replay_input_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe::hid::replay {
namespace {
void Put(std::vector<uint8_t>& b, size_t at, uint64_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    b[at + i] = uint8_t(value >> (8 * i));
  }
}
struct Sample {
  uint64_t time;
  uint32_t port, status, packet;
  uint16_t buttons;
};
std::vector<uint8_t> Bytes(std::initializer_list<Sample> samples,
                           uint64_t duration = 1000) {
  std::vector<uint8_t> b(24 + 32 * samples.size());
  const uint8_t magic[] = {'X', 'E', 'C', 'T', 'R', 'L', '1', 0};
  std::copy(std::begin(magic), std::end(magic), b.begin());
  Put(b, 8, duration, 8);
  Put(b, 16, samples.size(), 4);
  size_t p = 24;
  for (auto e : samples) {
    Put(b, p, e.time, 8);
    Put(b, p + 8, e.port, 4);
    Put(b, p + 12, e.status, 4);
    Put(b, p + 16, e.packet, 4);
    Put(b, p + 20, e.buttons, 2);
    p += 32;
  }
  return b;
}
struct File {
  std::filesystem::path path;
  explicit File(const std::vector<uint8_t>& b) {
    static std::atomic<uint64_t> serial{0};
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("xenia-controller-replay-" + std::to_string(tick) + "-" +
            std::to_string(serial++) + ".xctrl");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()),
              std::streamsize(b.size()));
    REQUIRE(out.good());
  }
  ~File() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};
}  // namespace

TEST_CASE("Replay holds state until timestamp, capabilities never arm clock",
          "[hid][replay]") {
  File file(Bytes(
      {{0, 0, 0, 4, 0}, {100, 0, 0, 5, X_INPUT_GAMEPAD_A}, {300, 0, 0, 6, 0}}));
  uint64_t now = 10000;
  ReplayInputDriver driver(nullptr, 0, file.path, [&] { return now; });
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  X_INPUT_CAPABILITIES caps;
  REQUIRE(driver.GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(caps.type == XINPUT_DEVTYPE_GAMEPAD);
  now += 5000;
  X_INPUT_KEYSTROKE key;
  CHECK(driver.GetKeystroke(0, 0, &key) == X_ERROR_EMPTY);
  now += 5000;
  X_INPUT_VIBRATION vibration = {};
  CHECK(driver.SetState(0, &vibration) == X_ERROR_SUCCESS);
  now += 5000;
  X_INPUT_STATE state;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  CHECK(uint32_t(state.packet_number) == 4);  // No earlier query armed EOF.
  now += 100;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  for (int i = 0; i < 3; ++i) {
    REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
    CHECK(uint32_t(state.packet_number) == 5);
    CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  }
  now += 199;
  driver.GetState(0, &state);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  ++now;
  driver.GetState(0, &state);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  CHECK(uint32_t(state.packet_number) == 6);
}

TEST_CASE("Replay preserves all state extrema and neutralizes EOF once",
          "[hid][replay]") {
  auto bytes = Bytes({{0, 0, 0, 0xFFFFFFFFu, X_INPUT_GAMEPAD_A}});
  bytes[24 + 22] = 255;
  bytes[24 + 23] = 254;
  Put(bytes, 24 + 24, 0x8000, 2);
  Put(bytes, 24 + 26, 0x7FFF, 2);
  Put(bytes, 24 + 28, 0xFFFF, 2);
  Put(bytes, 24 + 30, 1, 2);
  File file(bytes);
  uint64_t now = 10;
  ReplayInputDriver driver(nullptr, 0, file.path, [&] { return now; });
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  X_INPUT_STATE state;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.left_trigger == 255);
  CHECK(state.gamepad.right_trigger == 254);
  CHECK(int16_t(state.gamepad.thumb_lx) == -32768);
  CHECK(int16_t(state.gamepad.thumb_ly) == 32767);
  CHECK(int16_t(state.gamepad.thumb_rx) == -1);
  CHECK(int16_t(state.gamepad.thumb_ry) == 1);
  now += 999;
  driver.GetState(0, &state);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  ++now;
  driver.GetState(0, &state);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  CHECK(state.gamepad.left_trigger == 0);
  CHECK(state.gamepad.right_trigger == 0);
  CHECK(int16_t(state.gamepad.thumb_lx) == 0);
  CHECK(int16_t(state.gamepad.thumb_ly) == 0);
  CHECK(int16_t(state.gamepad.thumb_rx) == 0);
  CHECK(int16_t(state.gamepad.thumb_ry) == 0);
  CHECK(uint32_t(state.packet_number) == 0);  // Explicit modulo wrap.
  now += 1000;
  driver.GetState(0, &state);
  CHECK(uint32_t(state.packet_number) == 0);  // No loop or repeated increment.
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  driver.GetState(0, &state);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
}

TEST_CASE("Replay disconnect, reconnect, other ports and uncaptured keystrokes",
          "[hid][replay]") {
  File file(Bytes({{0, 0, 0, 1, 0},
                   {100, 0, 1167, 0, 0},
                   {200, 0, 0, 9, X_INPUT_GAMEPAD_B},
                   {300, 0, 1167, 0, 0}}));
  uint64_t now = 0;
  ReplayInputDriver driver(nullptr, 0, file.path, [&] { return now; });
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  X_INPUT_STATE state;
  X_INPUT_CAPABILITIES caps;
  X_INPUT_VIBRATION vibration = {};
  X_INPUT_KEYSTROKE key;
  CHECK(driver.GetKeystroke(0, 0, &key) == X_ERROR_EMPTY);
  CHECK(driver.SetState(0, &vibration) == X_ERROR_SUCCESS);
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(driver.GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(driver.GetState(4, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  now = 100;
  CHECK(driver.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(driver.GetCapabilities(0, 0, &caps) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(driver.GetKeystroke(XUserIndexAny, 0, &key) ==
        X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(driver.SetState(0, &vibration) == X_ERROR_DEVICE_NOT_CONNECTED);
  now = 200;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  CHECK(driver.GetKeystroke(XUserIndexAny, 0, &key) == X_ERROR_EMPTY);
  now = 1000;
  CHECK(driver.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("Replay coalesces elapsed states without stretching missed taps",
          "[hid][replay]") {
  File file(Bytes({{0, 0, 0, 1, 0},
                   {10, 0, 0, 2, X_INPUT_GAMEPAD_A},
                   {20, 0, 0, 3, 0},
                   {20, 0, 0, 4, X_INPUT_GAMEPAD_B}}));
  uint64_t now = 0;
  ReplayInputDriver driver(nullptr, 0, file.path, [&] { return now; });
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  X_INPUT_STATE state;
  driver.GetState(0, &state);
  now = 20;
  driver.GetState(0, &state);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  CHECK(uint32_t(state.packet_number) == 4);
}

TEST_CASE("Replay keeps recorded ports independent", "[hid][replay]") {
  File file(Bytes({{0, 0, 1167, 0, 0},
                   {0, 3, 0, 8, X_INPUT_GAMEPAD_Y},
                   {100, 0, 0, 1, X_INPUT_GAMEPAD_A}}));
  uint64_t now = 0;
  ReplayInputDriver driver(nullptr, 0, file.path, [&] { return now; });
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  X_INPUT_STATE state;
  REQUIRE(driver.GetState(3, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_Y);
  CHECK(driver.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  now = 100;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  REQUIRE(driver.GetState(3, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_Y);
}

TEST_CASE("Replay parser rejects malformed timelines and setup fails closed",
          "[hid][replay]") {
  auto original =
      Bytes({{0, 0, 0, 0, 0}, {100, 0, 0, 1, 0}, {200, 0, 0, 2, 0}});
  Recording out;
  std::string error;
  REQUIRE(ParseRecording(original, &out, &error));
  auto bytes = original;
  SECTION("magic") { bytes[0] = 0; }
  SECTION("truncated header") { bytes.resize(10); }
  SECTION("truncated event") { bytes.pop_back(); }
  SECTION("trailing") { bytes.push_back(0); }
  SECTION("reserved") { bytes[20] = 1; }
  SECTION("oversized count") { Put(bytes, 16, 1000001, 4); }
  SECTION("oversized file") { bytes.resize(24 + 1000000 * 32 + 1); }
  SECTION("zero duration") { Put(bytes, 8, 0, 8); }
  SECTION("excess duration") { Put(bytes, 8, 86400000001ull, 8); }
  SECTION("port") { Put(bytes, 24 + 8, 4, 4); }
  SECTION("status") { Put(bytes, 24 + 12, 5, 4); }
  SECTION("missing initial state") { Put(bytes, 24, 1, 8); }
  SECTION("decreasing") { Put(bytes, 24 + 64, 50, 8); }
  SECTION("outside duration") { Put(bytes, 24 + 32, 1000, 8); }
  SECTION("dirty disconnected state") { Put(bytes, 24 + 32 + 12, 1167, 4); }
  CHECK_FALSE(ParseRecording(bytes, &out, &error));
  CHECK_FALSE(error.empty());
  File file(bytes);
  ReplayInputDriver driver(nullptr, 0, file.path, [] { return uint64_t(0); });
  CHECK(driver.Setup() == X_STATUS_UNSUCCESSFUL);
  X_INPUT_STATE state;
  CHECK(driver.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}
}  // namespace xe::hid::replay
