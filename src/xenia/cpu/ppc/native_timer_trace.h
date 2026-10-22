// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#ifndef XENIA_CPU_PPC_NATIVE_TIMER_TRACE_H_
#define XENIA_CPU_PPC_NATIVE_TIMER_TRACE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace xe::cpu::ppc::native_timer_trace {
constexpr uint32_t kExports = 3, kPerExportLimit = 16;
constexpr uint32_t kNotSelected = kExports;

// This selects a candidate only. The caller must also verify pointer identity
// against the registered xboxkrnl.exe table before observing any registers.
inline uint32_t Select(uint16_t ordinal, std::string_view name,
                        bool function, bool implemented) {
  if (!function || implemented) return kNotSelected;
  if (ordinal == 0x54 && name == "KeCancelTimer") return 0;
  if (ordinal == 0xA6 && name == "KeSetTimer") return 1;
  if (ordinal == 0xA7 && name == "KeSetTimerEx") return 2;
  return kNotSelected;
}

class Budget {
 public:
  bool HasCapacity(uint32_t index) const {
    return index < kExports &&
           claimed_[index].load(std::memory_order_relaxed) < kPerExportLimit;
  }
  uint32_t TryClaim(uint32_t index) {
    if (index >= kExports) return 0;
    auto& field = claimed_[index];
    uint32_t old = field.load(std::memory_order_relaxed);
    while (old < kPerExportLimit) {
      if (field.compare_exchange_strong(old, old + 1,
                                       std::memory_order_relaxed)) return old + 1;
    }
    return 0;
  }
 private:
  // Strong-CAS failure implies a successful competing claim, so at most 16
  // successful competitors can delay a claim. No overshoot or reset.
  std::array<std::atomic<uint32_t>, kExports> claimed_{};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

struct Registers {
  std::array<uint64_t, 6> r3_to_r8;
  uint64_t lr;
};
inline Registers Capture(const uint64_t (&gpr)[32], uint64_t lr) {
  return {{gpr[3], gpr[4], gpr[5], gpr[6], gpr[7], gpr[8]}, lr};
}
}  // namespace xe::cpu::ppc::native_timer_trace
#endif  // XENIA_CPU_PPC_NATIVE_TIMER_TRACE_H_
