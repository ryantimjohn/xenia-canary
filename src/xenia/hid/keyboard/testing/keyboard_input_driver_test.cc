/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/keyboard/keyboard_input_driver.h"

#include <cstdint>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"

DECLARE_int32(keyboard_mode);
DECLARE_int32(keyboard_user_index);

namespace xe::hid::keyboard {

// Exercise the real event producer, queue, and GetKeystroke consumer without a
// native window, OS input, controller, guest, or graphics backend.
struct KeyboardInputDriverTestAccess {
  static void Bind(KeyboardInputDriver& driver) {
    driver.key_bindings_.clear();
    driver.ParseKeyBinding(ui::VirtualKey::kXInputPadA, "test A", "0xBA");
    driver.ParseKeyBinding(ui::VirtualKey::kXInputPadDpadLeft, "test upper",
                           "^A");
    driver.ParseKeyBinding(ui::VirtualKey::kXInputPadLThumbLeft, "test lower",
                           "_A");
  }

  static void Key(KeyboardInputDriver& driver, ui::VirtualKey key, bool down,
                  bool shift = false, bool caps = false, bool repeat = false,
                  bool ctrl = false, bool alt = false) {
    ui::KeyEvent event(nullptr, key, 1, !down || repeat, shift, ctrl, alt, caps);
    driver.OnKey(event, down);
  }

  static void Char(KeyboardInputDriver& driver, ui::VirtualKey key,
                   uint16_t unicode) {
    ui::KeyEvent event(nullptr, key, 1, false, false, false, false, false);
    event.set_unicode(unicode);
    driver.OnChar(event);
  }

