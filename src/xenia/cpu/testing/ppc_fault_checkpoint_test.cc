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

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

using Result = FaultCheckpointResult;

uint32_t DForm(uint32_t opcode, uint8_t rt_rs, uint8_t ra,
               int16_t displacement) {
  return (opcode << 26) | (uint32_t{rt_rs} << 21) | (uint32_t{ra} << 16) |
         static_cast<uint16_t>(displacement);
}

struct Fixture {
  FaultCheckpoint checkpoint{};
  FaultObservation observation{};

  Fixture() {
    checkpoint.version = kFaultCheckpointVersion;
    checkpoint.layout = kFaultCheckpointLayout;
    checkpoint.identity = {1, 2, 3, 4, 5};
    checkpoint.backend_mode = FaultBackendMode::kX64CheckpointedDirectRam;
    checkpoint.compilation_variant =
        FaultCompilationVariant::kSingleScalarAccess;
    checkpoint.guest_pc = 0x80001000;
    auto& state = checkpoint.state;
    state.modeled_valid.gpr = std::numeric_limits<uint32_t>::max();
    state.modeled_valid.raw_fpr = std::numeric_limits<uint32_t>::max();
    state.modeled_valid.vmx128.fill(std::numeric_limits<uint32_t>::max());
    state.modeled_valid.scalar = kAllModeledScalars;
    state.gpr[4] = 0x1000;
    auto& access = checkpoint.access;
    access.guest_pc = checkpoint.guest_pc;
    access.raw_instruction = DForm(32, 3, 4, 16);
    access.operation = FaultOperation::kLwz;
    access.width = 4;
    access.rt_rs = 3;
    access.ra = 4;
    access.displacement = 16;
    checkpoint.guest_ea = 0x1010;
    access.native_start = 0x5000;
    access.native_length = 4;
    access.native_access = FaultNativeAccess::kMovbe;
    checkpoint.memory_kind = FaultMemoryKind::kDirectRam;
    access.access_count = 1;
    checkpoint.mapping = {0x1000, 0x2000, 0x10000000, 0x10001000};
    observation.version = checkpoint.version;
    observation.layout = checkpoint.layout;
    observation.identity = checkpoint.identity;
    observation.backend_mode = checkpoint.backend_mode;
    observation.compilation_variant = checkpoint.compilation_variant;
    observation.event_kind = FaultEventKind::kSynchronousDataAccess;
    observation.host_access = FaultHostAccess::kRead;
    observation.guest_pc = access.guest_pc;
    observation.guest_ea = checkpoint.guest_ea;
    observation.guest_fault_address = checkpoint.guest_ea;
    observation.original_rip = access.native_start;
    observation.host_fault_address = 0x10000010;
  }

  Result Check() const { return CheckFaultCheckpoint(checkpoint, observation); }
};

TEST_CASE("Fault checkpoint only identifies an unproven structural candidate",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  f.observation.host_fault_address += 3;
  f.observation.guest_fault_address += 3;
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  f.observation.guest_fault_address--;
  CHECK(f.Check() == Result::kObservationMismatch);
}

TEST_CASE("One compiled access describes different owned invocations",
          "[cpu][fault-checkpoint]") {
  Fixture first;
  Fixture second;
  const FaultScalarAccess compiled_access = first.checkpoint.access;
  second.checkpoint.access = compiled_access;
  second.checkpoint.identity.checkpoint_epoch = 6;
  second.observation.identity = second.checkpoint.identity;
  second.checkpoint.state.gpr[4] = 0x2000;
  second.checkpoint.guest_ea = 0x2010;
  second.checkpoint.mapping = {0x2000, 0x3000, 0x20000000, 0x20001000};
  second.observation.guest_ea = 0x2010;
  second.observation.guest_fault_address = 0x2010;
  second.observation.host_fault_address = 0x20000010;
  CHECK(first.Check() == Result::kCandidateRequiresProducerProof);
  CHECK(second.Check() == Result::kCandidateRequiresProducerProof);
  CHECK(first.checkpoint.access.native_start ==
        second.checkpoint.access.native_start);
  CHECK(first.checkpoint.access.raw_instruction ==
        second.checkpoint.access.raw_instruction);
}

