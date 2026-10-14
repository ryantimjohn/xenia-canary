/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_virtual_unwind.h"

#include <array>
#include <cstdint>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

using Status = VirtualUnwindStatus;
using Provenance = UnwindCaptureProvenance;

constexpr std::array<uint32_t, 17> kPatternA{
    0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x3D4082D6, 0x7C6B1B78, 0x3D208231,
    0x38691A40, 0x916AC784, 0x484EA1F9, 0x3D6082D6, 0x39400001, 0x91400000,
    0x906BC794, 0x38210060, 0x8181FFF8, 0x7D8803A6, 0x4E800020};

constexpr std::array<uint32_t, 19> kPatternB{
    0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x3BE1FF90, 0x9421FF90,
    0x7C852378, 0x7C641B78, 0x387F0050, 0x4BFFF669, 0x387F0050,
    0x4BFFEA19, 0x387F0050, 0x4BFFDCD9, 0x38600000, 0x383F0070,
    0x8181FFF8, 0x7D8803A6, 0xEBE1FFF0, 0x4E800020};

void PutBe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = uint8_t(value >> (24 - i * 8));
  }
}

void PutBe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  PutBe32(bytes, offset, uint32_t(value >> 32));
  PutBe32(bytes, offset + 4, uint32_t(value));
}

struct Fixture {
  static constexpr uint32_t kSp = 0x10002000;
  static constexpr uint64_t kSavedR31 = 0xD5E6F70112345678;

  bool pattern_b = false;
  ExceptionMetadataFunction function;
  std::vector<uint8_t> code;
  std::vector<uint8_t> stack;
  VirtualUnwindRegisters registers;
  VirtualUnwindLimits limits;
  Provenance code_provenance = Provenance::kCoherentPrecise;
  Provenance stack_provenance = Provenance::kCoherentPrecise;

  explicit Fixture(bool use_pattern_b) : pattern_b(use_pattern_b) {
    function.begin = pattern_b ? 0x82313028 : 0x82311A68;
    function.raw_info = pattern_b ? 0xC0001305 : 0x40001103;
    function.packed_candidate = true;
    function.prologue_instructions = pattern_b ? 5 : 3;
    function.length_instructions = pattern_b ? 19 : 17;
    function.end =
        uint64_t(function.begin) + uint64_t(function.length_instructions) * 4;
    function.status = ExceptionMetadataStatus::kValid;
    const auto add_word = [&](uint32_t word) {
      const size_t offset = code.size();
      code.resize(offset + 4);
      PutBe32(code, offset, word);
    };
    if (pattern_b) {
      for (uint32_t word : kPatternB) {
        add_word(word);
      }
    } else {
      for (uint32_t word : kPatternA) {
        add_word(word);
      }
    }
    const uint32_t frame_bytes = pattern_b ? 112 : 96;
    stack.resize(frame_bytes);
    PutBe32(stack, 0, kSp + frame_bytes);
    PutBe32(stack, frame_bytes - 8, pattern_b ? 0x827FC5E0 : 0x82313054);
    if (pattern_b) {
      PutBe64(stack, frame_bytes - 16, kSavedR31);
    }
    registers.provenance = Provenance::kCoherentPrecise;
    registers.control_pc = pattern_b ? 0x82313050 : 0x82311A94;
    registers.gpr[1] = kSp;
    registers.full64_valid = uint32_t{1} << 1;
    if (pattern_b) {
      registers.gpr[31] = kSp;
      registers.low32_valid = uint32_t{1} << 31;
    }
  }

  RecipeBuildResult Build() const {
    return BuildBoundedUnwindRecipe(
        function, {function.begin, code, code_provenance}, limits);
  }

  VirtualUnwindResult Unwind(const BoundedUnwindRecipe& recipe) const {
    return TryVirtualUnwindFrame(recipe, registers,
                                 {kSp, stack, stack_provenance}, limits);
  }

  void SetCodeWord(size_t index, uint32_t word) {
    PutBe32(code, index * 4, word);
  }
};

void CheckFailure(const VirtualUnwindResult& result, Status status) {
  CHECK(result.status == status);
  CHECK(result.caller_sp_address32 == 0);
  CHECK(result.return_address32 == 0);
  CHECK(result.recovered_r31_full64 == 0);
  CHECK_FALSE(result.recovered_r31_valid);
}

TEST_CASE("Bounded PPC recipes recover only supported body frame values",
          "[cpu][virtual-unwind]") {
  for (bool pattern_b : {false, true}) {
    Fixture fixture(pattern_b);
    const auto build = fixture.Build();
    REQUIRE(build.status == Status::kSupportedPartialFrame);
    REQUIRE(build.recipe);
    CHECK(build.recipe->body_begin() ==
          fixture.function.begin + (pattern_b ? 20 : 12));
    CHECK(build.recipe->body_end() ==
          fixture.function.begin + (pattern_b ? 56 : 52));
    const auto frame = fixture.Unwind(*build.recipe);
    REQUIRE(frame.status == Status::kSupportedPartialFrame);
    CHECK(frame.caller_sp_address32 == Fixture::kSp + (pattern_b ? 112 : 96));
    CHECK(frame.return_address32 == (pattern_b ? 0x827FC5E0 : 0x82313054));
    CHECK(frame.recovered_r31_valid == pattern_b);
    CHECK(frame.recovered_r31_full64 == (pattern_b ? Fixture::kSavedR31 : 0));
  }
}

