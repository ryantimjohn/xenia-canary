/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_checkpoint_materialization.h"

#include <cstring>
#include <limits>

namespace xe::cpu::ppc {

bool CopyModeledCheckpointStateReference(const PPCContext* input,
                                         FaultModeledState* output) {
  if (!output) {
    return false;
  }
  output->modeled_valid = {};
  output->architectural_valid = {};
  if (!input) {
    return false;
  }

  static_assert(sizeof(input->r[0]) == sizeof(output->gpr[0]));
  static_assert(sizeof(input->f[0]) == sizeof(output->raw_fpr[0]));
  static_assert(sizeof(input->v[0]) == sizeof(output->vmx128[0]));
  static_assert(sizeof(input->vscr_vec) == sizeof(output->vscr_vector));
  for (size_t n = 0; n < output->gpr.size(); ++n) {
    output->gpr[n] = input->r[n];
  }
  output->lr = input->lr;
  output->ctr = input->ctr;
  output->cr = static_cast<uint32_t>(input->cr());
  for (size_t n = 0; n < output->raw_fpr.size(); ++n) {
    std::memcpy(&output->raw_fpr[n], &input->f[n], sizeof(output->raw_fpr[n]));
  }
  for (size_t n = 0; n < output->vmx128.size(); ++n) {
    std::memcpy(output->vmx128[n].data(), &input->v[n], sizeof(input->v[n]));
  }
  output->xer_ca = input->xer_ca != 0;
  output->xer_ov = input->xer_ov != 0;
  output->xer_so = input->xer_so != 0;
  output->fpscr = input->fpscr.value;
  std::memcpy(output->vscr_vector.data(), &input->vscr_vec,
              sizeof(input->vscr_vec));
  output->vscr_sat = input->vscr_sat != 0;
  output->vrsave = input->vrsave;
  output->msr = input->msr;

  // No payload source remains unread when modeled validity is committed.
  FaultModeledValidity complete{};
  complete.gpr = std::numeric_limits<uint32_t>::max();
  complete.raw_fpr = std::numeric_limits<uint32_t>::max();
  complete.vmx128.fill(std::numeric_limits<uint32_t>::max());
  complete.scalar = kAllModeledScalars;
  output->modeled_valid = complete;
  return true;
}

}  // namespace xe::cpu::ppc
