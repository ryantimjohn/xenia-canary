/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_player_mask.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {
namespace {

// The bytes of a texture in guest memory of the given pitch and rows, with
// every byte given a value so that a read that strays into the padding shows
// up.
std::vector<uint8_t> MakeTexture(uint32_t pitch, uint32_t rows, uint8_t fill) {
  return std::vector<uint8_t>(size_t(pitch) * rows, fill);
}

// Sets one pixel the way the guest holds it: a big-endian word.
void SetPixel(std::vector<uint8_t>* texture, uint32_t pitch, uint32_t column,
              uint32_t row, uint16_t value) {
  const size_t offset = size_t(row) * pitch + size_t(column) * 2;
  (*texture)[offset] = static_cast<uint8_t>(value >> 8);
  (*texture)[offset + 1] = static_cast<uint8_t>(value);
}

// Fills every byte of each row after its last pixel.
void FillPadding(std::vector<uint8_t>* texture, uint32_t pitch,
                 NuiHudTextureShape shape, uint8_t fill) {
  for (uint32_t row = 0; row < shape.rows; ++row) {
    for (uint32_t byte = shape.columns * 2; byte < pitch; ++byte) {
      (*texture)[size_t(row) * pitch + byte] = fill;
    }
  }
}

}  // namespace

TEST_CASE("A texture's shape follows from its pitch", "[nui][player_mask]") {
  // Rows are padded to a multiple of 256 bytes: 320 words take 640 bytes and
  // a pitch of 768, 80 words take 160 and a pitch of 256.
  CHECK_FALSE(NuiHudTextureShapeForPitch(0).valid());
  CHECK_FALSE(NuiHudTextureShapeForPitch(159).valid());

  for (uint32_t pitch : {160u, 256u, 512u, 639u}) {
    const NuiHudTextureShape small_shape = NuiHudTextureShapeForPitch(pitch);
    CHECK(small_shape.columns == 80u);
    CHECK(small_shape.rows == 60u);
  }
  for (uint32_t pitch : {640u, 768u, 1024u, 0xFFFFFFFFu}) {
    const NuiHudTextureShape full_shape = NuiHudTextureShapeForPitch(pitch);
    CHECK(full_shape.columns == 320u);
    CHECK(full_shape.rows == 240u);
  }
}

TEST_CASE("The shapes are named for the log", "[nui][player_mask]") {
  CHECK(std::string(NuiHudTextureShapeName(kNuiHudTextureFull)) == "320x240");
  CHECK(std::string(NuiHudTextureShapeName(kNuiHudTextureSmall)) == "80x60");
  CHECK(std::string(NuiHudTextureShapeName(NuiHudTextureShape())) == "unknown");
}

TEST_CASE("A texture's summary reads no further than its last row",
          "[nui][player_mask]") {
  // The rows before the last are each a whole pitch and the last is only as
  // long as its pixels, because nothing says that padding after it is mapped.
  CHECK(NuiHudTextureBytesRead(640, kNuiHudTextureFull) == 239u * 640u + 640u);
  CHECK(NuiHudTextureBytesRead(768, kNuiHudTextureFull) == 239u * 768u + 640u);
  CHECK(NuiHudTextureBytesRead(256, kNuiHudTextureSmall) == 59u * 256u + 160u);
  // A pitch near the top of what a title could hand over does not wrap.
  CHECK(NuiHudTextureBytesRead(0xFFFFFFFFu, kNuiHudTextureFull) ==
        239ull * 0xFFFFFFFFull + 640ull);
}

TEST_CASE("An empty full-size mask has no pixels and no centroid",
          "[nui][player_mask]") {
  const std::vector<uint8_t> texture = MakeTexture(768, 240, 0);
  NuiPlayerMaskSummary summary;
  // Every field must be overwritten, not accumulated into.
  summary.pixel_count = 99;
  summary.centroid_column = 99.0;
  summary.centroid_row = 99.0;
  summary.histogram[3] = 99;

  SummariseNuiPlayerMask(texture.data(), 768, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 0u);
  CHECK(summary.centroid_column == Approx(0.0));
  CHECK(summary.centroid_row == Approx(0.0));
  for (uint32_t bin : summary.histogram) {
    CHECK(bin == 0u);
  }
}

TEST_CASE("An empty small mask has no pixels and no centroid",
          "[nui][player_mask]") {
  const std::vector<uint8_t> texture = MakeTexture(256, 60, 0);
  NuiPlayerMaskSummary summary;
  summary.pixel_count = 99;

  SummariseNuiPlayerMask(texture.data(), 256, kNuiHudTextureSmall, &summary);

  CHECK(summary.pixel_count == 0u);
  CHECK(summary.centroid_column == Approx(0.0));
  CHECK(summary.centroid_row == Approx(0.0));
  for (uint32_t bin : summary.histogram) {
    CHECK(bin == 0u);
  }
}

TEST_CASE("A blob's pixels are counted and its centroid found",
          "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(768, 240, 0);
  // Player 1: four columns (100..103) by three rows (50..52), 12 pixels, at
  // depths that differ from pixel to pixel so that only the low three bits
  // can be what makes the pixel count.
  for (uint32_t row = 50; row <= 52; ++row) {
    for (uint32_t column = 100; column <= 103; ++column) {
      const uint16_t depth = static_cast<uint16_t>(1500 + column + row);
      SetPixel(&texture, 768, column, row,
               static_cast<uint16_t>((depth << 3) | 1));
    }
  }
  // Player 2: two columns (200..201) by two rows (100..101), 4 pixels.
  for (uint32_t row = 100; row <= 101; ++row) {
    for (uint32_t column = 200; column <= 201; ++column) {
      SetPixel(&texture, 768, column, row,
               static_cast<uint16_t>((3000 << 3) | 2));
    }
  }
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 768, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 16u);
  // The columns sum to 3 * 406 + 2 * 401 = 2020 and the rows to
  // 4 * 153 + 2 * 201 = 1014, over 16 pixels.
  CHECK(summary.centroid_column == Approx(2020.0 / 16.0));
  CHECK(summary.centroid_row == Approx(1014.0 / 16.0));
  CHECK(summary.histogram[0] == 12u);  // Index 1.
  CHECK(summary.histogram[1] == 4u);   // Index 2.
  for (size_t bin = 2; bin < kNuiPlayerMaskIndexCount; ++bin) {
    CHECK(summary.histogram[bin] == 0u);
  }
}

