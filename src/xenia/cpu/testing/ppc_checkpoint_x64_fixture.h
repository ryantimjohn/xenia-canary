/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_CPU_TESTING_PPC_CHECKPOINT_X64_FIXTURE_H_
#define XENIA_CPU_TESTING_PPC_CHECKPOINT_X64_FIXTURE_H_

#include "xenia/base/platform.h"

// Deliberately test-only Windows x64 scope. No production emitter, arena,
// exception handler, guest function, code cache or indirection table uses this.
#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/capstone/include/capstone/x86.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/ppc/ppc_checkpoint_activation.h"

#ifndef XBYAK_NO_OP_NAMES
#define XBYAK_NO_OP_NAMES
#endif
#include "third_party/xbyak/xbyak/xbyak.h"

namespace xe::cpu::ppc::test::x64_fixture {

constexpr uint32_t Phase(CheckpointActivationState state) {
  return static_cast<uint32_t>(state);
}

// Diagnostic output with host-owned lifetime, independent of the private
// reference arena. These plain markers are NOT publication or fault
// eligibility. After normal return, the owner may inspect its retained
// historical snapshot.
struct Record {
  FaultModeledState state{};
  uint64_t sequence = 0;
  uint32_t guest_ea = 0;
  uint32_t phase = Phase(CheckpointActivationState::kInvalid);
};

struct Mapping {
  uint64_t guest_begin = 0;
  uint64_t guest_end = 0;
  uint8_t* host_begin = nullptr;
};

// A small synthetic interval, not an emulator virtual mapping. Allocation and
// release happen outside generated code. Sixteen-byte sentinels flank the data.
class SyntheticMemory {
 public:
  SyntheticMemory(uint32_t guest_begin, uint32_t size) {
    const size_t page = xe::memory::page_size();
    if (!size || page <= 32 || size > page - 32 ||
        uint64_t{guest_begin} + size > (uint64_t{1} << 32)) {
      return;
    }
    allocation_ = static_cast<uint8_t*>(xe::memory::AllocFixed(
        nullptr, page, xe::memory::AllocationType::kReserveCommit,
        xe::memory::PageAccess::kReadWrite));
    if (!allocation_) {
      return;
    }
    std::memset(allocation_, 0xCD, page);
    mapping_ = {guest_begin, uint64_t{guest_begin} + size, allocation_ + 16};
  }
  ~SyntheticMemory() {
    if (allocation_) {
      xe::memory::DeallocFixed(allocation_, 0,
                               xe::memory::DeallocationType::kRelease);
    }
  }
  SyntheticMemory(const SyntheticMemory&) = delete;
  SyntheticMemory& operator=(const SyntheticMemory&) = delete;
  SyntheticMemory(SyntheticMemory&&) = delete;
  SyntheticMemory& operator=(SyntheticMemory&&) = delete;

  bool valid() const { return allocation_ != nullptr; }
  const Mapping& mapping() const { return mapping_; }
  uint8_t* data() { return mapping_.host_begin; }
  size_t size() const {
    return static_cast<size_t>(mapping_.guest_end - mapping_.guest_begin);
  }
  bool SentinelsIntact() const {
    if (!valid()) {
      return false;
    }
    for (size_t n = 0; n < 16; ++n) {
      if (allocation_[n] != 0xCD || data_end()[n] != 0xCD) {
        return false;
      }
    }
    return true;
  }

