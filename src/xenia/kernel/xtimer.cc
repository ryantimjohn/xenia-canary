/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <atomic>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_range.h"
#include "xenia/kernel/xthread.h"

DEFINE_bool(private_native_timer_candidate, false,
            "Windows private relative one-shot null-DPC native timer candidate. "
            "Requires time scalar 1 and process restart. Unsupported arms fail.",
            "Kernel");

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  if (native_) {
    native_->Invalidate();  // Never drain under object-table/global locks.
  }
}

bool NativeTimerCandidateEnabled() {
#if XE_PLATFORM_WIN32
  return cvars::private_native_timer_candidate;
#else
  return false;
#endif
}

bool XTimer::InitializeNative(void* native_ptr) {
  auto lock = global_critical_region::AcquireDirect();
  const uint32_t address = memory()->HostToGuestVirtual(native_ptr);
  if (kernel_state()->native_timers_closing() || !address ||
      address > UINT32_MAX - sizeof(X_KTIMER) + 1 ||
      !util::IsGuestRangeWritable(memory(), address, sizeof(X_KTIMER))) {
    return false;
  }
  auto* timer = static_cast<X_KTIMER*>(native_ptr);
  const auto type = timer->header.type;
  if (type != TimerNotificationObject && type != TimerSynchronizationObject) {
    return false;
  }
  RetireNativeAt(kernel_state(), address);  // No drain in this resolver path.
  auto event = type == TimerNotificationObject
                   ? threading::Event::CreateManualResetEvent(
                         timer->header.signal_state != 0)
                   : threading::Event::CreateAutoResetEvent(
                         timer->header.signal_state != 0);
  if (!event) {
    return false;
  }
  SetNativePointer(address, true);
  Memory* mem = memory();
  KernelState* kernel = kernel_state();
  const uint32_t identity = handle();
  native_ = std::make_shared<NativeTimerState>(
      std::move(event), [mem, kernel, address, identity, type]() -> X_KTIMER* {
        // Global serialization also covers heap protection/commit bookkeeping.
        if (kernel->native_timers_closing() ||
            !util::IsGuestRangeWritable(mem, address, sizeof(X_KTIMER))) {
          return nullptr;
        }
        auto* current = mem->TranslateVirtual<X_KTIMER*>(address);
        if (!NativeTimerAssociationMatches(*current, type, identity)) {
          return nullptr;
        }
        return current;
      });
  return true;
}

bool XTimer::MatchesNative(uint32_t address) const {
  return native_ && guest_object() == address && native_->associated();
}

bool XTimer::SetNativeTimer(int64_t due, bool* previous) {
  return native_ && native_->Arm(due, previous);
}

bool XTimer::CancelNativeTimer(bool* previous) {
  return native_ && native_->Cancel(previous);
}

void XTimer::WaitCallback() {
  if (native_ && !native_->AfterWait()) {
    static std::atomic<uint32_t> failures{0};
    uint32_t count = failures.load(std::memory_order_relaxed);
    while (count < 16 && !failures.compare_exchange_weak(
                             count, count + 1, std::memory_order_relaxed)) {}
    if (count < 16) {
      XELOGW("Private native timer: guest state query/identity unavailable; "
             "header left unchanged (bounded warning {})", count + 1);
    }
  }
}

std::vector<std::shared_ptr<NativeTimerState>> XTimer::RetireNativeAt(
    KernelState* kernel, uint32_t address) {
  auto lock = global_critical_region::AcquireDirect();
  std::vector<std::shared_ptr<NativeTimerState>> retired;
  for (auto& timer : kernel->object_table()->GetObjectsByType<XTimer>()) {
    if (timer->native_ && timer->guest_object() == address &&
        !timer->native_retired_) {
      timer->native_retired_ = true;
      timer->native_->Invalidate();
      retired.push_back(timer->native_);
      timer->ReleaseHandle();
    }
  }
  return retired;
}

void XTimer::QuiesceNativeTimers(KernelState* kernel) {
  std::vector<std::shared_ptr<NativeTimerState>> states;
  {
    auto lock = global_critical_region::AcquireDirect();
    kernel->set_native_timers_closing(true);
    for (auto& timer : kernel->object_table()->GetObjectsByType<XTimer>()) {
      if (timer->native_) {
        timer->native_->Invalidate();
        states.push_back(timer->native_);
      }
    }
  }
  for (auto& state : states) {
    state->Drain();
  }
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_tp = date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = date::clock_cast<WinSystemClock>(
        XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    auto cb_thread = callback_thread_;
    auto cb_routine = callback_routine_;
    auto cb_routine_arg = callback_routine_arg_;
    callback = [cb_thread, cb_routine, cb_routine_arg]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGI(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          cb_routine, cb_routine_arg, time_low, time_high);
      cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace kernel
}  // namespace xe
