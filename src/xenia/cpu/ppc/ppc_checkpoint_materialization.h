/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_CHECKPOINT_MATERIALIZATION_H_
#define XENIA_CPU_PPC_PPC_CHECKPOINT_MATERIALIZATION_H_

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_fault_checkpoint.h"

namespace xe::cpu::ppc {

// Offline/reference producer only. The caller assumes that input is a complete,
// canonical modeled input and exclusively owns both objects on this thread.
// This function does not establish that a live PPCContext is synchronized with
// native registers, nor establish ordering, publication or fault eligibility.
// Input and output must be distinct, nonoverlapping live objects. Raw FPR and
// vector storage is copied without numeric conversion or guest serialization.
// CR uses the low bit of each modeled byte, as PPCContext::cr() does. XER flags
// and split VSCR saturation use Boolean (nonzero) normalization.
// Validity is cleared before copying and committed only after the full payload;
// every architectural-validity claim stays zero. A null input invalidates a
// nonnull output and returns false. A null output returns false.
bool CopyModeledCheckpointStateReference(const PPCContext* input,
                                         FaultModeledState* output);

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_PPC_CHECKPOINT_MATERIALIZATION_H_
