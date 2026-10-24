// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#ifndef XENIA_KERNEL_UTIL_NATIVE_OBJECT_FAILURE_H_
#define XENIA_KERNEL_UTIL_NATIVE_OBJECT_FAILURE_H_

#include <atomic>
#include <cstdint>

namespace xe::kernel::util {

// Optional output, assigned only on a null resolver result. Every value comes
// from an existing resolver decision/read, never an additional header probe.
struct NativeObjectFailure {
  enum class Reason { kStashedLookupMiss, kUnsupportedFirstUseType };
  Reason reason;
  uint32_t effective_type;
  bool header_type_read;
  uint32_t header_type;
  uint32_t signature_word;
  bool stashed_handle_read;
  uint32_t stashed_handle;
};

// One production instance, process lifetime. Slots are never returned/reset.
// A failed strong CAS implies another successful claim: at most 16 competing
// successes can delay this loop. No overshoot, heap allocation or mutex.
class WaitFailureLogBudget {
 public:
  static constexpr uint32_t kLimit = 16;
  bool HasCapacity() const {
    return claimed_.load(std::memory_order_relaxed) < kLimit;
  }
  // Returns the unique sequence 1..16, or zero when exhausted.
  uint32_t TryClaim() {
    uint32_t old = claimed_.load(std::memory_order_relaxed);
    while (old < kLimit) {
      if (claimed_.compare_exchange_strong(old, old + 1,
                                           std::memory_order_relaxed)) {
        return old + 1;
      }
    }
    return 0;
  }

 private:
  std::atomic<uint32_t> claimed_{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

}  // namespace xe::kernel::util
#endif  // XENIA_KERNEL_UTIL_NATIVE_OBJECT_FAILURE_H_
