/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/testing/ppc_checkpoint_x64_fixture.h"

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

using namespace x64_fixture;
constexpr uint32_t kFixturePC = 0x80001000;
constexpr DWORD kCleanupFailure = 0xE043DC31;

// This translation unit owns a synchronous, exclusive same-thread experiment.
// No ThreadState, emulator handler, code publication or guest dispatch is used.
// These constant-initialized trivial TLS objects are touched before
// registration.
struct Admission;
thread_local Admission* current_admission = nullptr;
thread_local bool inside_handler = false;

struct CapturedFault {
  EXCEPTION_RECORD exception{};
  CONTEXT context{};
  CONTEXT recovery_context{};
  Record armed{};
  uint64_t raw_rip = 0;
  uint64_t raw_rsp = 0;
  uint64_t raw_r11 = 0;
  uint64_t raw_direction = 0;
  uint64_t raw_address = 0;
  uint64_t return_word = 0;
  uint32_t events = 0;
  bool complete = false;
};

struct Admission {
  PPCContext* input = nullptr;
  Record* record = nullptr;
  CapturedFault* captured = nullptr;
  uint64_t access = 0;
  uint64_t decline = 0;
  uint64_t host_target = 0;
  uint64_t page_begin = 0;
  uint64_t page_end = 0;
  uint64_t stack_begin = 0;
  uint64_t stack_end = 0;
  uint64_t sequence = 0;
  uint32_t ea = 0;
  ULONG_PTR direction = 0;
  bool accepting = false;
};

bool Span(uint64_t begin, uint64_t width, uint64_t low, uint64_t high) {
  return width && low < high && begin >= low && begin < high &&
         width <= high - begin;
}

bool CompleteModeled(const FaultModeledState& state) {
  if (state.modeled_valid.gpr != UINT32_MAX ||
      state.modeled_valid.raw_fpr != UINT32_MAX ||
      state.modeled_valid.scalar != kAllModeledScalars) {
    return false;
  }
  for (auto mask : state.modeled_valid.vmx128) {
    if (mask != UINT32_MAX) {
      return false;
    }
  }
  return true;
}

bool Admits(const Admission& owner, const EXCEPTION_RECORD& exception,
            const CONTEXT& context) {
  constexpr DWORD coverage = CONTEXT_CONTROL | CONTEXT_INTEGER;
  return owner.accepting && owner.input && owner.record && owner.captured &&
         owner.sequence && !owner.captured->events &&
         exception.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
         exception.ExceptionFlags == 0 && !exception.ExceptionRecord &&
         exception.NumberParameters >= 2 &&
         exception.NumberParameters <= EXCEPTION_MAXIMUM_PARAMETERS &&
         owner.direction <= 1 &&
         exception.ExceptionInformation[0] == owner.direction &&
         exception.ExceptionInformation[1] == owner.host_target &&
         reinterpret_cast<uint64_t>(exception.ExceptionAddress) ==
             owner.access &&
         context.Rip == owner.access &&
         (context.ContextFlags & coverage) == coverage &&
         context.Rcx == reinterpret_cast<uint64_t>(owner.input) &&
         context.Rdx == reinterpret_cast<uint64_t>(owner.record) &&
         context.R11 == owner.host_target && !(owner.ea & 3) &&
         !(owner.host_target & 3) &&
         Span(owner.host_target, 4, owner.page_begin, owner.page_end) &&
         owner.record->sequence == owner.sequence &&
         owner.record->phase == Phase(CheckpointActivationState::kArmed) &&
         owner.record->guest_ea == owner.ea &&
         CompleteModeled(owner.record->state) && !(context.Rsp & 7) &&
         (context.Rsp & 15) == 8 &&
         Span(context.Rsp, 8, owner.stack_begin, owner.stack_end);
}

