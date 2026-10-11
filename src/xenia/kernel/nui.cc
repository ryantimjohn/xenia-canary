/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui.h"

#include "xenia/base/assert.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

NUIController::NUIController(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

bool NUIController::NuiInitialized() {
  return kernel_state_->xam_state()->nui_state()->IsDevicePresent();
}

uint32_t NUIController::GetNUIDataPtr() {
  // This API has always returned a presence sentinel, not guest memory.
  return kernel_state_->xam_state()->nui_state()->hud_enabled() ? 1 : 0;
}

char NUIController::GetUnknown2() { return GetNUIDataPtr() ? 1 : 0; }

void NUIController::SetTiltCallback(uint32_t callback) {
  kernel_state_->xam_state()->nui_state()->runtime_state().SetTiltCallback(
      callback);
}

void NUIController::SetEngagedTrackingId(uint32_t tracking_id) {
  kernel_state_->xam_state()->nui_state()->set_engaged_tracking_id(tracking_id);
}

uint32_t NUIController::GetEngagedTrackingId() {
  return kernel_state_->xam_state()->nui_state()->engaged_tracking_id();
}

uint32_t NUIController::GetInitFlags() {
  return kernel_state_->xam_state()->nui_state()->hud_init_flags();
}

void NUIController::SetInitFlags(uint32_t flags) {
  kernel_state_->xam_state()->nui_state()->runtime_state().SetFlags(flags);
}

uint64_t NUIController::GetNUIVersion(uint32_t index) {
  if (index > 1) {
    assert_always();
    return 0;
  }
  const auto state =
      kernel_state_->xam_state()->nui_state()->runtime_state().snapshot();
  return index == 0 ? state.xam_version : state.xtl_version;
}

void NUIController::SetNUIVersion(uint64_t version, uint32_t index) {
  if (index > 1) {
    assert_always();
    return;
  }
  kernel_state_->xam_state()->nui_state()->runtime_state().SetVersion(version,
                                                                      index);
}

uint64_t NUIController::GetSessionId() {
  const auto state =
      kernel_state_->xam_state()->nui_state()->runtime_state().snapshot();
  return state.session_id;
}

void NUIController::SetSessionId(uint64_t id) {
  kernel_state_->xam_state()->nui_state()->runtime_state().SetSessionId(id);
}

uint32_t NUIController::NuiHudCheck(uint64_t tracking_id) {
  auto* state = kernel_state_->xam_state()->nui_state();
  if (!state->IsDevicePresent() || !state->runtime_state().EngageTrackingId(
                                       static_cast<uint32_t>(tracking_id))) {
    return X_ERROR_ACCESS_DENIED;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
