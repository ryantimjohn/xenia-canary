/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_NUI_GUIDE_DIALOG_H_
#define XENIA_APP_NUI_GUIDE_DIALOG_H_

#include "xenia/ui/imgui_dialog.h"

namespace xe {

class Emulator;

namespace app {

// The Kinect Guide's overlay, drawn while xam's guide is up
// (kernel::xam::NuiState::guide_open). It shows the sensor and the player the
// guide took, a cursor where that player's raised hand points, and one action,
// "Return to game", which the player chooses by holding the cursor over it for
// kHoverSeconds. Escape closes the guide too. There are no dashboard, settings
// or sign-in functions.
//
// EmulatorWindow makes the overlay when the guide opens and drops it when the
// guide closes, rather than keeping one registered for the whole session:
// while any dialog is registered, the window's ImGui layer listens to input,
// polls the gamepads and repaints every frame, and a title that never opens
// the guide must not pay for that.
class NuiGuideDialog final : public xe::ui::ImGuiDialog {
 public:
  // The cursor has to rest over "Return to game" for this long.
  static constexpr float kHoverSeconds = 2.0f;

  NuiGuideDialog(xe::ui::ImGuiDrawer* imgui_drawer, Emulator* emulator);
  ~NuiGuideDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  Emulator* emulator_ = nullptr;
  // How long the cursor has rested over "Return to game" without leaving it.
  float hover_seconds_ = 0.0f;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_NUI_GUIDE_DIALOG_H_
