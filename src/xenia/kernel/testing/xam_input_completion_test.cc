// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; inherited SDK-derived ancestry remains fork-only.
#include "xenia/kernel/xam/input_observe.h"

#include <array>
#include <cstring>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/util/shim_utils.h"

// The real exports retain these exact typed signatures. Exercise their actual
// production trampolines on early routes that require no emulator fixture.
namespace xe::kernel::xam {
dword_result_t XamInputGetState_entry(dword_t, dword_t,
                                      pointer_t<hid::X_INPUT_STATE>);
dword_result_t XamInputGetKeystroke_entry(dword_t, dword_t,
                                          pointer_t<hid::X_INPUT_KEYSTROKE>);
dword_result_t XamInputGetKeystrokeEx_entry(lpdword_t, dword_t,
                                            pointer_t<hid::X_INPUT_KEYSTROKE>);
}  // namespace xe::kernel::xam

namespace xe::kernel::xam::input_observe::test {
namespace {
struct Driver {
  uint32_t state_calls = 0, stroke_calls = 0;
  uint32_t stroke_result = X_ERROR_EMPTY;
  uint32_t GetState(uint32_t user, uint32_t flags, hid::X_INPUT_STATE* out) {
    ++state_calls;
    CHECK(user == 0);
    CHECK(flags == hid::X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD);
    if (out) {
      out->packet_number = 17;
      out->gamepad.buttons = 0x1000;
    }
    return X_ERROR_SUCCESS;
  }
  uint32_t GetKeystroke(uint32_t, uint32_t, hid::X_INPUT_KEYSTROKE* out) {
    ++stroke_calls;
    if (stroke_result == X_ERROR_SUCCESS) {
      out->virtual_key = 0x5800;
      out->flags = 2;
      out->user_index = 1;
    }
    return stroke_result;
  }
};
}  // namespace

TEST_CASE("Input UI gates preserve output writes and skip driver/user access",
          "[input-completion]") {
  Driver driver;
  uint32_t ui_queries = 0, driver_entries = 0;
  auto ui = [&] {
    ++ui_queries;
    return true;
  };
  auto with_driver = [&](auto&& operation) {
    ++driver_entries;
    return operation(driver);
  };
  hid::X_INPUT_STATE state;
  std::memset(&state, 0xA5, sizeof(state));
  Completion c;
  CHECK(GetState(0, 0, &state, ui, with_driver, c) == X_ERROR_SUCCESS);
  Snapshot(c, &state);
  CHECK(c.route == Route::kUI);
  CHECK(c.output_valid);
  CHECK_FALSE(c.a);
  CHECK(state.packet_number == 0);
  CHECK(ui_queries == 1);
  CHECK(driver_entries == 0);

  hid::X_INPUT_KEYSTROKE stroke;
  std::memset(&stroke, 0xA5, sizeof(stroke));
  c = {};
  // A null index is intentionally legal on the original gated path.
  CHECK(GetKeystroke(nullptr, 0, &stroke, ui, with_driver, c) ==
        X_ERROR_SUCCESS);
  Snapshot(c, &stroke);
  CHECK_FALSE(c.user_valid);
  CHECK(c.output_valid);
  CHECK(c.virtual_key == 0);
  CHECK(c.edge_flags == 0);
  CHECK(ui_queries == 2);
  CHECK(driver_entries == 0);

  c = {};
  CHECK(GetKeystroke(nullptr, 0, nullptr, ui, with_driver, c) ==
        X_ERROR_BAD_ARGUMENTS);
  CHECK_FALSE(c.ui_checked);
  CHECK_FALSE(c.output_initialized);
  CHECK(ui_queries == 2);
}

TEST_CASE("Ungated production dispatch preserves A and EMPTY with trace off/on",
          "[input-completion]") {
  for (bool enabled : {false, true}) {
    Collector collector(enabled);
    Driver driver;
    auto with_driver = [&](auto&& operation) { return operation(driver); };
    auto ui = [] { return false; };
    auto entry = collector.Begin(Api::kState, 6, 0x1234, 100);
    hid::X_INPUT_STATE state{};
    Completion c;
    CHECK(GetState(0, 0, &state, ui, with_driver, c) == X_ERROR_SUCCESS);
    Snapshot(c, &state);
    CHECK(c.a);
    CHECK(c.packet == 17);
    CHECK(c.driver_calls == 1);
    CHECK(driver.state_calls == 1);
    auto event = collector.End(entry, c, 101);
    CHECK_FALSE(event.row);

    be<uint32_t> index = 0;
    hid::X_INPUT_KEYSTROKE stroke{};
    c = {};
    CHECK(GetKeystroke(&index, 0, &stroke, ui, with_driver, c) ==
          X_ERROR_EMPTY);
    Snapshot(c, &stroke);
    CHECK(c.output_initialized);
    CHECK_FALSE(c.output_valid);
    CHECK(c.driver_calls == 1);
    CHECK(driver.stroke_calls == 1);
    CHECK(index == 0);

    driver.stroke_result = X_ERROR_SUCCESS;
    c = {};
    CHECK(GetKeystroke(&index, 0, &stroke, ui, with_driver, c) ==
          X_ERROR_SUCCESS);
    Snapshot(c, &stroke);
    CHECK(c.output_valid);
    CHECK(c.virtual_key == 0x5800);
    CHECK(c.edge_flags == 2);
    CHECK(index == 1);

    driver.stroke_result = X_ERROR_DEVICE_NOT_CONNECTED;
    c = {};
    CHECK(GetKeystroke(&index, 0, &stroke, ui, with_driver, c) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
    Snapshot(c, &stroke);
    CHECK_FALSE(c.output_valid);
    CHECK(index == 1);
  }
}

TEST_CASE("Input observation window excludes packet churn and retains counters",
          "[input-completion]") {
  Collector collector(true);
  Completion c;
  c.route = Route::kDriver;
  c.driver_calls = 1;
  auto first = collector.Begin(Api::kState, 6, 0x1234, 1000);
  CHECK(first.anchor);
  CHECK_FALSE(collector.End(first, c, 1001).row);
  auto entry = collector.Begin(Api::kState, 6, 0x1234, 180999);
  CHECK_FALSE(collector.End(entry, c, 181000).row);
  entry = collector.Begin(Api::kState, 6, 0x1234, 181000);
  CHECK(collector.End(entry, c, 181001).row);
  c.packet = 99;
  entry = collector.Begin(Api::kState, 6, 0x1234, 181002);
  CHECK_FALSE(collector.End(entry, c, 181003).row);
  for (uint32_t i = 1; i < 64; ++i) {
    c.a = !c.a;
    entry = collector.Begin(Api::kState, 6, 0x1234, 181003 + i);
    auto event = collector.End(entry, c, 181004 + i);
    CHECK(event.row);
    CHECK(event.cap_notice == (i == 63));
  }
  c.route = Route::kUI;
  c.driver_calls = 0;
  entry = collector.Begin(Api::kKeystroke, 6, 0x4567, 200000);
  CHECK_FALSE(collector.End(entry, c, 200001).row);
  entry = collector.Begin(Api::kKeystrokeEx, 6, 0x4567, 241000);
  auto summary = collector.End(entry, c, 241001);
  CHECK(summary.summary);
  CHECK(summary.counts[0].entries == 67);
  CHECK(summary.counts[0].completions == 67);
  CHECK(summary.counts[0].driver_calls == 67);
  CHECK(summary.counts[1].entries == 1);
  CHECK(summary.counts[1].gates == 1);
  CHECK(summary.counts[2].entries == 1);
  entry = collector.Begin(Api::kState, 6, 0, 241002);
  CHECK_FALSE(collector.End(entry, c, 241003).summary);
}

TEST_CASE("Actual input export trampolines keep the three-register guest ABI",
          "[input-completion]") {
  using StateRegistration = std::remove_reference_t<
      decltype(*shim::GetRegister<shim::KernelModuleId::xam, 0x191>(
          &XamInputGetState_entry))>;
  using StrokeRegistration = std::remove_reference_t<
      decltype(*shim::GetRegister<shim::KernelModuleId::xam, 0x193>(
          &XamInputGetKeystroke_entry))>;
  using ExRegistration = std::remove_reference_t<
      decltype(*shim::GetRegister<shim::KernelModuleId::xam, 0x198>(
          &XamInputGetKeystrokeEx_entry))>;
  auto* state_export =
      StateRegistration::RegisterExport<&XamInputGetState_entry, 0>(
          "InputStateAbiTest");
  auto* stroke_export =
      StrokeRegistration::RegisterExport<&XamInputGetKeystroke_entry, 0>(
          "InputStrokeAbiTest");
  auto* ex_export =
      ExRegistration::RegisterExport<&XamInputGetKeystrokeEx_entry, 0>(
          "InputExAbiTest");
  std::array<uint8_t, 64> memory;
  memory.fill(0xA5);
  PPCContext context{};
  context.virtual_membase = memory.data();
  context.r[3] = XUserMaxUserCount;
  context.r[4] = 0x12345678;
  context.r[5] = 16;
  context.r[6] = 0x1122334455667788;
  state_export->function_data.trampoline(&context);
  CHECK(context.r[3] == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(context.r[6] == 0x1122334455667788);
  for (size_t i = 0; i < memory.size(); ++i) {
    CHECK(memory[i] == (i >= 16 && i < 32 ? 0 : 0xA5));
  }
  for (auto* export_entry : {stroke_export, ex_export}) {
    context.r[3] = 0;  // Ex index null, never dereferenced for null output.
    context.r[4] = 0x12345678;
    context.r[5] = 0;
    export_entry->function_data.trampoline(&context);
    CHECK(context.r[3] == X_ERROR_BAD_ARGUMENTS);
    CHECK(context.r[4] == 0x12345678);
    CHECK(context.r[6] == 0x1122334455667788);
  }
}
}  // namespace xe::kernel::xam::input_observe::test
