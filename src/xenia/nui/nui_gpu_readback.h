/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_GPU_READBACK_H_
#define XENIA_NUI_NUI_GPU_READBACK_H_

namespace xe {
namespace nui {

// What to do with xenia's two GPU read-back settings when a title registers
// its skeletal-tracking runtime. The tracker classifies the depth image on the
// GPU and then reads the results back on the CPU, so both settings have to be
// on for it: its joint proposals arrive through resolve read-back and its HUD
// images through memory-export read-back, and with either at its compiled
// default the bytes it locks and reads are the ones its own allocation left
// there.
struct GpuReadbackPlan {
  // Raise readback_resolve from "none" to "full".
  bool enable_resolve = false;
  // Turn readback_memexport on.
  bool enable_memexport = false;
  // The setting holds something other than its compiled default, so somebody
  // chose it and it is left exactly as it is.
  bool resolve_configured = false;
  bool memexport_configured = false;

  bool enables_resolve_and_memexport() const {
    return enable_resolve && enable_memexport;
  }
};

// Decides that plan from the settings as they stand:
// resolve_readback_disabled is whether readback_resolve is still "none", and
// memexport_readback_enabled whether readback_memexport is already on.
//
// Neither cvar records where its value came from. The config file is written
// out complete, every value in it, so finding a value there proves only that
// xenia wrote the file - which makes "the value still equals the compiled
// default" the only practical test for "nobody chose this", and it is the test
// used here. Its limitation is that it cannot see somebody who asked for the
// default on purpose: a command line that says readback_resolve=none reads
// exactly like a command line that says nothing, and the tracker's setting is
// raised over it.
constexpr GpuReadbackPlan PlanSkeletalTrackerGpuReadback(
    bool resolve_readback_disabled, bool memexport_readback_enabled) {
  GpuReadbackPlan plan;
  plan.enable_resolve = resolve_readback_disabled;
  plan.resolve_configured = !resolve_readback_disabled;
  plan.enable_memexport = !memexport_readback_enabled;
  plan.memexport_configured = memexport_readback_enabled;
  return plan;
}

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_GPU_READBACK_H_