TEST_CASE("A-frame output does not manufacture B-frame register validity",
          "[cpu][virtual-unwind]") {
  Fixture first(false);
  Fixture second(true);
  const auto first_recipe = first.Build();
  const auto second_recipe = second.Build();
  REQUIRE(first_recipe.recipe);
  REQUIRE(second_recipe.recipe);
  const auto first_frame = first.Unwind(*first_recipe.recipe);
  REQUIRE(first_frame.status == Status::kSupportedPartialFrame);
  second.registers.low32_valid = 0;
  CheckFailure(second.Unwind(*second_recipe.recipe), Status::kMissingRegister);
}

TEST_CASE("Saved return address is kept exact at a function boundary",
          "[cpu][virtual-unwind]") {
  Fixture fixture(false);
  PutBe32(fixture.stack, 88, 0x82313028);
  const auto build = fixture.Build();
  REQUIRE(build.recipe);
  const auto frame = fixture.Unwind(*build.recipe);
  REQUIRE(frame.status == Status::kSupportedPartialFrame);
  CHECK(frame.return_address32 == 0x82313028);
}

TEST_CASE("Recipe retains bounded code evidence after input changes",
          "[cpu][virtual-unwind]") {
  Fixture fixture(false);
  const auto build = fixture.Build();
  REQUIRE(build.recipe);
  fixture.code.clear();
  const auto frame = fixture.Unwind(*build.recipe);
  REQUIRE(frame.status == Status::kSupportedPartialFrame);
  CHECK(frame.return_address32 == 0x82313054);
}

TEST_CASE("A full-width r31 value also supplies its low 32 bits",
          "[cpu][virtual-unwind]") {
  Fixture fixture(true);
  const auto build = fixture.Build();
  REQUIRE(build.recipe);
  fixture.registers.low32_valid = 0;
  fixture.registers.full64_valid |= uint32_t{1} << 31;
  CHECK(fixture.Unwind(*build.recipe).status == Status::kSupportedPartialFrame);
}

TEST_CASE("Every prologue and epilogue instruction declines",
          "[cpu][virtual-unwind]") {
  for (bool pattern_b : {false, true}) {
    Fixture fixture(pattern_b);
    const auto build = fixture.Build();
    REQUIRE(build.recipe);
    const uint32_t begin = fixture.function.begin;
    const uint32_t count = fixture.function.length_instructions;
    const uint32_t prologue = fixture.function.prologue_instructions;
    const uint32_t epilogue = count - (pattern_b ? 5 : 4);
    for (uint32_t i = 0; i < count; ++i) {
      if (i >= prologue && i < epilogue) {
        continue;
      }
      fixture.registers.control_pc = begin + i * 4;
      CheckFailure(fixture.Unwind(*build.recipe), Status::kUnsupportedPhase);
    }
    fixture.registers.control_pc = begin + 1;
    CheckFailure(fixture.Unwind(*build.recipe), Status::kBounds);
    fixture.registers.control_pc = uint32_t(fixture.function.end);
    CheckFailure(fixture.Unwind(*build.recipe), Status::kBounds);
  }
}

TEST_CASE("Register, provenance, stack and resource failures are transactional",
          "[cpu][virtual-unwind]") {
  Fixture fixture(true);
  const auto build = fixture.Build();
  REQUIRE(build.recipe);
  fixture.registers.provenance = Provenance::kUnknown;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kUntrustedCapture);
  fixture.registers.provenance = Provenance::kCoherentPrecise;
  fixture.stack_provenance = Provenance::kUnknown;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kUntrustedCapture);
  fixture.stack_provenance = Provenance::kCoherentPrecise;
  fixture.registers.full64_valid = 0;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kMissingRegister);
  fixture.registers.full64_valid = uint32_t{1} << 1;
  fixture.registers.low32_valid = 0;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kMissingRegister);
  fixture.registers.low32_valid = uint32_t{1} << 31;
  fixture.registers.gpr[31] = Fixture::kSp + 4;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kInconsistentFrame);
  fixture.registers.gpr[31] = Fixture::kSp;
  PutBe32(fixture.stack, 0, Fixture::kSp + 96);
  CheckFailure(fixture.Unwind(*build.recipe), Status::kInconsistentFrame);
  PutBe32(fixture.stack, 0, Fixture::kSp + 112);
  fixture.stack.resize(104);
  CheckFailure(fixture.Unwind(*build.recipe), Status::kBounds);
  fixture.stack.resize(112);
  fixture.limits.max_stack_read_bytes = 15;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kResourceLimit);
  fixture.limits.max_stack_read_bytes = 32;
  fixture.limits.max_stack_reads = 2;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kResourceLimit);
  fixture.limits.max_stack_reads = 4;
  fixture.limits.max_code_instructions = 18;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kResourceLimit);
}

