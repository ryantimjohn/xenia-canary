/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/testing/ppc_checkpoint_x64_fixture.h"

// No executable fixture tests are registered on other host platforms. This is
// host-only normal-access coverage, not a cross-platform backend claim.
#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

using namespace x64_fixture;
constexpr uint32_t kFixturePC = 0x80001000;

static_assert(!std::is_copy_constructible_v<LeafFixture>);
static_assert(!std::is_move_constructible_v<LeafFixture>);
static_assert(!std::is_copy_constructible_v<SyntheticMemory>);

uint32_t Word(uint32_t opcode, uint32_t rt_rs, uint32_t ra, int16_t disp) {
  return (opcode << 26) | (rt_rs << 21) | (ra << 16) |
         static_cast<uint16_t>(disp);
}

void Seed(PPCContext& input) {
  constexpr std::array<uint64_t, 8> raw_patterns = {
      0x0000000000000000ULL, 0x8000000000000000ULL, 0x7FF8000000000123ULL,
      0x7FF0000000000456ULL, 0xFFF8000000000789ULL, 0xFFF0000000000000ULL,
      0x0000000000000001ULL, 0x0123456789ABCDEFULL};
  for (size_t n = 0; n < 32; ++n) {
    input.r[n] = 0xFEDCBA9810203040ULL + n * 0x112233;
    const uint64_t bits = raw_patterns[n % raw_patterns.size()];
    std::memcpy(&input.f[n], &bits, sizeof(bits));
  }
  for (size_t n = 0; n < 128; ++n) {
    std::array<uint8_t, 16> bytes{};
    for (size_t b = 0; b < 16; ++b) {
      bytes[b] = static_cast<uint8_t>(n * 37 + b * 11);
    }
    std::memcpy(&input.v[n], bytes.data(), bytes.size());
  }
  std::array<uint8_t, 16> vscr{};
  for (size_t n = 0; n < 16; ++n) {
    vscr[n] = static_cast<uint8_t>(0xE7 - n * 7);
  }
  std::memcpy(&input.vscr_vec, vscr.data(), vscr.size());
  auto bytes = reinterpret_cast<uint8_t*>(&input) + offsetof(PPCContext, cr0);
  for (size_t n = 0; n < 32; ++n) {
    bytes[n] = static_cast<uint8_t>(0xA0 | ((n * 7 + n / 4) & 1));
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
  // These are excluded from the canonical snapshot and must remain untouched.
  input.scratch = 0x8765432112345678ULL;
  input.reserved_val = 0xCAFEF00D12345678ULL;
  input.thread_id = 0x12345678;
}

uint32_t IndependentCR(const PPCContext& input) {
  const auto bytes =
      reinterpret_cast<const uint8_t*>(&input) + offsetof(PPCContext, cr0);
  uint32_t cr = 0;
  for (size_t n = 0; n < 32; ++n) {
    cr |= uint32_t{bytes[n] & 1u} << (31 - n);
  }
  return cr;
}

void CheckState(const FaultModeledState& actual,
                const FaultModeledState& expected) {
  CHECK(actual.gpr == expected.gpr);
  CHECK(actual.raw_fpr == expected.raw_fpr);
  for (size_t n = 0; n < 128; ++n) {
    CHECK(actual.vmx128[n] == expected.vmx128[n]);
  }
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
  CHECK(actual.modeled_valid.gpr == UINT32_MAX);
  CHECK(actual.modeled_valid.raw_fpr == UINT32_MAX);
  CHECK(actual.modeled_valid.vmx128 == expected.modeled_valid.vmx128);
  CHECK(actual.modeled_valid.scalar == kAllModeledScalars);
  CHECK(actual.architectural_valid.gpr == 0);
  CHECK(actual.architectural_valid.raw_fpr == 0);
  for (auto bits : actual.architectural_valid.vmx128) {
    CHECK(bits == 0);
  }
  CHECK(actual.architectural_valid.scalar == 0);
  CHECK(actual.architectural_valid.xer_bits == 0);
  CHECK(actual.architectural_valid.fpscr_bits == 0);
}

void CheckInvalid(const Record& output) {
  CHECK(output.phase == Phase(CheckpointActivationState::kInvalid));
  CHECK(output.state.modeled_valid.gpr == 0);
  CHECK(output.state.modeled_valid.raw_fpr == 0);
  for (auto bits : output.state.modeled_valid.vmx128) {
    CHECK(bits == 0);
  }
  CHECK(output.state.modeled_valid.scalar == 0);
  CHECK(output.state.architectural_valid.gpr == 0);
  CHECK(output.state.architectural_valid.raw_fpr == 0);
  for (auto bits : output.state.architectural_valid.vmx128) {
    CHECK(bits == 0);
  }
  CHECK(output.state.architectural_valid.scalar == 0);
  CHECK(output.state.architectural_valid.xer_bits == 0);
  CHECK(output.state.architectural_valid.fpscr_bits == 0);
}

std::unique_ptr<LeafFixture> Create(uint32_t word) {
  BuildResult result;
  auto fixture = LeafFixture::Create(kFixturePC, word, result);
  REQUIRE(result == BuildResult::kSuccess);
  REQUIRE(fixture);
  REQUIRE(fixture->function());
  return fixture;
}

TEST_CASE(
    "Unpublished Windows x64 fixture captures all state and scalar aliases",
    "[cpu][checkpoint-x64-fixture]") {
  struct Case {
    uint32_t opcode, rt_rs, ra;
    int16_t displacement;
    uint64_t old_base;
    uint32_t guest_begin, span, expected_ea;
  };
  const std::array<Case, 9> cases = {{
      {36, 10, 0, 0, 0, 0, 8, 0},
      {36, 0, 0, -4, 0, 0xFFFFFFFC, 4, 0xFFFFFFFC},
      {36, 7, 7, 4, 0xAAAAAAAA0000001CULL, 16, 64, 32},
      {32, 4, 4, -4, 0xABCDEF1200000020ULL, 16, 64, 28},
      {32, 0, 0, 0, 0, 0, 8, 0},
      {32, 8, 9, 8, 0x11111111FFFFFFFCULL, 0, 16, 4},
      {36, 12, 11, -32768, 0xAABBCCDD00008000ULL, 0, 4, 0},
      {32, 14, 15, 32767, 0xAABBCCDD00000001ULL, 0x8000, 4, 0x8000},
      {32, 3, 0, -32768, 0, 0xFFFF8000, 4, 0xFFFF8000},
  }};
  uint64_t sequence = 1;
  for (const auto& c : cases) {
    CAPTURE(c.opcode, c.rt_rs, c.ra, c.displacement);
    PPCContext input{};
    Seed(input);
    if (c.ra) {
      input.r[c.ra] = c.old_base;
    }
    FaultModeledState expected{};
    REQUIRE(CopyModeledCheckpointStateReference(&input, &expected));
    const uint32_t cr = IndependentCR(input);
    const uint32_t source = static_cast<uint32_t>(input.r[c.rt_rs]);
    SyntheticMemory memory(c.guest_begin, c.span);
    REQUIRE(memory.valid());
    std::fill(memory.data(), memory.data() + memory.size(), uint8_t{0x7B});
    const size_t access_offset = uint64_t{c.expected_ea} - c.guest_begin;
    constexpr std::array<uint8_t, 4> load_bytes{0xFE, 0xDC, 0xBA, 0x98};
    if (c.opcode == 32) {
      std::copy(load_bytes.begin(), load_bytes.end(),
                memory.data() + access_offset);
    }
    const std::vector<uint8_t> before(memory.data(),
                                      memory.data() + memory.size());
    auto fixture = Create(Word(c.opcode, c.rt_rs, c.ra, c.displacement));
    REQUIRE(VerifyBytes(fixture->code(), fixture->size(),
                        Word(c.opcode, c.rt_rs, c.ra, c.displacement),
                        fixture->offsets()));
    Record output{};
    output.phase = Phase(CheckpointActivationState::kArmed);
    output.state.architectural_valid.scalar = UINT32_MAX;
    REQUIRE(fixture->Run(input, output, memory, sequence));
    CheckState(output.state, expected);
    CHECK(output.state.cr == cr);
    CHECK(output.phase == Phase(CheckpointActivationState::kDisarmed));
    CHECK(output.sequence == sequence++);
    CHECK(output.guest_ea == c.expected_ea);
    CHECK(input.scratch == 0x8765432112345678ULL);
    CHECK(input.reserved_val == 0xCAFEF00D12345678ULL);
    CHECK(input.thread_id == 0x12345678);
    CHECK(memory.SentinelsIntact());
    FaultModeledState after{};
    REQUIRE(CopyModeledCheckpointStateReference(&input, &after));
    if (c.opcode == 32) {
      CHECK(input.r[c.rt_rs] == 0x00000000FEDCBA98ULL);
      CHECK(output.state.gpr[c.rt_rs] == expected.gpr[c.rt_rs]);
      expected.gpr[c.rt_rs] = 0x00000000FEDCBA98ULL;
      CHECK(std::equal(before.begin(), before.end(), memory.data()));
    } else {
      const std::array<uint8_t, 4> store_bytes{
          static_cast<uint8_t>(source >> 24),
          static_cast<uint8_t>(source >> 16), static_cast<uint8_t>(source >> 8),
          static_cast<uint8_t>(source)};
      for (size_t n = 0; n < memory.size(); ++n) {
        CHECK(memory.data()[n] == (n >= access_offset && n < access_offset + 4
                                       ? store_bytes[n - access_offset]
                                       : before[n]));
      }
    }
    CheckState(after, expected);
  }
}

TEST_CASE("Unpublished Windows x64 fixture normalizes every flag byte form",
          "[cpu][checkpoint-x64-fixture]") {
  auto fixture = Create(Word(36, 10, 0, 0));
  SyntheticMemory memory(0, 4);
  REQUIRE(memory.valid());
  for (uint8_t flag : std::array<uint8_t, 5>{0, 1, 2, 0x80, 0xFF}) {
    PPCContext input{};
    Seed(input);
    input.xer_ca = input.xer_ov = input.xer_so = input.vscr_sat = flag;
    Record output{};
    REQUIRE(fixture->Run(input, output, memory, uint64_t{flag} + 1));
    CHECK(output.state.xer_ca == (flag ? 1 : 0));
    CHECK(output.state.xer_ov == (flag ? 1 : 0));
    CHECK(output.state.xer_so == (flag ? 1 : 0));
    CHECK(output.state.vscr_sat == (flag ? 1 : 0));
  }
}

TEST_CASE(
    "Unpublished Windows x64 fixture native preflight clears stale success",
    "[cpu][checkpoint-x64-fixture]") {
  for (uint32_t opcode : std::array<uint32_t, 2>{32, 36}) {
    auto fixture = Create(Word(opcode, 3, 4, 0));
    SyntheticMemory memory(16, 16);
    REQUIRE(memory.valid());
    PPCContext input{};
    Seed(input);
    input.r[4] = 16;
    Record output{};
    REQUIRE(fixture->Run(input, output, memory, 1));
    const std::vector<uint8_t> before(memory.data(),
                                      memory.data() + memory.size());
    for (uint64_t bad_ea : std::array<uint64_t, 4>{12, 18, 32, 0xFFFFFFFC}) {
      input.r[4] = bad_ea;
      FaultModeledState expected{};
      REQUIRE(CopyModeledCheckpointStateReference(&input, &expected));
      output.phase = Phase(CheckpointActivationState::kArmed);
      output.state.modeled_valid.gpr = UINT32_MAX;
      output.state.architectural_valid.scalar = UINT32_MAX;
      // Deliberately bypass the C++ preflight to exercise the native decline
      // path with valid, exclusively owned objects and a valid synthetic map.
      CHECK(fixture->function()(&input, &output, &memory.mapping()) == 0);
      CheckInvalid(output);
      FaultModeledState after{};
      REQUIRE(CopyModeledCheckpointStateReference(&input, &after));
      CheckState(after, expected);
      CHECK(std::equal(before.begin(), before.end(), memory.data()));
      CHECK(memory.SentinelsIntact());
    }
    output.phase = Phase(CheckpointActivationState::kArmed);
    Mapping null_host = memory.mapping();
    null_host.host_begin = nullptr;
    input.r[4] = 16;
    CHECK(fixture->function()(&input, &output, &null_host) == 0);
    CheckInvalid(output);
    CHECK(std::equal(before.begin(), before.end(), memory.data()));
  }
}

TEST_CASE(
    "Unpublished Windows x64 fixture host preflight rejects missing ownership",
    "[cpu][checkpoint-x64-fixture]") {
  auto fixture = Create(Word(32, 3, 0, 0));
  SyntheticMemory invalid(0, 0);
  SyntheticMemory wraps(0xFFFFFFFC, 8);
  SyntheticMemory valid(0, 4);
  REQUIRE(valid.valid());
  CHECK_FALSE(invalid.valid());
  CHECK_FALSE(wraps.valid());
  PPCContext input{};
  Seed(input);
  Record output{};
  output.phase = Phase(CheckpointActivationState::kArmed);
  CHECK_FALSE(fixture->Run(input, output, invalid, 1));
  CheckInvalid(output);
  output.phase = Phase(CheckpointActivationState::kArmed);
  CHECK_FALSE(fixture->Run(input, output, valid, 0));
  CheckInvalid(output);
}

TEST_CASE("Unpublished Windows x64 fixture releases without guest publication",
          "[cpu][checkpoint-x64-fixture]") {
  BuildResult result;
  CHECK_FALSE(LeafFixture::Create(kFixturePC + 1, Word(32, 3, 0, 0), result));
  CHECK(result == BuildResult::kUnsupportedInstruction);
  CHECK_FALSE(LeafFixture::Create(kFixturePC, Word(33, 3, 4, 0), result));
  CHECK(result == BuildResult::kUnsupportedInstruction);
  CHECK_FALSE(LeafFixture::Create(kFixturePC, Word(37, 3, 4, 0), result));
  CHECK(result == BuildResult::kUnsupportedInstruction);
  const uint8_t* released = nullptr;
  {
    auto fixture = Create(Word(36, 10, 0, 0));
    released = fixture->code();
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(released, &info, sizeof(info)) == sizeof(info));
    CHECK(info.State == MEM_COMMIT);
    CHECK((info.Protect & 0xFF) == PAGE_EXECUTE_READ);
    // A leaf has no registered function entry: it changes neither RSP nor
    // nonvolatile registers, as checked from the finished instruction bytes.
    DWORD64 image_base = 0;
    CHECK(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(released),
                                 &image_base, nullptr) == nullptr);
  }
  // No intervening allocation or code access occurs before this query.
  MEMORY_BASIC_INFORMATION info{};
  REQUIRE(VirtualQuery(released, &info, sizeof(info)) == sizeof(info));
  CHECK(info.State == MEM_FREE);
}

