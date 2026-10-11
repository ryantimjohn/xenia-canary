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
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace test {

namespace {

// IsRasterizationPotentiallyDone reads four registers and the caller's answer
// to "does the pixel shader this draw would use write to memory". Only the
// registers are set up here; the shader is a bool the caller supplies, which
// is the whole point of the contract in draw_util.h - every caller must
// supply the same value.
void SetUpRasterizedDrawRegisters(RegisterFile& regs) {
  // A surface to rasterize into. Zero would mean no draw at all.
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 640;
  // Colour and depth, the ordinary mode.
  regs[XE_GPU_REG_RB_MODECONTROL] = uint32_t(xenos::EdramMode::kColorDepth);
  // Vertex shader exports one position vector - the frame buffer export is
  // not suppressed.
  regs[XE_GPU_REG_SQ_PROGRAM_CNTL] =
      uint32_t(xenos::VertexShaderExportMode::kPosition1Vector) << 24;
  // Neither face culled.
  regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0;
}

void SetEdramMode(RegisterFile& regs, xenos::EdramMode mode) {
  regs[XE_GPU_REG_RB_MODECONTROL] = uint32_t(mode);
}

void SetFrameBufferExportSuppressed(RegisterFile& regs) {
  regs[XE_GPU_REG_SQ_PROGRAM_CNTL] =
      uint32_t(xenos::VertexShaderExportMode::kMultipass) << 24;
}

}  // namespace

TEST_CASE("An ordinary draw is rasterized whatever its pixel shader exports",
          "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);

  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
  // Not a polygon, so the face culling bits are not consulted.
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, false, false));
}

TEST_CASE("A draw into no EDRAM surface is not rasterized", "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 0;

  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  // A memory-exporting pixel shader does not resurrect it either - without a
  // surface there are no samples to run it on.
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
}

TEST_CASE("A polygonal draw with both faces culled is not rasterized",
          "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  // cull_front at bit 0, cull_back at bit 1.
  regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0x3;

  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
  // The bits describe polygon faces, so a non-polygonal primitive ignores
  // them.
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, false, true));
  // One face alone is not enough to drop the draw.
  regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0x1;
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
}

TEST_CASE("Only the two EDRAM modes that touch the surface rasterize",
          "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);

  SetEdramMode(regs, xenos::EdramMode::kColorDepth);
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  SetEdramMode(regs, xenos::EdramMode::kDepthOnly);
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, false));

  // Neither of these writes to the EDRAM surface at all, so no sample of the
  // draw is taken - with or without a memory-exporting pixel shader.
  SetEdramMode(regs, xenos::EdramMode::kNoOperation);
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
  SetEdramMode(regs, xenos::EdramMode::kCopy);
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
}

TEST_CASE(
    "A draw whose frame buffer export is suppressed is rasterized only for a "
    "memory-exporting pixel shader",
    "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  SetFrameBufferExportSuppressed(regs);
  REQUIRE(draw_util::IsFrameBufferExportSuppressed(regs));

  // Nothing observable would be left of it, so it can be dropped entirely.
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  // The memory it writes is the only thing the guest will read of it, so the
  // draw is kept and the pixel shader runs.
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
}

TEST_CASE(
    "A suppressed-export draw in the depth-only mode is not rasterized when "
    "its pixel shader is reported as not exporting",
    "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  SetFrameBufferExportSuppressed(regs);
  SetEdramMode(regs, xenos::EdramMode::kDepthOnly);

  // The pixel shader is never used with kDepthOnly - see xenos::EdramMode -
  // so it cannot export anything, and the caller that knows this must pass
  // false. The command processors do: they compute the flag with the same
  // EDRAM mode condition they then bind the pixel shader by, so that this
  // function, the pipeline cache and they themselves reach one answer.
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
}

TEST_CASE(
    "A suppressed-export draw in the depth-only mode is rasterized when the "
    "caller reports a memory-exporting pixel shader",
    "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  SetFrameBufferExportSuppressed(regs);
  SetEdramMode(regs, xenos::EdramMode::kDepthOnly);

  // Passed the value a caller that ignored the EDRAM mode would supply, the
  // function says the draw is rasterized - and that is the disagreement the
  // contract in draw_util.h forbids, because the pipeline setup would then be
  // asked for a pipeline with the pixel shader disabled. The function takes
  // the flag as it is given; the mode condition is the caller's to apply.
  CHECK(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
}

TEST_CASE("Suppressed framebuffer exports leave color and depth untouched",
          "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  regs[XE_GPU_REG_RB_COLOR_MASK] = 0xFFFF;
  // Enable depth testing/writes and stencil testing.
  regs[XE_GPU_REG_RB_DEPTHCONTROL] = 0x7;
  CHECK(draw_util::GetNormalizedColorMask(regs, 0xF) != 0);
  CHECK(draw_util::GetNormalizedDepthControl(regs).value != 0);
  SetFrameBufferExportSuppressed(regs);
  CHECK(draw_util::GetNormalizedColorMask(regs, 0xF) == 0);
  CHECK(draw_util::GetNormalizedDepthControl(regs).value == 0);
}

TEST_CASE("Post-hi-Z kills without a VIZ ID still suppress rasterization",
          "[gpu][draw]") {
  RegisterFile regs;
  SetUpRasterizedDrawRegisters(regs);
  auto viz_query = regs.Get<reg::PA_SC_VIZ_QUERY>();
  viz_query.kill_pix_post_hi_z = 1;
  viz_query.viz_query_ena = 0;
  regs[XE_GPU_REG_PA_SC_VIZ_QUERY] = viz_query.value;
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, false));
  CHECK_FALSE(draw_util::IsRasterizationPotentiallyDone(regs, true, true));
}

}  // namespace test
}  // namespace gpu
}  // namespace xe
