/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_EXCEPTION_METADATA_H_
#define XENIA_CPU_EXCEPTION_METADATA_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xe::cpu {

enum class ExceptionMetadataStatus {
  kValid,
  kMissing,
  kInvalid,
  kOpaque,
  kResourceLimit,
};

struct ExceptionMetadataSection {
  std::string name;
  uint32_t begin = 0;
  uint64_t end = 0;  // Exclusive; may be 0x100000000.
  bool readable = false;
  bool executable = false;
  uint32_t raw_flags = 0;
  bool available = true;
  std::string availability_diagnostic;
};

struct ExceptionMetadataImport {
  std::string library;
  uint32_t ordinal = 0;
  uint32_t thunk_address = 0;
};

struct ExceptionMetadataScope {
  uint32_t begin = 0;
  uint32_t end = 0;
  uint32_t filter_or_handler = 0;
  uint32_t jump_target = 0;
  ExceptionMetadataStatus status = ExceptionMetadataStatus::kInvalid;
  // Small/sentinel values and a zero jump remain uninterpreted roles.
  bool opaque_roles = false;
};

struct ExceptionMetadataHandler {
  uint32_t address = 0;
  uint32_t data_address = 0;
  bool imported_c_specific = false;
  ExceptionMetadataStatus status = ExceptionMetadataStatus::kOpaque;
  std::vector<ExceptionMetadataScope> scopes;
};

struct ExceptionMetadataFunction {
  uint32_t table_address = 0;
  uint32_t begin = 0;
  uint32_t raw_info = 0;
  // Only the evidenced bit30-set packed form is decoded. These are metadata
  // candidates, not a claim of complete console virtual-unwind semantics.
  bool packed_candidate = false;
  uint32_t prologue_instructions = 0;
  uint32_t length_instructions = 0;
  uint64_t end = 0;
  ExceptionMetadataStatus status = ExceptionMetadataStatus::kOpaque;
  std::optional<ExceptionMetadataHandler> handler;
};

struct ExceptionMetadataFrame {
  uint32_t pc = 0;
  ExceptionMetadataStatus status = ExceptionMetadataStatus::kMissing;
  std::optional<ExceptionMetadataFunction> function;
  std::vector<ExceptionMetadataScope> matching_scopes;
};

struct ExceptionMetadataSnapshot {
  uint32_t image_base = 0;
  uint64_t image_end = 0;
  ExceptionMetadataStatus status = ExceptionMetadataStatus::kMissing;
  bool lookup_valid = false;
  std::vector<std::string> diagnostics;
  std::vector<ExceptionMetadataSection> sections;
  std::vector<ExceptionMetadataImport> imports;
  std::vector<ExceptionMetadataFunction> functions;

  // Returns copied values. Never extends a packed range into padding or
  // invents a range for an opaque form. Frame PCs are supplied by the caller;
  // this operation neither unwinds frames nor chooses a continuation.
  ExceptionMetadataFrame LookupFrame(uint32_t pc) const;
  std::vector<ExceptionMetadataFrame> DescribeFrames(
      std::span<const uint32_t> pcs) const;
};

struct ExceptionMetadataLimits {
  size_t max_functions = 1 << 20;
  size_t max_scopes_per_handler = 4096;
  uint64_t max_read_bytes = 64 << 20;
};

// Reader and borrowed input spans are consumed synchronously. None is retained
// in the returned owned snapshot. The parser checks section bounds before each
// exact read; the adapter must additionally check current memory readability.
struct ExceptionMetadataInput {
  uint32_t image_base = 0;
  uint64_t image_end = 0;
  std::span<const ExceptionMetadataSection> sections;
  std::span<const ExceptionMetadataImport> imports;
  std::function<bool(uint32_t, std::span<uint8_t>)> read;
  ExceptionMetadataLimits limits;
};

ExceptionMetadataSnapshot ParseExceptionMetadata(
    const ExceptionMetadataInput& input);

}  // namespace xe::cpu

#endif  // XENIA_CPU_EXCEPTION_METADATA_H_
