/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_abi.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/nui_tilt_status.h"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

template <typename T>
struct NuiGuardedOutput {
  std::array<uint8_t, 16> before;
  T output;
  std::array<uint8_t, 16> after;

  NuiGuardedOutput() { std::memset(this, 0xA5, sizeof(*this)); }

  bool CanariesIntact() const {
    const auto is_canary = [](uint8_t value) { return value == 0xA5; };
    return std::all_of(before.begin(), before.end(), is_canary) &&
           std::all_of(after.begin(), after.end(), is_canary);
  }

  bool OutputCleared() const {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&output);
    return std::all_of(bytes, bytes + sizeof(output),
                       [](uint8_t value) { return value == 0; });
  }
};

TEST_CASE("Device status occupies the caller's four-word buffer",
          "[nui][abi]") {
  NuiGuardedOutput<X_NUI_DEVICE_STATUS> buffer;
  FillNuiDeviceStatus(&buffer.output, true);
  CHECK(buffer.CanariesIntact());
  CHECK(buffer.output.camera_status == 0);
  CHECK(buffer.output.tilt_status == 0);
  CHECK(buffer.output.mic_status == 0);
  CHECK(buffer.output.status_bits == 1);
  const auto* bytes = reinterpret_cast<const uint8_t*>(&buffer.output);
  CHECK(bytes[12] == 0);
  CHECK(bytes[13] == 0);
  CHECK(bytes[14] == 0);
  CHECK(bytes[15] == 1);

  FillNuiDeviceStatus(&buffer.output, false);
  CHECK(buffer.CanariesIntact());
  CHECK(buffer.OutputCleared());
}

TEST_CASE("Unavailable calibration clears exactly the 0x30-byte output",
          "[nui][abi]") {
  NuiGuardedOutput<X_NUI_DEPTH_CALIBRATION> buffer;
  ClearNuiDepthCalibration(&buffer.output);
  CHECK(buffer.CanariesIntact());
  CHECK(buffer.OutputCleared());
}

TEST_CASE("Engagement update clears exactly the 0x1C-byte output",
          "[nui][abi]") {
  NuiGuardedOutput<X_NUI_PLAYER_ENGAGEMENT_UPDATE> buffer;
  ClearNuiPlayerEngagement(&buffer.output);
  CHECK(buffer.CanariesIntact());
  CHECK(buffer.OutputCleared());
}

TEST_CASE("Tilt status requires the caller's size and XtSs tag",
          "[nui][abi][tilt]") {
  NuiGuardedOutput<X_NUI_TILT_STATUS> buffer;
  CHECK_FALSE(IsValidNuiTiltStatus(nullptr));
  buffer.output.cb_size = kNuiTiltStatusSize - 1;
  buffer.output.signature = kNuiTiltStatusSignature;
  const auto invalid_size = buffer.output;
  CHECK_FALSE(IsValidNuiTiltStatus(&buffer.output));
  CHECK(std::memcmp(&invalid_size, &buffer.output, sizeof(invalid_size)) == 0);
  CHECK(buffer.CanariesIntact());

  buffer.output.cb_size = kNuiTiltStatusSize;
  buffer.output.signature = 0;
  const auto invalid_tag = buffer.output;
  CHECK_FALSE(IsValidNuiTiltStatus(&buffer.output));
  CHECK(std::memcmp(&invalid_tag, &buffer.output, sizeof(invalid_tag)) == 0);
  CHECK(buffer.CanariesIntact());

  buffer.output.signature = kNuiTiltStatusSignature;
  REQUIRE(IsValidNuiTiltStatus(&buffer.output));
  FillNuiTiltStatusLegs(&buffer.output, NuiTiltStatusLegsForAngle(12), true);
  CHECK(buffer.CanariesIntact());
  CHECK(buffer.output.cb_size == kNuiTiltStatusSize);
  CHECK(buffer.output.signature == kNuiTiltStatusSignature);
  CHECK(buffer.output.status_flags == kNuiTiltBlockMoving);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
