/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/nui_depth_preview_dialog.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

#include "third_party/imgui/imgui.h"
#include "xenia/app/nui_app_helpers.h"
#include "xenia/base/string_util.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/nui_camera.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/nui/depth_frame.h"
#include "xenia/nui/nui_system.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

// The range of the preview's grey ramp. Anything closer than this is fully
// bright, anything beyond it is as dark as a reading ever gets, which still
// leaves it apart from the black used for "no reading at all".
static constexpr int32_t kPreviewNearMillimetres = 500;
static constexpr int32_t kPreviewRangeMillimetres = 4000;
static constexpr int32_t kPreviewShadeRange = 223;

NuiDepthPreviewDialog::NuiDepthPreviewDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                                             Emulator* emulator)
    : xe::ui::ImGuiDialog(imgui_drawer), emulator_(emulator) {}

NuiDepthPreviewDialog::~NuiDepthPreviewDialog() = default;

void NuiDepthPreviewDialog::ReleaseTextures() {
  previous_texture_.reset();
  texture_.reset();
  texture_drawer_ = nullptr;
  texture_sequence_ = 0;
}

void NuiDepthPreviewDialog::OnDraw(ImGuiIO& io) {
  // The texture the previous draw referenced has been rendered by now, so it
  // can go.
  previous_texture_.reset();

  ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Kinect Depth", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNavInputs)) {
    ImGui::End();
    return;
  }

  nui::NuiSystem* nui_system = emulator_ ? emulator_->nui_system() : nullptr;
  if (!nui_system) {
    ImGui::TextUnformatted("No NUI system is running.");
    ReleaseTextures();
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted(
      fmt::format("Backend: {}", nui_system->name()).c_str());
  ImGui::TextUnformatted(
      fmt::format("Status: {}",
                  DescribeNuiDeviceStatus(nui_system->device_status()))
          .c_str());
  ImGui::TextUnformatted(
      fmt::format("Frame: {}", nui_system->latest_sequence()).c_str());
  // Only shown, so read without asking the backend for its tilt thread: this
  // overlay is no title's tilt call.
  int32_t tilt_degrees = 0;
  if (nui_system->PeekTiltDegrees(&tilt_degrees)) {
    ImGui::TextUnformatted(
        fmt::format("Tilt: {} degrees", tilt_degrees).c_str());
  } else {
    ImGui::TextUnformatted("Tilt: unavailable");
  }

  // The camera transport and the title's own tracker both belong to the
  // kernel, which only exists while a title is loaded.
  kernel::xam::NuiState* nui_state = GetNuiState(emulator_);
  if (nui_state) {
    DrawCameraStats(*nui_state);
    DrawTrackerStats(*nui_state);
  } else {
    ImGui::TextUnformatted("Guest: no title");
  }

  UpdatePreviewTexture(nui_system);
  if (texture_) {
    ImGui::Image(reinterpret_cast<ImTextureID>(texture_.get()),
                 ImVec2(static_cast<float>(kPreviewWidth),
                        static_cast<float>(kPreviewHeight)));
  }

  ImGui::End();
}

void NuiDepthPreviewDialog::DrawCameraStats(kernel::xam::NuiState& nui_state) {
  const kernel::xam::NuiCameraStats stats = nui_state.GetCameraStats();
  ImGui::TextUnformatted(fmt::format("Guest: {}, streams {:X}, keep-alive {}",
                                     stats.streaming ? "streaming" : "idle",
                                     stats.stream_flags,
                                     stats.keep_alive ? "held" : "released")
                             .c_str());
  ImGui::TextUnformatted(fmt::format("Queued: {} depth, {} colour",
                                     stats.pending_depth_requests,
                                     stats.pending_colour_requests)
                             .c_str());
  ImGui::TextUnformatted(fmt::format("Fed: {} depth ({} dropped), {} colour",
                                     stats.depth_frames_fed,
                                     stats.depth_frames_dropped,
                                     stats.colour_frames_fed)
                             .c_str());
}

