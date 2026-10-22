/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_delay_source_map.h"

#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(private_delay_source_map_trace, false,
            "Private bounded compiler-side PAUSE run/source-map diagnostic; "
            "does not change emitted code.",
            "x64");
DECLARE_bool(delay_via_maybeyield);

namespace xe::cpu::backend::x64::delay_source_map {
namespace {

// A single compiler at a time owns this diagnostic budget and its logging.
// Enabled-only try_lock avoids adding a compiler stall under contention.
std::mutex budget_mutex;
Budget budget;
bool metadata_emitted = false;
bool truncation_emitted = false;

void LogTruncation(const char* reason) {
  if (!truncation_emitted) {
    truncation_emitted = true;
    XELOGI("XDELAYMAP001 truncated reason={} functions={} scan_bytes={} records={}",
           reason, budget.functions, budget.scanned_bytes, budget.records);
  }
}

}  // namespace

bool FindPauseRun(const uint8_t* code, size_t code_length, size_t start,
                  size_t* offset) {
  if (!code || !offset || code_length < kPauseRunBytes ||
      start > code_length - kPauseRunBytes) {
    return false;
  }
  for (size_t i = start; i <= code_length - kPauseRunBytes; ++i) {
    size_t j = 0;
    for (; j < kPauseRunBytes; j += 2) {
      if (code[i + j] != 0xF3 || code[i + j + 1] != 0x90) {
        break;
      }
    }
    if (j == kPauseRunBytes) {
      *offset = i;
      return true;
    }
  }
  return false;
}

Match DescribeMatch(size_t code_offset, size_t code_length,
                    const std::vector<SourceMapEntry>& source_map) {
  Match result;
  result.code_offset = code_offset;
  if (source_map.size() > kMaxMarkersPerFunction) {
    result.mapping = Mapping::kTruncated;
    return result;
  }
  if (code_offset >= code_length || source_map.empty() ||
      code_length > std::numeric_limits<uint32_t>::max()) {
    return result;
  }
  // A malformed/nonmonotonic map must not yield a qualified association.
  uint32_t previous = 0;
  bool have_previous = false;
  for (const auto& entry : source_map) {
    if (entry.code_offset >= code_length ||
        (have_previous && entry.code_offset < previous)) {
      return result;
    }
    previous = entry.code_offset;
    have_previous = true;
  }
  const SourceMapEntry* preceding = nullptr;
  uint32_t next = static_cast<uint32_t>(code_length);
  for (const auto& entry : source_map) {
    if (entry.code_offset > code_offset) {
      next = entry.code_offset;
      break;
    }
    preceding = &entry;
  }
  if (!preceding) {
    // The existing reverse lookup falls back to the first marker here.
    // Explicitly distinguish that fallback from a preceding marker.
    result.mapping = Mapping::kFallback;
    result.next_marker_offset = source_map.front().code_offset;
    return result;
  }
  result.mapping = Mapping::kPreceding;
  result.guest_address = preceding->guest_address;
  result.hir_offset = preceding->hir_offset;
  result.marker_offset = preceding->code_offset;
  result.next_marker_offset = next;
  return result;
}

std::array<Match, kPauseRunBytes> DescribeRun(
    size_t run_offset, size_t code_length,
    const std::vector<SourceMapEntry>& source_map) {
  std::array<Match, kPauseRunBytes> result{};
  for (size_t i = 0; i < result.size(); ++i) {
    result[i].code_offset = run_offset + i;
  }
  if (run_offset > code_length ||
      kPauseRunBytes > code_length - run_offset ||
      code_length > std::numeric_limits<uint32_t>::max()) {
    return result;
  }
  if (source_map.size() > kMaxMarkersPerFunction) {
    for (auto& entry : result) entry.mapping = Mapping::kTruncated;
    return result;
  }
  if (source_map.empty()) return result;

  uint32_t previous = 0;
  bool have_previous = false;
  for (const auto& entry : source_map) {
    if (entry.code_offset >= code_length ||
        (have_previous && entry.code_offset < previous)) {
      return result;
    }
    previous = entry.code_offset;
    have_previous = true;
  }

  size_t cursor = 0;
  for (auto& byte : result) {
    while (cursor < source_map.size() &&
           source_map[cursor].code_offset <= byte.code_offset) {
      ++cursor;
    }
    if (cursor == 0) {
      byte.mapping = Mapping::kFallback;
      byte.next_marker_offset = source_map.front().code_offset;
      continue;
    }
    const auto& preceding = source_map[cursor - 1];
    byte.mapping = Mapping::kPreceding;
    byte.guest_address = preceding.guest_address;
    byte.hir_offset = preceding.hir_offset;
    byte.marker_offset = preceding.code_offset;
    byte.next_marker_offset = cursor < source_map.size()
                                  ? source_map[cursor].code_offset
                                  : static_cast<uint32_t>(code_length);
  }
  return result;
}

void MaybeRecord(GuestFunction& function) {
  if (!cvars::private_delay_source_map_trace) {
    return;
  }
  // The enabled diagnostic may lose functions under budget contention.
  std::unique_lock<std::mutex> lock(budget_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  if (!metadata_emitted) {
    metadata_emitted = true;
    XELOGI("XDELAYMAP001 begin delay_via_maybeyield={} max_functions={} "
           "max_scan_bytes={} max_records={} max_per_function={} "
           "pattern=eight-F390 enabled_only=true",
           bool(cvars::delay_via_maybeyield), kMaxFunctions, kMaxScannedBytes,
           kMaxRecords, kMaxPerFunction);
  }
  const auto* code = function.machine_code();
  const size_t length = function.machine_code_length();
  if (!code) {
    LogTruncation("missing_code");
    return;
  }
  const Admission admission = budget.Admit(length);
  if (admission != Admission::kAccepted) {
    LogTruncation(admission == Admission::kFunctionCap
                      ? "function_cap"
                      : admission == Admission::kRecordCap ? "record_cap"
                                                         : "byte_cap");
    return;
  }
  size_t next_search = 0;
  size_t per_function = 0;
  size_t offset = 0;
  while (per_function < kMaxPerFunction &&
         budget.records < kMaxRecords &&
         FindPauseRun(code, length, next_search, &offset)) {
    const auto mappings = DescribeRun(offset, length, function.source_map());
    // Sixteen fixed 36-character fields, never a whole source-map dump.
    // Field: byte index, state, guest, HIR, preceding marker, next marker.
    std::array<char, kPauseRunBytes * 36 + 1> map_text{};
    size_t used = 0;
    for (size_t i = 0; i < mappings.size(); ++i) {
      const auto& mapping = mappings[i];
      const char state = mapping.mapping == Mapping::kPreceding
                             ? 'P'
                             : mapping.mapping == Mapping::kFallback
                                   ? 'F'
                                   : mapping.mapping == Mapping::kTruncated
                                         ? 'T'
                                         : 'U';
      const int written = std::snprintf(
          map_text.data() + used, map_text.size() - used,
          "%02X%c%08X%08X%08X%08X;", static_cast<unsigned>(i), state,
          mapping.guest_address, mapping.hir_offset, mapping.marker_offset,
          mapping.next_marker_offset);
      if (written != 36) {
        // No partial mapping is emitted on an unexpected formatting failure.
        used = 0;
        break;
      }
      used += 36;
    }
    XELOGI("XDELAYMAP001 match host_begin={:016X} code_length={} "
           "guest_function={:08X} host_offset={} map16={}",
           reinterpret_cast<uintptr_t>(code), length, function.address(),
           offset, used ? std::string_view(map_text.data(), used)
                        : std::string_view("format_error"));
    ++per_function;
    budget.Record();
    next_search = offset + kPauseRunBytes;
  }
  if (per_function == kMaxPerFunction &&
      FindPauseRun(code, length, next_search, &offset)) {
    LogTruncation("per_function_cap");
  } else if (budget.records == kMaxRecords) {
    LogTruncation("record_cap");
  }
}

}  // namespace xe::cpu::backend::x64::delay_source_map
