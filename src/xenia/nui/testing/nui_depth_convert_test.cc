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
#include <cmath>
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
  // At shift 1084 the PrimeSense denominator is still positive (about
  // 0.00188), so the relation does return a depth - about 533 metres. It is
  // the kMmMax ceiling that turns that into "no reading"; the denominator
  // only goes negative at 1085.
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

namespace {

// The width of one lookup-table step at a given depth, in millimetres: the
// derivative of the depth curve depth(i) = offset + scale / i.
double LutStepMmAt(double mm) {
  const double distance = mm - kLutDepthOffsetMm;
  return distance * distance / kLutDepthScaleMm;
}

}  // namespace

TEST_CASE("The LUT calibration anchors the guest depth window",
          "[nui][depth_convert]") {
  // A width other than a multiple of 640 would make the runtime's integer
  // w / 640 collapse its scale factor to zero.
  CHECK(kLutCalibration.w == 640);
  CHECK(kLutCalibration.h == 480);
  // The two ends of the window the encoding is designed for. One table entry
  // is a sixteenth of the calibration's unit, so the near end lands within
  // one of those of 400 mm rather than exactly on it.
  CHECK(LutDepthMmFromIndex(kLutIndexAtMaxDepth) == kLutDepthMaxMm);
  CHECK(std::abs(static_cast<int32_t>(LutDepthMmFromIndex(kLutIndexMax)) -
                 static_cast<int32_t>(kLutDepthMinMm)) <=
        1.0 / (kLutUnitsPerMm * kLutEntriesPerUnit) + 1.0);
  // The runtime's remap rule rejects 0 and anything from kLutIndexLimit up,
  // so they have to read as "no reading" on this side as well.
  CHECK(LutDepthMmFromIndex(0) == 0);
  CHECK(LutDepthMmFromIndex(kLutIndexLimit) == 0);
}

TEST_CASE(
    "Every entry of raw values 2000 to 3999 stays inside the guest's "
    "signed ceiling",
    "[nui][depth_convert]") {
  // The runtime binds its lookup tables as signed 16-bit integers, so an
  // entry above kLutEntryMax reaches the tracker as a negative depth, and
  // -32768 is the runtime's own "no reading" marker. Nothing in the window
  // may land there. That window is the encoder's own - kLutIndexAtMaxDepth
  // 2000 to kLutIndexMax 3999 - and not the runtime's full index range.
  for (uint32_t index = kLutIndexAtMaxDepth; index <= kLutIndexMax;
       index += kLutIndexStep) {
    const uint16_t entry = LutEntryFromIndex(index);
    CAPTURE(index, entry);
    REQUIRE(entry <= kLutEntryMax);
  }
}

TEST_CASE("LutDepthMmFromIndex falls monotonically across the window",
          "[nui][depth_convert]") {
  uint16_t previous_mm = LutDepthMmFromIndex(kLutIndexAtMaxDepth);
  REQUIRE(previous_mm == kLutDepthMaxMm);
  for (uint32_t index = kLutIndexAtMaxDepth + kLutIndexStep;
       index <= kLutIndexMax; index += kLutIndexStep) {
    const uint16_t mm = LutDepthMmFromIndex(index);
    CAPTURE(index, mm);
    CHECK(mm <= previous_mm);
    previous_mm = mm;
  }
  // As above, the near end is within one table entry of 400 mm.
  CHECK(std::abs(static_cast<int32_t>(previous_mm) -
                 static_cast<int32_t>(kLutDepthMinMm)) <=
        1.0 / (kLutUnitsPerMm * kLutEntriesPerUnit) + 1.0);
}

TEST_CASE("LutIndexFromDepthMm round-trips within one lookup-table step",
          "[nui][depth_convert]") {
  for (uint32_t mm = kLutDepthMinMm; mm <= kLutDepthMaxMm; ++mm) {
    const uint16_t index = LutIndexFromDepthMm(static_cast<uint16_t>(mm));
    CAPTURE(mm, index);
    REQUIRE(index >= kLutIndexStep);
    REQUIRE(index <= kLutIndexMax);
    REQUIRE(index % kLutIndexStep == 0);
    const int32_t recovered_mm =
        static_cast<int32_t>(LutDepthMmFromIndex(index));
    // One step of the table, plus the millimetre this helper rounds to.
    CHECK(std::abs(recovered_mm - static_cast<int32_t>(mm)) <=
          LutStepMmAt(mm) + 1.0);
  }
}

TEST_CASE("LutIndexFromDepthMm marks depths it cannot encode as no reading",
          "[nui][depth_convert]") {
  CHECK(LutIndexFromDepthMm(0) == kLutIndexNoReading);
  CHECK(LutIndexFromDepthMm(kLutDepthMaxMm + 1) == kLutIndexNoReading);
  CHECK(LutIndexFromDepthMm(65535) == kLutIndexNoReading);
  // Nearer than the window is clamped to the nearest value the table has
  // rather than thrown away: the sensor rarely reads that close, and an
  // occasional pixel at the clamp is better than a hole.
  CHECK(LutIndexFromDepthMm(kLutDepthMinMm - 100) == kLutIndexMax);
  CHECK(LutIndexFromDepthMm(kLutDepthMaxMm) == kLutIndexAtMaxDepth);
}