 private:
  const uint8_t* data_end() const { return mapping_.host_begin + size(); }
  uint8_t* allocation_ = nullptr;
  Mapping mapping_{};
};

struct Offsets {
  size_t capture = 0;
  size_t validity = 0;
  size_t store_swap = 0;
  size_t arm = 0;
  size_t access = 0;
  size_t disarm = 0;
  size_t load_swap = 0;
  size_t writeback = 0;
  size_t success_return = 0;
  size_t decline = 0;
  size_t decline_return = 0;
};

enum class BuildResult {
  kSuccess,
  kUnsupportedInstruction,
  kAllocationFailed,
  kEmissionFailed,
  kVerificationFailed,
  kProtectionFailed,
  kCacheFlushFailed,
};

inline bool Range(size_t offset, size_t width, size_t limit) {
  return width && offset <= limit && width <= limit - offset;
}

inline int Disp(size_t offset, size_t width, size_t limit) {
  if (!Range(offset, width, limit) ||
      offset > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    throw std::runtime_error("Invalid fixture field displacement");
  }
  return static_cast<int>(offset);
}

inline int P(size_t offset, size_t width) {
  return Disp(offset, width, sizeof(PPCContext));
}
inline int S(size_t offset, size_t width) {
  return Disp(offsetof(Record, state) + offset, width, sizeof(Record));
}
inline int R(size_t offset, size_t width) {
  return Disp(offset, width, sizeof(Record));
}
inline int M(size_t offset) { return Disp(offset, 8, sizeof(Mapping)); }

// Emits a Microsoft x64 ABI leaf: no calls, stack changes or nonvolatile
// register writes. RCX=input, RDX=output, R8=mapping then CR accumulator,
// R9d=old wrapped EA, R10d=old store source, R11=host target; RAX/XMM0=scratch.
class Generator : public Xbyak::CodeGenerator {
 public:
  Generator(void* buffer, size_t capacity, uint32_t word, Offsets& offsets)
      : Xbyak::CodeGenerator(capacity, buffer) {
    static_assert(std::is_standard_layout_v<PPCContext>);
    static_assert(std::is_standard_layout_v<Record>);
    static_assert(sizeof(PPCContext::r) == 32 * 8);
    static_assert(sizeof(PPCContext::f) == 32 * 8);
    static_assert(sizeof(PPCContext::v) == 128 * 16);
    static_assert(sizeof(PPCContext::lr) == 8 && sizeof(PPCContext::ctr) == 8 &&
                  sizeof(PPCContext::msr) == 8);
    static_assert(sizeof(PPCContext::fpscr) == 4 &&
                  sizeof(PPCContext::vrsave) == 4);
    static_assert(
        sizeof(PPCContext::xer_ca) == 1 && sizeof(PPCContext::xer_ov) == 1 &&
        sizeof(PPCContext::xer_so) == 1 && sizeof(PPCContext::vscr_sat) == 1);
    static_assert(sizeof(PPCContext::vscr_vec) == 16);
    static_assert(offsetof(PPCContext, cr1) == offsetof(PPCContext, cr0) + 4);
    static_assert(offsetof(PPCContext, cr2) == offsetof(PPCContext, cr0) + 8);
    static_assert(offsetof(PPCContext, cr3) == offsetof(PPCContext, cr0) + 12);
    static_assert(offsetof(PPCContext, cr4) == offsetof(PPCContext, cr0) + 16);
    static_assert(offsetof(PPCContext, cr5) == offsetof(PPCContext, cr0) + 20);
    static_assert(offsetof(PPCContext, cr6) == offsetof(PPCContext, cr0) + 24);
    static_assert(offsetof(PPCContext, cr7) == offsetof(PPCContext, cr0) + 28);
    static_assert(sizeof(FaultModeledValidity) == 28);
    static_assert(offsetof(FaultModeledValidity, scalar) == 24);
    static_assert(sizeof(FaultArchitecturalValidity) == 36);
    static_assert(sizeof(Mapping::host_begin) == 8);
    const uint32_t ra = (word >> 16) & 31;
    const uint32_t rt_rs = (word >> 21) & 31;
    const bool load = (word >> 26) == 32;
    const int32_t disp = (word & 0x8000)
                             ? static_cast<int32_t>(word & 0xFFFF) - 0x10000
                             : static_cast<int32_t>(word & 0xFFFF);
    Xbyak::Label decline;
    // Invalidate even direct-entry preflight failures before inspecting EA.
    mov(dword[rdx + R(offsetof(Record, phase), 4)],
        Phase(CheckpointActivationState::kInvalid));
    ZeroWords(offsetof(FaultModeledState, modeled_valid),
              sizeof(FaultModeledValidity));
    ZeroWords(offsetof(FaultModeledState, architectural_valid),
              sizeof(FaultArchitecturalValidity));
    if (ra) {
      mov(r9d, dword[rcx + P(offsetof(PPCContext, r) + ra * 8, 4)]);
    } else {
      xor_(r9d, r9d);
    }
    add(r9d, disp);
    test(r9d, 3);
    jnz(decline, T_NEAR);
    mov(eax, r9d);
    cmp(rax, qword[r8 + M(offsetof(Mapping, guest_begin))]);
    jb(decline, T_NEAR);
    lea(rax, ptr[r9 + 4]);
    cmp(rax, qword[r8 + M(offsetof(Mapping, guest_end))]);
    ja(decline, T_NEAR);
    mov(r11, qword[r8 + M(offsetof(Mapping, host_begin))]);
    test(r11, r11);
    jz(decline, T_NEAR);
    mov(eax, r9d);
    sub(rax, qword[r8 + M(offsetof(Mapping, guest_begin))]);
    add(r11, rax);
    if (!load) {
      mov(r10d, dword[rcx + P(offsetof(PPCContext, r) + rt_rs * 8, 4)]);
    }
    offsets.capture = getSize();
    mov(dword[rdx + R(offsetof(Record, phase), 4)],
        Phase(CheckpointActivationState::kWriting));
    mov(dword[rdx + R(offsetof(Record, guest_ea), 4)], r9d);
    for (size_t n = 0; n < 32; ++n) {
      Copy64(offsetof(PPCContext, r) + n * 8,
             offsetof(FaultModeledState, gpr) + n * 8);
      Copy64(offsetof(PPCContext, f) + n * 8,
             offsetof(FaultModeledState, raw_fpr) + n * 8);
    }
    Copy64(offsetof(PPCContext, lr), offsetof(FaultModeledState, lr));
    Copy64(offsetof(PPCContext, ctr), offsetof(FaultModeledState, ctr));
    Copy64(offsetof(PPCContext, msr), offsetof(FaultModeledState, msr));
    xor_(r8d, r8d);
    for (size_t n = 0; n < 32; ++n) {
      movzx(eax, byte[rcx + P(offsetof(PPCContext, cr0) + n, 1)]);
      and_(eax, 1);
      shl(eax, static_cast<uint8_t>(31 - n));
      or_(r8d, eax);
    }
    mov(dword[rdx + S(offsetof(FaultModeledState, cr), 4)], r8d);
    for (size_t n = 0; n < 128; ++n) {
      Copy16(offsetof(PPCContext, v) + n * 16,
             offsetof(FaultModeledState, vmx128) + n * 16);
    }
    Copy16(offsetof(PPCContext, vscr_vec),
           offsetof(FaultModeledState, vscr_vector));
    CopyFlag(offsetof(PPCContext, xer_ca), offsetof(FaultModeledState, xer_ca));
    CopyFlag(offsetof(PPCContext, xer_ov), offsetof(FaultModeledState, xer_ov));
    CopyFlag(offsetof(PPCContext, xer_so), offsetof(FaultModeledState, xer_so));
    CopyFlag(offsetof(PPCContext, vscr_sat),
             offsetof(FaultModeledState, vscr_sat));
    Copy32(offsetof(PPCContext, fpscr), offsetof(FaultModeledState, fpscr));
    Copy32(offsetof(PPCContext, vrsave), offsetof(FaultModeledState, vrsave));
    offsets.validity = getSize();
    for (size_t n = 0; n < sizeof(FaultModeledValidity) / 4; ++n) {
      const uint32_t value = n + 1 == sizeof(FaultModeledValidity) / 4
                                 ? kAllModeledScalars
                                 : UINT32_MAX;
      mov(dword[rdx + S(offsetof(FaultModeledState, modeled_valid) + n * 4, 4)],
          value);
    }
    if (!load) {
      offsets.store_swap = getSize();
      bswap(r10d);
    }
    offsets.arm = getSize();
    mov(dword[rdx + R(offsetof(Record, phase), 4)],
        Phase(CheckpointActivationState::kArmed));
    offsets.access = getSize();
    if (load) {
      mov(eax, dword[r11]);
    } else {
      mov(dword[r11], r10d);
    }
    offsets.disarm = getSize();
    mov(dword[rdx + R(offsetof(Record, phase), 4)],
        Phase(CheckpointActivationState::kDisarmed));
    if (load) {
      offsets.load_swap = getSize();
      bswap(eax);
      offsets.writeback = getSize();
      mov(qword[rcx + P(offsetof(PPCContext, r) + rt_rs * 8, 8)], rax);
    }
    mov(eax, 1);
    offsets.success_return = getSize();
    ret();
    offsets.decline = getSize();
    L(decline);
    xor_(eax, eax);
    offsets.decline_return = getSize();
    ret();
    ready();
  }