TEST_CASE("Fault checkpoint does not mutate old source or base values",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  auto& access = f.checkpoint.access;
  access.ra = access.rt_rs = 3;
  access.raw_instruction = DForm(32, 3, 3, 16);
  f.checkpoint.guest_ea = 0x11;
  f.checkpoint.state.gpr[3] = 1;
  f.checkpoint.mapping = {0, 0x1000, 0x10000000, 0x10001000};
  f.observation.guest_ea = 0x11;
  f.observation.guest_fault_address = 0x11;
  f.observation.host_fault_address = 0x10000011;
  // The unaligned address is rejected without rewriting the old RT/RA.
  CHECK(f.Check() == Result::kInvalidMapping);
  f.checkpoint.state.gpr[3] = 0xABCDEF0100001000ULL;
  f.checkpoint.guest_ea = 0x1010;
  f.checkpoint.mapping = {0x1000, 0x2000, 0x10000000, 0x10001000};
  f.observation.guest_ea = 0x1010;
  f.observation.guest_fault_address = 0x1010;
  f.observation.host_fault_address = 0x10000010;
  const uint64_t old_rt_ra = f.checkpoint.state.gpr[3];
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  CHECK(f.checkpoint.state.gpr[3] == old_rt_ra);

  access.operation = FaultOperation::kStw;
  access.raw_instruction = DForm(36, 3, 3, 16);
  f.observation.host_access = FaultHostAccess::kWrite;
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  CHECK(f.checkpoint.state.gpr[3] == old_rt_ra);
}

TEST_CASE("Fault checkpoint uses zero base without discarding r0",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  auto& access = f.checkpoint.access;
  f.checkpoint.state.gpr[0] = 0xDEADBEEF12345678ULL;
  access.ra = 0;
  access.displacement = 0x1010;
  access.raw_instruction = DForm(32, 3, 0, 0x1010);
  const uint64_t old_r0 = f.checkpoint.state.gpr[0];
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  CHECK(f.checkpoint.state.gpr[0] == old_r0);
}

TEST_CASE("Fault checkpoint address addition narrows to 32 bits",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.checkpoint.state.gpr[4] = 0xCAFEBABEFFFFF000ULL;
  f.checkpoint.access.displacement = 0x2010;
  f.checkpoint.access.raw_instruction = DForm(32, 3, 4, 0x2010);
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  f.checkpoint.access.displacement = -16;
  f.checkpoint.access.raw_instruction = DForm(32, 3, 4, -16);
  f.checkpoint.state.gpr[4] = 0x1020;
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
}

TEST_CASE("Fault checkpoint requires exact native access start",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.observation.original_rip++;
  CHECK(f.Check() == Result::kObservationMismatch);
  f.observation.original_rip = f.checkpoint.access.native_start;
  f.checkpoint.access.native_start = std::numeric_limits<uint64_t>::max() - 1;
  CHECK(f.Check() == Result::kUnsupportedAccess);
}

TEST_CASE("Fault checkpoint checks raw operation and fields",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.checkpoint.access.raw_instruction = DForm(33, 3, 4, 16);
  CHECK(f.Check() == Result::kInvalidInstruction);
  f.checkpoint.access.raw_instruction = DForm(36, 3, 4, 16);
  CHECK(f.Check() == Result::kInvalidInstruction);
  f.checkpoint.access.raw_instruction = DForm(32, 2, 4, 16);
  CHECK(f.Check() == Result::kInvalidInstruction);
  f.checkpoint.access.raw_instruction = DForm(32, 3, 5, 16);
  CHECK(f.Check() == Result::kInvalidInstruction);
  f.checkpoint.access.raw_instruction = DForm(32, 3, 4, 20);
  CHECK(f.Check() == Result::kInvalidInstruction);
  f.checkpoint.access.raw_instruction = DForm(32, 3, 4, 16);
  f.checkpoint.access.width = 8;
  CHECK(f.Check() == Result::kInvalidInstruction);
}

TEST_CASE("Fault checkpoint bounds the entire one-page access",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.checkpoint.state.gpr[4] = 0x1FEE;
  CHECK(f.Check() == Result::kInvalidMapping);
  f.checkpoint.state.gpr[4] = 0x1000;
  f.checkpoint.mapping.guest_end = 0x100001000ULL;
  CHECK(f.Check() == Result::kInvalidMapping);
  f.checkpoint.mapping.guest_end = 0x2000;
  f.checkpoint.mapping.host_end = std::numeric_limits<uint64_t>::max();
  CHECK(f.Check() == Result::kInvalidMapping);
}

TEST_CASE("Fault checkpoint handles the final guest word without wrapping",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.checkpoint.mapping = {0xFFFFF000, 0x100000000ULL, 0x10000000, 0x10001000};
  f.checkpoint.state.gpr[4] = 0xFFFFFFEC;
  f.checkpoint.guest_ea = 0xFFFFFFFC;
  f.observation.guest_ea = 0xFFFFFFFC;
  f.observation.guest_fault_address = 0xFFFFFFFC;
  f.observation.host_fault_address = 0x10000FFC;
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
  f.checkpoint.state.gpr[4] = 0xFFFFFFEE;
  CHECK(f.Check() == Result::kInvalidMapping);
}

