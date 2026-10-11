/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_RUNTIME_STATE_H_
#define XENIA_KERNEL_XAM_NUI_RUNTIME_STATE_H_

#include <cstdint>
#include <mutex>

namespace xe {
namespace kernel {
namespace xam {

// Metadata shared by the NUI exports and the app 0xFE runtime messages.
// Host device and camera transport lifetime are independent of this state.
class NuiRuntimeState {
 public:
  struct Snapshot {
    uint64_t xam_version = 0;
    uint64_t xtl_version = 0;
    uint32_t flags = 0;
    uint32_t engaged_tracking_id = 0;
    uint32_t tilt_callback = 0;
    uint32_t identity_callback = 0;
    bool initialized = false;
    // Identity sessions belong to the kernel, not a runtime initialization.
    uint64_t session_id = 0;
  };

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  // XNuiTitleInitialize supports XAM version 5 and does not implement the
  // high-bit flag path. Validate before publishing any part of the request.
  bool Initialize(uint64_t xam_version, uint64_t xtl_version, uint32_t flags) {
    if (xam_version != 5 || (flags & 0x80000000)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_.xam_version = xam_version;
    state_.xtl_version = xtl_version;
    state_.flags = flags;
    state_.initialized = true;
    return true;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t session_id = state_.session_id;
    state_ = Snapshot();
    state_.session_id = session_id;
  }

  void SetVersion(uint64_t version, uint32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index == 0) {
      state_.xam_version = version;
    } else if (index == 1) {
      state_.xtl_version = version;
    }
  }

  void SetFlags(uint32_t flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.flags = flags;
  }

  void SetEngagedTrackingId(uint32_t tracking_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.engaged_tracking_id = tracking_id;
  }

  // A HUD check and engagement update must not straddle Reset(). ID zero
  // checks readiness without clearing the previously engaged skeleton.
  bool EngageTrackingId(uint32_t tracking_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.initialized) {
      return false;
    }
    if (tracking_id) {
      state_.engaged_tracking_id = tracking_id;
    }
    return true;
  }

  void SetTiltCallback(uint32_t callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.tilt_callback = callback;
  }

  void SetIdentityCallback(uint32_t callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.identity_callback = callback;
  }

  void SetSessionId(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.session_id = session_id;
  }

  uint64_t GetOrCreateSessionId() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.session_id) {
      // Keep the existing deterministic stub until identity events exist.
      state_.session_id = 0xDEADF00DDEADF00D;
    }
    return state_.session_id;
  }

 private:
  mutable std::mutex mutex_;
  Snapshot state_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_RUNTIME_STATE_H_
