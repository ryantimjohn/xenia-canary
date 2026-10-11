/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/nui_guide_dialog.h"

#include <algorithm>
#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "xenia/app/nui_app_helpers.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/nui/nui_system.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

namespace {

// The action's box, in the overlay's own units.
constexpr float kActionWidth = 260.0f;
constexpr float kActionHeight = 52.0f;
constexpr float kActionRounding = 6.0f;

// The hand cursor's size.
constexpr float kCursorRadius = 14.0f;
constexpr float kCursorOutline = 3.0f;

constexpr char kActionLabel[] = "Return to game";
constexpr char kInstructions[] =
    "Hold your hand over Return to game, press Escape, or repeat the gesture";

}  // namespace

NuiGuideDialog::NuiGuideDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                               Emulator* emulator)
    : xe::ui::ImGuiDialog(imgui_drawer), emulator_(emulator) {}

NuiGuideDialog::~NuiGuideDialog() = default;

void NuiGuideDialog::OnDraw(ImGuiIO& io) {
  kernel::xam::NuiState* state = GetNuiState(emulator_);
  // The window drops the overlay once it hears that the guide closed; until
  // then there is nothing to draw.
  if (!state || !state->guide_open()) {
    hover_seconds_ = 0.0f;
    return;
  }

  // Escape is the one key the guide answers. While this overlay exists,
  // EmulatorWindow lets Escape through rather than leaving fullscreen with it,
  // so the press arrives here and closes the guide without leaving fullscreen.
  // The drawer marks no key press handled, so key presses, Escape included,
  // still reach the emulator's keyboard input as well.
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    hover_seconds_ = 0.0f;
    state->CloseGuide("escape");
    return;
  }

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Kinect Guide", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoNavInputs)) {
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted("Kinect Guide - the game is paused");
  ImGui::Separator();
  nui::NuiSystem* nui_system = emulator_ ? emulator_->nui_system() : nullptr;
  if (nui_system) {
    ImGui::TextUnformatted(
        fmt::format("Sensor: {}, {}", nui_system->name(),
                    DescribeNuiDeviceStatus(nui_system->device_status()))
            .c_str());
  } else {
    ImGui::TextUnformatted("Sensor: none");
  }
  ImGui::TextUnformatted(
      fmt::format("Tracking id: {}", state->guide_tracking_id()).c_str());
  ImGui::Spacing();

  // The action is drawn by hand rather than as an ImGui button: the hand
  // chooses it, and a mouse click is not one of the ways the guide closes.
  ImGui::SetCursorPosX(std::max(
      ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - kActionWidth) * 0.5f));
  ImGui::Dummy(ImVec2(kActionWidth, kActionHeight));
  const ImVec2 action_min = ImGui::GetItemRectMin();
  const ImVec2 action_max = ImGui::GetItemRectMax();
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(action_min, action_max,
                           ImGui::GetColorU32(ImGuiCol_Button),
                           kActionRounding);
  const float fill = std::clamp(hover_seconds_ / kHoverSeconds, 0.0f, 1.0f);
  if (fill > 0.0f) {
    draw_list->AddRectFilled(
        action_min,
        ImVec2(action_min.x + (action_max.x - action_min.x) * fill,
               action_max.y),
        ImGui::GetColorU32(ImGuiCol_ButtonActive), kActionRounding);
  }
  draw_list->AddRect(action_min, action_max,
                     ImGui::GetColorU32(ImGuiCol_Border), kActionRounding);
  const ImVec2 label_size = ImGui::CalcTextSize(kActionLabel);
  draw_list->AddText(
      ImVec2(action_min.x + (kActionWidth - label_size.x) * 0.5f,
             action_min.y + (kActionHeight - label_size.y) * 0.5f),
      ImGui::GetColorU32(ImGuiCol_Text), kActionLabel);

  ImGui::Spacing();
  ImGui::TextUnformatted(kInstructions);

  // The hand's box maps onto the whole overlay, so the cursor never leaves it.
  bool over_action = false;
  float hand_x = 0.0f;
  float hand_y = 0.0f;
  if (state->GuideHandPosition(&hand_x, &hand_y)) {
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2 cursor(window_pos.x + hand_x * window_size.x,
                        window_pos.y + hand_y * window_size.y);
    over_action = cursor.x >= action_min.x && cursor.x <= action_max.x &&
                  cursor.y >= action_min.y && cursor.y <= action_max.y;
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    foreground->AddCircleFilled(cursor, kCursorRadius,
                                IM_COL32(255, 255, 255, 200));
    foreground->AddCircle(cursor, kCursorRadius, IM_COL32(40, 120, 220, 255), 0,
                          kCursorOutline);
  }
  ImGui::End();

  // The cursor has to rest over the action without a break.
  hover_seconds_ = over_action ? hover_seconds_ + io.DeltaTime : 0.0f;
  if (hover_seconds_ >= kHoverSeconds) {
    hover_seconds_ = 0.0f;
    state->CloseGuide("hover");
  }
}

}  // namespace app
}  // namespace xe