TEST_CASE("Fault checkpoint rejects stale identity and unsupported paths",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.checkpoint.version = 0;
  CHECK(f.Check() == Result::kUnsupportedVersion);
  f.checkpoint.version = kFaultCheckpointVersion;
  f.observation.identity.thread_epoch++;
  CHECK(f.Check() == Result::kInvalidIdentity);
  f.observation.identity = f.checkpoint.identity;
  f.checkpoint.identity.code_epoch = 0;
  CHECK(f.Check() == Result::kInvalidIdentity);
  f.checkpoint.identity = f.observation.identity;
  f.checkpoint.compilation_variant = FaultCompilationVariant::kUnknown;
  CHECK(f.Check() == Result::kUnsupportedMode);
  f.checkpoint.compilation_variant =
      FaultCompilationVariant::kSingleScalarAccess;
  f.checkpoint.backend_mode = FaultBackendMode::kUnknown;
  CHECK(f.Check() == Result::kUnsupportedMode);
  f.checkpoint.backend_mode = FaultBackendMode::kX64CheckpointedDirectRam;
  f.observation.event_kind = FaultEventKind::kUnknown;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.observation.event_kind = FaultEventKind::kSynchronousDataAccess;
  f.observation.host_access = FaultHostAccess::kExecute;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.observation.host_access = FaultHostAccess::kWrite;
  CHECK(f.Check() == Result::kObservationMismatch);
  f.observation.host_access = FaultHostAccess::kRead;
  f.checkpoint.access.native_length = 0;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.access.native_length = 3;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.access.native_length = 4;
  f.checkpoint.access.access_count = 2;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.access.access_count = 1;
  f.checkpoint.access.has_helper = true;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.access.has_helper = false;
  f.checkpoint.has_watch = true;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.has_watch = false;
  f.checkpoint.access.has_other_effects = true;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.access.has_other_effects = false;
  f.checkpoint.access.updates_base = true;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.access.updates_base = false;
  f.checkpoint.memory_kind = FaultMemoryKind::kMmio;
  CHECK(f.Check() == Result::kUnsupportedAccess);
  f.checkpoint.memory_kind = FaultMemoryKind::kWatch;
  CHECK(f.Check() == Result::kUnsupportedAccess);
}

TEST_CASE("Fault checkpoint does not promote incomplete modeled state",
          "[cpu][fault-checkpoint]") {
  Fixture f;
  f.checkpoint.state.modeled_valid.gpr &= ~(1u << 4);
  CHECK(f.Check() == Result::kIncompleteModeledState);
  f.checkpoint.state.modeled_valid.gpr = std::numeric_limits<uint32_t>::max();
  f.checkpoint.state.modeled_valid.raw_fpr &= ~1u;
  CHECK(f.Check() == Result::kIncompleteModeledState);
  f.checkpoint.state.modeled_valid.raw_fpr =
      std::numeric_limits<uint32_t>::max();
  f.checkpoint.state.modeled_valid.vmx128[3] = 0;
  CHECK(f.Check() == Result::kIncompleteModeledState);
  f.checkpoint.state.modeled_valid.vmx128[3] =
      std::numeric_limits<uint32_t>::max();
  f.checkpoint.state.modeled_valid.scalar &= ~kModeledMsr;
  CHECK(f.Check() == Result::kIncompleteModeledState);
  f.checkpoint.state.modeled_valid.scalar = kAllModeledScalars | (1u << 12);
  CHECK(f.Check() == Result::kIncompleteModeledState);
  f.checkpoint.state.modeled_valid.scalar = kAllModeledScalars;
  f.checkpoint.state.xer_ca = 2;
  CHECK(f.Check() == Result::kIncompleteModeledState);
  f.checkpoint.state.xer_ca = 0;
  f.checkpoint.state.architectural_valid.fpscr_bits =
      std::numeric_limits<uint32_t>::max();
  CHECK(f.Check() == Result::kInvalidArchitecturalValidity);
  f.checkpoint.state.architectural_valid.fpscr_bits = 1;
  CHECK(f.Check() == Result::kInvalidArchitecturalValidity);
  f.checkpoint.state.architectural_valid.fpscr_bits = 0;
  f.checkpoint.state.architectural_valid.scalar = kModeledFpscr;
  CHECK(f.Check() == Result::kInvalidArchitecturalValidity);
  f.checkpoint.state.architectural_valid.scalar = 0;
  CHECK(f.Check() == Result::kCandidateRequiresProducerProof);
}

}  // namespace
}  // namespace xe::cpu::ppc::test