TEST_CASE(
    "Unpublished Windows x64 fixture verifier rejects changed native bytes",
    "[cpu][checkpoint-x64-fixture]") {
  const uint32_t word = Word(32, 3, 4, 0);
  auto fixture = Create(word);
  const auto offsets = fixture->offsets();
  std::vector<uint8_t> bytes(fixture->code(),
                             fixture->code() + fixture->size());
  size_t length = 0;
  REQUIRE(VerifyBytes(bytes.data(), bytes.size(), word, offsets, &length));
  CHECK(length == fixture->access_length());
  REQUIRE(length >= 2);

  SECTION("A wider target access is rejected") {
    // Baseline MOV eax,[r11] begins with REX; adding W changes it to 64 bits.
    REQUIRE((bytes[offsets.access] & 0xF0) == 0x40);
    bytes[offsets.access] |= 0x08;
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, offsets));
  }
  SECTION("A helper call at the access is rejected") {
    bytes[offsets.access] = 0xE8;
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, offsets));
  }
  SECTION("A nonvolatile result register is rejected") {
    REQUIRE(bytes[offsets.load_swap] == 0x0F);
    REQUIRE(bytes[offsets.load_swap + 1] == 0xC8);
    bytes[offsets.load_swap + 1] = 0xCF;  // bswap edi
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, offsets));
  }
  SECTION("Explicit stack use is rejected") {
    bytes[offsets.access] = 0x50;  // push rax
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, offsets));
  }
  SECTION("Disarm must immediately follow the access") {
    auto separated = offsets;
    ++separated.disarm;
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, separated));
  }
  SECTION("Arm must immediately precede the access") {
    auto separated = offsets;
    --separated.arm;
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, separated));
  }
  SECTION("Permitted instructions after the final return are rejected") {
    bytes.push_back(0x31);  // xor eax,eax is otherwise on the whitelist
    bytes.push_back(0xC0);
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size(), word, offsets));
  }
  SECTION("Truncated code is rejected") {
    CHECK_FALSE(VerifyBytes(bytes.data(), bytes.size() - 1, word, offsets));
  }
  SECTION("Mismatched opcode and writeback semantics are rejected") {
    CHECK_FALSE(
        VerifyBytes(bytes.data(), bytes.size(), Word(36, 3, 4, 0), offsets));
  }
}

}  // namespace
}  // namespace xe::cpu::ppc::test

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
