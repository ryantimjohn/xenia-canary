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
  const double mm = 1000.0 / denominator;
  if (mm > static_cast<double>(kMmMax)) {
    return 0;
  }
  return static_cast<uint16_t>(std::lround(mm));
}

uint16_t MmToShiftApprox(uint16_t mm) {
  if (mm == 0 || mm > kMmMax) {
    return kShiftNoReading;
  }
  const double target_denominator = 1000.0 / static_cast<double>(mm);
  const double shift =
      (target_denominator - kShiftToDepthIntercept) / kShiftToDepthSlope;
  const long rounded_shift = std::lround(shift);
  return static_cast<uint16_t>(
      std::clamp<long>(rounded_shift, 0, kShiftApproxMax));
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

void ConvertDepthFrame(const DepthFrame& frame, GuestDepthFormat format,
                       const std::vector<uint16_t>* mm_to_shift,
                       uint16_t* out) {
  assert_not_null(out);
  if (!out) {
    return;
  }
  const size_t pixel_count = static_cast<size_t>(frame.width) * frame.height;
  assert_true(frame.depth_mm.size() == pixel_count);
  for (size_t i = 0; i < pixel_count; ++i) {
    const uint16_t mm = frame.depth_mm[i];
    switch (format) {
      case GuestDepthFormat::kMm: {
        out[i] = mm;
        break;
      }
      case GuestDepthFormat::kMmShift3: {
        if (mm == 0 || mm > kMmShift3Max) {
          out[i] = 0;
        } else {
          out[i] = static_cast<uint16_t>(mm << 3);
        }
        break;
      }
      case GuestDepthFormat::kShift11: {
        if (mm_to_shift) {
          out[i] =
              mm < mm_to_shift->size() ? (*mm_to_shift)[mm] : kShiftNoReading;
        } else {
          out[i] = MmToShiftApprox(mm);
        }
        break;
      }
      default:
        assert_unhandled_case(format);
        break;
    }
  }
}

}  // namespace nui
}  // namespace xe