LONG CALLBACK ObserveOwnedFault(EXCEPTION_POINTERS* pointers) noexcept {
  // Guard nesting before reading borrowed exception/context pointers. Other
  // threads have no TLS owner and cannot consume this fixture's fault.
  if (inside_handler || !current_admission) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  inside_handler = true;
  auto& owner = *current_admission;
  if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord ||
      !Admits(owner, *pointers->ExceptionRecord, *pointers->ContextRecord)) {
    inside_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
  }

  auto& captured = *owner.captured;
  const auto& exception = *pointers->ExceptionRecord;
  auto& context = *pointers->ContextRecord;
  // Every borrowed fact and the complete Armed payload are copied before any
  // recovery edit. A chained record was rejected, so no borrowed chain escapes.
  // CONTEXT is a fixed SDK copy; only CONTROL + INTEGER coverage is claimed.
  // There is no XSAVE/XSTATE completeness claim.
  std::memcpy(&captured.exception, &exception, sizeof(exception));
  std::memcpy(&captured.context, &context, sizeof(context));
  std::memcpy(&captured.armed, owner.record, sizeof(Record));
  captured.raw_rip = context.Rip;
  captured.raw_rsp = context.Rsp;
  captured.raw_r11 = context.R11;
  captured.raw_direction = exception.ExceptionInformation[0];
  captured.raw_address = exception.ExceptionInformation[1];
  // Setup queried/touched this bounded committed readable current-thread stack
  // interval. Admission proved alignment and ownership of the ENTIRE word.
  // The verified leaf never changed RSP or nonvolatile registers.
  captured.return_word = *reinterpret_cast<const uint64_t*>(context.Rsp);
  captured.complete = true;
  ++captured.events;

  // Test-only recovery: no permission bypass/retry, load writeback, guest PC
  // edit or dispatch. The verified private XOR EAX,EAX; RET returns decline.
  owner.record->phase = Phase(CheckpointActivationState::kDisarmed);
  context.Rip = owner.decline;
  std::memcpy(&captured.recovery_context, &context, sizeof(context));
  inside_handler = false;
  return EXCEPTION_CONTINUE_EXECUTION;
}

bool Readable(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS)) {
    return false;
  }
  switch (protection & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

// Queries and page touches are confined to setup, never the callback. No guard
// page is read or committed. Ownership is supplied separately by each caller.
bool PrepareReadable(uint64_t begin, uint64_t size) {
  if (!size || begin > UINT64_MAX - size) {
    return false;
  }
  const uint64_t end = begin + size;
  for (uint64_t cursor = begin; cursor < end;) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                     sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || !Readable(info.Protect)) {
      return false;
    }
    const uint64_t region = reinterpret_cast<uint64_t>(info.BaseAddress);
    if (region > cursor || info.RegionSize > UINT64_MAX - region ||
        region + info.RegionSize <= cursor) {
      return false;
    }
    const uint64_t next = std::min(end, region + info.RegionSize);
    const uint64_t page = xe::memory::page_size();
    if (!page) {
      return false;
    }
    for (uint64_t touched = cursor; touched < next;) {
      const volatile uint8_t byte =
          *reinterpret_cast<const volatile uint8_t*>(touched);
      (void)byte;
      if (next - touched <= page) {
        break;
      }
      touched += page;
    }
    const volatile uint8_t last =
        *reinterpret_cast<const volatile uint8_t*>(next - 1);
    (void)last;
    cursor = next;
  }
  return true;
}