TEST_CASE("LutDepthTableEntry answers a raw value with its depth",
          "[nui][depth_convert]") {
  // The tracker reads an entry of 0 as "no reading", so every raw value the
  // encoding never produces has to be 0: index 0 itself, everything nearer
  // than the window's far end, and everything past its near end.
  CHECK(LutDepthTableEntry(0) == 0);
  CHECK(LutDepthTableEntry(1) == 0);
  CHECK(LutDepthTableEntry(kLutIndexAtMaxDepth - 1) == 0);
  CHECK(LutDepthTableEntry(kLutIndexMax + 1) == 0);
  CHECK(LutDepthTableEntry(kLutIndexLimit) == 0);
  CHECK(LutDepthTableEntry(8191) == 0);
  // The two ends of the window carry the two ends of the depth range, the
  // near one within one entry of 400 mm as everywhere else.
  CHECK(LutDepthTableEntry(kLutIndexAtMaxDepth) == kLutDepthMaxMm);
  CHECK(std::abs(static_cast<int32_t>(LutDepthTableEntry(kLutIndexMax)) -
                 static_cast<int32_t>(kLutDepthMinMm)) <=
        1.0 / (kLutUnitsPerMm * kLutEntriesPerUnit) + 1.0);
}

TEST_CASE("LutDepthTableEntry returns the depth the ring encoded",
          "[nui][depth_convert]") {
  // The round trip a depth pixel makes: the encoder picks the raw value, the
  // guest's tracker looks that value up in this table, and what comes back is
  // its depth in millimetres. The tolerance is one step of the hyperbola at
  // that depth - 0.9 mm at 400 mm, 3.6 mm at 4000 mm - plus a millimetre for
  // the two roundings on the way (the runtime truncates an entry to a
  // sixteenth of the calibration's unit, and LutDepthMmFromIndex rounds to a
  // whole millimetre). The worst case over the window is 2 mm.
  for (uint32_t mm = kLutDepthMinMm; mm <= kLutDepthMaxMm; ++mm) {
    const uint16_t index = LutIndexFromDepthMm(static_cast<uint16_t>(mm));
    const int32_t recovered_mm =
        static_cast<int32_t>(LutDepthTableEntry(index));
    CAPTURE(mm, index, recovered_mm);
    REQUIRE(recovered_mm != 0);
    CHECK(std::abs(recovered_mm - static_cast<int32_t>(mm)) <=
          LutStepMmAt(mm) + 1.0);
  }
}

TEST_CASE("LutDepthTableEntry falls as the raw value rises",
          "[nui][depth_convert]") {
  // The encoding is a hyperbola, so a larger raw value is a nearer object.
  // The tracker's own background test reads "nearer" as "numerically
  // smaller", which only works while the table runs the other way.
  uint16_t previous_mm = LutDepthTableEntry(kLutIndexAtMaxDepth);
  REQUIRE(previous_mm == kLutDepthMaxMm);
  for (uint32_t index = kLutIndexAtMaxDepth + kLutIndexStep;
       index <= kLutIndexMax; index += kLutIndexStep) {
    const uint16_t mm = LutDepthTableEntry(index);
    CAPTURE(index, mm);
    REQUIRE(mm != 0);
    REQUIRE(mm <= previous_mm);
    previous_mm = mm;
  }
}

