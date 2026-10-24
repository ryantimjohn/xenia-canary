/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_FAULT_CHECKPOINT_H_
#define XENIA_CPU_PPC_PPC_FAULT_CHECKPOINT_H_

#include <array>
#include <cstdint>

namespace xe::cpu::ppc {

// This is an owned description of modeled values, not PPCContext or an
// architectural context image. No producer or fault handler uses it yet.
constexpr uint32_t kFaultCheckpointVersion = 1;
constexpr uint32_t kFaultCheckpointLayout = 1;
constexpr uint32_t kFaultPageSize = 4096;

enum class FaultOperation : uint8_t { kUnknown, kLwz, kStw };
enum class FaultBackendMode : uint8_t {
  kUnknown,
  kX64CheckpointedDirectRam,
};
enum class FaultCompilationVariant : uint8_t {
  kUnknown,
  kSingleScalarAccess,
};
enum class FaultNativeAccess : uint8_t { kUnknown, kMov, kMovbe };
enum class FaultHostAccess : uint8_t { kUnknown, kRead, kWrite, kExecute };
enum class FaultEventKind : uint8_t {
  kUnknown,
  kSynchronousDataAccess,
};
enum class FaultMemoryKind : uint8_t {
  kUnknown,
  kDirectRam,
  kMmio,
  kWatch,
  kHelper,
};

enum FaultModeledScalar : uint32_t {
  kModeledLr = 1u << 0,
  kModeledCtr = 1u << 1,
  kModeledCr = 1u << 2,
  kModeledXerFlags = 1u << 3,
  kModeledFpscr = 1u << 4,
  kModeledVscrVector = 1u << 5,
  kModeledVscrSat = 1u << 6,
  kModeledVrsave = 1u << 7,
  kModeledMsr = 1u << 8,
};
constexpr uint32_t kAllModeledScalars = (1u << 9) - 1;

struct FaultModeledValidity {
  uint32_t gpr = 0;
  uint32_t raw_fpr = 0;
  std::array<uint32_t, 4> vmx128{};
  uint32_t scalar = 0;
};

// These masks describe only caller-claimed architectural bits. In particular,
// modeled XER and FPSCR storage does not make either register complete. The
// split VSCR representation has no canonical architectural-bit claim here.
struct FaultArchitecturalValidity {
  uint32_t gpr = 0;
  uint32_t raw_fpr = 0;
  std::array<uint32_t, 4> vmx128{};
  uint32_t scalar = 0;      // LR, CTR, CR, VRSAVE, and MSR only.
  uint32_t xer_bits = 0;    // Only CA (29), OV (30), and SO (31).
  uint32_t fpscr_bits = 0;  // No FPSCR bit has architectural proof here.
};

struct FaultModeledState {
  // Whole numeric register values. No PPCContext layout is serialized here.
  std::array<uint64_t, 32> gpr{};
  uint64_t lr = 0;
  uint64_t ctr = 0;
  // CR0 occupies bits 31:28, through CR7 in bits 3:0; each nibble is LT,
  // GT, EQ, SO. See PPCContext::cr() for the current modeled packing.
  uint32_t cr = 0;
  // IEEE binary64 bit patterns as integers, independent of host byte order.
  std::array<uint64_t, 32> raw_fpr{};
  // Raw modeled vector lane bytes: byte 0 is the low storage byte. This is
  // not a claim of canonical guest byte-order serialization.
  std::array<std::array<uint8_t, 16>, 128> vmx128{};
  // Each modeled flag is normalized to 0 or 1 for this contract. A future
  // producer must establish that normalization from its own source values.
  uint8_t xer_ca = 0;
  uint8_t xer_ov = 0;
  uint8_t xer_so = 0;
  uint32_t fpscr = 0;
  // The modeled VSCR vector bytes and saturation byte remain separate.
  std::array<uint8_t, 16> vscr_vector{};
  uint8_t vscr_sat = 0;
  uint32_t vrsave = 0;
  uint64_t msr = 0;
  FaultModeledValidity modeled_valid{};
  FaultArchitecturalValidity architectural_valid{};
};

struct FaultCheckpointIdentity {
  uint64_t code_epoch = 0;
  uint64_t module_epoch = 0;
  uint64_t owner_epoch = 0;
  uint64_t thread_epoch = 0;
  uint64_t checkpoint_epoch = 0;
};

// All ranges are half-open numeric ranges, not host pointers. These are
// supplied mapping facts; checking them cannot establish OS mapping lifetime.
struct FaultDirectRamRange {
  uint64_t guest_begin = 0;
  uint64_t guest_end = 0;
  uint64_t host_begin = 0;
  uint64_t host_end = 0;
};

// A future producer must publish this descriptor immutably with the matching
// code and checkpoint. Const references here do not prove that publication.
struct FaultScalarAccess {
  uint32_t guest_pc = 0;
  uint32_t raw_instruction = 0;
  FaultOperation operation = FaultOperation::kUnknown;
  uint8_t width = 0;
  uint8_t rt_rs = 0;
  uint8_t ra = 0;
  int16_t displacement = 0;
  uint64_t native_start = 0;
  uint8_t native_length = 0;
  FaultNativeAccess native_access = FaultNativeAccess::kUnknown;
  uint8_t access_count = 0;
  bool updates_base = false;
  // Static lowering and instrumentation exclusions supplied by a producer.
  bool has_helper = false;
  bool has_other_effects = false;
};

struct FaultCheckpoint {
  uint32_t version = 0;
  uint32_t layout = 0;
  FaultCheckpointIdentity identity{};
  FaultBackendMode backend_mode = FaultBackendMode::kUnknown;
  FaultCompilationVariant compilation_variant =
      FaultCompilationVariant::kUnknown;
  uint32_t guest_pc = 0;
  FaultModeledState state{};
  FaultScalarAccess access{};
  // Owned per-invocation claims, not immutable compiled-code-site metadata.
  uint32_t guest_ea = 0;
  FaultMemoryKind memory_kind = FaultMemoryKind::kUnknown;
  bool has_watch = false;
  FaultDirectRamRange mapping{};
};

// Event kind, host access, RIP, and host fault address are copied original
// fault facts. Identity, mode, and guest fields are supplied derived claims;
// agreement with them is not independent evidence. RIP must equal the native
// instruction start. Guest and host fault byte offsets must agree.
struct FaultObservation {
  uint32_t version = 0;
  uint32_t layout = 0;
  FaultCheckpointIdentity identity{};
  FaultBackendMode backend_mode = FaultBackendMode::kUnknown;
  FaultCompilationVariant compilation_variant =
      FaultCompilationVariant::kUnknown;
  FaultEventKind event_kind = FaultEventKind::kUnknown;
  FaultHostAccess host_access = FaultHostAccess::kUnknown;
  uint32_t guest_pc = 0;
  uint32_t guest_ea = 0;
  uint32_t guest_fault_address = 0;
  uint64_t original_rip = 0;
  uint64_t host_fault_address = 0;
};

enum class FaultCheckpointResult {
  kUnsupportedVersion,
  kInvalidIdentity,
  kUnsupportedMode,
  kIncompleteModeledState,
  kInvalidArchitecturalValidity,
  kInvalidInstruction,
  kUnsupportedAccess,
  kInvalidMapping,
  kObservationMismatch,
  kCandidateRequiresProducerProof,
};

// Pure structural check. Even the candidate result does not establish JIT
// materialization, publication, ordering, lifetime, mapping coherence, native
// instruction effects, restartability, or permission to dispatch a fault.
FaultCheckpointResult CheckFaultCheckpoint(const FaultCheckpoint& checkpoint,
                                           const FaultObservation& observation);

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_PPC_FAULT_CHECKPOINT_H_
