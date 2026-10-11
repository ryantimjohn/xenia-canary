/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_H_
#define XENIA_KERNEL_NUI_H_

#include <cstdint>
#include "xenia/base/byte_order.h"

namespace xe {
namespace kernel {

class KernelState;

// Compatibility API for kernel callers; guest NUI state belongs to NuiState.
class NUIController {
 public:
  explicit NUIController(KernelState* kernel_state);
  ~NUIController() = default;

  bool NuiInitialized();
  uint32_t GetNUIDataPtr();
  char GetUnknown2();
  void SetTiltCallback(uint32_t callback);
  uint32_t GetEngagedTrackingId();
  void SetEngagedTrackingId(uint32_t tracking_id);
  uint32_t GetInitFlags();
  void SetInitFlags(uint32_t flags);
  uint64_t GetNUIVersion(uint32_t index);
  void SetNUIVersion(uint64_t version, uint32_t index);
  uint64_t GetSessionId();
  void SetSessionId(uint64_t id);
  uint32_t NuiHudCheck(uint64_t tracking_id);

 private:
  KernelState* kernel_state_;
};

enum X_TILT_STATUS_FLAGS : uint32_t {
  // 0x1 and 0x2 moving
  TILT_STATUS_UNK1 = 0x1,
  TILT_STATUS_UNK2 = 0x2,
  TILT_STATUS_STALLED = 0x4,
  TILT_STATUS_ERROR = 0x8,
};

struct X_NUI_TILT_STATUS {
  be<uint32_t> buffer_size;
  be<uint32_t> signature;  // 'XtSs'
  be<uint32_t> unknown_08[6];
  // Gravity direction.
  be<int32_t> gravity_long_avg[3];
  be<X_TILT_STATUS_FLAGS> flags;
  be<uint32_t> unknown_30[8];
};
static_assert_size(X_NUI_TILT_STATUS, 0x50);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_H_
