/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_ARCH_AMD64

#include <array>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/x64/x64_delay_source_map.h"

namespace xe::cpu::testing {
namespace {

using backend::x64::delay_source_map::DescribeMatch;
using backend::x64::delay_source_map::FindPauseRun;
using backend::x64::delay_source_map::Mapping;
using backend::x64::delay_source_map::Admission;
using backend::x64::delay_source_map::Budget;

TEST_CASE("PAUSE byte selector requires eight complete F3 90 pairs",
          "[delay_source_map]") {
  std::array<uint8_t, 18> code{};
  code[0] = 0xF3;
  code[1] = 0x90;
  for (size_t i = 2; i < code.size(); i += 2) {
    code[i] = 0xF3;
    code[i + 1] = 0x90;
  }
  size_t offset = 100;
  REQUIRE(FindPauseRun(code.data(), code.size(), 0, &offset));
  REQUIRE(offset == 0);
  REQUIRE(FindPauseRun(code.data(), code.size(), 2, &offset));
  REQUIRE(offset == 2);
  REQUIRE_FALSE(FindPauseRun(code.data(), 15, 0, &offset));
  REQUIRE_FALSE(FindPauseRun(code.data(), 16, 1, &offset));
  code[9] = 0x91;
  REQUIRE_FALSE(FindPauseRun(code.data(), code.size(), 0, &offset));
}

TEST_CASE("PAUSE source association distinguishes first-entry fallback",
          "[delay_source_map]") {
  std::vector<SourceMapEntry> map = {
      {0x82000000, 0x01000200, 20},
      {0x82000004, 0x01000300, 40},
      {0x82000008, 0x01000400, 40},
      {0x8200000C, 0x01000500, 60},
  };
  auto before = DescribeMatch(16, 80, map);
  REQUIRE(before.mapping == Mapping::kFallback);
  REQUIRE(before.next_marker_offset == 20);
  REQUIRE(before.guest_address == 0);

  auto within = DescribeMatch(42, 80, map);
  REQUIRE(within.mapping == Mapping::kPreceding);
  REQUIRE(within.guest_address == 0x82000008);
  REQUIRE(within.marker_offset == 40);
  REQUIRE(within.next_marker_offset == 60);

  auto after = DescribeMatch(75, 80, map);
  REQUIRE(after.mapping == Mapping::kPreceding);
  REQUIRE(after.guest_address == 0x8200000C);
  REQUIRE(after.next_marker_offset == 80);
}

TEST_CASE("Invalid source map cannot qualify a host byte match",
          "[delay_source_map]") {
  std::vector<SourceMapEntry> map = {{0x82000000, 0, 60},
                                     {0x82000004, 0, 20}};
  REQUIRE(DescribeMatch(64, 80, map).mapping == Mapping::kUnmapped);
  map[1].code_offset = 80;
  REQUIRE(DescribeMatch(64, 80, map).mapping == Mapping::kUnmapped);
  REQUIRE(DescribeMatch(80, 80, {}).mapping == Mapping::kUnmapped);
  std::vector<SourceMapEntry> excessive(65537, {0x82000000, 0, 0});
  REQUIRE(DescribeMatch(0, 80, excessive).mapping == Mapping::kTruncated);
}

TEST_CASE("Compiler diagnostic budget rejects over-cap work and records",
          "[delay_source_map]") {
  Budget budget;
  REQUIRE(budget.Admit(16 * 1024 * 1024) == Admission::kAccepted);
  REQUIRE(budget.Admit(1) == Admission::kByteCap);
  REQUIRE(budget.functions == 1);
  REQUIRE(budget.scanned_bytes == 16 * 1024 * 1024);
  budget.records = 15;
  REQUIRE(budget.Record());
  REQUIRE_FALSE(budget.Record());
  REQUIRE(budget.Admit(0) == Admission::kRecordCap);

  Budget functions;
  functions.functions = 4095;
  REQUIRE(functions.Admit(0) == Admission::kAccepted);
  REQUIRE(functions.Admit(0) == Admission::kFunctionCap);
  REQUIRE(functions.functions == 4096);
}

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_ARCH_AMD64
