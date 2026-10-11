/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_LOG_H_
#define XENIA_KERNEL_XAM_NUI_LOG_H_

#include <string_view>

#include "xenia/base/logging.h"
#include "xenia/nui/nui_flags.h"

namespace xe {
namespace kernel {
namespace xam {

// Logs a NUI export call and its arguments, but only while nui_log_messages is
// set. Every NUI entry point uses it so that a Kinect trace shows the whole
// conversation between the title and xam.
template <typename... Args>
void LogNuiCall(std::string_view format, const Args&... args) {
  if (cvars::nui_log_messages) {
    XELOGI(format, args...);
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_LOG_H_
