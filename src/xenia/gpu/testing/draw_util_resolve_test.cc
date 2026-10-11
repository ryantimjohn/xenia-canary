/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {
namespace test {

namespace {

// Register state observed in 0x4D5308ED's 160x120 occupancy-mask resolve.
// Fetch 0 is not a vertex buffer, so GetResolveInfo reads no guest memory.
void SetUpOneBitPlaneResolveRegisters(RegisterFile& regs) {
  regs[XE_GPU_REG_RB_COPY_CONTROL] =
      uint32_t(xenos::CopyCommand::kConvertTo1111) << 20;
  regs[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x150A0000;
  // Destination pitch and height count 4x4 blocks.
  regs[XE_GPU_REG_RB_COPY_DEST_PITCH] = 64 | (30 << 16);
  regs[XE_GPU_REG_RB_COPY_DEST_INFO] =
      1 | (uint32_t(xenos::TextureFormat::k_DXT3A_AS_1_1_1_1) << 7);
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 160;
  regs[XE_GPU_REG_RB_COLOR_INFO] = 0;
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 0;
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 160 | (120 << 16);
  regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL] = 0;
  regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 8192 | (8192 << 16);
  regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = 0;
  regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0;
  regs[XE_GPU_REG_RB_COPY_FUNC] = 0x00001111;
  regs[XE_GPU_REG_RB_COPY_REF] = 0x80808080;
  regs[XE_GPU_REG_RB_COPY_MASK] = 0xFFFFFFFF;
}

bool GetResolveInfoForTest(const RegisterFile& regs,
                           draw_util::ResolveInfo& info_out) {
  Memory memory;
  TraceWriter trace_writer(nullptr);
  return draw_util::GetResolveInfo(regs, memory, trace_writer, 1, 1, false,
                                   false, info_out);
}

}  // namespace

TEST_CASE("An inferred command-2 resolve converts destination blocks to pixels",
          "[gpu][resolve]") {
  RegisterFile regs;
  SetUpOneBitPlaneResolveRegisters(regs);
  draw_util::ResolveInfo info;
  REQUIRE(GetResolveInfoForTest(regs, info));

  CHECK(info.copy_width_blocks == 40);
  CHECK(info.copy_height_blocks == 30);
  CHECK(info.copy_dest_coordinate_info.pitch_aligned_div_32 == 2);
  CHECK(info.coordinate_info.width_div_8 == 20);
  CHECK(info.height_div_8 == 15);
  CHECK(info.copy_dest_base == 0x150A0000);
  CHECK(info.copy_dest_extent_start == 0x150A0000);
  CHECK(info.copy_dest_extent_length == 16384);

  // The existing upstream shader and constant layout carry the compare.
  draw_util::ResolveCopyShaderConstants constants;
  uint32_t group_count_x, group_count_y;
  CHECK(info.GetCopyShader(1, 1, constants, group_count_x, group_count_y) ==
        draw_util::ResolveCopyShaderIndex::kDXT3AAs1111);
  CHECK(constants.copy_func == 0x00001111);
  CHECK(constants.copy_ref == 0x80808080);
  CHECK(constants.copy_mask == 0xFFFFFFFF);
  CHECK(constants.copy_size_blocks == (40 | (30 << 16)));
  // Each upstream group covers 256x8 source pixels.
  CHECK(group_count_x == 1);
  CHECK(group_count_y == 15);
}

TEST_CASE("Inferred command-2 resolves retain partial blocks without padding",
          "[gpu][resolve]") {
  struct SeamCase {
    uint32_t width, height, blocks_x, blocks_y, width_div_8, height_div_8;
  };
  // Odd block columns/rows must survive while the 8-pixel dispatch padding
  // must not write another block. These include classifier pyramid levels
  // observed in 0x4D5308ED and a rectangle smaller than one block.
  static constexpr SeamCase kSeamCases[] = {
      {2, 2, 1, 1, 1, 1},
      {80, 30, 20, 8, 10, 4},
      {40, 15, 10, 4, 5, 2},
      {10, 4, 3, 1, 2, 1},
  };
  for (const SeamCase& seam : kSeamCases) {
    INFO("scissor " << seam.width << "x" << seam.height);
    RegisterFile regs;
    SetUpOneBitPlaneResolveRegisters(regs);
    regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = seam.width | (seam.height << 16);
    draw_util::ResolveInfo info;
    REQUIRE(GetResolveInfoForTest(regs, info));
    CHECK(info.copy_width_blocks == seam.blocks_x);
    CHECK(info.copy_height_blocks == seam.blocks_y);
    CHECK(info.coordinate_info.width_div_8 == seam.width_div_8);
    CHECK(info.height_div_8 == seam.height_div_8);
    draw_util::ResolveCopyShaderConstants constants;
    uint32_t groups_x, groups_y;
    REQUIRE(info.GetCopyShader(1, 1, constants, groups_x, groups_y) ==
            draw_util::ResolveCopyShaderIndex::kDXT3AAs1111);
    CHECK(constants.copy_size_blocks ==
          (seam.blocks_x | (seam.blocks_y << 16)));
  }
}

TEST_CASE("Inferred command-2 resolves obey destination and source bounds",
          "[gpu][resolve]") {
  RegisterFile regs;
  SetUpOneBitPlaneResolveRegisters(regs);
  // The declared seven block rows bound the copy even when the scissor is
  // taller. The shader must skip the eighth row used by dispatch alignment.
  regs[XE_GPU_REG_RB_COPY_DEST_PITCH] = 32 | (7 << 16);
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 80;
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 80 | (60 << 16);
  draw_util::ResolveInfo info;
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.copy_width_blocks == 20);
  CHECK(info.copy_height_blocks == 7);
  CHECK(info.coordinate_info.width_div_8 == 10);
  CHECK(info.height_div_8 == 4);

  // Source pitch alone clips the horizontal extent.
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 40;
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.copy_width_blocks == 10);
  // Destination pitch alone clips it in this case.
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 160;
  regs[XE_GPU_REG_RB_COPY_DEST_PITCH] = 8 | (7 << 16);
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.copy_width_blocks == 8);
}