 private:
  void ZeroWords(size_t destination, size_t bytes) {
    for (size_t n = 0; n < bytes; n += 4) {
      mov(dword[rdx + S(destination + n, 4)], 0);
    }
  }
  void Copy64(size_t source, size_t destination) {
    mov(rax, qword[rcx + P(source, 8)]);
    mov(qword[rdx + S(destination, 8)], rax);
  }
  void Copy32(size_t source, size_t destination) {
    mov(eax, dword[rcx + P(source, 4)]);
    mov(dword[rdx + S(destination, 4)], eax);
  }
  void Copy16(size_t source, size_t destination) {
    movdqu(xmm0, ptr[rcx + P(source, 16)]);
    movdqu(ptr[rdx + S(destination, 16)], xmm0);
  }
  void CopyFlag(size_t source, size_t destination) {
    movzx(eax, byte[rcx + P(source, 1)]);
    test(eax, eax);
    setne(al);
    mov(byte[rdx + S(destination, 1)], al);
  }
};

inline x86_reg WholeRegister(unsigned reg) {
  switch (reg) {
    case X86_REG_AL:
    case X86_REG_AH:
    case X86_REG_AX:
    case X86_REG_EAX:
    case X86_REG_RAX:
      return X86_REG_RAX;
    case X86_REG_CL:
    case X86_REG_CH:
    case X86_REG_CX:
    case X86_REG_ECX:
    case X86_REG_RCX:
      return X86_REG_RCX;
    case X86_REG_DL:
    case X86_REG_DH:
    case X86_REG_DX:
    case X86_REG_EDX:
    case X86_REG_RDX:
      return X86_REG_RDX;
    case X86_REG_R8B:
    case X86_REG_R8W:
    case X86_REG_R8D:
    case X86_REG_R8:
      return X86_REG_R8;
    case X86_REG_R9B:
    case X86_REG_R9W:
    case X86_REG_R9D:
    case X86_REG_R9:
      return X86_REG_R9;
    case X86_REG_R10B:
    case X86_REG_R10W:
    case X86_REG_R10D:
    case X86_REG_R10:
      return X86_REG_R10;
    case X86_REG_R11B:
    case X86_REG_R11W:
    case X86_REG_R11D:
    case X86_REG_R11:
      return X86_REG_R11;
    default:
      return static_cast<x86_reg>(reg);
  }
}

inline bool ScratchRegister(unsigned reg, bool writing) {
  switch (WholeRegister(reg)) {
    case X86_REG_RAX:
    case X86_REG_R8:
    case X86_REG_R9:
    case X86_REG_R10:
    case X86_REG_R11:
    case X86_REG_XMM0:
    case X86_REG_EFLAGS:
      return true;
    case X86_REG_RCX:
    case X86_REG_RDX:
      return !writing;
    default:
      return false;
  }
}

template <size_t N>
inline void Mark(std::array<bool, N>& bytes, size_t offset, size_t size) {
  if (!Range(offset, size, N)) {
    throw std::runtime_error("Invalid fixture expected range");
  }
  for (size_t n = offset; n < offset + size; ++n) {
    bytes[n] = true;
  }
}

inline std::array<bool, sizeof(PPCContext)> SourceMask() {
  std::array<bool, sizeof(PPCContext)> bytes{};
  Mark(bytes, offsetof(PPCContext, r), 32 * 8);
  Mark(bytes, offsetof(PPCContext, f), 32 * 8);
  Mark(bytes, offsetof(PPCContext, v), 128 * 16);
  Mark(bytes, offsetof(PPCContext, cr0), 32);
  Mark(bytes, offsetof(PPCContext, lr), 8);
  Mark(bytes, offsetof(PPCContext, ctr), 8);
  Mark(bytes, offsetof(PPCContext, msr), 8);
  Mark(bytes, offsetof(PPCContext, fpscr), 4);
  Mark(bytes, offsetof(PPCContext, vrsave), 4);
  Mark(bytes, offsetof(PPCContext, vscr_vec), 16);
  Mark(bytes, offsetof(PPCContext, xer_ca), 1);
  Mark(bytes, offsetof(PPCContext, xer_ov), 1);
  Mark(bytes, offsetof(PPCContext, xer_so), 1);
  Mark(bytes, offsetof(PPCContext, vscr_sat), 1);
  return bytes;
}

inline std::array<bool, sizeof(FaultModeledState)> PayloadMask() {
  std::array<bool, sizeof(FaultModeledState)> bytes{};
  Mark(bytes, offsetof(FaultModeledState, gpr), 32 * 8);
  Mark(bytes, offsetof(FaultModeledState, raw_fpr), 32 * 8);
  Mark(bytes, offsetof(FaultModeledState, vmx128), 128 * 16);
  Mark(bytes, offsetof(FaultModeledState, cr), 4);
  Mark(bytes, offsetof(FaultModeledState, lr), 8);
  Mark(bytes, offsetof(FaultModeledState, ctr), 8);
  Mark(bytes, offsetof(FaultModeledState, msr), 8);
  Mark(bytes, offsetof(FaultModeledState, fpscr), 4);
  Mark(bytes, offsetof(FaultModeledState, vrsave), 4);
  Mark(bytes, offsetof(FaultModeledState, vscr_vector), 16);
  Mark(bytes, offsetof(FaultModeledState, xer_ca), 1);
  Mark(bytes, offsetof(FaultModeledState, xer_ov), 1);
  Mark(bytes, offsetof(FaultModeledState, xer_so), 1);
  Mark(bytes, offsetof(FaultModeledState, vscr_sat), 1);
  return bytes;
}

struct Decoder {
  csh handle = 0;
  cs_insn* instruction = nullptr;
  Decoder() {
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK &&
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK) {
      instruction = cs_malloc(handle);
    }
  }
  ~Decoder() {
    if (instruction) {
      cs_free(instruction, 1);
    }
    if (handle) {
      cs_close(&handle);
    }
  }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

inline bool ImmediateStore(const cs_insn& instruction, size_t destination,
                           uint32_t value) {
  const auto& x = instruction.detail->x86;
  return instruction.id == X86_INS_MOV && x.op_count == 2 &&
         x.operands[0].type == X86_OP_MEM &&
         x.operands[0].mem.base == X86_REG_RDX &&
         x.operands[0].mem.disp == static_cast<int64_t>(destination) &&
         x.operands[0].size == 4 && x.operands[1].type == X86_OP_IMM &&
         static_cast<uint32_t>(x.operands[1].imm) == value;
}

// Decode the final bytes, not emission callbacks or opcode enums alone. This
// establishes this fixture's bounded leaf shape and memory operand coverage;
// executable comparisons independently establish copied values and semantics.
inline bool VerifyBytes(const uint8_t* code, size_t size, uint32_t word,
                        const Offsets& o, size_t* access_length = nullptr) {
  if (!code || !size || ((word >> 26) != 32 && (word >> 26) != 36) ||
      o.capture >= o.validity || o.validity >= o.arm || o.arm >= o.access ||
      o.access >= o.disarm || o.disarm >= o.success_return ||
      o.success_return >= o.decline || o.decline >= o.decline_return ||
      o.decline_return != size - 1) {
    return false;
  }
  Decoder decoder;
  if (!decoder.instruction) {
    return false;
  }
  auto expected_source = SourceMask();
  auto expected_payload = PayloadMask();
  std::array<bool, sizeof(PPCContext)> read_source{};
  std::array<bool, sizeof(FaultModeledState)> wrote_payload{};
  std::array<bool, sizeof(FaultModeledValidity) / 4> initial_validity{};
  std::array<bool, sizeof(FaultModeledValidity) / 4> final_validity{};
  std::array<bool, sizeof(FaultArchitecturalValidity) / 4> architectural{};
  const bool load = (word >> 26) == 32;
  const uint32_t ra = (word >> 16) & 31;
  const uint32_t rt_rs = (word >> 21) & 31;
  size_t target_accesses = 0;
  size_t returns = 0;
  bool found_invalid = false, found_writing = false;
  bool found_arm = false, found_access = false, found_disarm = false;
  bool found_writeback = !load, found_swap = false;
  size_t previous_end = 0;
  uint64_t address = reinterpret_cast<uintptr_t>(code);
  const uint8_t* cursor = code;
  size_t remaining = size;
  while (remaining) {
    if (!cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                        decoder.instruction)) {
      return false;
    }
    const auto& i = *decoder.instruction;
    if (!i.detail) {
      return false;
    }
    const size_t offset =
        static_cast<size_t>(i.address - reinterpret_cast<uintptr_t>(code));
    const auto& x = i.detail->x86;
    if (x.prefix[1] || x.prefix[2] || x.prefix[3] ||
        (x.prefix[0] && !(i.id == X86_INS_MOVDQU && x.prefix[0] == 0xF3))) {
      return false;
    }
    const bool capture = offset >= o.capture && offset < o.validity;
    const bool branch = i.id == X86_INS_JA || i.id == X86_INS_JB ||
                        i.id == X86_INS_JE || i.id == X86_INS_JNE;
    switch (i.id) {
      case X86_INS_MOV:
      case X86_INS_MOVDQU:
      case X86_INS_MOVZX:
      case X86_INS_XOR:
      case X86_INS_AND:
      case X86_INS_OR:
      case X86_INS_SHL:
      case X86_INS_ADD:
      case X86_INS_SUB:
      case X86_INS_CMP:
      case X86_INS_TEST:
      case X86_INS_SETNE:
      case X86_INS_LEA:
      case X86_INS_BSWAP:
        break;
      case X86_INS_JA:
      case X86_INS_JB:
      case X86_INS_JE:
      case X86_INS_JNE:
        if (offset >= o.capture || x.op_count != 1 ||
            x.operands[0].type != X86_OP_IMM ||
            x.operands[0].imm !=
                static_cast<int64_t>(reinterpret_cast<uintptr_t>(code) +
                                     o.decline)) {
          return false;
        }
        break;
      case X86_INS_RET:
        if (i.size != 1 || i.bytes[0] != 0xC3 || x.op_count ||
            (offset != o.success_return && offset != o.decline_return)) {
          return false;
        }
        ++returns;
        break;
      default:
        return false;
    }
    cs_regs reads{}, writes{};
    uint8_t read_count = 0, write_count = 0;
    if (cs_regs_access(decoder.handle, &i, reads, &read_count, writes,
                       &write_count) != CS_ERR_OK) {
      return false;
    }
    for (size_t n = 0; n < read_count; ++n) {
      if (!(i.id == X86_INS_RET && reads[n] == X86_REG_RSP) &&
          !((i.id == X86_INS_RET || branch) && reads[n] == X86_REG_RIP) &&
          !ScratchRegister(reads[n], false)) {
        return false;
      }
    }
    for (size_t n = 0; n < write_count; ++n) {
      if (!(i.id == X86_INS_RET && writes[n] == X86_REG_RSP) &&
          !((i.id == X86_INS_RET || branch) && writes[n] == X86_REG_RIP) &&
          !ScratchRegister(writes[n], true)) {
        return false;
      }
      const auto whole = WholeRegister(writes[n]);
      if (offset < o.capture && whole == X86_REG_R8) {
        return false;
      }
      if (offset >= o.capture &&
          (whole == X86_REG_R9 || whole == X86_REG_R11 ||
           (whole == X86_REG_R10 && offset != o.store_swap))) {
        return false;
      }
    }
    if (offset == o.arm) {
      if (!ImmediateStore(i, offsetof(Record, phase),
                          Phase(CheckpointActivationState::kArmed)) ||
          offset + i.size != o.access) {
        return false;
      }
      found_arm = true;
    }
    if (offset == o.disarm) {
      if (previous_end != o.disarm ||
          !ImmediateStore(i, offsetof(Record, phase),
                          Phase(CheckpointActivationState::kDisarmed))) {
        return false;
      }
      found_disarm = true;
      if (load && offset + i.size != o.load_swap) {
        return false;
      }
    }
    if (i.id == X86_INS_BSWAP) {
      if (x.op_count != 1 || x.operands[0].type != X86_OP_REG ||
          x.operands[0].reg != (load ? X86_REG_EAX : X86_REG_R10D) ||
          offset != (load ? o.load_swap : o.store_swap) ||
          offset + i.size != (load ? o.writeback : o.arm)) {
        return false;
      }
      found_swap = true;
    }
    for (size_t n = 0; n < x.op_count; ++n) {
      const auto& operand = x.operands[n];
      if (operand.type != X86_OP_MEM) {
        continue;
      }
      // LEA computes a number; it does not access memory.
      if (i.id == X86_INS_LEA) {
        if (offset >= o.capture || operand.mem.base != X86_REG_R9 ||
            operand.mem.index != X86_REG_INVALID || operand.mem.disp != 4) {
          return false;
        }
        continue;
      }
      if (operand.mem.segment != X86_REG_INVALID ||
          operand.mem.index != X86_REG_INVALID || operand.mem.disp < 0) {
        return false;
      }
      const size_t field = static_cast<size_t>(operand.mem.disp);
      if (operand.mem.base == X86_REG_R11) {
        if (offset != o.access || !found_arm || previous_end != o.access ||
            i.id != X86_INS_MOV || x.op_count != 2 || field ||
            operand.size != 4 ||
            operand.access != (load ? CS_AC_READ : CS_AC_WRITE) ||
            x.operands[load ? 0 : 1].type != X86_OP_REG ||
            x.operands[load ? 0 : 1].reg !=
                (load ? X86_REG_EAX : X86_REG_R10D) ||
            offset + i.size != o.disarm) {
          return false;
        }
        ++target_accesses;
        found_access = true;
        if (access_length) {
          *access_length = i.size;
        }
      } else if (operand.mem.base == X86_REG_RCX) {
        if (!Range(field, operand.size, sizeof(PPCContext))) {
          return false;
        }
        if (operand.access == CS_AC_WRITE) {
          if (!load || offset != o.writeback || i.id != X86_INS_MOV ||
              field != offsetof(PPCContext, r) + rt_rs * 8 ||
              operand.size != 8 || x.op_count != 2 ||
              x.operands[1].type != X86_OP_REG ||
              x.operands[1].reg != X86_REG_RAX || !found_disarm) {
            return false;
          }
          found_writeback = true;
        } else if (operand.access == CS_AC_READ && capture) {
          for (size_t b = field; b < field + operand.size; ++b) {
            if (!expected_source[b] || read_source[b]) {
              return false;
            }
            read_source[b] = true;
          }
        } else if (operand.access != CS_AC_READ || offset >= o.capture ||
                   operand.size != 4 ||
                   (!(ra && field == offsetof(PPCContext, r) + ra * 8) &&
                    (load || field != offsetof(PPCContext, r) + rt_rs * 8))) {
          return false;
        }
      } else if (operand.mem.base == X86_REG_R8) {
        if (offset >= o.capture || operand.access != CS_AC_READ ||
            operand.size != 8 ||
            (field != offsetof(Mapping, guest_begin) &&
             field != offsetof(Mapping, guest_end) &&
             field != offsetof(Mapping, host_begin))) {
          return false;
        }
      } else if (operand.mem.base == X86_REG_RDX) {
        if (operand.access != CS_AC_WRITE ||
            !Range(field, operand.size, sizeof(Record))) {
          return false;
        }
        if (field == offsetof(Record, phase)) {
          if (offset == 0 &&
              ImmediateStore(i, field,
                             Phase(CheckpointActivationState::kInvalid))) {
            found_invalid = true;
          } else if (offset == o.capture &&
                     ImmediateStore(
                         i, field,
                         Phase(CheckpointActivationState::kWriting))) {
            found_writing = true;
          } else if (offset != o.arm && offset != o.disarm) {
            return false;
          }
        } else if (field == offsetof(Record, guest_ea)) {
          if (!capture || operand.size != 4 || i.id != X86_INS_MOV ||
              x.op_count != 2 || x.operands[1].type != X86_OP_REG ||
              x.operands[1].reg != X86_REG_R9D) {
            return false;
          }
        } else {
          if (field < offsetof(Record, state)) {
            return false;
          }
          const size_t state_field = field - offsetof(Record, state);
          const size_t modeled = offsetof(FaultModeledState, modeled_valid);
          const size_t arch = offsetof(FaultModeledState, architectural_valid);
          if (state_field >= modeled &&
              state_field < modeled + sizeof(FaultModeledValidity)) {
            if (operand.size != 4 || (state_field - modeled) % 4) {
              return false;
            }
            const size_t slot = (state_field - modeled) / 4;
            const bool initializing = offset < o.capture;
            auto& seen = initializing ? initial_validity : final_validity;
            const uint32_t value =
                initializing
                    ? 0
                    : (slot + 1 == final_validity.size() ? kAllModeledScalars
                                                         : UINT32_MAX);
            if ((!initializing && (offset < o.validity || offset >= o.arm)) ||
                seen[slot] || !ImmediateStore(i, field, value)) {
              return false;
            }
            seen[slot] = true;
          } else if (state_field >= arch &&
                     state_field < arch + sizeof(FaultArchitecturalValidity)) {
            if (offset >= o.capture || operand.size != 4 ||
                (state_field - arch) % 4 || !ImmediateStore(i, field, 0)) {
              return false;
            }
            const size_t slot = (state_field - arch) / 4;
            if (architectural[slot]) {
              return false;
            }
            architectural[slot] = true;
          } else {
            if (!capture ||
                !Range(state_field, operand.size, sizeof(FaultModeledState))) {
              return false;
            }
            for (size_t b = state_field; b < state_field + operand.size; ++b) {
              if (!expected_payload[b] || wrote_payload[b]) {
                return false;
              }
              wrote_payload[b] = true;
            }
          }
        }
      } else {
        return false;
      }
    }
    previous_end = offset + i.size;
  }
  for (bool seen : initial_validity) {
    if (!seen) {
      return false;
    }
  }
  for (bool seen : final_validity) {
    if (!seen) {
      return false;
    }
  }
  for (bool seen : architectural) {
    if (!seen) {
      return false;
    }
  }
  return previous_end == size && read_source == expected_source &&
         wrote_payload == expected_payload && returns == 2 &&
         target_accesses == 1 && found_arm && found_access && found_disarm &&
         found_invalid && found_writing && found_swap && found_writeback;
}