bool PrepareStack(Admission& owner) {
  uint64_t marker = 0;
  const uint64_t address = reinterpret_cast<uint64_t>(&marker);
  // The TIB supplies current-thread bounds, not arbitrary caller RSP claims.
  const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const uint64_t limit = reinterpret_cast<uint64_t>(tib->StackLimit);
  const uint64_t base = reinterpret_cast<uint64_t>(tib->StackBase);
  MEMORY_BASIC_INFORMATION info{};
  if (!Span(address, sizeof(marker), limit, base) ||
      VirtualQuery(&marker, &info, sizeof(info)) != sizeof(info) ||
      info.State != MEM_COMMIT || !Readable(info.Protect)) {
    return false;
  }
  const uint64_t region = reinterpret_cast<uint64_t>(info.BaseAddress);
  if (region > address || info.RegionSize > UINT64_MAX - region) {
    return false;
  }
  constexpr uint64_t radius = 64 * 1024;
  owner.stack_begin = std::max(std::max(limit, region),
                               address > radius ? address - radius : 0);
  owner.stack_end =
      std::min(std::min(base, region + info.RegionSize),
               address > UINT64_MAX - radius ? UINT64_MAX : address + radius);
  return owner.stack_begin < owner.stack_end &&
         PrepareReadable(owner.stack_begin,
                         owner.stack_end - owner.stack_begin);
}

[[noreturn]] void CleanupFatal() noexcept {
  // An unregister failure leaves a possibly installed callback. Clear admission
  // but retain ALL storage and code/page owners by terminating without unwind.
  // The isolated child's nonzero exit is FAIL, never a continued test run.
  current_admission = nullptr;
  TerminateProcess(GetCurrentProcess(), kCleanupFailure);
  std::abort();  // Also terminates without unwinding if TerminateProcess fails.
}

class HostFaultFixture {
 public:
  HostFaultFixture(uint32_t word, uint32_t guest_begin, uint32_t span)
      : memory(guest_begin, span), word_(word) {
    leaf = LeafFixture::Create(kFixturePC, word, build_result);
  }
  ~HostFaultFixture() { Close(); }
  HostFaultFixture(const HostFaultFixture&) = delete;
  HostFaultFixture& operator=(const HostFaultFixture&) = delete;

