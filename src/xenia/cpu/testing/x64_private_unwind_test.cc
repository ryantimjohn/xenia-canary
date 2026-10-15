/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/x64/x64_code_cache.h"

namespace xe::cpu::testing {
namespace {

using backend::EmitFunctionInfo;
using backend::x64::X64CodeCache;
constexpr uint8_t kSentinel = 0xA5;
constexpr size_t kCapacity = 16;

EmitFunctionInfo Info(size_t stack) {
  EmitFunctionInfo info{};
  info.code_size = {8, 52, 4, 0, 64};
  info.prolog_stack_alloc_offset = stack ? 8 : 0;
  info.stack_size = stack;
  return info;
}

struct GuardedStorage {
  alignas(16) std::array<uint8_t, 48> bytes;
  GuardedStorage() { bytes.fill(kSentinel); }
  std::span<uint8_t> output() { return {bytes.data() + 16, kCapacity}; }
  void CheckGuards() const {
    REQUIRE(std::all_of(bytes.begin(), bytes.begin() + 16,
                        [](uint8_t byte) { return byte == kSentinel; }));
    REQUIRE(std::all_of(bytes.begin() + 32, bytes.end(),
                        [](uint8_t byte) { return byte == kSentinel; }));
  }
};

TEST_CASE("Private Windows unwind encoding preserves legacy byte forms",
          "[cpu][code-cache-prepare][x64-private-unwind]") {
  // No cache Initialize: this operation must not depend on mapped code,
  // runtime-table allocation/counts, placement or registration.
  auto cache = X64CodeCache::Create();
  REQUIRE(cache);
  REQUIRE(cache->QueryUnwindStorageSize() == kCapacity);
  for (const size_t stack :
       {size_t{0}, size_t{8}, size_t{128}, size_t{136}, size_t{65528}}) {
    GuardedStorage guarded;
    const auto info = Info(stack);
    REQUIRE(cache->SerializeUnwindInfo(info, guarded.output()));
    guarded.CheckGuards();
    const auto bytes = guarded.output();
    REQUIRE(bytes[0] == 1);
    REQUIRE(bytes[1] == 8);
    REQUIRE(bytes[3] == 0);
    if (!stack) {
      REQUIRE(bytes[2] == 0);
      REQUIRE(std::all_of(bytes.begin() + 4, bytes.end(),
                          [](uint8_t byte) { return byte == kSentinel; }));
    } else if (stack <= 128) {
      REQUIRE(bytes[2] == 1);
      REQUIRE(bytes[4] == 8);
      REQUIRE(bytes[5] == (stack == 8 ? 0x02 : 0xF2));
      REQUIRE(bytes[6] == kSentinel);
      REQUIRE(bytes[7] == kSentinel);
      REQUIRE(bytes[8] == 0);
      REQUIRE(bytes[9] == 0);
      REQUIRE(std::all_of(bytes.begin() + 10, bytes.end(),
                          [](uint8_t byte) { return byte == kSentinel; }));
    } else {
      REQUIRE(bytes[2] == 2);
      REQUIRE(bytes[4] == 8);
      REQUIRE(bytes[5] == 1);  // UWOP_ALLOC_LARGE, scaled 16-bit size.
      REQUIRE(bytes[6] == (stack == 136 ? 0x11 : 0xFF));
      REQUIRE(bytes[7] == (stack == 136 ? 0x00 : 0x1F));
      REQUIRE(std::all_of(bytes.begin() + 8, bytes.end(),
                          [](uint8_t byte) { return byte == kSentinel; }));
    }
  }
  GuardedStorage boundary;
  auto info = Info(104);
  info.code_size = {255, 253, 4, 0, 512};
  info.prolog_stack_alloc_offset = 255;
  REQUIRE(cache->SerializeUnwindInfo(info, boundary.output()));
  boundary.CheckGuards();
  REQUIRE(boundary.output()[1] == 255);
  REQUIRE(boundary.output()[4] == 255);
}

TEST_CASE("Private Windows unwind encoding rejects before any output write",
          "[cpu][code-cache-prepare][x64-private-unwind]") {
  auto cache = X64CodeCache::Create();
  REQUIRE(cache);
  auto reject = [&](const EmitFunctionInfo& info, size_t capacity = kCapacity,
                    size_t shift = 0) {
    GuardedStorage guarded;
    REQUIRE_FALSE(cache->SerializeUnwindInfo(
        info, {guarded.bytes.data() + 16 + shift, capacity}));
    REQUIRE(std::all_of(guarded.bytes.begin(), guarded.bytes.end(),
                        [](uint8_t byte) { return byte == kSentinel; }));
  };
  for (size_t capacity = 0; capacity < kCapacity; ++capacity) {
    reject(Info(104), capacity);
  }
  REQUIRE_FALSE(cache->SerializeUnwindInfo(Info(104), {}));
  reject(Info(104), kCapacity, 1);  // Unaligned native struct storage.
  for (const auto stack :
       {size_t{1}, size_t{7}, size_t{129}, size_t{65535}, size_t{65536},
        size_t{65544}, size_t{524280}, std::numeric_limits<size_t>::max()}) {
    // In particular, reject the cast-before-divide boundary privately without
    // changing how the ordinary legacy writer handles its existing inputs.
    reject(Info(stack));
  }
  auto info = Info(104);
  info.code_size.total = 0;
  reject(info);
  info = Info(104);
  info.code_size = {256, 252, 4, 0, 512};
  reject(info);
  info = Info(104);
  info.prolog_stack_alloc_offset = 256;
  reject(info);
  info = Info(104);
  info.prolog_stack_alloc_offset = 9;
  reject(info);
  info = Info(104);
  info.prolog_stack_alloc_offset = 0;
  reject(info);
  info = Info(0);
  info.prolog_stack_alloc_offset = 1;
  reject(info);
  info = Info(104);
  info.code_size.body = std::numeric_limits<size_t>::max();
  reject(info);
  info = Info(104);
  --info.code_size.total;
  reject(info);
  info = Info(104);
  ++info.code_size.total;
  reject(info);
}

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
