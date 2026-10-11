/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/nui_app_helpers.h"

#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace app {

kernel::xam::NuiState* GetNuiState(Emulator* emulator) {
  kernel::KernelState* kernel_state =
      emulator ? emulator->kernel_state() : nullptr;
  kernel::xam::XamState* xam_state =
      kernel_state ? kernel_state->xam_state() : nullptr;
  return xam_state ? xam_state->nui_state() : nullptr;
}

const char* DescribeNuiDeviceStatus(nui::NuiSystem::DeviceStatus status) {
  switch (status) {
    case nui::NuiSystem::DeviceStatus::kNotPresent:
      return "no device";
    case nui::NuiSystem::DeviceStatus::kInitializing:
      return "initializing";
    case nui::NuiSystem::DeviceStatus::kReady:
      return "ready";
    case nui::NuiSystem::DeviceStatus::kError:
      return "error";
    default:
      return "unknown";
  }
}

}  // namespace app
}  // namespace xe
