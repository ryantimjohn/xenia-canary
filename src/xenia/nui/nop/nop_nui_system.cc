/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nop/nop_nui_system.h"

namespace xe {
namespace nui {
namespace nop {

std::unique_ptr<NuiSystem> NopNuiSystem::Create() {
  return std::make_unique<NopNuiSystem>();
}

NopNuiSystem::NopNuiSystem() = default;

NopNuiSystem::~NopNuiSystem() = default;

bool NopNuiSystem::GetTiltDegrees(int32_t* out_degrees) {
  // There is no sensor to read an angle from.
  return false;
}

bool NopNuiSystem::SetTiltDegrees(int32_t degrees) {
  // There is no tilt motor to move.
  return false;
}

}  // namespace nop
}  // namespace nui
}  // namespace xe
