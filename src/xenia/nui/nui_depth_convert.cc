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
#include <cmath>
#include <limits>

#include "xenia/base/assert.h"

namespace xe {
namespace nui {

namespace {

// Standard PrimeSense shift-to-depth relation, from the sensor's factory
// calibration: depth_m = 1 / (shift * kShiftToDepthSlope +
// kShiftToDepthIntercept).
constexpr double kShiftToDepthSlope = -0.0030711016;
constexpr double kShiftToDepthIntercept = 3.3309495161;

// The relation above is in metres and every depth on this side of the API is
// in millimetres, so the numerator of the one and the target of the other are
// scaled by this.
constexpr double kMillimetresPerMetre = 1000.0;

// The largest shift MmToShiftApprox may return: one below kShiftNoReading,
// so an approximated value is never mistaken for "no reading".
constexpr uint16_t kShiftApproxMax = 0x7FE;

// The largest millimetre value that still fits after <<3 without overflowing
// the 16-bit kMmShift3 word: 8191 << 3 == 65528, the largest multiple of 8
// representable in a uint16_t.
constexpr uint16_t kMmShift3Max = 8191;

}  // namespace

uint16_t ShiftToMmApprox(uint16_t shift) {
  if (shift == kShiftNoReading) {
    return 0;
  }
  const double denominator =
      static_cast<double>(shift) * kShiftToDepthSlope + kShiftToDepthIntercept;
  if (denominator <= 0.0) {
    // The relation is only meaningful while the denominator is positive;
    // beyond that the sensor's shift range has no corresponding depth.
    return 0;
  }
  const double mm = kMillimetresPerMetre / denominator;
  if (mm > static_cast<double>(kMmMax)) {
    return 0;
  }
  return static_cast<uint16_t>(std::lround(mm));
}

uint16_t MmToShiftApprox(uint16_t mm) {
  if (mm == 0 || mm > kMmMax) {
    return kShiftNoReading;
  }
  const double target_denominator =
      kMillimetresPerMetre / static_cast<double>(mm);
  const double shift =
      (target_denominator - kShiftToDepthIntercept) / kShiftToDepthSlope;
  const long rounded_shift = std::lround(shift);
  return static_cast<uint16_t>(
      std::clamp<long>(rounded_shift, 0, kShiftApproxMax));
}

uint16_t LutEntryFromIndex(uint32_t index) {
  // The three constants the runtime works out once per table, in its order
  // (the code at 0x82d238d8): the width scale is an integer division, and the
  // height is folded into d and b rather than into every entry.
  const double width_scale = static_cast<double>(kLutCalibration.w / 640);
  const double height = static_cast<double>(kLutCalibration.h);
  const double k = 1.0 / ((width_scale * height) * kLutCalibration.a);
  const double k_height = k * height;
  const double k_height_d = k_height * kLutCalibration.d;
  const double k_height_b = k_height * kLutCalibration.b;

  const double index_k = static_cast<double>(index) * k;
  const double ratio = (index_k - k_height_b) / index_k;
  // The runtime's multiply-add is fused, so it rounds once.
  const double value =
      std::fma(ratio, k_height_d, kLutCalibration.c) * kLutEntriesPerUnit;

  // It converts with fctiwz, which truncates towards zero and saturates at
  // the ends of the 32-bit range, and then stores the low halfword.
  int32_t truncated;
  if (std::isnan(value)) {
    truncated = 0;
  } else if (value >= 2147483647.0) {
    truncated = std::numeric_limits<int32_t>::max();
  } else if (value <= -2147483648.0) {
    truncated = std::numeric_limits<int32_t>::min();
  } else {
    truncated = static_cast<int32_t>(value);
  }
  return static_cast<uint16_t>(static_cast<uint32_t>(truncated));
}

uint16_t LutDepthMmFromIndex(uint32_t index) {
  if (index == kLutIndexNoReading || index >= kLutIndexLimit) {
    // The runtime's remap rule gives these no depth at all.
    return 0;
  }
  const double mm = static_cast<double>(LutEntryFromIndex(index)) /
                    (kLutEntriesPerUnit * kLutUnitsPerMm);
  return static_cast<uint16_t>(std::clamp<long>(
      std::lround(mm), 0, std::numeric_limits<uint16_t>::max()));
}

uint16_t LutIndexFromDepthMm(uint16_t mm) {
  if (mm == 0 || mm > kLutDepthMaxMm) {
    return kLutIndexNoReading;
  }
  // Inverts depth(index) = kLutDepthOffsetMm + kLutDepthScaleMm / index. The
  // offset is behind the sensor, so the denominator is always positive.
  const double index =
      kLutDepthScaleMm / (static_cast<double>(mm) - kLutDepthOffsetMm);
  const long steps = std::lround(index / kLutIndexStep);
  const long clamped_steps =
      std::clamp<long>(steps, 1, kLutIndexMax / kLutIndexStep);
  return static_cast<uint16_t>(clamped_steps * kLutIndexStep);
}

uint16_t LutDepthTableEntry(uint32_t index) {
  // Only the raw values the encoder produces stand for a depth: the window
  // runs from kLutIndexAtMaxDepth, which carries kLutDepthMaxMm, to
  // kLutIndexMax, which carries kLutDepthMinMm, and, when the ring shifts its
  // values up, holds only the multiples of kLutIndexStep between them. Index
  // 0 and everything outside that window read 0, which is what both the
  // tracker and the runtime treat as "no reading".
  if (index < kLutIndexAtMaxDepth || index > kLutIndexMax ||
      index % kLutIndexStep != 0) {
    return 0;
  }
  return LutDepthMmFromIndex(index);
}

void BuildMmToShiftTable(const uint16_t* shift_to_mm, size_t shift_count,
                         std::vector<uint16_t>* mm_to_shift) {
  assert_not_null(mm_to_shift);
  if (!mm_to_shift) {
    return;
  }
  mm_to_shift->assign(static_cast<size_t>(kMmMax) + 1, kShiftNoReading);
  if (!shift_to_mm || !shift_count) {
    return;
  }

  // The span of depths the table actually samples; targets outside it are
  // not covered by the table and stay kShiftNoReading.
  uint16_t min_valid_mm = 0;
  uint16_t max_valid_mm = 0;
  bool has_valid_entry = false;
  for (size_t shift = 0; shift < shift_count; ++shift) {
    const uint16_t mm = shift_to_mm[shift];
    if (mm == 0 || mm > kMmMax) {
      continue;
    }
    if (!has_valid_entry) {
      min_valid_mm = mm;
      max_valid_mm = mm;
      has_valid_entry = true;
      continue;
    }
    min_valid_mm = std::min(min_valid_mm, mm);
    max_valid_mm = std::max(max_valid_mm, mm);
  }
  if (!has_valid_entry) {
    return;
  }

  for (uint32_t target_mm = min_valid_mm; target_mm <= max_valid_mm;
       ++target_mm) {
    uint16_t nearest_shift = kShiftNoReading;
    uint32_t nearest_distance = std::numeric_limits<uint32_t>::max();
    for (size_t shift = 0; shift < shift_count; ++shift) {
      const uint16_t mm = shift_to_mm[shift];
      if (mm == 0 || mm > kMmMax) {
        continue;
      }
      const uint32_t distance =
          mm > target_mm ? mm - target_mm : target_mm - mm;
      if (distance < nearest_distance) {
        nearest_distance = distance;
        nearest_shift = static_cast<uint16_t>(shift);
      }
    }
    (*mm_to_shift)[target_mm] = nearest_shift;
  }
}

namespace {

// The guest's word for one depth sample, in the given format.
uint16_t ConvertDepthSample(uint16_t mm, GuestDepthFormat format,
                            const std::vector<uint16_t>* mm_to_shift) {
  switch (format) {
    case GuestDepthFormat::kMm: {
      return mm;
    }
    case GuestDepthFormat::kMmShift3: {
      if (mm == 0 || mm > kMmShift3Max) {
        return 0;
      }
      return static_cast<uint16_t>(mm << 3);
    }
    case GuestDepthFormat::kLutIndex: {
      return LutIndexFromDepthMm(mm);
    }
    case GuestDepthFormat::kShift11: {
      if (mm_to_shift) {
        return mm < mm_to_shift->size() ? (*mm_to_shift)[mm] : kShiftNoReading;
      }
      return MmToShiftApprox(mm);
    }
    default:
      assert_unhandled_case(format);
      return 0;
  }
}

}  // namespace

void ConvertDepthFrame(const DepthFrame& frame, GuestDepthFormat format,
                       const std::vector<uint16_t>* mm_to_shift, uint16_t* out,
                       bool mirror_columns) {
  assert_not_null(out);
  if (!out) {
    return;
  }
  const size_t width = frame.width;
  const size_t pixel_count = width * frame.height;
  assert_true(frame.depth_mm.size() == pixel_count);
  for (size_t row_start = 0; row_start < pixel_count; row_start += width) {
    for (size_t column = 0; column < width; ++column) {
      // Where the sample lands in its row: its own column, or the one at the
      // other end of the row.
      const size_t out_column = mirror_columns ? width - 1 - column : column;
      out[row_start + out_column] = ConvertDepthSample(
          frame.depth_mm[row_start + column], format, mm_to_shift);
    }
  }
}

}  // namespace nui
}  // namespace xe