TEST_CASE("A pixel with a depth and no index is not part of the mask",
          "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(640, 240, 0);
  // 1500 mm for player 1, 2750 mm for player 5, 4000 mm for player 2, and
  // 3000 mm for nobody.
  SetPixel(&texture, 640, 10, 20, static_cast<uint16_t>((1500 << 3) | 1));
  SetPixel(&texture, 640, 30, 40, static_cast<uint16_t>((2750 << 3) | 5));
  SetPixel(&texture, 640, 50, 60, static_cast<uint16_t>((4000 << 3) | 2));
  SetPixel(&texture, 640, 200, 200, static_cast<uint16_t>(3000 << 3));
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 640, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 3u);
  CHECK(summary.centroid_column == Approx((10.0 + 30.0 + 50.0) / 3.0));
  CHECK(summary.centroid_row == Approx((20.0 + 40.0 + 60.0) / 3.0));
  CHECK(summary.histogram[0] == 1u);  // Index 1.
  CHECK(summary.histogram[1] == 1u);  // Index 2.
  CHECK(summary.histogram[4] == 1u);  // Index 5.
  CHECK(summary.histogram[2] == 0u);
  CHECK(summary.histogram[3] == 0u);
  CHECK(summary.histogram[5] == 0u);
  CHECK(summary.histogram[6] == 0u);
}

TEST_CASE("A single pixel is its own centroid", "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(768, 240, 0);
  SetPixel(&texture, 768, 319, 239, 7);  // The last pixel of the texture.
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 768, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 1u);
  CHECK(summary.centroid_column == Approx(319.0));
  CHECK(summary.centroid_row == Approx(239.0));
  CHECK(summary.histogram[6] == 1u);  // Index 7.
}

TEST_CASE("A word is read big endian", "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(768, 240, 0);
  // The word 0x0102 is player 2 with a depth of 0x20; read little endian it
  // would be 0x0201, player 1.
  texture[0] = 0x01;
  texture[1] = 0x02;
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 768, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 1u);
  CHECK(summary.histogram[0] == 0u);
  CHECK(summary.histogram[1] == 1u);
}

