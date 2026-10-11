/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_NUI_APP_HELPERS_H_
#define XENIA_APP_NUI_APP_HELPERS_H_

#include "xenia/nui/nui_system.h"

namespace xe {

class Emulator;

namespace kernel {
namespace xam {
class NuiState;
}  // namespace xam
}  // namespace kernel

namespace app {

// These helpers are shared by the app's NUI (Kinect) overlays and the emulator
// window.

// Returns xam's NUI state, or null while there is no emulator or the emulator
// has no kernel.
kernel::xam::NuiState* GetNuiState(Emulator* emulator);

// Returns the words the NUI overlays use for the sensor's status.
const char* DescribeNuiDeviceStatus(nui::NuiSystem::DeviceStatus status);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_NUI_APP_HELPERS_H_
