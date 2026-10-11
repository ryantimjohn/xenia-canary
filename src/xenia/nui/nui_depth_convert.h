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
  // Index into the lookup table the NUI runtime builds from the calibration
  // parameters below; kLutIndexNoReading when unknown. This is what a Kinect
  // title's depth ring holds.
  kLutIndex,
};

// Parameters the kernel answers a code-14 camera request with. The NUI
// runtime builds its 8192-entry depth lookup table out of them:
//   k = 1 / ((w / 640) * h * a)        (w / 640 is an integer division)
//   LUT[i] = (uint16_t) trunc(16 * (d*k*h * (1 - b*k*h / (i*k)) + c))
// so an entry is a depth in sixteenths of whatever unit a, b, c and d are
// expressed in, and the curve reduces to LUT[i] = A - B/i. See docs/nui.md
// "Calibration source".
struct LutCalibration {
  double a;
  double b;
  double c;
  double d;
  uint16_t w;
  uint16_t h;
};

// The runtime's table entries count sixteenths of the calibration's unit.
constexpr double kLutEntriesPerUnit = 16.0;

// The largest entry the guest can read back. The runtime binds its lookup
// tables with all four sign fields set to kSigned and an integer number
// format (D3DFORMAT 0x1A22AA58), so the shader receives each entry as a
// signed 16-bit integer: anything above this is negative, and the runtime's
// own "no reading" marker 0x8000 is the most negative value rather than a
// separate flag bit.
constexpr uint16_t kLutEntryMax = 32767;

// Calibration units per millimetre: how many of the units a, b, c and d are
// expressed in go into one millimetre. A table entry for a depth is
// kLutEntriesPerUnit * kLutUnitsPerMm * depth_mm, and the guest reads entries
// as signed 16-bit integers (kLutEntryMax), so this cannot be 1.0:
// millimetres would cap the sensor at 2047.9 mm and hand the tracker -1536
// for a person four metres away. 0.1 is centimetres - 4000 mm becomes 6400, a
// quarter of the ceiling, and one entry is still 1/16 cm = 0.625 mm, finer
// than the table's own 0.9 to 3.6 mm steps. Anything from about 0.5 down
// would fit the ceiling; metres would be 0.001. Centimetres are a deduction
// from that ceiling and from the millimetres the NUI skeleton API works in,
// not an observation, so this stays a one-line switch.
constexpr double kLutUnitsPerMm = 0.1;

// Switch (inferred, not observed): how the raw value sits in a 16-bit ring
// slot. The runtime indexes its lookup table with the value itself, so 0
// (low-aligned) is the reading the disassembly supports; 3 would move it up
// by three bits, in the way the console's depth images carry a player index
// in their low bits, and the encoding below follows along by using only
// multiples of 8.
constexpr uint32_t kLutIndexSlotShift = 0;

// A raw value of 0 means "no reading", and the runtime's own remap rule
// treats everything from kLutIndexLimit up the same way.
constexpr uint16_t kLutIndexNoReading = 0;
constexpr uint32_t kLutIndexLimit = 4000;

// The depths the encoding is laid out for. 400 mm is about as close as a
// Kinect reads, and 4000 mm is the far end of a title's play space.
constexpr uint16_t kLutDepthMinMm = 400;
constexpr uint16_t kLutDepthMaxMm = 4000;

// The spacing of the raw values, and the two ends of the range they use.
constexpr uint32_t kLutIndexStep = 1u << kLutIndexSlotShift;
constexpr uint32_t kLutIndexMax =
    ((kLutIndexLimit - 1) / kLutIndexStep) * kLutIndexStep;
// The raw value that encodes kLutDepthMaxMm. The curve is a hyperbola, so
// its steps grow towards the low end of the range; putting the far end of
// the depth window halfway up the range is what makes the widest of those
// steps as narrow as it can be (3.6 mm at 4000 mm, 0.9 mm at 400 mm).
constexpr uint32_t kLutIndexAtMaxDepth =
    ((kLutIndexLimit / 2) / kLutIndexStep) * kLutIndexStep;

// The curve that lays out: depth_mm(i) = kLutDepthOffsetMm +
// kLutDepthScaleMm / i, solved so that the two ends of the depth window land
// on the two ends of the value range.
constexpr double kLutDepthScaleMm =
    (kLutDepthMaxMm - kLutDepthMinMm) /
    (1.0 / kLutIndexAtMaxDepth - 1.0 / kLutIndexMax);
constexpr double kLutDepthOffsetMm =
    kLutDepthMaxMm - kLutDepthScaleMm / kLutIndexAtMaxDepth;

// The image the calibration describes. The width must be a multiple of 640,
// because the runtime divides it by 640 as an integer.
constexpr uint16_t kLutCalibrationWidth = 640;
constexpr uint16_t kLutCalibrationHeight = 480;

// The parameters themselves, chosen so that the runtime's formula reproduces
// the curve above: with a = 1 and w = 640 the runtime's k*h is 1, which
// leaves depth(i) = d * (1 - b*h/i) + c.
constexpr LutCalibration kLutCalibration = {
    /*a=*/1.0,
    /*b=*/-kLutDepthScaleMm / kLutCalibrationHeight,
    /*c=*/(kLutDepthOffsetMm - 1.0) * kLutUnitsPerMm,
    /*d=*/kLutUnitsPerMm,
    /*w=*/kLutCalibrationWidth,
    /*h=*/kLutCalibrationHeight,
};

// The lookup table entry the NUI runtime computes for a raw value, in
// sixteenths of the calibration's unit. Reproduces the runtime's arithmetic:
// doubles throughout, a fused multiply-add, truncation towards zero, and only
// the low 16 bits kept. Keeping only those bits means that entries past
// 65535, which is every raw value at or below 326 with these parameters, wrap
// and are meaningless - exactly as they are in the runtime's own table.
// LutIndexFromDepthMm never produces one: the window starts at 2000.
uint16_t LutEntryFromIndex(uint32_t index);

// The depth in millimetres the NUI runtime reads for a raw value. 0 for the
// raw values the runtime treats as "no reading".
uint16_t LutDepthMmFromIndex(uint32_t index);

// The raw value that encodes a depth, inverting LutDepthMmFromIndex.
// kLutIndexNoReading for 0 and for depths beyond kLutDepthMaxMm; depths
// nearer than kLutDepthMinMm are clamped to the nearest value the table has.
uint16_t LutIndexFromDepthMm(uint16_t mm);

// The entry the kernel answers a code-24 camera request with for a raw ring
// value: the depth in millimetres that value encodes. The title's skeletal
// tracker reads this table with an unsigned halfword load and uses the entry
// as its metric depth, with no scaling of any kind, so the table is the
// sensor's raw-value-to-millimetre conversion - the one a real camera's
// firmware supplies. See docs/nui.md "Calibration source". 0 means "no
// reading", the same convention as DepthFrame, and is what index 0 and every
// index outside the window the encoding lays out (kLutIndexAtMaxDepth to
// kLutIndexMax) get.
uint16_t LutDepthTableEntry(uint32_t index);

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
//
// With mirror_columns set every row is written in reverse: out receives the
// conversion of the pixel at (row, column) at (row, width - 1 - column), for
// every format. A frame keeps the orientation its backend gave it (see
// DepthFrame), and this is how the guest gets the other one. It defaults to
// off, so that a caller that has no reason to flip the image gets it as it is.
void ConvertDepthFrame(const DepthFrame& frame, GuestDepthFormat format,
                       const std::vector<uint16_t>* mm_to_shift, uint16_t* out,
                       bool mirror_columns = false);

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_DEPTH_CONVERT_H_