void NuiDepthPreviewDialog::DrawTrackerStats(kernel::xam::NuiState& nui_state) {
  // The title's tracker runs inside the title; all xenia sees of it is the
  // HUD skeleton frame the title hands over once per frame it processes, and
  // this is what the last one said.
  const kernel::xam::NuiHudSkeletonFrame frame =
      nui_state.GetHudSkeletonFrame();
  if (!frame.frames) {
    ImGui::TextUnformatted("Tracker: no skeleton frame yet");
    return;
  }

  // Tracking state per slot is 0 not tracked, 1 position only, 2 tracked, and
  // a slot that is anything but 0 carries the id the tracker gave whoever is
  // in it.
  std::string states;
  std::string ids;
  for (size_t i = 0; i < kernel::xam::kNuiSkeletonCount; i++) {
    fmt::format_to(std::back_inserter(states), "{}{}", i ? " " : "",
                   frame.tracking_states[i]);
    fmt::format_to(std::back_inserter(ids), "{}{}", i ? " " : "",
                   frame.tracking_ids[i]);
  }
  ImGui::TextUnformatted(
      fmt::format("Tracker: states [{}], ids [{}], engaged {} {}, floor "
                  "{:.2f} {:.2f} {:.2f} {:.2f}",
                  states, ids, frame.engaged_tracking_ids[0],
                  frame.engaged_tracking_ids[1], frame.floor_clip_plane[0],
                  frame.floor_clip_plane[1], frame.floor_clip_plane[2],
                  frame.floor_clip_plane[3])
          .c_str());
}

void NuiDepthPreviewDialog::UpdatePreviewTexture(nui::NuiSystem* nui_system) {
  if (!nui_system->IsDevicePresent()) {
    // Without a sensor there is nothing to show but the status text, and a
    // stale image would only be misleading.
    ReleaseTextures();
    return;
  }

  xe::ui::ImmediateDrawer* immediate_drawer =
      imgui_drawer()->immediate_drawer();
  if (!immediate_drawer) {
    ReleaseTextures();
    return;
  }
  if (immediate_drawer != texture_drawer_) {
    // The old textures belong to a drawer that is no longer in use.
    ReleaseTextures();
    texture_drawer_ = immediate_drawer;
  }

  const uint64_t sequence = nui_system->latest_sequence();
  if (sequence == texture_sequence_ && texture_) {
    return;
  }
  std::shared_ptr<const nui::DepthFrame> frame =
      nui_system->AcquireLatestDepthFrame();
  if (!frame || frame->width != nui::DepthFrame::kWidth ||
      frame->height != nui::DepthFrame::kHeight ||
      frame->depth_mm.size() <
          size_t(nui::DepthFrame::kWidth) * nui::DepthFrame::kHeight) {
    return;
  }

  preview_pixels_.resize(size_t(kPreviewWidth) * kPreviewHeight * 4);
  uint8_t* target = preview_pixels_.data();
  for (uint32_t y = 0; y < kPreviewHeight; ++y) {
    const uint16_t* source_row =
        frame->depth_mm.data() + size_t(y) * 2 * nui::DepthFrame::kWidth;
    for (uint32_t x = 0; x < kPreviewWidth; ++x) {
      const uint16_t millimetres = source_row[size_t(x) * 2];
      uint8_t shade = 0;
      if (millimetres) {
        const int32_t distance =
            (int32_t(millimetres) - kPreviewNearMillimetres) *
            kPreviewShadeRange / kPreviewRangeMillimetres;
        shade = static_cast<uint8_t>(
            255 - std::clamp(distance, 0, kPreviewShadeRange));
      }
      target[0] = shade;
      target[1] = shade;
      target[2] = shade;
      target[3] = 255;
      target += 4;
    }
  }

  // Textures are immutable here, so a new frame means a new texture. The one
  // it replaces is still referenced by the draw list of the previous frame.
  previous_texture_ = std::move(texture_);
  texture_ = immediate_drawer->CreateTexture(
      kPreviewWidth, kPreviewHeight, xe::ui::ImmediateTextureFilter::kNearest,
      false, preview_pixels_.data());
  texture_sequence_ = sequence;
}

}  // namespace app
}  // namespace xe