class LeafFixture {
 public:
  using Function = uint32_t (*)(PPCContext*, Record*, const Mapping*);
  static constexpr size_t kCodeCapacity = 16 * 1024;

  static std::unique_ptr<LeafFixture> Create(uint32_t pc, uint32_t word,
                                             BuildResult& result) {
    result = BuildResult::kUnsupportedInstruction;
    if ((pc & 3) || ((word >> 26) != 32 && (word >> 26) != 36)) {
      return nullptr;
    }
    auto owner = std::unique_ptr<LeafFixture>(new LeafFixture(word));
    owner->allocation_ = static_cast<uint8_t*>(xe::memory::AllocFixed(
        nullptr, kCodeCapacity, xe::memory::AllocationType::kReserveCommit,
        xe::memory::PageAccess::kReadWrite));
    if (!owner->allocation_) {
      result = BuildResult::kAllocationFailed;
      return nullptr;
    }
    result = BuildResult::kEmissionFailed;
    try {
      Generator generator(owner->allocation_, kCodeCapacity, word,
                          owner->offsets_);
      if (generator.hasUndefinedLabel()) {
        return nullptr;
      }
      owner->size_ = generator.getSize();
      result = BuildResult::kVerificationFailed;
      if (!VerifyBytes(owner->allocation_, owner->size_, word, owner->offsets_,
                       &owner->access_length_)) {
        return nullptr;
      }
    } catch (const std::exception&) {
      return nullptr;
    }
    result = BuildResult::kProtectionFailed;
    if (!xe::memory::Protect(owner->allocation_, kCodeCapacity,
                             xe::memory::PageAccess::kExecuteReadOnly)) {
      return nullptr;
    }
    result = BuildResult::kCacheFlushFailed;
    if (!FlushInstructionCache(GetCurrentProcess(), owner->allocation_,
                               owner->size_)) {
      return nullptr;
    }
    // The only callable pointer assignment occurs after verification, RX and
    // checked cache visibility. No code-cache publication is performed.
    owner->function_ = reinterpret_cast<Function>(owner->allocation_);
    result = BuildResult::kSuccess;
    return owner;
  }

