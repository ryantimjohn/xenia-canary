// SDK-derived private fork research. Exclude from upstream contributions.
#ifndef XENIA_KERNEL_NATIVE_TIMER_STATE_H_
#define XENIA_KERNEL_NATIVE_TIMER_STATE_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"

namespace xe::kernel {
struct X_KTIMER;
bool NativeTimerAssociationMatches(const X_KTIMER& timer, uint8_t type,
                                   uint32_t handle);

// Private relative, one-shot, null-DPC candidate. All guest access and event
// transitions use global_critical_region; publication and drains never do.
class NativeTimerState : public std::enable_shared_from_this<NativeTimerState> {
 public:
  using Clock = std::chrono::steady_clock;
  using Item = threading::TimerQueueWaitItem;
  using Access =
      std::function<X_KTIMER*()>;  // Validated identity, under global.
  using Publish = std::function<std::weak_ptr<Item>(std::function<void(void*)>,
                                                    Clock::time_point)>;

  NativeTimerState(std::unique_ptr<threading::Event> event, Access access,
                   Publish publish = {});
  static bool RelativeDeadline(int64_t due, Clock::time_point now,
                               Clock::time_point* deadline);
  threading::WaitHandle* wait_handle() const { return event_.get(); }
  // False means unsupported/retired; previous is valid only on success.
  bool Arm(int64_t due, bool* previous);
  bool Cancel(bool* previous);
  bool AfterWait();
  bool associated() const;
  void Invalidate();  // Nonblocking; safe with global already held.
  void Drain();       // Caller must hold neither global nor state locks.

 private:
  void Expire(uint64_t generation);
  void PublisherDone();
  std::unique_ptr<threading::Event> event_;  // Stable until last state owner.
  Access access_;
  Publish publish_;
  // All fields below except publisher bookkeeping are protected by global.
  bool active_ = true;
  bool armed_ = false;
  uint64_t generation_ = 0;
  std::weak_ptr<Item> pending_;
  std::mutex publishers_mutex_;
  std::condition_variable publishers_cv_;
  size_t publishers_ = 0;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_TIMER_STATE_H_
