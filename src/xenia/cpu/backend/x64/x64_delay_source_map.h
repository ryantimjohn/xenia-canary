/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_DELAY_SOURCE_MAP_H_
#define XENIA_CPU_BACKEND_X64_X64_DELAY_SOURCE_MAP_H_

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

#include "xenia/cpu/function.h"

namespace xe::cpu::backend::x64::delay_source_map {

constexpr size_t kPauseRunBytes = 16;  // Eight consecutive F3 90 byte pairs.
constexpr size_t kMaxFunctions = 4096;
constexpr size_t kMaxScannedBytes = 16 * 1024 * 1024;
constexpr size_t kMaxRecords = 16;
constexpr size_t kMaxPerFunction = 4;
constexpr size_t kMaxMarkersPerFunction = 65536;

enum class Admission : uint8_t { kAccepted, kFunctionCap, kByteCap, kRecordCap };

// Used only while the diagnostic budget mutex is held. Counters are process
// wide and do not reset when a new title loads in the same process.
struct Budget {
  size_t functions = 0;
  size_t scanned_bytes = 0;
  size_t records = 0;

  Admission Admit(size_t code_length) {
    if (functions >= kMaxFunctions) return Admission::kFunctionCap;
    if (records >= kMaxRecords) return Admission::kRecordCap;
    if (code_length > kMaxScannedBytes - scanned_bytes) {
      return Admission::kByteCap;
    }
    ++functions;
    scanned_bytes += code_length;
    return Admission::kAccepted;
  }

  bool Record() {
    if (records >= kMaxRecords) return false;
    ++records;
    return true;
  }
};

enum class Mapping : uint8_t { kPreceding, kFallback, kUnmapped, kTruncated };

struct Match {
  size_t code_offset = 0;
  Mapping mapping = Mapping::kUnmapped;
  uint32_t guest_address = 0;
  uint32_t hir_offset = 0;
  uint32_t marker_offset = 0;
  uint32_t next_marker_offset = 0;
};

// Returns the first eligible run at or after start. A match is an emitted host
// byte pattern, not proof of a particular guest instruction or guest PC.
bool FindPauseRun(const uint8_t* code, size_t code_length, size_t start,
                  size_t* offset);
Match DescribeMatch(size_t code_offset, size_t code_length,
                    const std::vector<SourceMapEntry>& source_map);
// Covers every byte in the 16-byte candidate. Entry i describes host offset
// run_offset + i, including marker changes within an F3 90 byte pair.
std::array<Match, kPauseRunBytes> DescribeRun(
    size_t run_offset, size_t code_length,
    const std::vector<SourceMapEntry>& source_map);

// Compiler-owned call only, after successful emission, source-map finalization,
// and X64Function::Setup. The disabled path does no scan, allocation, or log.
void MaybeRecord(GuestFunction& function);

}  // namespace xe::cpu::backend::x64::delay_source_map

#endif  // XENIA_CPU_BACKEND_X64_X64_DELAY_SOURCE_MAP_H_