  ~LeafFixture() {
    // Exclusive owner must keep this alive until every invocation returns.
    // No asynchronous reader or escaped function-pointer lifetime is supported.
    function_ = nullptr;
    if (allocation_) {
      xe::memory::DeallocFixed(allocation_, 0,
                               xe::memory::DeallocationType::kRelease);
    }
  }
  LeafFixture(const LeafFixture&) = delete;
  LeafFixture& operator=(const LeafFixture&) = delete;
  LeafFixture(LeafFixture&&) = delete;
  LeafFixture& operator=(LeafFixture&&) = delete;

  Function function() const { return function_; }
  const uint8_t* code() const { return allocation_; }
  size_t size() const { return size_; }
  size_t access_length() const { return access_length_; }
  const Offsets& offsets() const { return offsets_; }

  // Exclusive same-thread input/output/mapping ownership and nonoverlapping
  // live objects are assumed. This helper does not prove live JIT currency.
  bool Run(PPCContext& input, Record& output, const SyntheticMemory& memory,
           uint64_t sequence) const {
    output.phase = Phase(CheckpointActivationState::kInvalid);
    output.state.modeled_valid = {};
    output.state.architectural_valid = {};
    if (!function_ || !sequence || !memory.valid()) {
      return false;
    }
    const auto& mapping = memory.mapping();
    const uint32_t ra = (word_ >> 16) & 31;
    const int32_t disp = (word_ & 0x8000)
                             ? static_cast<int32_t>(word_ & 0xFFFF) - 0x10000
                             : static_cast<int32_t>(word_ & 0xFFFF);
    const uint32_t ea = (ra ? static_cast<uint32_t>(input.r[ra]) : 0) +
                        static_cast<uint32_t>(disp);
    if ((ea & 3) || ea < mapping.guest_begin ||
        uint64_t{ea} + 4 > mapping.guest_end) {
      return false;
    }
    output.sequence = sequence;
    return function_(&input, &output, &mapping) == 1;
  }

 private:
  explicit LeafFixture(uint32_t word) : word_(word) {}
  const uint32_t word_;
  uint8_t* allocation_ = nullptr;
  size_t size_ = 0;
  size_t access_length_ = 0;
  Offsets offsets_{};
  Function function_ = nullptr;
};

}  // namespace xe::cpu::ppc::test::x64_fixture

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
#endif  // XENIA_CPU_TESTING_PPC_CHECKPOINT_X64_FIXTURE_H_
