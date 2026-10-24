// SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect after validation.
// MUST NOT enter upstream PRs/cherry-pick series, including
// xenia-canary:canary_experimental. CONTRIBUTING.md excludes derived
// information. See SDK_DERIVED_PRIVATE.md. No Xbox SDK include/link dependency
// is used.

#include "xenia/kernel/sdk_private_first_chance.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <span>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/exception_metadata.h"
#include "xenia/cpu/ppc/ppc_cpp_first_phase.h"
#include "xenia/cpu/ppc/ppc_virtual_unwind.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/sdk_private_guest_context.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

DEFINE_bool(private_zero_page_first_chance_dispatch, false,
            "Fork-only bounded first-chance search in the primary executable; "
            "no continuation or unwind support.",
            "Kernel");
DECLARE_bool(apply_patches);

namespace xe::kernel {
namespace {

using cpu::ExceptionMetadataStatus;
using cpu::ppc::UnwindCaptureProvenance;
constexpr auto kOwned = UnwindCaptureProvenance::kCoherentPrecise;
constexpr size_t kMaxSections = 256;
constexpr uint64_t kMaxImageBytes = 64 << 20;
constexpr size_t kMaxFrames = 8;
constexpr size_t kMaxScopes = 16;
constexpr uint32_t kMaxStackBytes = 0x10000;
constexpr uint32_t kCallStackReserve = 0x1000;
constexpr uint32_t kRecordSize = 0x50;
constexpr uint32_t kContextOffset = kRecordSize;
constexpr uint32_t kPointersOffset =
    kContextOffset + sdk_private::kGuestContextRecordSize;
constexpr uint32_t kDispatcherOffset = kPointersOffset + 8;
constexpr uint32_t kStorageSize = kDispatcherOffset + 8;

void WriteBe32(uint8_t* out, uint32_t value) {
  out[0] = uint8_t(value >> 24);
  out[1] = uint8_t(value >> 16);
  out[2] = uint8_t(value >> 8);
  out[3] = uint8_t(value);
}

uint32_t ReadBe32(std::span<const uint8_t> bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

bool Accessible(Memory* memory, uint32_t begin, size_t size,
                bool write = false) {
  const uint64_t end = uint64_t(begin) + size;
  auto* heap = memory->LookupHeap(begin);
  if (!heap || !size || end > (uint64_t{1} << 32)) {
    return false;
  }
  auto access = heap->QueryRangeAccess(begin, uint32_t(end - 1));
  return write ? access == xe::memory::PageAccess::kReadWrite
               : access != xe::memory::PageAccess::kNoAccess;
}

struct SectionCopy {
  uint32_t begin;
  std::vector<uint8_t> bytes;
};

struct Evidence {
  object_ref<UserModule> module;
  cpu::XexModule* cpu_module =
      nullptr;  // Retained by module under stated rules.
  cpu::ExceptionMetadataSnapshot metadata;
  std::vector<SectionCopy> image;
  std::vector<uint8_t> stack;
  uint32_t stack_begin = 0;

  std::span<const uint8_t> Find(uint32_t begin, size_t size) const {
    for (const auto& section : image) {
      if (begin >= section.begin &&
          uint64_t(begin - section.begin) + size <= section.bytes.size()) {
        return std::span(section.bytes).subspan(begin - section.begin, size);
      }
    }
    return {};
  }

  bool Code(uint32_t begin, size_t size) const {
    return std::any_of(metadata.sections.begin(), metadata.sections.end(),
                       [=](const auto& section) {
                         return section.available && section.readable &&
                                section.executable && begin >= section.begin &&
                                uint64_t(begin) + size <= section.end;
                       });
  }

  cpu::ExceptionMetadataInput Input() const {
    cpu::ExceptionMetadataInput input;
    input.image_base = metadata.image_base;
    input.image_end = metadata.image_end;
    input.sections = metadata.sections;
    input.imports = metadata.imports;
    input.read = [this](uint32_t address, std::span<uint8_t> output) {
      auto bytes = Find(address, output.size());
      if (bytes.size() != output.size()) {
        return false;
      }
      std::copy(bytes.begin(), bytes.end(), output.begin());
      return true;
    };
    return input;
  }

  // Called with the global region acquired. Normal execution/lifetime rules,
  // not this check, exclude an unload or code mutation after lock release.
  bool Current(KernelState* kernel, uint32_t target) const {
    if (kernel->GetExecutableModule().get() != module.get() ||
        kernel->processor()->LookupModule(target) != cpu_module ||
        !cpu_module->loaded() || !Code(target, 4)) {
      return false;
    }
    auto* heap = kernel->memory()->LookupHeap(metadata.image_base);
    HeapAllocationInfo allocation{};
    if (!heap || !heap->QueryRegionInfo(metadata.image_base, &allocation) ||
        allocation.allocation_base != metadata.image_base ||
        !(allocation.state & kMemoryAllocationCommit) ||
        metadata.image_end - metadata.image_base > allocation.allocation_size) {
      return false;
    }
    for (const auto& section : image) {
      if (!Accessible(kernel->memory(), section.begin, section.bytes.size()) ||
          std::memcmp(kernel->memory()->TranslateVirtual(section.begin),
                      section.bytes.data(), section.bytes.size()) != 0) {
        return false;
      }
    }
    return true;
  }

  bool StackCurrent(Memory* memory) const {
    return Accessible(memory, stack_begin, stack.size()) &&
           std::memcmp(memory->TranslateVirtual(stack_begin), stack.data(),
                       stack.size()) == 0;
  }
};

bool Capture(KernelState* kernel, cpu::ThreadState* thread,
             const cpu::Processor::ScalarAccessFirstChance& fault,
             Evidence& evidence) {
  auto lock = global_critical_region::AcquireDirect();
  auto* xthread = XThread::GetCurrentThread();
  if (!xthread || xthread->thread_state() != thread ||
      !xthread->is_guest_thread() || !fault.modeled_context) {
    return false;
  }
  evidence.module = kernel->GetExecutableModule();
  if (!evidence.module) {
    return false;
  }
  // The accessor rejects ELF without invoking the XEX-only accessor assertion.
  void* execution_info = nullptr;
  if (evidence.module->GetOptHeader(XEX_HEADER_EXECUTION_INFO,
                                    &execution_info) != X_STATUS_SUCCESS ||
      !execution_info) {
    return false;
  }
  evidence.cpu_module = evidence.module->xex_module();
  if (kernel->processor()->LookupModule(fault.guest_pc) !=
      evidence.cpu_module) {
    return false;
  }
  evidence.metadata = evidence.cpu_module->GetExceptionMetadataSnapshot();
  if (!evidence.metadata.lookup_valid ||
      evidence.metadata.sections.size() > kMaxSections ||
      evidence.metadata.image_end <= evidence.metadata.image_base ||
      evidence.metadata.image_end - evidence.metadata.image_base >
          kMaxImageBytes) {
    return false;
  }
  uint64_t copied = 0;
  for (auto& section : evidence.metadata.sections) {
    // Mutable game data is not code/metadata evidence. Required writable
    // metadata is unsupported rather than silently treated as a stable copy.
    if (!section.available || !section.readable ||
        (section.raw_flags & kXEPESectionMemoryWrite)) {
      section.available = false;
      continue;
    }
    const uint64_t size = section.end - section.begin;
    if (!size || section.begin < evidence.metadata.image_base ||
        section.end > evidence.metadata.image_end ||
        size > kMaxImageBytes - copied ||
        !Accessible(kernel->memory(), section.begin, size)) {
      return false;
    }
    SectionCopy copy{section.begin, std::vector<uint8_t>(size)};
    std::memcpy(copy.bytes.data(),
                kernel->memory()->TranslateVirtual(section.begin), size);
    copied += size;
    evidence.image.push_back(std::move(copy));
  }
  // Reparse from only the owned, eligible bytes: a live parsed handler whose
  // prefix/table was in mutable or unavailable memory must not be admitted.
  auto input = evidence.Input();
  input.limits.max_functions = 65536;
  input.limits.max_scopes_per_handler = 256;
  input.limits.max_read_bytes = 8 << 20;
  auto owned_metadata = cpu::ParseExceptionMetadata(input);
  if (!owned_metadata.lookup_valid) {
    return false;
  }
  evidence.metadata = std::move(owned_metadata);
  const auto instruction = evidence.Find(fault.guest_pc, 4);
  if (instruction.size() != 4 ||
      ReadBe32(instruction) != fault.raw_instruction) {
    return false;
  }
  const uint32_t sp = uint32_t(fault.modeled_context->r[1]);
  if ((sp & 15) || sp < xthread->stack_limit() || sp >= xthread->stack_base() ||
      uint64_t(xthread->stack_limit()) + kCallStackReserve > sp ||
      !Accessible(kernel->memory(), sp - kCallStackReserve, kCallStackReserve,
                  true)) {
    return false;
  }
  evidence.stack_begin = sp;
  evidence.stack.resize(std::min(kMaxStackBytes, xthread->stack_base() - sp));
  if (!Accessible(kernel->memory(), sp, evidence.stack.size())) {
    return false;
  }
  std::memcpy(evidence.stack.data(), kernel->memory()->TranslateVirtual(sp),
              evidence.stack.size());
  return evidence.Current(kernel, fault.guest_pc);
}

class GuestStorage {
 public:
  explicit GuestStorage(Memory* memory)
      : memory_(memory), base_(memory->SystemHeapAlloc(kStorageSize)) {}
  ~GuestStorage() {
    if (base_) {
      memory_->SystemHeapFree(base_);
    }
  }
  GuestStorage(const GuestStorage&) = delete;
  GuestStorage& operator=(const GuestStorage&) = delete;
  uint32_t record() const { return base_; }
  uint32_t context() const { return base_ + kContextOffset; }
  uint32_t pointers() const { return base_ + kPointersOffset; }
  uint32_t dispatcher() const { return base_ + kDispatcherOffset; }

  bool Reset(const cpu::Processor::ScalarAccessFirstChance& fault,
             uint32_t entry) {
    if (!base_ || uint64_t(base_) + kStorageSize > (uint64_t{1} << 32) ||
        !Accessible(memory_, base_, kStorageSize, true)) {
      return false;
    }
    std::array<uint8_t, kStorageSize> bytes{};
    WriteBe32(bytes.data(), 0xC0000005u);
    WriteBe32(bytes.data() + 12, fault.guest_pc);
    WriteBe32(bytes.data() + 16, 2);
    WriteBe32(bytes.data() + 20, fault.is_write ? 1 : 0);
    WriteBe32(bytes.data() + 24, fault.guest_address);
    // Full valid storage with flags=0. No architectural state is represented;
    // the admitted C++ path does not read CONTEXT and filters get no promise
    // that a field is captured, resumable or restorable.
    sdk_private::GuestContextRecord context;
    if (!sdk_private::EncodeGuestContextRecord(
            context,
            std::span(bytes).subspan(kContextOffset,
                                     sdk_private::kGuestContextRecordSize))) {
      return false;
    }
    WriteBe32(bytes.data() + kPointersOffset, record());
    WriteBe32(bytes.data() + kPointersOffset + 4, this->context());
    WriteBe32(bytes.data() + kDispatcherOffset + 4, entry);
    std::memcpy(memory_->TranslateVirtual(base_), bytes.data(), bytes.size());
    return true;
  }

 private:
  Memory* memory_;
  uint32_t base_;
};

// Reset between returning calls, including failures/host exception unwinding.
// This restores modeled emulator state, not a guest CONTEXT or JIT
// continuation.
class LiveContextScope {
 public:
  LiveContextScope(cpu::ThreadState* thread, const cpu::ppc::PPCContext& saved)
      : live_(thread->context()), saved_(saved) {
    Restore();
  }
  ~LiveContextScope() { Restore(); }
  void Restore() { std::memcpy(live_, &saved_, sizeof(saved_)); }

 private:
  cpu::ppc::PPCContext* live_;
  const cpu::ppc::PPCContext& saved_;
};

void Search(KernelState* kernel, cpu::ThreadState* thread,
            const cpu::Processor::ScalarAccessFirstChance& fault) {
  Evidence evidence;
  if (!Capture(kernel, thread, fault, evidence)) {
    XELOGI("Private first-chance declined unavailable owned evidence at {:08X}",
           fault.guest_pc);
    return;
  }
  GuestStorage storage(kernel->memory());
  LiveContextScope context(thread, *fault.modeled_context);
  cpu::ppc::VirtualUnwindRegisters registers;
  registers.control_pc = fault.guest_pc;
  registers.provenance = kOwned;
  // CALL_EXTERN materializes these modeled integer values at a regular JIT
  // boundary. Only their low32 is admitted; recovered SP has no invented high
  // bits.
  registers.gpr[1] = uint32_t(fault.modeled_context->r[1]);
  registers.gpr[31] = uint32_t(fault.modeled_context->r[31]);
  registers.low32_valid = (1u << 1) | (1u << 31);
  uint32_t raw_saved_lr =
      0;  // No caller return evidence for the initial fault.
  for (size_t depth = 0; depth < kMaxFrames; ++depth) {
    auto frame = evidence.metadata.LookupFrame(registers.control_pc);
    if (frame.status != ExceptionMetadataStatus::kValid || !frame.function ||
        !evidence.Code(registers.control_pc, 4)) {
      return;
    }
    const auto& function = *frame.function;
    const uint32_t frame_sp = uint32_t(registers.gpr[1]);
    XELOGI("Private first-chance frame {} pc {:08X} sp {:08X} raw_lr {:08X}",
           depth, registers.control_pc, frame_sp, raw_saved_lr);
    if (frame_sp < evidence.stack_begin ||
        uint64_t(frame_sp - evidence.stack_begin) >= evidence.stack.size()) {
      return;
    }
    if (function.handler) {
      const auto& handler = *function.handler;
      if (handler.imported_c_specific) {
        if (handler.status != ExceptionMetadataStatus::kValid ||
            frame.matching_scopes.size() > kMaxScopes) {
          return;
        }
        for (const auto& scope : frame.matching_scopes) {
          // A zero/sentinel role or termination-only scope is unsupported.
          if (scope.status != ExceptionMetadataStatus::kValid ||
              scope.opaque_roles || !scope.jump_target ||
              !evidence.Code(scope.filter_or_handler, 4) ||
              !evidence.Code(scope.jump_target, 4)) {
            return;
          }
          {
            auto lock = global_critical_region::AcquireDirect();
            if (!evidence.Current(kernel, scope.filter_or_handler) ||
                !evidence.StackCurrent(kernel->memory()) ||
                !storage.Reset(fault, function.table_address)) {
              return;
            }
          }
          context.Restore();
          uint64_t args[] = {storage.pointers(), frame_sp};
          XELOGI(
              "Private first-chance calling C scope filter {:08X} for {:08X}",
              scope.filter_or_handler, registers.control_pc);
          const uint64_t result = kernel->processor()->Execute(
              thread, scope.filter_or_handler, args, 2);
          context.Restore();
          // Search-only. Continue-search is the only accepted returning value;
          // continue-execution/execute-handler require unsupported
          // resume/unwind.
          if (result != 0) {
            return;
          }
        }
      } else {
        const auto admission = cpu::ppc::AdmitNoCatchCppFirstPhase(
            function, 0, evidence.Input(), kOwned);
        if (admission.status !=
            cpu::ppc::CppFirstPhaseAdmissionStatus::kAdmitted) {
          return;
        }
        {
          auto lock = global_critical_region::AcquireDirect();
          if (!evidence.Current(kernel, admission.handler_address) ||
              !evidence.StackCurrent(kernel->memory()) ||
              !storage.Reset(fault, admission.function_entry_address)) {
            return;
          }
        }
        context.Restore();
        uint64_t args[] = {storage.record(), frame_sp, storage.context(),
                           storage.dispatcher()};
        const uint64_t result = kernel->processor()->Execute(
            thread, admission.handler_address, args, 4);
        context.Restore();
        if (result != 1) {
          return;
        }
        XELOGI("Private first-chance guest no-catch handler {:08X} returned 1",
               admission.handler_address);
      }
    }
    {
      auto lock = global_critical_region::AcquireDirect();
      if (!evidence.StackCurrent(kernel->memory())) {
        return;
      }
    }
    if (function.end <= function.begin ||
        function.end - function.begin > 1024) {
      return;
    }
    auto code =
        evidence.Find(function.begin, size_t(function.end - function.begin));
    const auto recipe = cpu::ppc::BuildBoundedUnwindRecipe(
        function, {function.begin, code, kOwned});
    if (!recipe.recipe) {
      return;
    }
    const auto next = cpu::ppc::TryVirtualUnwindFrame(
        *recipe.recipe, registers,
        {evidence.stack_begin, evidence.stack, kOwned});
    if (next.status != cpu::ppc::VirtualUnwindStatus::kSupportedPartialFrame ||
        next.caller_sp_address32 <= frame_sp || !next.return_address32) {
      return;
    }
    // A linked call saves its following instruction in LR. That continuation
    // may be outside the caller's half-open scope (or in an adjacent scope).
    // Keep the raw return evidence, but select only a proven suspended call.
    const auto caller_pc = sdk_private::SelectSuspendedCallSite(
        next.return_address32, function.begin, evidence.Input());
    if (!caller_pc) {
      return;
    }
    const auto caller_frame = evidence.metadata.LookupFrame(*caller_pc);
    if (caller_frame.status != ExceptionMetadataStatus::kValid ||
        !caller_frame.function ||
        (function.begin >= caller_frame.function->begin &&
         uint64_t(function.begin) < caller_frame.function->end)) {
      return;  // Preserve the bounded recipe's external-call restriction.
    }
    raw_saved_lr = next.return_address32;
    registers.control_pc = *caller_pc;
    registers.gpr[1] = next.caller_sp_address32;
    registers.low32_valid |= 1u << 1;
    registers.full64_valid &= ~(1u << 1);
    // Recipe A does not save r31. The admitted ordinary call path preserves
    // that nonvolatile value; B recovers a full64 value from its owned save
    // slot.
    if (next.recovered_r31_valid) {
      registers.gpr[31] = next.recovered_r31_full64;
      registers.full64_valid |= 1u << 31;
    }
  }
}

}  // namespace

std::optional<uint32_t> sdk_private::SelectSuspendedCallSite(
    uint32_t saved_lr, uint32_t callee_entry,
    const cpu::ExceptionMetadataInput& owned_image) {
  if (saved_lr < 4 || (saved_lr & 3) || !callee_entry || (callee_entry & 3) ||
      !owned_image.read || owned_image.image_base >= owned_image.image_end ||
      owned_image.image_end > (uint64_t{1} << 32)) {
    return std::nullopt;
  }
  const uint32_t call_pc = saved_lr - 4;
  const auto code = [&](uint32_t address) {
    const uint64_t end = uint64_t(address) + 4;
    return address >= owned_image.image_base && end <= owned_image.image_end &&
           std::any_of(owned_image.sections.begin(), owned_image.sections.end(),
                       [&](const auto& section) {
                         return section.available && section.readable &&
                                section.executable &&
                                address >= section.begin && end <= section.end;
                       });
  };
  if (!code(call_pc) || !code(callee_entry)) {
    return std::nullopt;
  }
  std::array<uint8_t, 4> bytes{};
  if (!owned_image.read(call_pc, bytes)) {
    return std::nullopt;
  }
  const uint32_t word = ReadBe32(bytes);
  if ((word >> 26) != 18 || (word & 3) != 1) {
    return std::nullopt;  // Relative BL only; never subtract from an arbitrary
                          // LR.
  }
  const uint32_t encoded = word & 0x03FFFFFC;
  const uint64_t magnitude =
      encoded & 0x02000000 ? uint64_t{0x04000000} - encoded : encoded;
  if (!magnitude || ((encoded & 0x02000000) && magnitude > call_pc)) {
    return std::nullopt;
  }
  const uint64_t target = (encoded & 0x02000000)
                              ? uint64_t(call_pc) - magnitude
                              : uint64_t(call_pc) + magnitude;
  return target == callee_entry ? std::optional<uint32_t>(call_pc)
                                : std::nullopt;
}

void InstallPrivateFirstChanceDispatcher(KernelState* kernel) {
  // Install only during initial kernel setup with patch loading disabled. The
  // process must have started with apply_patches=false, so PatchDB is empty;
  // toggling the flag in a process that already loaded rules is insufficient.
  if (cvars::apply_patches) {
    return;
  }
  kernel->processor()->set_scalar_access_first_chance_handler(
      [kernel](cpu::ThreadState* thread,
               const cpu::Processor::ScalarAccessFirstChance& fault) {
        if (!cvars::private_zero_page_first_chance_dispatch ||
            cvars::apply_patches) {
          return;
        }
        try {
          Search(kernel, thread, fault);
        } catch (const std::bad_alloc&) {
          // Allocation failure declines; unrelated host control transfers and
          // guest exceptions keep their original propagation behavior.
        }
      });
}

void UninstallPrivateFirstChanceDispatcher(KernelState* kernel) {
  kernel->processor()->set_scalar_access_first_chance_handler({});
}

}  // namespace xe::kernel
