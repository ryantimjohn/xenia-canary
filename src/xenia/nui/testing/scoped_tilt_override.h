/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_TESTING_SCOPED_TILT_OVERRIDE_H_
#define XENIA_NUI_TESTING_SCOPED_TILT_OVERRIDE_H_

#include <cstdint>

#include "xenia/nui/nui_flags.h"

namespace xe {
namespace nui {
namespace test {

// Puts nui_tilt_override back the way the test found it.
class ScopedTiltOverride {
 public:
  explicit ScopedTiltOverride(int32_t degrees)
      : saved_(cvars::nui_tilt_override) {
    cvars::nui_tilt_override = degrees;
  }
  ~ScopedTiltOverride() { cvars::nui_tilt_override = saved_; }

 private:
  int32_t saved_;
};

}  // namespace test
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_TESTING_SCOPED_TILT_OVERRIDE_H_
