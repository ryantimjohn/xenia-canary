/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_NUI_DEPTH_PREVIEW_DIALOG_H_
#define XENIA_APP_NUI_DEPTH_PREVIEW_DIALOG_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/immediate_drawer.h"

namespace xe {

class Emulator;

namespace kernel {
namespace xam {
class NuiState;
}  // namespace xam
}  // namespace kernel

namespace nui {
class NuiSystem;
}  // namespace nui

namespace app {

// An overlay that shows what the NUI (Kinect) backend is feeding the guest:
// the backend's name, the sensor's state and tilt, the depth frames
// themselves and what the title's own tracker is making of them. Enabled with
// the nui_show_depth_preview cvar, and happy to run with no sensor - then it
// just says so.
class NuiDepthPreviewDialog final : public xe::ui::ImGuiDialog {
 public:
  // Half of a depth frame in each direction, which is enough to see the room
  // and cheap enough to rebuild every time a frame arrives.
  static constexpr uint32_t kPreviewWidth = 320;
  static constexpr uint32_t kPreviewHeight = 240;

  NuiDepthPreviewDialog(xe::ui::ImGuiDrawer* imgui_drawer, Emulator* emulator);
  ~NuiDepthPreviewDialog() override;

  // Drops the preview textures. Must be called before the immediate drawer
  // that created them goes away, since it owns their GPU resources.
  void ReleaseTextures();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Shows what the guest side of the camera is doing: the keep-alive, the
  // frame requests it has queued and the frames that have been fed to them.
  void DrawCameraStats(kernel::xam::NuiState& nui_state);

  // Shows what the title's own skeletal tracker is finding, as of the last
  // HUD skeleton frame it handed over.
  void DrawTrackerStats(kernel::xam::NuiState& nui_state);

  // Rebuilds the preview texture when the backend has a frame we have not
  // shown yet.
  void UpdatePreviewTexture(nui::NuiSystem* nui_system);

  Emulator* emulator_ = nullptr;

  // The drawer the current textures came from, so that they can be dropped if
  // it is ever swapped out from under us.
  xe::ui::ImmediateDrawer* texture_drawer_ = nullptr;
  std::unique_ptr<xe::ui::ImmediateTexture> texture_;
  // The texture the previous draw referenced. ImGui only renders its draw
  // lists after every dialog has been drawn, so it stays alive until we are
  // asked to draw again.
  std::unique_ptr<xe::ui::ImmediateTexture> previous_texture_;
  // Sequence number of the frame the current texture was built from.
  uint64_t texture_sequence_ = 0;
  // Scratch space for the R8G8B8A8 image, kept around between frames.
  std::vector<uint8_t> preview_pixels_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_NUI_DEPTH_PREVIEW_DIALOG_H_
