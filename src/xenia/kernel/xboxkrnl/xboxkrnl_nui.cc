/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The Kinect depth camera is a kernel device on the console; the NUI runtime
// linked into a title drives it entirely through this one export, passing a
// request block whose first dword selects the operation. The guest-visible
// state lives with the rest of the Kinect state in xam.
dword_result_t PsCamDeviceRequest_entry(lpvoid_t request_ptr) {
  return kernel_state()->xam_state()->nui_state()->HandleCameraRequest(
      request_ptr.guest_address());
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Nui);
