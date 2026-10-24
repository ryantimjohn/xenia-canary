/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_virtual_unwind.h"

#include <algorithm>
#include <limits>

namespace xe::cpu::ppc {
namespace {

constexpr uint64_t kGuestAddressEnd = uint64_t{1} << 32;
constexpr size_t kCodeHardLimit = 256;
constexpr size_t kStackReadHardLimit = 32;
constexpr size_t kStackReadsHardLimit = 4;
constexpr uint32_t kFrameHardLimit = 4096;

uint32_t ReadBe32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

uint64_t ReadBe64(const uint8_t* bytes) {
  return (uint64_t(ReadBe32(bytes)) << 32) | ReadBe32(bytes + 4);
}

uint32_t RegisterA(uint32_t word) { return (word >> 16) & 31; }
uint32_t RegisterS(uint32_t word) { return (word >> 21) & 31; }
uint32_t RegisterB(uint32_t word) { return (word >> 11) & 31; }

bool IsProtectedDestination(uint32_t reg) { return reg == 1 || reg == 31; }

// The accepted body vocabulary is deliberately small. Each form has a decoded
// destination; none can change SP, the frame base, LR or control flow except a
// direct linked call. Stack-based stores are excluded because aliasing the
// backchain or a saved register cannot be established from code alone.
bool IsOrdinaryBodyInstruction(uint32_t word) {
  const uint32_t opcode = word >> 26;
  if (opcode == 14) {  // addi, including li when RA is zero.
    return !IsProtectedDestination(RegisterS(word));
  }
  if (opcode == 15) {  // addis, including lis when RA is zero.
    return !IsProtectedDestination(RegisterS(word));
  }
  if (opcode == 36) {  // stw (non-updating D form).
    return RegisterA(word) != 1 && RegisterA(word) != 31;
  }
  if (opcode == 32) {  // lwz (non-updating D form).
    return !IsProtectedDestination(RegisterS(word));
  }
  if (opcode == 31 && ((word >> 1) & 0x3FF) == 444 &&
      (word & 1) == 0) {  // or rA,rS,rB; only the mr alias is admitted.
    return RegisterS(word) == RegisterB(word) &&
           !IsProtectedDestination(RegisterA(word));
  }
  return false;
}

bool IsExternalDirectCall(uint32_t word, uint64_t pc, uint32_t begin,
                          uint64_t end) {
  if ((word >> 26) != 18 || (word & 3) != 1) {
    return false;  // Only relative b with LK=1 and AA=0.
  }
  const uint32_t encoded = word & 0x03FFFFFC;
  const uint64_t magnitude =
      encoded & 0x02000000 ? uint64_t{0x04000000} - encoded : encoded;
  if (!magnitude) {
    return false;
  }
  uint64_t target = 0;
  if (encoded & 0x02000000) {
    if (magnitude > pc) {
      return false;
    }
    target = pc - magnitude;
  } else {
    target = pc + magnitude;
    if (target >= kGuestAddressEnd) {
      return false;
    }
  }
  return (target & 3) == 0 && (target < begin || target >= end);
}

bool HasLow32(const VirtualUnwindRegisters& registers, uint32_t number) {
  const uint32_t bit = uint32_t{1} << number;
  return ((registers.low32_valid | registers.full64_valid) & bit) != 0;
}

uint32_t SignExtended16(uint32_t word) {
  const uint32_t value = word & 0xFFFF;
  return value & 0x8000 ? value | 0xFFFF0000 : value;
}

bool ReadStack(const OwnedStackSnapshotView& stack, uint64_t address,
               size_t size, const uint8_t** bytes) {
  if (stack.bytes.size() > kGuestAddressEnd - stack.begin ||
      address < stack.begin || address > kGuestAddressEnd ||
      size > kGuestAddressEnd - address ||
      address + size > uint64_t(stack.begin) + stack.bytes.size()) {
    return false;
  }
  *bytes = stack.bytes.data() + size_t(address - stack.begin);
  return true;
}

}  // namespace

RecipeBuildResult BuildBoundedUnwindRecipe(
    const ExceptionMetadataFunction& function,
    const OwnedCodeSnapshotView& code, const VirtualUnwindLimits& limits) {
  RecipeBuildResult result;
  if (code.provenance != UnwindCaptureProvenance::kCoherentPrecise) {
    result.status = VirtualUnwindStatus::kUntrustedCapture;
    return result;
  }
  const uint32_t raw = function.raw_info;
  const uint32_t prologue = raw & 0xFF;
  const uint32_t count = (raw >> 8) & 0x3FFFFF;
  const uint64_t end = uint64_t(function.begin) + uint64_t(count) * 4;
  if (function.status != ExceptionMetadataStatus::kValid ||
      !function.packed_candidate || (raw & 0x40000000) == 0 ||
      function.begin == 0 || (function.begin & 3) || !count ||
      prologue != function.prologue_instructions ||
      count != function.length_instructions || prologue > count ||
      end > kGuestAddressEnd || end != function.end ||
      code.begin != function.begin ||
      code.bytes.size() != uint64_t(count) * 4) {
    return result;
  }
  if (count > std::min(limits.max_code_instructions, kCodeHardLimit)) {
    result.status = VirtualUnwindStatus::kResourceLimit;
    return result;
  }
  if (count < 7 || (prologue != 3 && prologue != 5)) {
    return result;
  }

  BoundedUnwindRecipe recipe;
  recipe.begin_ = function.begin;
  recipe.end_ = end;
  recipe.instruction_count_ = count;
  for (size_t i = 0; i < count; ++i) {
    recipe.instructions_[i] = ReadBe32(code.bytes.data() + i * 4);
  }

  const auto& words = recipe.instructions_;
  const bool saves_r31 = prologue == 5;
  const size_t epilogue_count = saves_r31 ? 5 : 4;
  if (count <= prologue + epilogue_count || words[0] != 0x7D8802A6 ||
      words[1] != 0x9181FFF8) {
    return result;
  }
  const uint32_t stwu = words[prologue - 1];
  const uint32_t negative = stwu & 0xFFFF;
  if ((stwu >> 16) != 0x9421 || negative < 0x8000) {
    return result;
  }
  const uint32_t frame_bytes = 0x10000 - negative;
  // The saved r31 slot must not overlap the backchain at body SP.
  if (frame_bytes < (saves_r31 ? 32u : 16u) || (frame_bytes & 15) ||
      frame_bytes > std::min(limits.max_frame_bytes, kFrameHardLimit)) {
    result.status =
        frame_bytes > std::min(limits.max_frame_bytes, kFrameHardLimit)
            ? VirtualUnwindStatus::kResourceLimit
            : VirtualUnwindStatus::kUnsupportedPattern;
    return result;
  }
  if (saves_r31 &&
      (words[2] != 0xFBE1FFF0 || words[3] != (0x3BE10000 | negative))) {
    return result;
  }
  const size_t epilogue = count - epilogue_count;
  const uint32_t restore_sp =
      (saves_r31 ? 0x383F0000 : 0x38210000) | frame_bytes;
  if (words[epilogue] != restore_sp || words[epilogue + 1] != 0x8181FFF8 ||
      words[epilogue + 2] != 0x7D8803A6 ||
      (saves_r31 && words[epilogue + 3] != 0xEBE1FFF0) ||
      words[count - 1] != 0x4E800020) {
    return result;
  }
  std::array<bool, 32> constant_valid{};
  std::array<uint32_t, 32> constants{};
  for (size_t i = prologue; i < epilogue; ++i) {
    const uint32_t word = words[i];
    const uint64_t pc = uint64_t(function.begin) + i * 4;
    if (IsExternalDirectCall(word, pc, function.begin, end)) {
      // Direct callees must preserve the frame, but no tracked constant is
      // carried across the call without inspecting that callee.
      constant_valid.fill(false);
      continue;
    }
    if (!IsOrdinaryBodyInstruction(word)) {
      return result;
    }
    const uint32_t opcode = word >> 26;
    if (opcode == 14 || opcode == 15) {
      const uint32_t dest = RegisterS(word);
      const uint32_t source = RegisterA(word);
      const bool valid = source == 0 || constant_valid[source];
      const uint32_t base = source == 0 ? 0 : constants[source];
      constant_valid[dest] = valid;
      if (valid) {
        const uint32_t offset =
            opcode == 15 ? (word & 0xFFFF) << 16 : SignExtended16(word);
        constants[dest] = base + offset;
      }
    } else if (opcode == 31) {
      const uint32_t dest = RegisterA(word);
      const uint32_t source = RegisterS(word);
      constant_valid[dest] = constant_valid[source];
      if (constant_valid[dest]) {
        constants[dest] = constants[source];
      }
    } else if (opcode == 32) {
      constant_valid[RegisterS(word)] = false;
    } else if (opcode == 36) {
      const uint32_t base_reg = RegisterA(word);
      if (base_reg != 0 && !constant_valid[base_reg]) {
        return result;
      }
      const uint32_t base = base_reg == 0 ? 0 : constants[base_reg];
      const uint32_t address = base + SignExtended16(word);
      if ((address & 3) || address > UINT32_MAX - 3) {
        return result;
      }
      recipe.constant_stores_[recipe.store_count_++] = address;
    }
  }
  recipe.valid_ = true;
  recipe.saves_r31_ = saves_r31;
  recipe.body_begin_ = function.begin + prologue * 4;
  recipe.body_end_ = function.begin + uint32_t(epilogue * 4);
  recipe.frame_bytes_ = frame_bytes;
  recipe.lr_offset_ = frame_bytes - 8;
  recipe.r31_offset_ = saves_r31 ? frame_bytes - 16 : 0;
  result.status = VirtualUnwindStatus::kSupportedPartialFrame;
  result.recipe = recipe;
  return result;
}

VirtualUnwindResult TryVirtualUnwindFrame(
    const BoundedUnwindRecipe& recipe, const VirtualUnwindRegisters& registers,
    const OwnedStackSnapshotView& stack, const VirtualUnwindLimits& limits) {
  VirtualUnwindResult result;
  if (!recipe.valid_) {
    return result;
  }
  if (registers.provenance != UnwindCaptureProvenance::kCoherentPrecise ||
      stack.provenance != UnwindCaptureProvenance::kCoherentPrecise) {
    result.status = VirtualUnwindStatus::kUntrustedCapture;
    return result;
  }
  const uint32_t pc = registers.control_pc;
  if ((pc & 3) || pc < recipe.begin_ || uint64_t(pc) >= recipe.end_) {
    result.status = VirtualUnwindStatus::kBounds;
    return result;
  }
  if (pc < recipe.body_begin_ || pc >= recipe.body_end_) {
    result.status = VirtualUnwindStatus::kUnsupportedPhase;
    return result;
  }
  if (!HasLow32(registers, 1) ||
      (recipe.saves_r31_ && !HasLow32(registers, 31))) {
    result.status = VirtualUnwindStatus::kMissingRegister;
    return result;
  }
  const uint32_t sp = uint32_t(registers.gpr[1]);
  const uint64_t caller_sp = uint64_t(sp) + recipe.frame_bytes_;
  if ((sp & 3) || caller_sp > std::numeric_limits<uint32_t>::max()) {
    result.status = VirtualUnwindStatus::kBounds;
    return result;
  }
  if (recipe.saves_r31_ && uint32_t(registers.gpr[31]) != sp) {
    result.status = VirtualUnwindStatus::kInconsistentFrame;
    return result;
  }
  const uint64_t lr_address = uint64_t(sp) + recipe.lr_offset_;
  const uint64_t r31_address = uint64_t(sp) + recipe.r31_offset_;
  for (size_t i = 0; i < recipe.store_count_; ++i) {
    const uint64_t address = recipe.constant_stores_[i];
    if (address < caller_sp && address + 4 > sp) {
      result.status = VirtualUnwindStatus::kInconsistentFrame;
      return result;
    }
  }
  if ((lr_address & 3) || (recipe.saves_r31_ && (r31_address & 7)) ||
      lr_address < sp || lr_address + 4 > caller_sp ||
      (recipe.saves_r31_ &&
       (r31_address < sp || r31_address + 8 > caller_sp))) {
    result.status = VirtualUnwindStatus::kBounds;
    return result;
  }
  const size_t read_count = recipe.saves_r31_ ? 3 : 2;
  const size_t read_bytes = recipe.saves_r31_ ? 16 : 8;
  if (recipe.instruction_count_ >
          std::min(limits.max_code_instructions, kCodeHardLimit) ||
      read_count > std::min(limits.max_stack_reads, kStackReadsHardLimit) ||
      read_bytes > std::min(limits.max_stack_read_bytes, kStackReadHardLimit) ||
      recipe.frame_bytes_ > std::min(limits.max_frame_bytes, kFrameHardLimit)) {
    result.status = VirtualUnwindStatus::kResourceLimit;
    return result;
  }
  const uint8_t* backchain_bytes = nullptr;
  const uint8_t* lr_bytes = nullptr;
  const uint8_t* r31_bytes = nullptr;
  if (!ReadStack(stack, sp, 4, &backchain_bytes) ||
      !ReadStack(stack, lr_address, 4, &lr_bytes) ||
      (recipe.saves_r31_ && !ReadStack(stack, r31_address, 8, &r31_bytes))) {
    result.status = VirtualUnwindStatus::kBounds;
    return result;
  }
  if (ReadBe32(backchain_bytes) != caller_sp) {
    result.status = VirtualUnwindStatus::kInconsistentFrame;
    return result;
  }
  result.status = VirtualUnwindStatus::kSupportedPartialFrame;
  result.caller_sp_address32 = uint32_t(caller_sp);
  result.return_address32 = ReadBe32(lr_bytes);
  if (recipe.saves_r31_) {
    result.recovered_r31_full64 = ReadBe64(r31_bytes);
    result.recovered_r31_valid = true;
  }
  return result;
}

}  // namespace xe::cpu::ppc