TEST_CASE("LutEntryFromIndex matches the closed form of the runtime's formula",
          "[nui][depth_convert]") {
  // Folding k, w and h into the runtime's expression leaves LUT[i] = A - B/i
  // in sixteenths of a millimetre. Evaluating A and B here, in a different
  // order than the nested form the runtime (and LutEntryFromIndex) uses, is
  // an independent check of both the constants and the arithmetic.
  const double k = 1.0 / ((kLutCalibration.w / 640) * kLutCalibration.a);
  const double a_term = 16.0 * (kLutCalibration.d * k + kLutCalibration.c);
  const double b_term =
      16.0 * kLutCalibration.d * k * kLutCalibration.b * kLutCalibration.h;
  for (uint32_t index = kLutIndexAtMaxDepth; index <= kLutIndexMax;
       index += kLutIndexStep) {
    const double expected = std::trunc(a_term - b_term / index);
    CAPTURE(index, expected);
    // Both sides are truncated the way the runtime truncates; the two
    // arrangements can still disagree by one unit in the last place, which is
    // a sixteenth of the calibration's unit.
    CHECK(std::abs(static_cast<double>(LutEntryFromIndex(index)) - expected) <=
          1.0);
  }
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

  SECTION("kLutIndex encodes every sample as a lookup-table index") {
    ConvertDepthFrame(frame, GuestDepthFormat::kLutIndex, nullptr, out.data());
    CHECK(out[0] == kLutIndexNoReading);  // 0 -> unknown.
    CHECK(out[1] == LutIndexFromDepthMm(500));
    CHECK(out[2] == kLutIndexNoReading);   // 8191 is past the depth window.
    CHECK(out[3] == kLutIndexNoReading);   // 8192 likewise.
    CHECK(out[4] == kLutIndexAtMaxDepth);  // 4000 is the far end exactly.
    CHECK(out[5] == kLutIndexNoReading);   // 10000 is past the window.
    CHECK(out[6] == kLutIndexNoReading);   // 10001 likewise.
    CHECK(out[7] == kLutIndexNoReading);   // 65535 likewise.
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

TEST_CASE("ConvertDepthFrame mirrors each row when asked to",
          "[nui][depth_convert]") {
  // Distinct values on every pixel, all of them depths every format encodes,
  // so that a row written in the wrong order cannot pass by accident.
  DepthFrame frame;
  frame.width = 4;
  frame.height = 2;
  frame.depth_mm = {500, 1000, 2000, 3000, 800, 1500, 2500, 3500};

  const GuestDepthFormat formats[] = {
      GuestDepthFormat::kMm, GuestDepthFormat::kMmShift3,
      GuestDepthFormat::kShift11, GuestDepthFormat::kLutIndex};
  for (const GuestDepthFormat format : formats) {
    CAPTURE(static_cast<uint32_t>(format));
    std::array<uint16_t, 8> plain{};
    std::array<uint16_t, 8> mirrored{};
    ConvertDepthFrame(frame, format, nullptr, plain.data(), false);
    ConvertDepthFrame(frame, format, nullptr, mirrored.data(), true);

    // Every pixel of the mirrored image is the conversion of the pixel in the
    // opposite column of the same row; nothing crosses from one row to the
    // other.
    for (size_t row = 0; row < frame.height; ++row) {
      for (size_t column = 0; column < frame.width; ++column) {
        CAPTURE(row, column);
        CHECK(mirrored[row * frame.width + (frame.width - 1 - column)] ==
              plain[row * frame.width + column]);
      }
    }
    // The conversion itself is untouched, and the two images do differ, so
    // the check above is not satisfied by a row that reads the same either
    // way round.
    CHECK(plain[0] != plain[3]);
    CHECK(mirrored[0] == plain[3]);
    CHECK(mirrored[3] == plain[0]);
    CHECK(mirrored[4] == plain[7]);
    CHECK(mirrored[7] == plain[4]);
  }
}

TEST_CASE("ConvertDepthFrame writes the reversed rows a title expects",
          "[nui][depth_convert]") {
  DepthFrame frame;
  frame.width = 4;
  frame.height = 2;
  frame.depth_mm = {100, 200, 300, 400, 500, 600, 700, 800};
  std::array<uint16_t, 8> out{};

  SECTION("kMm reverses each row and nothing else") {
    ConvertDepthFrame(frame, GuestDepthFormat::kMm, nullptr, out.data(), true);
    const std::array<uint16_t, 8> expected = {400, 300, 200, 100,
                                              800, 700, 600, 500};
    CHECK(out == expected);
  }

  SECTION("kMm without mirroring leaves the rows as they were") {
    ConvertDepthFrame(frame, GuestDepthFormat::kMm, nullptr, out.data(), false);
    const std::array<uint16_t, 8> expected = {100, 200, 300, 400,
                                              500, 600, 700, 800};
    CHECK(out == expected);
  }

  SECTION("kLutIndex reverses each row of encoded values") {
    frame.depth_mm = {500, 1000, 2000, 3000, 4000, 0, 2500, 3500};
    ConvertDepthFrame(frame, GuestDepthFormat::kLutIndex, nullptr, out.data(),
                      true);
    const std::array<uint16_t, 8> expected = {
        LutIndexFromDepthMm(3000), LutIndexFromDepthMm(2000),
        LutIndexFromDepthMm(1000), LutIndexFromDepthMm(500),
        LutIndexFromDepthMm(3500), LutIndexFromDepthMm(2500),
        kLutIndexNoReading,        LutIndexFromDepthMm(4000)};
    CHECK(out == expected);
  }

  SECTION("a single column is its own mirror image") {
    frame.width = 1;
    frame.height = 4;
    frame.depth_mm = {100, 200, 300, 400};
    std::array<uint16_t, 4> column_out{};
    ConvertDepthFrame(frame, GuestDepthFormat::kMm, nullptr, column_out.data(),
                      true);
    const std::array<uint16_t, 4> expected = {100, 200, 300, 400};
    CHECK(column_out == expected);
  }

  SECTION("an odd width keeps its middle column in place") {
    frame.width = 3;
    frame.height = 1;
    frame.depth_mm = {100, 200, 300};
    std::array<uint16_t, 3> row_out{};
    ConvertDepthFrame(frame, GuestDepthFormat::kMm, nullptr, row_out.data(),
                      true);
    const std::array<uint16_t, 3> expected = {300, 200, 100};
    CHECK(row_out == expected);
  }
}

}  // namespace test
}  // namespace nui
}  // namespace xe
