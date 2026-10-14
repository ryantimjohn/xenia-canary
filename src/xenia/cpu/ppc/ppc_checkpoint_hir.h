/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_CHECKPOINT_HIR_H_
#define XENIA_CPU_PPC_PPC_CHECKPOINT_HIR_H_

#include <cstdint>

#include "xenia/cpu/hir/hir_builder.h"

namespace xe::cpu::ppc {

enum class CheckpointHIRResult {
  kNoPairs,
  kInvalidIdentity,
  kUnsupportedInstruction,
  kInvalidOperands,
  kInvalidOrdering,
  kStructuralPairRequiresFullProducerProof,
};

// Constructs retained, non-executable requests for D-form lwz/stw. The pair ID
// is unique within this HIR function, not a runtime epoch or lifetime proof.
// The request owns numeric guest PC/raw instruction data; it does NOT store a
// complete modeled state, reserve invocation storage, or publish a checkpoint.
// Failure for an unsupported word/PC/ID appends no instructions.
// As for other HIR construction, the caller must make this builder current.
bool BuildCheckpointScalarPair(hir::HIRBuilder& builder, uint64_t pair_id,
                               uint32_t guest_pc, uint32_t raw_instruction);

// Structural verification only, including exact request/access adjacency.
// No result grants native lowering, precise context capture, fault eligibility,
// or guest dispatch. Full modeled materialization, invocation ownership,
// immutable native descriptors and coherent publication are prerequisites.
CheckpointHIRResult VerifyCheckpointHIR(const hir::HIRBuilder& builder);

// Backends must reject every dedicated opcode, even malformed/orphaned ones,
// before emission or code placement. This is independent of structural
// validity.
bool ContainsCheckpointHIR(const hir::HIRBuilder& builder);

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_PPC_CHECKPOINT_HIR_H_
