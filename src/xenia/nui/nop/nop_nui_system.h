/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NOP_NOP_NUI_SYSTEM_H_
#define XENIA_NUI_NOP_NOP_NUI_SYSTEM_H_

#include <cstdint>
#include <memory>
#include <string>

#include "xenia/nui/nui_system.h"

namespace xe {
namespace nui {
namespace nop {

// A NUI system that never has a sensor. It is what every host without Kinect
// support runs, so the guest sees exactly what it saw before the NUI subsystem
// existed: no device, no frames, no tilt.
class NopNuiSystem : public NuiSystem {
 public:
  NopNuiSystem();
  ~NopNuiSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<NuiSystem> Create();

  std::string name() const override { return "NOP"; }

  bool GetTiltDegrees(int32_t* out_degrees) override;
  bool SetTiltDegrees(int32_t degrees) override;
};

}  // namespace nop
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NOP_NOP_NUI_SYSTEM_H_