  static bool AnyPressed(const KeyboardInputDriver& driver) {
    for (const auto& binding : driver.key_bindings_) {
      if (binding.is_pressed) {
        return true;
      }
    }
    return false;
  }
};

namespace {

struct KeyboardModeScope {
  int32_t mode = cvars::keyboard_mode;
  int32_t user = cvars::keyboard_user_index;
  KeyboardModeScope() {
    cvars::keyboard_mode = 1;
    cvars::keyboard_user_index = 0;
  }
  ~KeyboardModeScope() {
    cvars::keyboard_mode = mode;
    cvars::keyboard_user_index = user;
  }
};

constexpr auto kPhysicalA = static_cast<ui::VirtualKey>('A');
constexpr auto kSemicolon = static_cast<ui::VirtualKey>(0xBA);

X_INPUT_KEYSTROKE ReadKey(KeyboardInputDriver& driver, ui::VirtualKey expected,
                         uint16_t flags) {
  X_INPUT_KEYSTROKE key = {};
  REQUIRE(driver.GetKeystroke(0, X_INPUT_FLAG_GAMEPAD, &key) == X_ERROR_SUCCESS);
  CHECK(uint16_t(key.virtual_key) == static_cast<uint16_t>(expected));
  CHECK(uint16_t(key.flags) == flags);
  CHECK(key.user_index == 0);
  return key;
}

void CheckEmpty(KeyboardInputDriver& driver) {
  X_INPUT_KEYSTROKE key = {};
  CHECK(driver.GetKeystroke(0, X_INPUT_FLAG_GAMEPAD, &key) == X_ERROR_EMPTY);
}

}  // namespace

TEST_CASE("A complete keyboard tap survives until the guest polls",
          "[hid][keyboard][queued-edges]") {
  KeyboardModeScope scope;
  KeyboardInputDriver driver(nullptr, 0);
  KeyboardInputDriverTestAccess::Bind(driver);
  KeyboardInputDriverTestAccess::Key(driver, kSemicolon, true);
  KeyboardInputDriverTestAccess::Key(driver, kSemicolon, false);

  // Historical edges must not keep the binding held after physical release.
  CHECK_FALSE(KeyboardInputDriverTestAccess::AnyPressed(driver));
  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  ReadKey(driver, ui::VirtualKey::kXInputPadA, X_INPUT_KEYSTROKE_KEYDOWN);
  ReadKey(driver, ui::VirtualKey::kXInputPadA, X_INPUT_KEYSTROKE_KEYUP);
  CheckEmpty(driver);
}

TEST_CASE("A keyboard release is delivered after its press was consumed",
          "[hid][keyboard][queued-edges]") {
  KeyboardModeScope scope;
  KeyboardInputDriver driver(nullptr, 0);
  KeyboardInputDriverTestAccess::Bind(driver);
  KeyboardInputDriverTestAccess::Key(driver, kSemicolon, true);
  ReadKey(driver, ui::VirtualKey::kXInputPadA, X_INPUT_KEYSTROKE_KEYDOWN);
  KeyboardInputDriverTestAccess::Key(driver, kSemicolon, false);
  ReadKey(driver, ui::VirtualKey::kXInputPadA, X_INPUT_KEYSTROKE_KEYUP);
  CheckEmpty(driver);
}

TEST_CASE("Repeats and release keep the binding selected before modifiers change",
          "[hid][keyboard][queued-edges]") {
  const bool use_caps = GENERATE(false, true);
  KeyboardModeScope scope;
  KeyboardInputDriver driver(nullptr, 0);
  KeyboardInputDriverTestAccess::Bind(driver);
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, true, !use_caps,
                                    use_caps);
  // Shift/Caps is now off, while A remains down. The repeat belongs to the
  // original upper-case binding, and key-up must release that same binding.
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, true, false, false,
                                    true);
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, false);
  CHECK_FALSE(KeyboardInputDriverTestAccess::AnyPressed(driver));
  ReadKey(driver, ui::VirtualKey::kXInputPadDpadLeft, X_INPUT_KEYSTROKE_KEYDOWN);
  ReadKey(driver, ui::VirtualKey::kXInputPadDpadLeft,
          X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT);
  ReadKey(driver, ui::VirtualKey::kXInputPadDpadLeft, X_INPUT_KEYSTROKE_KEYUP);

  // A new press takes a fresh modifier snapshot; a modified release must not
  // redirect it to the upper-case binding or leave the lower one held.
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, true);
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, false, true, true);
  CHECK_FALSE(KeyboardInputDriverTestAccess::AnyPressed(driver));
  ReadKey(driver, ui::VirtualKey::kXInputPadLThumbLeft, X_INPUT_KEYSTROKE_KEYDOWN);
  ReadKey(driver, ui::VirtualKey::kXInputPadLThumbLeft, X_INPUT_KEYSTROKE_KEYUP);
  CheckEmpty(driver);
}

TEST_CASE("A release without a selected keyboard binding creates no controller edge",
          "[hid][keyboard][queued-edges]") {
  KeyboardModeScope scope;
  KeyboardInputDriver driver(nullptr, 0);
  KeyboardInputDriverTestAccess::Bind(driver);
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, false, true);
  CheckEmpty(driver);
  CHECK_FALSE(KeyboardInputDriverTestAccess::AnyPressed(driver));
}

TEST_CASE("Keyboard passthrough retains raw keys modifiers and Unicode",
          "[hid][keyboard][queued-edges]") {
  KeyboardModeScope scope;
  cvars::keyboard_mode = 2;
  KeyboardInputDriver driver(nullptr, 0);
  KeyboardInputDriverTestAccess::Bind(driver);
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, true, true, true, false,
                                    true, true);
  KeyboardInputDriverTestAccess::Char(driver, kPhysicalA, 'A');
  KeyboardInputDriverTestAccess::Key(driver, kPhysicalA, false);

  auto down = ReadKey(driver, kPhysicalA,
                      X_INPUT_KEYSTROKE_KEYDOWN | 0x0008 | 0x0010 | 0x0020 |
                          0x1000);
  CHECK(uint16_t(down.unicode) == 'A');
  CHECK(down.hid_code == 0x04);
  auto up = ReadKey(driver, kPhysicalA, X_INPUT_KEYSTROKE_KEYUP);
  CHECK(uint16_t(up.unicode) == 0);
  CHECK(up.hid_code == 0x04);
  CheckEmpty(driver);
}

}  // namespace xe::hid::keyboard