  bool Prepare(uint32_t ea, uint64_t sequence) {
    if (!leaf || !memory.valid() || !sequence || current_admission ||
        inside_handler || (ea & 3) || ea < memory.mapping().guest_begin ||
        uint64_t{ea} + 4 > memory.mapping().guest_end ||
        !VerifyBytes(leaf->code(), leaf->size(), word_, leaf->offsets())) {
      return false;
    }
    const auto& offsets = leaf->offsets();
    if (offsets.decline_return != offsets.decline + 2 ||
        leaf->code()[offsets.decline] != 0x31 ||
        leaf->code()[offsets.decline + 1] != 0xC0 ||
        leaf->code()[offsets.decline_return] != 0xC3) {
      return false;
    }
    admission.input = &input;
    admission.record = &record;
    admission.captured = &captured;
    admission.access =
        reinterpret_cast<uint64_t>(leaf->code()) + offsets.access;
    admission.decline =
        reinterpret_cast<uint64_t>(leaf->code()) + offsets.decline;
    admission.host_target = reinterpret_cast<uint64_t>(memory.data()) +
                            (uint64_t{ea} - memory.mapping().guest_begin);
    admission.page_begin = reinterpret_cast<uint64_t>(memory.data() - 16);
    const size_t page = xe::memory::page_size();
    if (!page || admission.page_begin > UINT64_MAX - page) {
      return false;
    }
    admission.page_end = admission.page_begin + page;
    admission.sequence = sequence;
    admission.ea = ea;
    admission.direction = (word_ >> 26) == 32 ? 0 : 1;
    record.sequence = sequence;

    DWORD64 image_base = 0;
    // This actual verified allocation is a leaf. A null function entry is NOT
    // supplied to RtlVirtualUnwind; caller simulation happens from owned
    // copies.
    if (RtlLookupFunctionEntry(admission.access, &image_base, nullptr)) {
      return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(admission.page_begin), &info,
                     sizeof(info)) != sizeof(info) ||
        reinterpret_cast<uint64_t>(info.AllocationBase) !=
            admission.page_begin ||
        reinterpret_cast<uint64_t>(info.BaseAddress) != admission.page_begin ||
        info.RegionSize != page || info.State != MEM_COMMIT ||
        info.Protect != PAGE_READWRITE || !PrepareStack(admission) ||
        !PrepareReadable(reinterpret_cast<uint64_t>(this), sizeof(*this)) ||
        !PrepareReadable(reinterpret_cast<uint64_t>(leaf->code()),
                         leaf->size()) ||
        !PrepareReadable(admission.page_begin, page)) {
      return false;
    }
    // Force trivial TLS access before registration, including its storage page.
    current_admission = nullptr;
    inside_handler = false;
    return PrepareReadable(reinterpret_cast<uint64_t>(&current_admission),
                           sizeof(current_admission)) &&
           PrepareReadable(reinterpret_cast<uint64_t>(&inside_handler),
                           sizeof(inside_handler));
  }

  bool Open() {
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(admission.page_begin),
                        admission.page_end - admission.page_begin,
                        PAGE_NOACCESS, &previous)) {
      return false;
    }
    protected_ = true;
    if (previous != PAGE_READWRITE) {
      return false;
    }
    current_admission = &admission;
    admission.accepting = true;
    // Install LAST, after all allocation, queries, touches and protection.
    // FirstHandler=1 does not guarantee permanent precedence over later VEH.
    registration_ = AddVectoredExceptionHandler(1, ObserveOwnedFault);
    if (!registration_) {
      admission.accepting = false;
      current_admission = nullptr;
      return false;
    }
    return true;
  }

  void Close() noexcept {
    admission.accepting = false;
    if (current_admission == &admission) {
      current_admission = nullptr;
    }
    // Supported only after this exclusive synchronous invocation and callback
    // returned; removal is not a general cross-thread quiescence proof.
    if (inside_handler) {
      CleanupFatal();
    }
    if (registration_) {
      if (!RemoveVectoredExceptionHandler(registration_)) {
        CleanupFatal();
      }
      registration_ = nullptr;
      removed = true;
    }
    if (protected_) {
      DWORD previous = 0;
      if (!VirtualProtect(reinterpret_cast<void*>(admission.page_begin),
                          admission.page_end - admission.page_begin,
                          PAGE_READWRITE, &previous)) {
        CleanupFatal();
      }
      protected_ = false;
      restored = true;
      if (previous != PAGE_NOACCESS) {
        CleanupFatal();
      }
    }
  }

  BuildResult build_result = BuildResult::kUnsupportedInstruction;
  std::unique_ptr<LeafFixture> leaf;
  SyntheticMemory memory;
  PPCContext input{};
  Record record{};
  CapturedFault captured{};
  Admission admission{};
  bool removed = false;
  bool restored = false;

 private:
  const uint32_t word_;
  void* registration_ = nullptr;
  bool protected_ = false;
};

uint32_t Word(uint32_t opcode, uint32_t rt_rs, uint32_t ra,
              int16_t displacement) {
  return (opcode << 26) | (rt_rs << 21) | (ra << 16) |
         static_cast<uint16_t>(displacement);
}

void Seed(PPCContext& input) {
  constexpr std::array<uint64_t, 4> raw = {
      0x8000000000000000ULL, 0x7FF8000000000123ULL, 0x7FF0000000000456ULL,
      0x0000000000000001ULL};
  for (size_t n = 0; n < 32; ++n) {
    input.r[n] = 0xFEDCBA9810203040ULL + n * 0x112233;
    std::memcpy(&input.f[n], &raw[n % raw.size()], 8);
  }
  for (size_t n = 0; n < 128; ++n) {
    auto* bytes = reinterpret_cast<uint8_t*>(&input.v[n]);
    for (size_t b = 0; b < 16; ++b) {
      bytes[b] = static_cast<uint8_t>(n * 37 + b * 11);
    }
  }
  auto* cr = reinterpret_cast<uint8_t*>(&input) + offsetof(PPCContext, cr0);
  for (size_t n = 0; n < 32; ++n) {
    cr[n] = static_cast<uint8_t>(0xA0 | ((n * 7 + n / 4) & 1));
  }
  auto* vscr = reinterpret_cast<uint8_t*>(&input.vscr_vec);
  for (size_t n = 0; n < 16; ++n) {
    vscr[n] = static_cast<uint8_t>(0xE7 - n * 7);
  }
  input.lr = 0xDEADBEEF12345678ULL;
  input.ctr = 0xABCDEF0123456789ULL;
  input.msr = 0xFEDCBA9876549030ULL;
  input.fpscr.value = 0xA59C7E13;
  input.vrsave = 0x81234567;
  input.xer_ca = 0x80;
  input.xer_ov = 0;
  input.xer_so = 0xFE;
  input.vscr_sat = 0xFF;
  input.scratch = 0x8765432112345678ULL;
  input.reserved_val = 0xCAFEF00D12345678ULL;
  input.thread_id = 0x12345678;
}

