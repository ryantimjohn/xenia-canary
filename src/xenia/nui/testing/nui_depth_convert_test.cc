/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_depth_convert.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

#include "xenia/nui/depth_frame.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

TEST_CASE("ShiftToMmApprox matches the known PrimeSense points",
          "[nui][depth_convert]") {
  // The reference points are only accurate to the nearest millimetre.
  CHECK(std::abs(static_cast<int>(ShiftToMmApprox(600)) - 672) <= 1);
  CHECK(std::abs(static_cast<int>(ShiftToMmApprox(800)) - 1144) <= 1);
  CHECK(std::abs(static_cast<int>(ShiftToMmApprox(1000)) - 3848) <= 1);
}

TEST_CASE("ShiftToMmApprox handles its edge cases", "[nui][depth_convert]") {
  CHECK(ShiftToMmApprox(kShiftNoReading) == 0);
  // At shift 1084 the PrimeSense denominator is no longer positive, and the
  // point is far beyond kMmMax regardless.
  CHECK(ShiftToMmApprox(1084) == 0);
}

TEST_CASE("MmToShiftApprox handles its edge cases", "[nui][depth_convert]") {
  CHECK(MmToShiftApprox(0) == kShiftNoReading);
  CHECK(MmToShiftApprox(static_cast<uint16_t>(kMmMax + 1)) == kShiftNoReading);
}

TEST_CASE("ShiftToMmApprox is non-decreasing over the usable shift range",
          "[nui][depth_convert]") {
  uint16_t previous_mm = ShiftToMmApprox(200);
  for (int shift = 201; shift <= 1050; ++shift) {
    const uint16_t mm = ShiftToMmApprox(static_cast<uint16_t>(shift));
    CAPTURE(shift);
    CHECK(mm >= previous_mm);
    previous_mm = mm;
  }
}

TEST_CASE("MmToShiftApprox inverts ShiftToMmApprox to within 1",
          "[nui][depth_convert]") {
  for (int shift = 400; shift <= 1050; ++shift) {
    const uint16_t mm = ShiftToMmApprox(static_cast<uint16_t>(shift));
    const uint16_t recovered_shift = MmToShiftApprox(mm);
    CAPTURE(shift, mm, recovered_shift);
    CHECK(std::abs(static_cast<int>(recovered_shift) - shift) <= 1);
  }
}

TEST_CASE("BuildMmToShiftTable agrees with the closed-form inverse",
          "[nui][depth_convert]") {
  // A full 11-bit shift table, generated the same way a real backend would
  // build one from its own shift-to-depth curve.
  std::vector<uint16_t> shift_to_mm(2048);
  uint16_t max_valid_mm = 0;
  for (size_t shift = 0; shift < shift_to_mm.size(); ++shift) {
    shift_to_mm[shift] = ShiftToMmApprox(static_cast<uint16_t>(shift));
    max_valid_mm = std::max(max_valid_mm, shift_to_mm[shift]);
  }
  REQUIRE(max_valid_mm > 0);
  REQUIRE(max_valid_mm < kMmMax);

  std::vector<uint16_t> mm_to_shift;
  BuildMmToShiftTable(shift_to_mm.data(), shift_to_mm.size(), &mm_to_shift);
  REQUIRE(mm_to_shift.size() == static_cast<size_t>(kMmMax) + 1);

  CHECK(mm_to_shift[0] == kShiftNoReading);

  for (int mm = 500; mm <= 4000; ++mm) {
    CAPTURE(mm);
    const int table_shift = mm_to_shift[mm];
    const int approx_shift = MmToShiftApprox(static_cast<uint16_t>(mm));
    CHECK(std::abs(table_shift - approx_shift) <= 1);
  }

  // Nothing in the table samples a depth this far out, so it is not covered.
  CHECK(mm_to_shift[max_valid_mm + 1] == kShiftNoReading);
}

TEST_CASE("ConvertDepthFrame converts a 4x2 frame", "[nui][depth_convert]") {
  DepthFrame frame;
  frame.width = 4;
  frame.height = 2;
  // Covers: no reading, an ordinary depth, the kMmShift3 boundary and just
  // past it, kMmMax exactly and just past it, and a wildly out-of-range
  // value.
  frame.depth_mm = {0, 500, 8191, 8192, 4000, 10000, 10001, 65535};
  std::array<uint16_t, 8> out{};

  SECTION("kMm copies the samples as-is") {
    ConvertDepthFrame(frame, GuestDepthFormat::kMm, nullptr, out.data());
    for (size_t i = 0; i < frame.depth_mm.size(); ++i) {
      CAPTURE(i);
      CHECK(out[i] == frame.depth_mm[i]);
    }
  }

  SECTION("kMmShift3 shifts by 3 and zeroes unknown or out-of-range depths") {
    ConvertDepthFrame(frame, GuestDepthFormat::kMmShift3, nullptr, out.data());
    CHECK(out[0] == 0);      // 0 -> unknown.
    CHECK(out[1] == 4000);   // 500 << 3.
    CHECK(out[2] == 65528);  // 8191 << 3, the largest representable value.
    CHECK(out[3] == 0);      // 8192 > 8191 -> 0.
    CHECK(out[4] == 32000);  // 4000 << 3.
    CHECK(out[5] == 0);      // 10000 > 8191 -> 0.
    CHECK(out[6] == 0);      // 10001 > 8191 -> 0.
    CHECK(out[7] == 0);      // 65535 > 8191 -> 0.
  }

  SECTION("kShift11 with a null table falls back to the approximation") {
    ConvertDepthFrame(frame, GuestDepthFormat::kShift11, nullptr, out.data());
    CHECK(out[0] == kShiftNoReading);  // 0 -> unknown.
    CHECK(out[1] == MmToShiftApprox(500));
    CHECK(out[2] == MmToShiftApprox(8191));
    CHECK(out[3] == MmToShiftApprox(8192));
    CHECK(out[4] == MmToShiftApprox(4000));
    CHECK(out[5] == MmToShiftApprox(10000));
    CHECK(out[6] == kShiftNoReading);  // 10001 > kMmMax -> no reading.
    CHECK(out[7] == kShiftNoReading);  // 65535 > kMmMax -> no reading.
  }

  SECTION("kShift11 with a table uses the table instead of the approximation") {
    std::vector<uint16_t> mm_to_shift(static_cast<size_t>(kMmMax) + 1,
                                      kShiftNoReading);
    // Sentinel values the approximation would never produce, so a passing
    // test proves the table was actually consulted.
    mm_to_shift[500] = 111;
    mm_to_shift[8191] = 222;
    mm_to_shift[4000] = 333;
    mm_to_shift[10000] = 444;

    ConvertDepthFrame(frame, GuestDepthFormat::kShift11, &mm_to_shift,
                      out.data());
    CHECK(out[0] == kShiftNoReading);  // mm 0 always looks up as no reading.
    CHECK(out[1] == 111);
    CHECK(out[2] == 222);
    CHECK(out[3] == kShiftNoReading);  // 8192 was left at the table default.
    CHECK(out[4] == 333);
    CHECK(out[5] == 444);
    // Indices past the end of a kMmMax + 1 entry table are treated as
    // uncovered rather than silently falling back to the approximation.
    CHECK(out[6] == kShiftNoReading);
    CHECK(out[7] == kShiftNoReading);
  }
}

}  // namespace test
}  // namespace nui
}  // namespace xe
