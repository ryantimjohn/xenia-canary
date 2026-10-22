// SDK-derived private fork research. Exclude from upstream contributions.
#include "xenia/kernel/native_timer_state.h"

#include <limits>

#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {
void Disarm(std::weak_ptr<NativeTimerState::Item> item) {
  if (auto retained = item.lock()) {
    retained->Disarm();
  }
}
}  // namespace

bool NativeTimerAssociationMatches(const X_KTIMER& timer, uint8_t type,
                                   uint32_t handle) {
  return (type == TimerNotificationObject ||
          type == TimerSynchronizationObject) &&
         timer.header.type == type &&
         timer.header.wait_list.flink_ptr == kXObjSignature &&
         timer.header.wait_list.blink_ptr == handle;
}

NativeTimerState::NativeTimerState(std::unique_ptr<threading::Event> event,
                                 Access access, Publish publish)
    : event_(std::move(event)), access_(std::move(access)),
      publish_(std::move(publish)) {
  if (!publish_) {
    publish_ = [](std::function<void(void*)> callback, Clock::time_point due) {
      return threading::QueueTimerOnce(std::move(callback), nullptr, due);
    };
  }
}

bool NativeTimerState::RelativeDeadline(int64_t due, Clock::time_point now,
                                      Clock::time_point* deadline) {
  // Zero/absolute times and INT64_MIN are deliberately unsupported. Convert
  // only after bounding against the host's remaining representable interval.
  if (!deadline || due >= 0 || due == std::numeric_limits<int64_t>::min() ||
      now.time_since_epoch() < Clock::duration::zero()) {
    return false;
  }
  using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
  const auto room = Clock::time_point::max() - now;
  const auto max_ticks = std::chrono::duration_cast<Ticks>(room).count();
  if (-due > max_ticks) {
    return false;
  }
  *deadline = now + std::chrono::duration_cast<Clock::duration>(Ticks(-due));
  return true;
}

bool NativeTimerState::Arm(int64_t due, bool* previous) {
  Clock::time_point deadline;
  if (!RelativeDeadline(due, Clock::now(), &deadline)) {
    return false;
  }
  std::weak_ptr<Item> old;
  uint64_t generation;
  {
    auto lock = global_critical_region::AcquireDirect();
    auto* timer = active_ ? access_() : nullptr;
    if (!timer) {
      return false;
    }
    *previous = armed_;
    old = pending_;
    pending_.reset();
    generation = ++generation_;
    armed_ = true;
    event_->Reset();
    timer->header.signal_state = 0;
    timer->header.inserted = 1;
    // Private candidate retains the requested relative value, not an asserted
    // absolute guest interrupt-time representation. Guest list links stay ours.
    timer->due_time = static_cast<uint64_t>(due);
    timer->period = 0;
    timer->dpc = 0;
    std::lock_guard<std::mutex> publishing(publishers_mutex_);
    ++publishers_;
  }
  // Both may block. No global/state lock is held here.
  Disarm(old);
  std::weak_ptr<Item> item;
  try {
    std::weak_ptr<NativeTimerState> weak = shared_from_this();
    item = publish_([weak, generation](void*) {
      if (auto state = weak.lock()) {
        state->Expire(generation);
      }
    }, deadline);
  } catch (...) {
    {
      auto lock = global_critical_region::AcquireDirect();
      if (generation_ == generation) {
        armed_ = false;
        if (auto* timer = active_ ? access_() : nullptr) {
          timer->header.inserted = 0;
        }
      }
    }
    PublisherDone();
    // No successful BOOLEAN result for a failed queue publication. The entry
    // rejects this failure; the reset/inserted rollback is explicit.
    return false;
  }
  bool keep;
  {
    auto lock = global_critical_region::AcquireDirect();
    keep = active_ && armed_ && generation_ == generation;
    if (keep) {
      pending_ = item;
    }
  }
  // An expiry before publication already signaled the event. Do not reset it.
  if (!keep) {
    Disarm(item);
  }
  PublisherDone();
  return true;
}

void NativeTimerState::PublisherDone() {
  std::lock_guard<std::mutex> lock(publishers_mutex_);
  --publishers_;
  publishers_cv_.notify_all();
}

bool NativeTimerState::Cancel(bool* previous) {
  std::weak_ptr<Item> old;
  {
    auto lock = global_critical_region::AcquireDirect();
    auto* timer = active_ ? access_() : nullptr;
    if (!timer) {
      return false;
    }
    *previous = armed_;
    armed_ = false;
    ++generation_;
    old = pending_;
    pending_.reset();
    timer->header.inserted = 0;
    // Cancellation does not reset a timer that has already become signaled.
  }
  Disarm(old);
  return true;
}

void NativeTimerState::Expire(uint64_t generation) {
  auto lock = global_critical_region::AcquireDirect();
  if (!active_ || !armed_ || generation != generation_) {
    return;
  }
  auto* timer = access_();
  if (!timer) {
    active_ = false;
    armed_ = false;
    ++generation_;
    return;
  }
  armed_ = false;
  timer->header.inserted = 0;
  timer->header.signal_state = 1;
  event_->Set();
}

bool NativeTimerState::AfterWait() {
  auto lock = global_critical_region::AcquireDirect();
  auto* timer = active_ ? access_() : nullptr;
  if (!timer) {
    return false;
  }
  threading::EventInfo info{};
  if (!event_->TryQuery(&info)) {
    return false;
  }
  // A new expiry cannot interleave between this query and header update. Host
  // waiters may still consume an auto-reset signal independently of this lock.
  timer->header.signal_state = info.state ? 1 : 0;
  return true;
}

bool NativeTimerState::associated() const {
  auto lock = global_critical_region::AcquireDirect();
  return active_ && access_();
}

void NativeTimerState::Invalidate() {
  auto lock = global_critical_region::AcquireDirect();
  active_ = false;
  armed_ = false;
  ++generation_;
}

void NativeTimerState::Drain() {
  {
    std::unique_lock<std::mutex> lock(publishers_mutex_);
    publishers_cv_.wait(lock, [this] { return !publishers_; });
  }
  std::weak_ptr<Item> old;
  {
    auto lock = global_critical_region::AcquireDirect();
    old = pending_;
    pending_.reset();
  }
  Disarm(old);
}
}  // namespace xe::kernel
