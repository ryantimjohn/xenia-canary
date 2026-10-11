/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_PLAYER_MASK_H_
#define XENIA_KERNEL_XAM_NUI_PLAYER_MASK_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace kernel {
namespace xam {

// These functions summarise the two locked textures a HUD skeleton frame
// (message 0x2B002) carries, the player-index texture and the depth texture,
// for the play-frame log. They work on the texture's bytes as they lie in
// guest memory, so that they can be tested without a guest, and they include
// nothing of the kernel.
//
// Every pixel of both textures is a 16-bit word, big endian as everything in
// guest memory is, and each row is padded up to a multiple of 256 bytes. The
// runtime keeps two streams of that kind: the depth-plus-player-index image,
// 320 columns by 240 rows, whose words hold the depth in millimetres in the
// high 13 bits and the player index in the low three, as a Kinect's own depth
// image does, and a small image of 80 columns by 60 rows. So the pitch, which
// is all a locked rect says about a texture, tells the stream.

// A texture's size in pixels; 0 by 0 is no stream.
struct NuiHudTextureShape {
  uint32_t columns = 0;
  uint32_t rows = 0;

  constexpr bool valid() const { return columns && rows; }
};

inline constexpr NuiHudTextureShape kNuiHudTextureFull = {320, 240};
inline constexpr NuiHudTextureShape kNuiHudTextureSmall = {80, 60};

// The bytes of one row of pixels, not counting the padding after them.
constexpr uint32_t NuiHudTextureRowBytes(NuiHudTextureShape shape) {
  return shape.columns * 2;
}

// Returns the stream a texture of this pitch is: the full one when the pitch
// holds a row of 320 words, the small one when it holds a row of 80 but not of
// 320, and none when it does not even hold 80.
constexpr NuiHudTextureShape NuiHudTextureShapeForPitch(uint32_t pitch) {
  if (pitch >= NuiHudTextureRowBytes(kNuiHudTextureFull)) {
    return kNuiHudTextureFull;
  }
  if (pitch >= NuiHudTextureRowBytes(kNuiHudTextureSmall)) {
    return kNuiHudTextureSmall;
  }
  return NuiHudTextureShape();
}

// The name a log line gives a shape.
constexpr const char* NuiHudTextureShapeName(NuiHudTextureShape shape) {
  if (shape.columns == kNuiHudTextureFull.columns &&
      shape.rows == kNuiHudTextureFull.rows) {
    return "320x240";
  }
  if (shape.columns == kNuiHudTextureSmall.columns &&
      shape.rows == kNuiHudTextureSmall.rows) {
    return "80x60";
  }
  return "unknown";
}

// Returns how many bytes from the texture's start the summaries read: the rows
// before the last are each a whole pitch and the last is only as long as its
// pixels, because nothing says that the padding after it is mapped. The
// result is 64 bits wide so that a wild pitch cannot wrap it.
constexpr uint64_t NuiHudTextureBytesRead(uint32_t pitch,
                                          NuiHudTextureShape shape) {
  return uint64_t(shape.rows - 1) * pitch + NuiHudTextureRowBytes(shape);
}

// A player index runs from 1 to 7; 0 is no player.
constexpr size_t kNuiPlayerMaskIndexCount = 7;

// What the low three bits of a texture's words hold.
struct NuiPlayerMaskSummary {
  // How many pixels have a non-zero index.
  uint32_t pixel_count = 0;
  // The mean column and row of those pixels, and 0 when there are none.
  double centroid_column = 0.0;
  double centroid_row = 0.0;
  // How many pixels hold each index, 1 to 7 as elements 0 to 6.
  uint32_t histogram[kNuiPlayerMaskIndexCount] = {};
};

// Reads a big-endian 16-bit word.
constexpr uint32_t NuiHudTextureWord(const uint8_t* bits) {
  return (uint32_t(bits[0]) << 8) | bits[1];
}

// Summarises the player index in the texture in bits, which must be readable
// for NuiHudTextureBytesRead(pitch, shape) bytes. Every field of out is
// written.
inline void SummariseNuiPlayerMask(const uint8_t* bits, uint32_t pitch,
                                   NuiHudTextureShape shape,
                                   NuiPlayerMaskSummary* out) {
  *out = NuiPlayerMaskSummary();
  uint64_t column_sum = 0;
  uint64_t row_sum = 0;
  for (uint32_t row = 0; row < shape.rows; ++row) {
    const uint8_t* pixel = bits + size_t(row) * pitch;
    for (uint32_t column = 0; column < shape.columns; ++column) {
      const uint32_t index = NuiHudTextureWord(pixel + size_t(column) * 2) & 7;
      if (!index) {
        continue;
      }
      ++out->pixel_count;
      column_sum += column;
      row_sum += row;
      ++out->histogram[index - 1];
    }
  }
  if (out->pixel_count) {
    out->centroid_column = double(column_sum) / out->pixel_count;
    out->centroid_row = double(row_sum) / out->pixel_count;
  }
}

// What the depth texture holds.
struct NuiDepthSummary {
  // How many pixels are not zero.
  uint32_t pixel_count = 0;
  // The mean of those pixels' words as stored, and 0 when there are none. Zero
  // pixels are left out so that a texture that is mostly empty still shows
  // what its filled part says.
  double mean = 0.0;
};

// Summarises the depth texture in bits, read the way SummariseNuiPlayerMask
// reads the player-index one. Every field of out is written.
inline void SummariseNuiDepthTexture(const uint8_t* bits, uint32_t pitch,
                                     NuiHudTextureShape shape,
                                     NuiDepthSummary* out) {
  *out = NuiDepthSummary();
  uint64_t sum = 0;
  for (uint32_t row = 0; row < shape.rows; ++row) {
    const uint8_t* pixel = bits + size_t(row) * pitch;
    for (uint32_t column = 0; column < shape.columns; ++column) {
      const uint32_t value = NuiHudTextureWord(pixel + size_t(column) * 2);
      if (value) {
        ++out->pixel_count;
        sum += value;
      }
    }
  }
  if (out->pixel_count) {
    out->mean = double(sum) / out->pixel_count;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_PLAYER_MASK_H_