TEST_CASE("A small texture is summarised over its own columns and rows",
          "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(256, 60, 0);
  SetPixel(&texture, 256, 0, 0, static_cast<uint16_t>((900 << 3) | 4));
  SetPixel(&texture, 256, 79, 59, static_cast<uint16_t>((950 << 3) | 4));
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 256, kNuiHudTextureSmall, &summary);

  CHECK(summary.pixel_count == 2u);
  CHECK(summary.centroid_column == Approx(39.5));
  CHECK(summary.centroid_row == Approx(29.5));
  CHECK(summary.histogram[3] == 2u);  // Index 4.
}

TEST_CASE("A pitch wider than the row skips the padding",
          "[nui][player_mask]") {
  // A pitch of 768 leaves 128 bytes after each 640-byte row, and they are
  // filled with a value that would be player 7 if it were read.
  std::vector<uint8_t> texture = MakeTexture(768, 240, 0);
  FillPadding(&texture, 768, kNuiHudTextureFull, 0xFF);
  SetPixel(&texture, 768, 5, 7, 3);
  SetPixel(&texture, 768, 9, 7, 3);
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 768, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 2u);
  CHECK(summary.centroid_column == Approx(7.0));
  CHECK(summary.centroid_row == Approx(7.0));
  CHECK(summary.histogram[2] == 2u);  // Index 3.
  CHECK(summary.histogram[6] == 0u);
}

TEST_CASE("A small texture's padding is skipped too", "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(256, 60, 0);
  FillPadding(&texture, 256, kNuiHudTextureSmall, 0xFF);
  SetPixel(&texture, 256, 10, 10, 1);
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), 256, kNuiHudTextureSmall, &summary);

  CHECK(summary.pixel_count == 1u);
  CHECK(summary.histogram[0] == 1u);
  CHECK(summary.histogram[6] == 0u);
}

TEST_CASE("A texture with no padding after its last row is read whole",
          "[nui][player_mask]") {
  // The buffer ends exactly where the summary's reads end, so a read past it
  // is undefined behaviour that a sanitizer build would catch.
  constexpr uint32_t kPitch = 768;
  std::vector<uint8_t> texture(
      static_cast<size_t>(NuiHudTextureBytesRead(kPitch, kNuiHudTextureFull)),
      0);
  SetPixel(&texture, kPitch, 319, 239, 6);
  NuiPlayerMaskSummary summary;

  SummariseNuiPlayerMask(texture.data(), kPitch, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 1u);
  CHECK(summary.centroid_column == Approx(319.0));
  CHECK(summary.centroid_row == Approx(239.0));
  CHECK(summary.histogram[5] == 1u);  // Index 6.
}

TEST_CASE("A blank depth texture has no non-zero pixels",
          "[nui][player_mask]") {
  const std::vector<uint8_t> texture = MakeTexture(256, 60, 0);
  NuiDepthSummary summary;
  summary.pixel_count = 99;
  summary.mean = 99.0;

  SummariseNuiDepthTexture(texture.data(), 256, kNuiHudTextureSmall, &summary);

  CHECK(summary.pixel_count == 0u);
  CHECK(summary.mean == Approx(0.0));
}

TEST_CASE("A depth texture's mean covers the non-zero values only",
          "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(768, 240, 0);
  SetPixel(&texture, 768, 1, 1, 1000);
  SetPixel(&texture, 768, 2, 2, 2000);
  SetPixel(&texture, 768, 300, 200, 3000);
  NuiDepthSummary summary;

  SummariseNuiDepthTexture(texture.data(), 768, kNuiHudTextureFull, &summary);

  CHECK(summary.pixel_count == 3u);
  CHECK(summary.mean == Approx(2000.0));
}

TEST_CASE("A small depth texture's padding stays out of its mean",
          "[nui][player_mask]") {
  std::vector<uint8_t> texture = MakeTexture(256, 60, 0);
  FillPadding(&texture, 256, kNuiHudTextureSmall, 0xFF);
  SetPixel(&texture, 256, 0, 0, 10);
  SetPixel(&texture, 256, 79, 59, 30);
  NuiDepthSummary summary;

  SummariseNuiDepthTexture(texture.data(), 256, kNuiHudTextureSmall, &summary);

  CHECK(summary.pixel_count == 2u);
  CHECK(summary.mean == Approx(20.0));
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
