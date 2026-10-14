/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_checkpoint_materialization.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

void CheckNoArchitecturalClaims(const FaultArchitecturalValidity& validity) {
  CHECK(validity.gpr == 0);
  CHECK(validity.raw_fpr == 0);
  for (auto bits : validity.vmx128) {
    CHECK(bits == 0);
  }
  CHECK(validity.scalar == 0);
  CHECK(validity.xer_bits == 0);
  CHECK(validity.fpscr_bits == 0);
}

void CheckInvalidModeledState(const FaultModeledValidity& validity) {
  CHECK(validity.gpr == 0);
  CHECK(validity.raw_fpr == 0);
  for (auto bits : validity.vmx128) {
    CHECK(bits == 0);
  }
  CHECK(validity.scalar == 0);
}

TEST_CASE("Reference checkpoint captures every modeled slot and raw bit",
          "[cpu][checkpoint-materialization]") {
  PPCContext input{};
  FaultModeledState output{};
  // Include both quiet/signaling NaNs and signed zero. The producer must never
  // load these as numeric floating-point operands or quiet a signaling NaN.
  constexpr std::array<uint64_t, 8> raw_patterns = {
      0x0000000000000000ULL, 0x8000000000000000ULL, 0x7FF8000000000123ULL,
      0x7FF0000000000456ULL, 0xFFF8000000000789ULL, 0xFFF0000000000000ULL,
      0x0000000000000001ULL, 0x0123456789ABCDEFULL};
  for (size_t n = 0; n < 32; ++n) {
    input.r[n] = 0xFEDCBA9800000000ULL | (n * 0x10203);
    const uint64_t bits = raw_patterns[n % raw_patterns.size()];
    std::memcpy(&input.f[n], &bits, sizeof(bits));
  }
  for (size_t n = 0; n < 128; ++n) {
    std::array<uint8_t, 16> bytes{};
    for (size_t b = 0; b < bytes.size(); ++b) {
      bytes[b] = static_cast<uint8_t>(n * 37 + b * 11);
    }
    std::memcpy(&input.v[n], bytes.data(), bytes.size());
  }
  std::array<uint8_t, 16> vscr_bytes{};
  for (size_t b = 0; b < vscr_bytes.size(); ++b) {
    vscr_bytes[b] = static_cast<uint8_t>(0xE7 - b * 7);
  }
  std::memcpy(&input.vscr_vec, vscr_bytes.data(), vscr_bytes.size());
  input.lr = 0xDEADBEEF12345678ULL;
  input.ctr = 0xABCDEF0123456789ULL;
  input.fpscr.value = 0xA59C7E13;
  input.vrsave = 0x81234567;
  input.msr = 0xFEDCBA9876549030ULL;
  input.xer_ca = 0x80;
  input.xer_ov = 0;
  input.xer_so = 0xFE;
  input.vscr_sat = 0xFF;

  // Exercise low-bit CR normalization independently of PPCContext::cr().
  auto cr_bytes =
      reinterpret_cast<uint8_t*>(&input) + offsetof(PPCContext, cr0);
  uint32_t expected_cr = 0;
  for (size_t n = 0; n < 32; ++n) {
    cr_bytes[n] = static_cast<uint8_t>(0xA0 | ((n * 7 + n / 4) & 1));
    expected_cr |= uint32_t{cr_bytes[n] & 1u} << (31 - n);
  }
  output.architectural_valid.gpr = UINT32_MAX;
  output.architectural_valid.raw_fpr = UINT32_MAX;
  output.architectural_valid.vmx128.fill(UINT32_MAX);
  output.architectural_valid.scalar = UINT32_MAX;
  output.architectural_valid.xer_bits = UINT32_MAX;
  output.architectural_valid.fpscr_bits = UINT32_MAX;

  REQUIRE(CopyModeledCheckpointStateReference(&input, &output));
  for (size_t n = 0; n < 32; ++n) {
    CHECK(output.gpr[n] == input.r[n]);
    CHECK(output.raw_fpr[n] == raw_patterns[n % raw_patterns.size()]);
  }
  for (size_t n = 0; n < 128; ++n) {
    for (size_t b = 0; b < 16; ++b) {
      CHECK(output.vmx128[n][b] == static_cast<uint8_t>(n * 37 + b * 11));
    }
  }
  CHECK(output.lr == input.lr);
  CHECK(output.ctr == input.ctr);
  CHECK(output.cr == expected_cr);
  CHECK(output.xer_ca == 1);
  CHECK(output.xer_ov == 0);
  CHECK(output.xer_so == 1);
  CHECK(output.fpscr == input.fpscr.value);
  CHECK(output.vscr_vector == vscr_bytes);
  CHECK(output.vscr_sat == 1);
  CHECK(output.vrsave == input.vrsave);
  CHECK(output.msr == input.msr);
  CHECK(output.modeled_valid.gpr == UINT32_MAX);
  CHECK(output.modeled_valid.raw_fpr == UINT32_MAX);
  for (auto bits : output.modeled_valid.vmx128) {
    CHECK(bits == UINT32_MAX);
  }
  CHECK(output.modeled_valid.scalar == kAllModeledScalars);
  CheckNoArchitecturalClaims(output.architectural_valid);

  input.r[0] = 0;
  input.fpscr.value = 0;
  input.v[127] = {};
  CHECK(output.gpr[0] == 0xFEDCBA9800000000ULL);
  CHECK(output.fpscr == 0xA59C7E13);
  CHECK(output.vmx128[127][15] == static_cast<uint8_t>(127 * 37 + 15 * 11));
}

TEST_CASE("Reference checkpoint Boolean flags normalize all byte forms",
          "[cpu][checkpoint-materialization]") {
  for (uint8_t value : std::array<uint8_t, 5>{0, 1, 2, 0x80, 0xFF}) {
    PPCContext input{};
    FaultModeledState output{};
    input.xer_ca = value;
    input.xer_ov = value;
    input.xer_so = value;
    input.vscr_sat = value;
    REQUIRE(CopyModeledCheckpointStateReference(&input, &output));
    const uint8_t expected = value ? 1 : 0;
    CHECK(output.xer_ca == expected);
    CHECK(output.xer_ov == expected);
    CHECK(output.xer_so == expected);
    CHECK(output.vscr_sat == expected);
  }
}

TEST_CASE("Reference checkpoint rejects missing input without stale validity",
          "[cpu][checkpoint-materialization]") {
  PPCContext input{};
  FaultModeledState output{};
  REQUIRE(CopyModeledCheckpointStateReference(&input, &output));
  output.architectural_valid.scalar = UINT32_MAX;
  CHECK_FALSE(CopyModeledCheckpointStateReference(nullptr, &output));
  CheckInvalidModeledState(output.modeled_valid);
  CheckNoArchitecturalClaims(output.architectural_valid);
  CHECK_FALSE(CopyModeledCheckpointStateReference(&input, nullptr));
  CHECK_FALSE(CopyModeledCheckpointStateReference(nullptr, nullptr));
}

}  // namespace
}  // namespace xe::cpu::ppc::test
