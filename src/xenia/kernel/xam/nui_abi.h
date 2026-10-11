/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_ABI_H_
#define XENIA_KERNEL_XAM_NUI_ABI_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/nui_messages.h"

namespace xe {
namespace kernel {
namespace xam {

struct X_NUI_DEVICE_STATUS {
  xe::be<uint32_t> camera_status;
  xe::be<uint32_t> tilt_status;
  xe::be<uint32_t> mic_status;
  xe::be<uint32_t> status_bits;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 0x10);
static_assert(offsetof(X_NUI_DEVICE_STATUS, status_bits) == 0xC);

struct X_NUI_DEPTH_CALIBRATION {
  uint8_t data[0x30];
};
static_assert(sizeof(X_NUI_DEPTH_CALIBRATION) == 0x30);

struct X_NUI_PLAYER_ENGAGEMENT_UPDATE {
  uint8_t data[0x1C];
};
static_assert(sizeof(X_NUI_PLAYER_ENGAGEMENT_UPDATE) == 0x1C);

inline void FillNuiDeviceStatus(X_NUI_DEVICE_STATUS* status, bool ready) {
  std::memset(status, 0, sizeof(*status));
  status->status_bits = ready ? 1 : 0;
}

inline void ClearNuiDepthCalibration(X_NUI_DEPTH_CALIBRATION* calibration) {
  std::memset(calibration, 0, sizeof(*calibration));
}

inline void ClearNuiPlayerEngagement(
    X_NUI_PLAYER_ENGAGEMENT_UPDATE* engagement) {
  std::memset(engagement, 0, sizeof(*engagement));
}

// The caller supplies the size and tag before the export can write 0x50 bytes.
inline bool IsValidNuiTiltStatus(const X_NUI_TILT_STATUS* status) {
  return status && status->cb_size >= kNuiTiltStatusSize &&
         status->signature == kNuiTiltStatusSignature;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_ABI_H_