// Independent member-wise oracle, including modeled masks and zero
// architectural masks. Raw FPR bits, vectors and normalized flag bytes remain
// distinct.
FaultModeledState Expected(const PPCContext& input) {
  FaultModeledState state{};
  for (size_t n = 0; n < 32; ++n) {
    state.gpr[n] = input.r[n];
    std::memcpy(&state.raw_fpr[n], &input.f[n], 8);
  }
  for (size_t n = 0; n < 128; ++n) {
    std::memcpy(state.vmx128[n].data(), &input.v[n], 16);
  }
  const auto* cr =
      reinterpret_cast<const uint8_t*>(&input) + offsetof(PPCContext, cr0);
  for (size_t n = 0; n < 32; ++n) {
    state.cr |= uint32_t{cr[n] & 1u} << (31 - n);
  }
  state.lr = input.lr;
  state.ctr = input.ctr;
  state.msr = input.msr;
  state.fpscr = input.fpscr.value;
  state.vrsave = input.vrsave;
  state.xer_ca = input.xer_ca != 0;
  state.xer_ov = input.xer_ov != 0;
  state.xer_so = input.xer_so != 0;
  state.vscr_sat = input.vscr_sat != 0;
  std::memcpy(state.vscr_vector.data(), &input.vscr_vec, 16);
  state.modeled_valid.gpr = state.modeled_valid.raw_fpr = UINT32_MAX;
  state.modeled_valid.vmx128.fill(UINT32_MAX);
  state.modeled_valid.scalar = kAllModeledScalars;
  return state;
}

void CheckState(const FaultModeledState& actual,
                const FaultModeledState& expected) {
  CHECK(actual.gpr == expected.gpr);
  CHECK(actual.raw_fpr == expected.raw_fpr);
  CHECK(actual.vmx128 == expected.vmx128);
  CHECK(actual.lr == expected.lr);
  CHECK(actual.ctr == expected.ctr);
  CHECK(actual.cr == expected.cr);
  CHECK(actual.xer_ca == expected.xer_ca);
  CHECK(actual.xer_ov == expected.xer_ov);
  CHECK(actual.xer_so == expected.xer_so);
  CHECK(actual.fpscr == expected.fpscr);
  CHECK(actual.vscr_vector == expected.vscr_vector);
  CHECK(actual.vscr_sat == expected.vscr_sat);
  CHECK(actual.vrsave == expected.vrsave);
  CHECK(actual.msr == expected.msr);
  CHECK(actual.modeled_valid.gpr == expected.modeled_valid.gpr);
  CHECK(actual.modeled_valid.raw_fpr == expected.modeled_valid.raw_fpr);
  CHECK(actual.modeled_valid.vmx128 == expected.modeled_valid.vmx128);
  CHECK(actual.modeled_valid.scalar == expected.modeled_valid.scalar);
  CHECK(actual.architectural_valid.gpr == 0);
  CHECK(actual.architectural_valid.raw_fpr == 0);
  CHECK(actual.architectural_valid.vmx128 ==
        expected.architectural_valid.vmx128);
  CHECK(actual.architectural_valid.scalar == 0);
  CHECK(actual.architectural_valid.xer_bits == 0);
  CHECK(actual.architectural_valid.fpscr_bits == 0);
}

