/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <mutex>
#include <vector>

#include "xenia/base/threading.h"
#include "xenia/kernel/native_timer_state.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XThread;
bool NativeTimerCandidateEnabled();

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume);
  X_STATUS Cancel();

  bool InitializeNative(void* native_ptr);
  bool MatchesNative(uint32_t address) const;
  bool SetNativeTimer(int64_t due, bool* previous);
  bool CancelNativeTimer(bool* previous);
  // Nonblocking retirement, including inline initialization that erased the
  // stashed signature. Caller serializes guest header changes with global.
  static std::vector<std::shared_ptr<NativeTimerState>> RetireNativeAt(
      KernelState* kernel, uint32_t address);
  static void QuiesceNativeTimers(KernelState* kernel);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override {
    return native_ ? native_->wait_handle() : timer_.get();
  }
  void WaitCallback() override;
  void SyncFromGuest() override {
  }  // Never re-signal from a stale guest header.

 private:
  std::unique_ptr<xe::threading::Timer> timer_;
  std::shared_ptr<NativeTimerState> native_;
  bool native_retired_ = false;  // Global-protected internal handle retirement.
  std::mutex timer_lock_;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
