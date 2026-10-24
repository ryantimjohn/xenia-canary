/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_VIRTUAL_UNWIND_H_
#define XENIA_CPU_PPC_PPC_VIRTUAL_UNWIND_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "xenia/cpu/exception_metadata.h"

namespace xe::cpu::ppc {

// This API consumes caller-owned copies synchronously. The caller must have
// captured metadata and code from the same retained module/code generation,
// and registers and stack from one precise, stable thread capture. Control
// must have entered through the recognized prologue; direct calls must return
// normally and preserve ABI nonvolatile state and this frame's save slots.
// Local code inspection cannot establish those external conditions. Neither
// an identity tag nor a host fault record establishes capture coherence.
enum class UnwindCaptureProvenance {
  kUnknown,
  kCoherentPrecise,
};

enum class VirtualUnwindStatus {
  kSupportedPartialFrame,
  kUnsupportedPattern,
  kUnsupportedPhase,
  kMissingRegister,
  kBounds,
  kInconsistentFrame,
  kResourceLimit,
  kUntrustedCapture,
};

struct VirtualUnwindLimits {
  size_t max_code_instructions = 256;
  size_t max_stack_read_bytes = 32;
  size_t max_stack_reads = 4;
  uint32_t max_frame_bytes = 4096;
};

struct OwnedCodeSnapshotView {
  uint32_t begin = 0;
  std::span<const uint8_t> bytes;
  UnwindCaptureProvenance provenance = UnwindCaptureProvenance::kUnknown;
};

struct OwnedStackSnapshotView {
  uint32_t begin = 0;
  std::span<const uint8_t> bytes;
  UnwindCaptureProvenance provenance = UnwindCaptureProvenance::kUnknown;
};

struct VirtualUnwindRegisters {
  uint32_t control_pc = 0;  // Exact instruction address, before its effects.
  UnwindCaptureProvenance provenance = UnwindCaptureProvenance::kUnknown;
  std::array<uint64_t, 32> gpr{};
  uint32_t low32_valid = 0;
  uint32_t full64_valid = 0;  // Also permits use of its low 32 bits.
};

struct RecipeBuildResult;
struct VirtualUnwindResult;

// Only BuildBoundedUnwindRecipe can create a usable recipe. It owns the
// complete bounded code evidence and retains no guest or host code pointer.
class BoundedUnwindRecipe {
 public:
  BoundedUnwindRecipe(const BoundedUnwindRecipe&) = default;
  BoundedUnwindRecipe(BoundedUnwindRecipe&&) = default;
  BoundedUnwindRecipe& operator=(const BoundedUnwindRecipe&) = default;
  BoundedUnwindRecipe& operator=(BoundedUnwindRecipe&&) = default;

  uint32_t function_begin() const { return begin_; }
  uint64_t function_end() const { return end_; }
  uint32_t body_begin() const { return body_begin_; }
  uint32_t body_end() const { return body_end_; }
  uint32_t frame_bytes() const { return frame_bytes_; }

 private:
  BoundedUnwindRecipe() = default;
  friend RecipeBuildResult BuildBoundedUnwindRecipe(
      const ExceptionMetadataFunction&, const OwnedCodeSnapshotView&,
      const VirtualUnwindLimits&);
  friend VirtualUnwindResult TryVirtualUnwindFrame(
      const BoundedUnwindRecipe&, const VirtualUnwindRegisters&,
      const OwnedStackSnapshotView&, const VirtualUnwindLimits&);

  bool valid_ = false;
  bool saves_r31_ = false;
  uint32_t begin_ = 0;
  uint64_t end_ = 0;
  uint32_t body_begin_ = 0;
  uint32_t body_end_ = 0;
  uint32_t frame_bytes_ = 0;
  uint32_t lr_offset_ = 0;
  uint32_t r31_offset_ = 0;
  size_t instruction_count_ = 0;
  std::array<uint32_t, 256> instructions_{};
  size_t store_count_ = 0;
  std::array<uint32_t, 256> constant_stores_{};
};

struct RecipeBuildResult {
  VirtualUnwindStatus status = VirtualUnwindStatus::kUnsupportedPattern;
  std::optional<BoundedUnwindRecipe> recipe;
};

struct VirtualUnwindResult {
  VirtualUnwindStatus status = VirtualUnwindStatus::kUnsupportedPattern;
  // All fields stay zero and unusable on failure.
  uint32_t caller_sp_address32 = 0;
  uint32_t return_address32 = 0;  // Exact saved LR; no implicit PC adjustment.
  uint64_t recovered_r31_full64 = 0;
  bool recovered_r31_valid = false;
};

RecipeBuildResult BuildBoundedUnwindRecipe(
    const ExceptionMetadataFunction& function,
    const OwnedCodeSnapshotView& code, const VirtualUnwindLimits& limits = {});

VirtualUnwindResult TryVirtualUnwindFrame(
    const BoundedUnwindRecipe& recipe, const VirtualUnwindRegisters& registers,
    const OwnedStackSnapshotView& stack,
    const VirtualUnwindLimits& limits = {});

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_PPC_VIRTUAL_UNWIND_H_
