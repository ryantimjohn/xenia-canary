// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#ifndef XENIA_CPU_PPC_TIMER_CALLSITE_BYTES_H_
#define XENIA_CPU_PPC_TIMER_CALLSITE_BYTES_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace xe::cpu::ppc::timer_callsite_bytes {
constexpr uint32_t kGuestStart = 0x8286ECA0, kImport = 0x82CE8A6C;
constexpr uint64_t kCallerLR = 0x8286EE20;
constexpr size_t kBytes = 512, kWords = kBytes / 4;
inline bool Selected(uint16_t ordinal, std::string_view name, bool function,
                     bool implemented, uint32_t import, uint64_t full_lr) {
  return function && !implemented && ordinal == 0xA6 && name == "KeSetTimer" &&
         import == kImport && full_lr == kCallerLR;
}

class OneShot {
 public:
  bool Available() const { return !claimed_.load(std::memory_order_relaxed); }
  bool TryClaim() {
    return !claimed_.exchange(true, std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> claimed_{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);

inline bool HostWindow(uintptr_t membase, uintptr_t& host) {
  constexpr auto max = std::numeric_limits<uintptr_t>::max();
  if (!membase || membase > max - kGuestStart - kBytes) {
    return false;
  }
  host = membase + kGuestStart;
  return true;
}

// Public Windows MEM_COMMIT/PAGE_* values, kept dependency-light for tests.
// No guest code or direct host-memory read occurs here.
inline bool ReadableRegion(uintptr_t host, uintptr_t region,
                           size_t region_bytes, uint32_t state,
                           uint32_t protect) {
  if (state != 0x1000 || (protect & (0x100 | 0x01)) || region_bytes < kBytes ||
      host > std::numeric_limits<uintptr_t>::max() - kBytes || host < region ||
      host - region > region_bytes - kBytes) {
    return false;
  }
  switch (protect & 0xff) {
    case 0x02:
    case 0x04:
    case 0x08:
    case 0x20:
    case 0x40:
    case 0x80:
      return true;
    default:
      return false;
  }
}

inline uint32_t Word(const std::array<uint8_t, kBytes>& bytes, size_t index) {
  const size_t at = index * 4;  // Caller supplies index < kWords.
  return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
         uint32_t(bytes[at + 2]) << 8 | uint32_t(bytes[at + 3]);
}
}  // namespace xe::cpu::ppc::timer_callsite_bytes
#endif  // XENIA_CPU_PPC_TIMER_CALLSITE_BYTES_H_