TEST_CASE("Owned Windows x64 AV retains pre-access state and returns decline",
          "[cpu][checkpoint-x64-host-fault]") {
  struct Case {
    uint32_t opcode, rt_rs, ra;
    int16_t displacement;
    uint64_t old_base;
    uint32_t guest_begin, span, ea;
  };
  const std::array<Case, 5> cases = {{
      {32, 4, 4, -4, 0xABCDEF1200000020ULL, 16, 64, 28},
      {36, 7, 7, 4, 0xAAAAAAAA0000001CULL, 16, 64, 32},
      {32, 0, 0, 0, 0, 0, 16, 0},
      {36, 0, 0, -4, 0, 0xFFFFFFFC, 4, 0xFFFFFFFC},
      {32, 8, 9, 8, 0x11111111FFFFFFFCULL, 0, 16, 4},
  }};
  uint64_t sequence = 1;
  for (const auto& c : cases) {
    CAPTURE(c.opcode, c.rt_rs, c.ra, c.displacement);
    HostFaultFixture owner(Word(c.opcode, c.rt_rs, c.ra, c.displacement),
                           c.guest_begin, c.span);
    REQUIRE(owner.build_result == BuildResult::kSuccess);
    REQUIRE(owner.leaf);
    REQUIRE(owner.memory.valid());
    Seed(owner.input);
    if (c.ra) {
      owner.input.r[c.ra] = c.old_base;
    }
    const FaultModeledState expected = Expected(owner.input);
    std::array<uint8_t, sizeof(PPCContext)> input_before{};
    std::memcpy(input_before.data(), &owner.input, sizeof(owner.input));
    std::array<uint8_t, 64> memory_before{};
    REQUIRE(owner.memory.size() <= memory_before.size());
    std::fill(owner.memory.data(), owner.memory.data() + owner.memory.size(),
              uint8_t{0x7B});
    std::memcpy(memory_before.data(), owner.memory.data(), owner.memory.size());
    REQUIRE(owner.Prepare(c.ea, sequence));
    REQUIRE(owner.Open());
    const uint32_t returned = owner.leaf->function()(
        &owner.input, &owner.record, &owner.memory.mapping());
    // Cleanup occurs before ANY assertion can throw, with RAII also covering
    // failed Open/REQUIRE paths. All owners remain live through checked
    // removal.
    owner.Close();
    REQUIRE(owner.removed);
    REQUIRE(owner.restored);
    REQUIRE_FALSE(current_admission);
    REQUIRE_FALSE(inside_handler);
    REQUIRE(returned == 0);
    const auto& result = owner.captured;
    REQUIRE(result.complete);
    REQUIRE(result.events == 1);
    CHECK(result.exception.ExceptionCode == EXCEPTION_ACCESS_VIOLATION);
    CHECK(result.exception.ExceptionFlags == 0);
    CHECK(result.exception.ExceptionRecord == nullptr);
    CHECK(result.exception.NumberParameters >= 2);
    CHECK(result.exception.NumberParameters <= EXCEPTION_MAXIMUM_PARAMETERS);
    CHECK(result.raw_rip == owner.admission.access);
    CHECK(result.raw_r11 == owner.admission.host_target);
    CHECK(result.raw_address == owner.admission.host_target);
    CHECK(result.raw_direction == owner.admission.direction);
    CHECK(result.context.Rip == result.raw_rip);
    CHECK(result.context.Rsp == result.raw_rsp);
    CHECK(result.context.R11 == result.raw_r11);
    CHECK(reinterpret_cast<uint64_t>(result.exception.ExceptionAddress) ==
          result.raw_rip);
    CHECK(result.armed.phase == Phase(CheckpointActivationState::kArmed));
    CHECK(result.armed.sequence == sequence++);
    CHECK(result.armed.guest_ea == c.ea);
    CheckState(result.armed.state, expected);
    CheckState(owner.record.state, expected);
    CHECK(owner.record.phase == Phase(CheckpointActivationState::kDisarmed));
    CHECK(owner.record.sequence == result.armed.sequence);
    CHECK(owner.record.guest_ea == result.armed.guest_ea);
    CHECK(std::memcmp(input_before.data(), &owner.input, sizeof(owner.input)) ==
          0);
    CHECK(std::memcmp(memory_before.data(), owner.memory.data(),
                      owner.memory.size()) == 0);
    CHECK(owner.memory.SentinelsIntact());

    // Verify the handler changed only RIP in the borrowed fixed CONTEXT.
    CONTEXT redirected{};
    std::memcpy(&redirected, &result.context, sizeof(CONTEXT));
    redirected.Rip = owner.admission.decline;
    CHECK(std::memcmp(&redirected, &result.recovery_context, sizeof(CONTEXT)) ==
          0);
    REQUIRE(result.return_word != 0);
    REQUIRE(Span(result.raw_rsp, 8, owner.admission.stack_begin,
                 owner.admission.stack_end));
    // Leaf caller simulation uses ONLY the copies, after the callback exited.
    // Never reread the live return slot or call RtlVirtualUnwind with null
    // entry.
    CONTEXT caller{};
    std::memcpy(&caller, &result.context, sizeof(CONTEXT));
    caller.Rip = result.return_word;
    caller.Rsp = result.raw_rsp + 8;
    CHECK(caller.Rip == result.return_word);
    CHECK(caller.Rsp == result.context.Rsp + 8);
    CHECK(caller.Rbx == result.context.Rbx);
    CHECK(caller.Rbp == result.context.Rbp);
    CHECK(caller.Rsi == result.context.Rsi);
    CHECK(caller.Rdi == result.context.Rdi);
    CHECK(caller.R12 == result.context.R12);
    CHECK(caller.R13 == result.context.R13);
    CHECK(caller.R14 == result.context.R14);
    CHECK(caller.R15 == result.context.R15);
  }
}

