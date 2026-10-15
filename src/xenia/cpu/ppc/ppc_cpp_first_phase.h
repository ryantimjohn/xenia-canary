// FORK-ONLY INTEGRATION STAGING: Eligible for this fork's kinect after
// validation. Keep SDK-derived work out of upstream PR/cherry-pick series,
// including xenia-canary:canary_experimental. CONTRIBUTING.md excludes
// XDK-derived information, not just copied code.

#ifndef XENIA_CPU_PPC_PPC_CPP_FIRST_PHASE_H_
#define XENIA_CPU_PPC_PPC_CPP_FIRST_PHASE_H_

#include <cstdint>

#include "xenia/cpu/exception_metadata.h"
#include "xenia/cpu/ppc/ppc_virtual_unwind.h"

namespace xe::cpu::ppc {

enum class CppFirstPhaseAdmissionStatus {
  kAdmitted,
  kUnsupported,
  kBounds,
  kReadFailed,
  kResourceLimit,
  kUntrustedEvidence,
};

struct CppFirstPhaseAdmission {
  CppFirstPhaseAdmissionStatus status =
      CppFirstPhaseAdmissionStatus::kUnsupported;
  // Zero on every failure. No generic dispatcher layout is established.
  uint32_t handler_address = 0;
  uint32_t function_entry_address = 0;
};

// Admits only the supplied-image-evidenced C++ wrapper/core/helper instruction
// shapes whose taken first-search path, with flags&0x66==0 and zero try blocks,
// returns literal 1 without reading CONTEXT or dispatcher fields beyond +4.
// All branch targets are decoded; there are no title/address allowlists.
//
// The reader must serve one caller-owned, coherent module/code/data snapshot.
// kCoherentPrecise asserts that evidence condition only, not a runtime fault
// capture. The caller must retain the corresponding live code/data generation
// and mapping validity through any actual invocation; this result is no lease.
// At most 256 section views, nine exact reads and 224 bytes are considered.
// Reader exceptions decline.
//
// This function does not skip or invoke a handler. An invoking caller must
// supply valid record/CONTEXT storage, an 8-byte dispatcher prefix with
// BE32[+4] pointing to the admitted function entry, and a valid guest call
// stack (the matched core allocates 0x90 bytes). CONTEXT flags may be zero,
// making no architectural state or resumption claim. The actual record's flags
// must equal exception_flags through the call. Invoke the actual handler and
// require actual return 1; other results, nested faults, capture, restoration
// and unwind are unsupported.
CppFirstPhaseAdmission AdmitNoCatchCppFirstPhase(
    const ExceptionMetadataFunction& function, uint32_t exception_flags,
    const ExceptionMetadataInput& owned_image,
    UnwindCaptureProvenance provenance =
        UnwindCaptureProvenance::kUnknown) noexcept;

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_PPC_CPP_FIRST_PHASE_H_
