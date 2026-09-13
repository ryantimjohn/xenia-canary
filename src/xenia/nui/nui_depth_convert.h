/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_DEPTH_CONVERT_H_
#define XENIA_NUI_NUI_DEPTH_CONVERT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenia/nui/depth_frame.h"

namespace xe {
namespace nui {

// Raw 11-bit PrimeSense shift value meaning "no measurement".
constexpr uint16_t kShiftNoReading = 0x7FF;
// Depths beyond this are treated as no reading.
constexpr uint16_t kMmMax = 10000;

// Millimetre depth formats a guest title may ask the NUI subsystem to
// produce.
enum class GuestDepthFormat : uint32_t {
  // 16-bit words holding raw PrimeSense 11-bit shift values; kShiftNoReading
  // when unknown.
  kShift11,
  // depth_mm << 3 (Kinect for Windows packing); 0 when unknown; mm > 8191
  // maps to 0.
  kMmShift3,
  // depth_mm as-is; 0 when unknown.
  kMm,
};

// Converts a raw PrimeSense shift value to an approximate depth in
// millimetres, using the standard relation
// depth_m = 1 / (shift * -0.0030711016 + 3.3309495161). Returns 0 for
// kShiftNoReading, for shift >= 1084 where the denominator is no longer
// positive, and for results greater than kMmMax.
uint16_t ShiftToMmApprox(uint16_t shift);

// Converts an approximate depth in millimetres to the raw PrimeSense shift
// value that would have produced it, inverting ShiftToMmApprox. Returns
// kShiftNoReading for mm == 0 or mm > kMmMax; otherwise the rounded inverse,
// clamped to [0, 0x7FE].
uint16_t MmToShiftApprox(uint16_t mm);

// Inverts a guest-provided shift->mm table (index = shift, value = mm, 0 =
// unknown) into an mm->shift table of kMmMax + 1 entries: for each mm, the
// shift whose mm is nearest. mm values not covered by the table map to
// kShiftNoReading.
void BuildMmToShiftTable(const uint16_t* shift_to_mm, size_t shift_count,
                         std::vector<uint16_t>* mm_to_shift);

// Converts frame.width * frame.height pixels of frame.depth_mm into out,
// according to format. mm_to_shift may be null, in which case
// MmToShiftApprox is used to convert each sample for kShift11.
void ConvertDepthFrame(const DepthFrame& frame, GuestDepthFormat format,
                       const std::vector<uint16_t>* mm_to_shift, uint16_t* out);

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_DEPTH_CONVERT_H_
