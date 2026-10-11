/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_KEEPALIVE_H_
#define XENIA_KERNEL_XAM_NUI_KEEPALIVE_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xam {

// Tracks how long the title wants the camera kept awake. Titles renew the
// request long before it expires, so the deadline is simply moved forward on
// every request. All times are microseconds on an arbitrary monotonic clock;
// the caller decides which one.
class NuiKeepAlive {
 public:
  // Keeps the camera awake for timeout_ms starting at now_us. A timeout of
  // zero releases the request instead, the way the guest message does.
  void Request(uint32_t timeout_ms, uint64_t now_us) {
    if (timeout_ms == 0) {
      Release();
      return;
    }
    deadline_us_ = now_us + static_cast<uint64_t>(timeout_ms) * 1000;
  }

  // Drops the request immediately.
  void Release() { deadline_us_ = 0; }

  bool IsActive(uint64_t now_us) const {
    return deadline_us_ != 0 && now_us <= deadline_us_;
  }

  // Time the current request expires at, or zero when there is none.
  uint64_t deadline_us() const { return deadline_us_; }

 private:
  uint64_t deadline_us_ = 0;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_KEEPALIVE_H_
