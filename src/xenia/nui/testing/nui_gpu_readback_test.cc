/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_gpu_readback.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

TEST_CASE("The tracker turns both read-backs on from their defaults",
          "[nui][gpu_readback]") {
  // readback_resolve is "none" and readback_memexport is off, which is how
  // both are compiled, so nobody has chosen either of them.
  constexpr GpuReadbackPlan plan = PlanSkeletalTrackerGpuReadback(true, false);
  CHECK(plan.enable_resolve);
  CHECK(plan.enable_memexport);
  CHECK_FALSE(plan.resolve_configured);
  CHECK_FALSE(plan.memexport_configured);
  CHECK(plan.enables_resolve_and_memexport());
}

TEST_CASE("A configured resolve read-back is left alone",
          "[nui][gpu_readback]") {
  // readback_resolve is "fast" or "full" - either way not its compiled
  // default, so it was chosen and the tracker does not raise it.
  constexpr GpuReadbackPlan plan = PlanSkeletalTrackerGpuReadback(false, false);
  CHECK_FALSE(plan.enable_resolve);
  CHECK(plan.resolve_configured);
  CHECK(plan.enable_memexport);
  CHECK_FALSE(plan.memexport_configured);
  CHECK_FALSE(plan.enables_resolve_and_memexport());
}

TEST_CASE("A memexport read-back that is already on is left alone",
          "[nui][gpu_readback]") {
  constexpr GpuReadbackPlan plan = PlanSkeletalTrackerGpuReadback(true, true);
  CHECK(plan.enable_resolve);
  CHECK_FALSE(plan.resolve_configured);
  CHECK_FALSE(plan.enable_memexport);
  CHECK(plan.memexport_configured);
  CHECK_FALSE(plan.enables_resolve_and_memexport());
}

TEST_CASE("Nothing is changed when both are already configured",
          "[nui][gpu_readback]") {
  constexpr GpuReadbackPlan plan = PlanSkeletalTrackerGpuReadback(false, true);
  CHECK_FALSE(plan.enable_resolve);
  CHECK_FALSE(plan.enable_memexport);
  CHECK(plan.resolve_configured);
  CHECK(plan.memexport_configured);
}

}  // namespace test
}  // namespace nui
}  // namespace xe
