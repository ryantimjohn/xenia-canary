/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_fault_checkpoint.h"

#include <limits>

namespace xe::cpu::ppc {
namespace {

bool ValidIdentity(const FaultCheckpointIdentity& identity) {
  return identity.code_epoch && identity.module_epoch && identity.owner_epoch &&
         identity.thread_epoch && identity.checkpoint_epoch;
}

bool SameIdentity(const FaultCheckpointIdentity& a,
                  const FaultCheckpointIdentity& b) {
  return a.code_epoch == b.code_epoch && a.module_epoch == b.module_epoch &&
         a.owner_epoch == b.owner_epoch && a.thread_epoch == b.thread_epoch &&
         a.checkpoint_epoch == b.checkpoint_epoch;
}

bool CompleteModeledState(const FaultModeledState& state) {
  const auto& valid = state.modeled_valid;
  if (valid.gpr != std::numeric_limits<uint32_t>::max() ||
      valid.raw_fpr != std::numeric_limits<uint32_t>::max() ||
      valid.scalar != kAllModeledScalars) {
    return false;
  }
  for (uint32_t vmx_valid : valid.vmx128) {
    if (vmx_valid != std::numeric_limits<uint32_t>::max()) {
      return false;
    }
  }
  return state.xer_ca <= 1 && state.xer_ov <= 1 && state.xer_so <= 1 &&
         state.vscr_sat <= 1;
}

bool ValidArchitecturalClaims(const FaultArchitecturalValidity& valid) {
  constexpr uint32_t kSupportedScalars =
      kModeledLr | kModeledCtr | kModeledCr | kModeledVrsave | kModeledMsr;
  constexpr uint32_t kModeledXerBits = (1u << 29) | (1u << 30) | (1u << 31);
  return !(valid.scalar & ~kSupportedScalars) &&
         !(valid.xer_bits & ~kModeledXerBits) && !valid.fpscr_bits;
}

bool ValidInstruction(const FaultScalarAccess& access) {
  if (access.width != 4 || access.rt_rs >= 32 || access.ra >= 32 ||
      (access.guest_pc & 3)) {
    return false;
  }
  // D-form lwz and stw field use follows ppc_emit_memory.cc's instruction
  // semantics. The raw word alone says nothing about native access effects.
  const uint32_t primary_opcode = access.raw_instruction >> 26;
  const FaultOperation decoded = primary_opcode == 32 ? FaultOperation::kLwz
                                 : primary_opcode == 36
                                     ? FaultOperation::kStw
                                     : FaultOperation::kUnknown;
  const uint32_t raw_disp = access.raw_instruction & 0xFFFF;
  const int32_t signed_disp = raw_disp < 0x8000
                                  ? static_cast<int32_t>(raw_disp)
                                  : static_cast<int32_t>(raw_disp) - 0x10000;
  return decoded != FaultOperation::kUnknown && decoded == access.operation &&
         ((access.raw_instruction >> 21) & 31) == access.rt_rs &&
         ((access.raw_instruction >> 16) & 31) == access.ra &&
         signed_disp == access.displacement;
}

bool SupportedAccess(const FaultScalarAccess& access) {
  // Intel SDM Vol. 2B MOV/MOVBE encodings give minimum lengths of two and
  // four bytes here. This does not verify the actual emitted bytes.
  const bool supported_encoding =
      (access.native_access == FaultNativeAccess::kMov &&
       access.native_length >= 2) ||
      (access.native_access == FaultNativeAccess::kMovbe &&
       access.native_length >= 4);
  return access.access_count == 1 && !access.updates_base &&
         !access.has_helper && !access.has_other_effects &&
         supported_encoding && access.native_length <= 15 &&
         access.native_start <=
             std::numeric_limits<uint64_t>::max() - access.native_length;
}

bool ValidMapping(const FaultDirectRamRange& mapping, uint32_t ea) {
  constexpr uint64_t kGuestLimit = uint64_t{1} << 32;
  if (mapping.guest_begin >= mapping.guest_end ||
      mapping.guest_end > kGuestLimit ||
      mapping.host_begin >= mapping.host_end ||
      (mapping.guest_begin & (kFaultPageSize - 1)) ||
      (mapping.guest_end & (kFaultPageSize - 1)) ||
      (mapping.host_begin & (kFaultPageSize - 1)) ||
      (mapping.host_end & (kFaultPageSize - 1)) ||
      mapping.host_end - mapping.host_begin !=
          mapping.guest_end - mapping.guest_begin) {
    return false;
  }
  // The address calculation wraps at 32 bits. The access itself may not wrap
  // or cross a guest page. Existing JIT lowering is not qualified by this.
  const uint64_t access_end = uint64_t{ea} + 4;
  return !(ea & 3) && access_end <= kGuestLimit &&
         (ea / kFaultPageSize) == ((access_end - 1) / kFaultPageSize) &&
         ea >= mapping.guest_begin && access_end <= mapping.guest_end;
}

}  // namespace

FaultCheckpointResult CheckFaultCheckpoint(
    const FaultCheckpoint& checkpoint, const FaultObservation& observation) {
  if (checkpoint.version != kFaultCheckpointVersion ||
      observation.version != kFaultCheckpointVersion ||
      checkpoint.layout != kFaultCheckpointLayout ||
      observation.layout != kFaultCheckpointLayout) {
    return FaultCheckpointResult::kUnsupportedVersion;
  }
  if (!ValidIdentity(checkpoint.identity) ||
      !ValidIdentity(observation.identity) ||
      !SameIdentity(checkpoint.identity, observation.identity)) {
    return FaultCheckpointResult::kInvalidIdentity;
  }
  if (checkpoint.backend_mode != FaultBackendMode::kX64CheckpointedDirectRam ||
      observation.backend_mode != checkpoint.backend_mode ||
      checkpoint.compilation_variant !=
          FaultCompilationVariant::kSingleScalarAccess ||
      observation.compilation_variant != checkpoint.compilation_variant) {
    return FaultCheckpointResult::kUnsupportedMode;
  }
  if (!CompleteModeledState(checkpoint.state)) {
    return FaultCheckpointResult::kIncompleteModeledState;
  }
  if (!ValidArchitecturalClaims(checkpoint.state.architectural_valid)) {
    return FaultCheckpointResult::kInvalidArchitecturalValidity;
  }
  const auto& access = checkpoint.access;
  if (!ValidInstruction(access)) {
    return FaultCheckpointResult::kInvalidInstruction;
  }
  if (!SupportedAccess(access) ||
      checkpoint.memory_kind != FaultMemoryKind::kDirectRam ||
      checkpoint.has_watch) {
    return FaultCheckpointResult::kUnsupportedAccess;
  }
  if (observation.event_kind != FaultEventKind::kSynchronousDataAccess ||
      (observation.host_access != FaultHostAccess::kRead &&
       observation.host_access != FaultHostAccess::kWrite)) {
    return FaultCheckpointResult::kUnsupportedAccess;
  }
  // RA=0 is a literal zero base. The actual r0 remains in the snapshot, even
  // when RT/RS is r0. Only the effective address is narrowed to 32 bits.
  const uint32_t base =
      access.ra ? static_cast<uint32_t>(checkpoint.state.gpr[access.ra]) : 0;
  const uint32_t ea =
      base + static_cast<uint32_t>(static_cast<int32_t>(access.displacement));
  if (!ValidMapping(checkpoint.mapping, ea)) {
    return FaultCheckpointResult::kInvalidMapping;
  }
  const uint64_t host_ea = checkpoint.mapping.host_begin +
                           (uint64_t{ea} - checkpoint.mapping.guest_begin);
  if (checkpoint.guest_pc != access.guest_pc ||
      observation.guest_pc != access.guest_pc ||
      (observation.host_access == FaultHostAccess::kRead) !=
          (access.operation == FaultOperation::kLwz) ||
      checkpoint.guest_ea != ea || observation.guest_ea != ea ||
      observation.original_rip != access.native_start ||
      observation.host_fault_address < host_ea ||
      observation.host_fault_address >= host_ea + 4 ||
      observation.guest_fault_address !=
          ea + (observation.host_fault_address - host_ea)) {
    return FaultCheckpointResult::kObservationMismatch;
  }
  return FaultCheckpointResult::kCandidateRequiresProducerProof;
}

}  // namespace xe::cpu::ppc
