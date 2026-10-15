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

#include <array>
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

// Compile-lifetime handles owned by this HIRBuilder. Emit changes local Values
// into numeric stack offsets; neither form may outlive/reset the builder. These
// slots are not an invocation record, a FaultModeledState or a precision claim.
struct DormantCheckpointGprCapture {
  std::array<hir::Value*, 32> gpr_slots{};
  hir::Value* ea_slot = nullptr;
};

// Standalone, dormant capture-only HIR; never composed with checkpoint pairs.
// Appends 32 INT64 loads, ordered stores to distinct locals, then an INT32
// uint32(pre-access RA + signed displacement) local (literal zero for RA=0).
// Promotion may replace loads with the current SSA values. The caller must
// retain stores through its optimizer and prove emitted effects separately.
// Requires a current builder, empty output/locals, one unlabelled entry block,
// and a final SOURCE_OFFSET matching guest_pc. The prefix accepts only aligned
// increasing source markers and typed GPR load/store/plain INT64 ADD
// operations. Rejects calls, joins, effects and unsupported D-form words before
// mutation; rejected input leaves builder and output unchanged. No guest-memory
// access is emitted (only context reads and function-local writes). This does
// not relax pair verification or any profile/backend/validity gate.
bool BuildDormantCheckpointGprCapture(hir::HIRBuilder& builder,
                                      uint32_t guest_pc,
                                      uint32_t raw_instruction,
                                      DormantCheckpointGprCapture* output);

// Separate, equally dormant construction-time alternative for exactly two
// unlabelled blocks. The first has the same pure typed GPR prefix, ending in
// SOURCE_OFFSET/constant SET_RETURN_ADDRESS/constant LR store/direct CALL with
// flags zero; the second starts at that return PC and has the entry helper's
// same-block prefix. No other transfer, labels, analysis edges or locals are
// accepted. Fresh context loads represent the returning call's current context,
// not pre-call SSA; this assumes the ordinary returning guest-call contract.
// All rejection precedes mutation. Later construction can invalidate this
// shape: callers must independently prove finalized CFG and emitted effects.
// This neither admits a game CFG nor proves callee runtime behavior or CALL
// lowering. The entry helper and checkpoint-pair restrictions remain unchanged.
bool BuildDormantPostCallCheckpointGprCapture(
    hir::HIRBuilder& builder, uint32_t guest_pc, uint32_t raw_instruction,
    DormantCheckpointGprCapture* output);

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
