/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_CPU_PRIVATE_DELAY_GUEST_SOURCE_H_
#define XENIA_CPU_PRIVATE_DELAY_GUEST_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xe::cpu::private_delay_guest_source {

constexpr uint32_t kSelectedFunction = 0x829E6360;
constexpr size_t kMaxWords = 64;
constexpr size_t kMaxTextBytes = 640;
constexpr std::string_view kTag = "XGUESTWORDS001:";

struct Record {
  uint32_t function = 0;
  uint32_t end = 0;
  uint32_t first = 0;
  size_t count = 0;
  bool partial = false;
};

inline bool TakeLiteral(std::string_view* input, std::string_view literal) {
  if (!input->starts_with(literal)) {
    return false;
  }
  input->remove_prefix(literal.size());
  return true;
}

inline bool TakeHex(std::string_view* input, size_t digits, uint32_t* value) {
  if (input->size() < digits) {
    return false;
  }
  uint32_t parsed = 0;
  for (size_t i = 0; i < digits; ++i) {
    const char ch = (*input)[i];
    uint32_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = uint32_t(ch - '0');
    } else if (ch >= 'A' && ch <= 'F') {
      digit = uint32_t(ch - 'A' + 10);
    } else {
      return false;
    }
    parsed = (parsed << 4) | digit;
  }
  input->remove_prefix(digits);
  *value = parsed;
  return true;
}

// The carrier is owned by FunctionDebugInfo. Validate its complete bounded
// grammar before joining it to an emitted host range; no guest address is
// inferred from the host PAUSE bytes alone.
inline bool ParseRecord(std::string_view input, uint32_t expected_end,
                        Record* result) {
  if (!result || input.size() > kMaxTextBytes || !TakeLiteral(&input, kTag)) {
    return false;
  }
  Record parsed;
  uint32_t count = 0;
  uint32_t partial = 0;
  uint32_t contiguous = 0;
  if (!TakeLiteral(&input, "f=") || !TakeHex(&input, 8, &parsed.function) ||
      !TakeLiteral(&input, ":e=") || !TakeHex(&input, 8, &parsed.end) ||
      !TakeLiteral(&input, ":a=") || !TakeHex(&input, 8, &parsed.first) ||
      !TakeLiteral(&input, ":n=") || !TakeHex(&input, 2, &count) ||
      !TakeLiteral(&input, ":p=") || !TakeHex(&input, 1, &partial) ||
      !TakeLiteral(&input, ":c=") || !TakeHex(&input, 1, &contiguous) ||
      !TakeLiteral(&input, ":w=")) {
    return false;
  }
  if (parsed.function != kSelectedFunction || parsed.first != parsed.function ||
      parsed.end != expected_end || parsed.end < parsed.first ||
      (parsed.end - parsed.first) % 4 || count == 0 || count > kMaxWords ||
      partial > 1 || contiguous != 1) {
    return false;
  }
  const size_t function_words = size_t((parsed.end - parsed.first) / 4) + 1;
  if (count != (function_words < kMaxWords ? function_words : kMaxWords) ||
      bool(partial) != (function_words > kMaxWords) ||
      input.size() != size_t(count) * 8) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    uint32_t ignored = 0;
    if (!TakeHex(&input, 8, &ignored)) {
      return false;
    }
  }
  if (!input.empty()) {
    return false;
  }
  parsed.count = count;
  parsed.partial = bool(partial);
  *result = parsed;
  return true;
}

}  // namespace xe::cpu::private_delay_guest_source

#endif  // XENIA_CPU_PRIVATE_DELAY_GUEST_SOURCE_H_