TEST_CASE("Owned AV admission rejects borrowed mismatches without edits",
          "[cpu][checkpoint-x64-host-fault]") {
  // These are rejection-only unit cases. No VEH is installed and no page's
  // protection is changed. Never pass an arbitrary RSP to a successful copy.
  HostFaultFixture owner(Word(32, 4, 4, -4), 16, 64);
  REQUIRE(owner.build_result == BuildResult::kSuccess);
  REQUIRE(owner.memory.valid());
  Seed(owner.input);
  owner.input.r[4] = 32;
  REQUIRE(owner.Prepare(28, 1));
  owner.record.state = Expected(owner.input);
  owner.record.phase = Phase(CheckpointActivationState::kArmed);
  owner.record.guest_ea = 28;
  owner.admission.accepting = true;
  EXCEPTION_RECORD baseline_exception{};
  baseline_exception.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  baseline_exception.ExceptionAddress =
      reinterpret_cast<void*>(owner.admission.access);
  baseline_exception.NumberParameters = 2;
  baseline_exception.ExceptionInformation[0] = 0;
  baseline_exception.ExceptionInformation[1] = owner.admission.host_target;
  CONTEXT baseline_context{};
  baseline_context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  baseline_context.Rip = owner.admission.access;
  baseline_context.Rcx = reinterpret_cast<uint64_t>(&owner.input);
  baseline_context.Rdx = reinterpret_cast<uint64_t>(&owner.record);
  baseline_context.R11 = owner.admission.host_target;
  // This numeric slot establishes the positive predicate, but is never read.
  baseline_context.Rsp = (owner.admission.stack_begin + 23) & ~uint64_t{15};
  baseline_context.Rsp += 8;
  REQUIRE(Admits(owner.admission, baseline_exception, baseline_context));
  for (uint32_t mismatch = 0; mismatch < 25; ++mismatch) {
    CAPTURE(mismatch);
    auto exception = baseline_exception;
    auto context = baseline_context;
    const auto saved_admission = owner.admission;
    const auto saved_record = owner.record;
    switch (mismatch) {
      case 0:
        exception.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
      case 1:
        exception.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        break;
      case 2:
        exception.ExceptionFlags = EXCEPTION_UNWINDING;
        break;
      case 3:
        exception.ExceptionRecord = reinterpret_cast<EXCEPTION_RECORD*>(1);
        break;
      case 4:
        exception.NumberParameters = 1;
        break;
      case 5:
        exception.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS + 1;
        break;
      case 6:
        exception.ExceptionInformation[0] = 1;
        break;
      case 7:
        exception.ExceptionInformation[0] = 8;
        break;
      case 8:
        ++exception.ExceptionInformation[1];
        break;
      case 9:
        exception.ExceptionAddress =
            reinterpret_cast<void*>(owner.admission.access + 1);
        break;
      case 10:
        ++context.Rip;
        break;
      case 11:
        context.ContextFlags = CONTEXT_CONTROL;
        break;
      case 12:
        ++context.Rcx;
        break;
      case 13:
        ++context.Rdx;
        break;
      case 14:
        ++context.R11;
        break;
      case 15:
        ++owner.record.sequence;
        break;
      case 16:
        owner.record.phase = Phase(CheckpointActivationState::kWriting);
        break;
      case 17:
        ++owner.record.guest_ea;
        break;
      case 18:
        owner.record.state.modeled_valid.vmx128[3] = 0;
        break;
      case 19:
        owner.admission.accepting = false;
        break;
      case 20:
        owner.admission.page_end = owner.admission.host_target + 3;
        break;
      case 21:
        ++context.Rsp;
        break;
      case 22:
        context.Rsp = owner.admission.stack_end;
        break;
      case 23:
        owner.admission.stack_end = context.Rsp + 7;
        break;
      case 24:
        owner.admission.ea |= 1;
        break;
    }
    std::array<uint8_t, sizeof(CONTEXT)> context_before{};
    std::array<uint8_t, sizeof(EXCEPTION_RECORD)> exception_before{};
    std::array<uint8_t, sizeof(Record)> record_before{};
    std::memcpy(context_before.data(), &context, sizeof(context));
    std::memcpy(exception_before.data(), &exception, sizeof(exception));
    std::memcpy(record_before.data(), &owner.record, sizeof(owner.record));
    EXCEPTION_POINTERS pointers{&exception, &context};
    current_admission = &owner.admission;
    const LONG disposition = ObserveOwnedFault(&pointers);
    current_admission = nullptr;
    REQUIRE(disposition == EXCEPTION_CONTINUE_SEARCH);
    CHECK_FALSE(inside_handler);
    CHECK(std::memcmp(context_before.data(), &context, sizeof(context)) == 0);
    CHECK(std::memcmp(exception_before.data(), &exception, sizeof(exception)) ==
          0);
    CHECK(std::memcmp(record_before.data(), &owner.record,
                      sizeof(owner.record)) == 0);
    CHECK(owner.captured.events == 0);
    CHECK_FALSE(owner.captured.complete);
    owner.admission = saved_admission;
    owner.record = saved_record;
  }
  // A deliberately invalid borrowed pointer must never be read while nested or
  // ownerless. Reset TLS before any assertion can throw.
  current_admission = &owner.admission;
  inside_handler = true;
  const LONG nested =
      ObserveOwnedFault(reinterpret_cast<EXCEPTION_POINTERS*>(1));
  inside_handler = false;
  current_admission = nullptr;
  const LONG ownerless =
      ObserveOwnedFault(reinterpret_cast<EXCEPTION_POINTERS*>(1));
  CHECK(nested == EXCEPTION_CONTINUE_SEARCH);
  CHECK(ownerless == EXCEPTION_CONTINUE_SEARCH);
  CHECK(owner.captured.events == 0);
}

}  // namespace
}  // namespace xe::cpu::ppc::test

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