TEST_CASE("Command-2 resolves preserve upstream support for offset rectangles",
          "[gpu][resolve]") {
  RegisterFile regs;
  SetUpOneBitPlaneResolveRegisters(regs);
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 8 | (8 << 16);
  draw_util::ResolveInfo info;
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.copy_width_blocks == 38);
  CHECK(info.copy_height_blocks == 28);
  CHECK(info.copy_dest_coordinate_info.offset_x_div_8 == 1);
  CHECK(info.copy_dest_coordinate_info.offset_y_div_8 == 1);
  CHECK(info.copy_dest_extent_length != 0);
}

TEST_CASE("Sub-eight-pixel origins retain resolve alignment",
          "[gpu][resolve]") {
  for (uint32_t offset = 1; offset < 8; ++offset) {
    RegisterFile regs;
    SetUpOneBitPlaneResolveRegisters(regs);
    regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = offset | (offset << 16);
    draw_util::ResolveInfo info;
    REQUIRE(GetResolveInfoForTest(regs, info));
    CHECK(info.copy_width_blocks == 40);
    CHECK(info.copy_height_blocks == 30);
    CHECK(info.copy_dest_extent_length == 16384);
  }
}

TEST_CASE("Unsupported command-2 formats and sample averaging do not write",
          "[gpu][resolve]") {
  RegisterFile regs;
  SetUpOneBitPlaneResolveRegisters(regs);
  SECTION("Destination is not one-bit planes") {
    regs[XE_GPU_REG_RB_COPY_DEST_INFO] =
        1 | (uint32_t(xenos::TextureFormat::k_8_8_8_8) << 7);
  }
  SECTION("Array destination") { regs[XE_GPU_REG_RB_COPY_DEST_INFO] |= 1 << 3; }
  SECTION("Unsupported source format") {
    regs[XE_GPU_REG_RB_COLOR_INFO] =
        uint32_t(xenos::ColorRenderTargetFormat::k_16_16) << 16;
  }
  SECTION("Sample averaging") {
    regs[XE_GPU_REG_RB_SURFACE_INFO] |= uint32_t(xenos::MsaaSamples::k2X) << 16;
    regs[XE_GPU_REG_RB_COPY_CONTROL] |= uint32_t(xenos::CopySampleSelect::k01)
                                        << 4;
  }
  draw_util::ResolveInfo info;
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.copy_dest_extent_length == 0);
}

TEST_CASE("An empty inferred scissor is not expanded into a copy",
          "[gpu][resolve]") {
  RegisterFile regs;
  SetUpOneBitPlaneResolveRegisters(regs);
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 2 | (2 << 16);
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 2 | (2 << 16);
  draw_util::ResolveInfo info;
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.coordinate_info.width_div_8 == 0);
  CHECK(info.height_div_8 == 0);
}

TEST_CASE("An ordinary inferred resolve keeps pixel destination units",
          "[gpu][resolve]") {
  RegisterFile regs;
  SetUpOneBitPlaneResolveRegisters(regs);
  regs[XE_GPU_REG_RB_COPY_CONTROL] = uint32_t(xenos::CopyCommand::kConvert)
                                     << 20;
  regs[XE_GPU_REG_RB_COPY_DEST_INFO] =
      1 | (uint32_t(xenos::TextureFormat::k_8_8_8_8) << 7);
  draw_util::ResolveInfo info;
  REQUIRE(GetResolveInfoForTest(regs, info));
  CHECK(info.coordinate_info.width_div_8 == 8);
  CHECK(info.height_div_8 == 4);
  CHECK(info.copy_width_blocks == 0);
  CHECK(info.copy_height_blocks == 0);
  CHECK(info.copy_dest_extent_length != 0);
}

}  // namespace test
}  // namespace gpu
}  // namespace xe