TEST_CASE("Wrapped and misaligned frames decline before stack access",
          "[cpu][virtual-unwind]") {
  Fixture fixture(false);
  const auto build = fixture.Build();
  REQUIRE(build.recipe);
  fixture.registers.gpr[1] = 0xFFFFFFD0;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kBounds);
  fixture.registers.gpr[1] = Fixture::kSp + 2;
  CheckFailure(fixture.Unwind(*build.recipe), Status::kBounds);
  fixture.registers.gpr[1] = Fixture::kSp;
  fixture.stack.resize(91);
  CheckFailure(fixture.Unwind(*build.recipe), Status::kBounds);
}

TEST_CASE("Recipe admission requires exact complete supported code",
          "[cpu][virtual-unwind]") {
  Fixture fixture(false);
  fixture.function.status = ExceptionMetadataStatus::kMissing;
  CHECK_FALSE(fixture.Build().recipe);
  fixture.function.status = ExceptionMetadataStatus::kValid;
  fixture.function.prologue_instructions = 2;
  CHECK_FALSE(fixture.Build().recipe);
  fixture.function.prologue_instructions = 3;
  fixture.function.end -= 4;
  CHECK_FALSE(fixture.Build().recipe);
  fixture.function.end += 4;
  fixture.code.pop_back();
  CHECK_FALSE(fixture.Build().recipe);
  fixture.code.push_back(0x20);
  fixture.limits.max_code_instructions = 16;
  CHECK(fixture.Build().status == Status::kResourceLimit);
  fixture.limits.max_code_instructions = 256;
  fixture.code_provenance = Provenance::kUnknown;
  CHECK(fixture.Build().status == Status::kUntrustedCapture);
}

TEST_CASE("Unexpected body effects and control flow reject the full recipe",
          "[cpu][virtual-unwind]") {
  for (uint32_t word : {0x38210010u,     // addi r1,r1,16.
                        0x3BE10010u,     // addi r31,r1,16.
                        0x48000008u,     // Unlinked branch.
                        0x41820008u,     // Conditional branch.
                        0x48000009u,     // Linked branch inside this function.
                        0x4E800420u,     // Indirect branch.
                        0x9421FFF0u,     // Dynamic SP update.
                        0x7C0007ACu}) {  // Unrecognized instruction.
    Fixture fixture(false);
    fixture.SetCodeWord(4, word);
    CHECK_FALSE(fixture.Build().recipe);
  }
  Fixture helper(false);
  helper.SetCodeWord(1, 0x4BFE1EF5);
  CHECK_FALSE(helper.Build().recipe);
  Fixture unknown_store(false);
  unknown_store.SetCodeWord(7, 0x9163C784);  // stw through unknown r3.
  CHECK_FALSE(unknown_store.Build().recipe);
}

TEST_CASE("Constant store knowledge ends at calls and GPR writes",
          "[cpu][virtual-unwind]") {
  Fixture after_call(false);
  after_call.SetCodeWord(12, 0x9069C794);  // r9 was known before the call.
  CHECK_FALSE(after_call.Build().recipe);
  Fixture after_load(false);
  after_load.SetCodeWord(4, 0x81400000);  // lwz r10,0(r0) clobbers known r10.
  CHECK_FALSE(after_load.Build().recipe);
}

TEST_CASE("Pattern B save slots cannot overlap the backchain",
          "[cpu][virtual-unwind]") {
  Fixture overlapping(true);
  overlapping.SetCodeWord(3, 0x3BE1FFF0);
  overlapping.SetCodeWord(4, 0x9421FFF0);
  overlapping.SetCodeWord(14, 0x383F0010);
  CHECK_FALSE(overlapping.Build().recipe);

  Fixture minimum(true);
  minimum.SetCodeWord(3, 0x3BE1FFE0);
  minimum.SetCodeWord(4, 0x9421FFE0);
  minimum.SetCodeWord(14, 0x383F0020);
  minimum.stack.resize(32);
  PutBe32(minimum.stack, 0, Fixture::kSp + 32);
  PutBe64(minimum.stack, 16, Fixture::kSavedR31);
  PutBe32(minimum.stack, 24, 0x827FC5E0);
  const auto build = minimum.Build();
  REQUIRE(build.recipe);
  const auto frame = minimum.Unwind(*build.recipe);
  REQUIRE(frame.status == Status::kSupportedPartialFrame);
  CHECK(frame.caller_sp_address32 == Fixture::kSp + 32);
  CHECK(frame.recovered_r31_full64 == Fixture::kSavedR31);
}

TEST_CASE("A constant body store overlapping the frame declines use",
          "[cpu][virtual-unwind]") {
  Fixture fixture(false);
  // lis r10,0x1000; stw r11,0x2058(r10) aliases the saved LR.
  fixture.SetCodeWord(3, 0x3D401000);
  fixture.SetCodeWord(7, 0x916A2058);
  const auto build = fixture.Build();
  REQUIRE(build.recipe);
  CheckFailure(fixture.Unwind(*build.recipe), Status::kInconsistentFrame);
}

}  // namespace
}  // namespace xe::cpu::ppc::test
