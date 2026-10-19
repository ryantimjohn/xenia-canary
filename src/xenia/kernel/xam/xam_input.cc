/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/input_observe.h"
#include "xenia/kernel/xam/paired_capture.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DECLARE_bool(allow_mic_initialization);

DEFINE_bool(input_completion_trace, false,
            "Private bounded input completion diagnostic (180-240s from first "
            "input shim entry; no input changes).",
            "HID");

namespace xe {
namespace kernel {
namespace xam {

using xe::hid::X_INPUT_CAPABILITIES;
using xe::hid::X_INPUT_CAPABILITIES_EX;
using xe::hid::X_INPUT_FLAG;
using xe::hid::X_INPUT_KEYSTROKE;
using xe::hid::X_INPUT_STATE;
using xe::hid::X_INPUT_VIBRATION;
using xe::hid::X_USER_DEVICE_CLASS;
using xe::hid::X_USER_DEVICE_TYPE;

namespace {
namespace observe = input_observe;

observe::Collector& InputObserver() {
  // Process-lifetime snapshot: changing the cvar after the first input shim has
  // no effect. No guest/title object layout or input-driver state is changed.
  static observe::Collector collector(cvars::input_completion_trace);
  return collector;
}

uint64_t InputEpochMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

observe::Collector::Entry BeginInputObservation(observe::Api api) {
  auto& collector = InputObserver();
  if (!collector.enabled()) {
    return {};
  }
  auto* thread = XThread::GetCurrentThread();
  const uint32_t thread_id = thread ? thread->thread_id() : 0;
  const uint64_t lr = thread ? thread->thread_state()->context()->lr : 0;
  auto entry = collector.Begin(api, thread_id, lr, InputEpochMillis());
  if (entry.anchor) {
    XELOGI(
        "INPUT_COMPLETE anchor_epoch_ms={} entry_sequence={} api={} "
        "thread={:08X} lr={:016X} window_entry_ms=[180000,240000)",
        entry.anchor_ms, entry.sequence, uint32_t(api), thread_id, lr);
  }
  return entry;
}

void EndInputObservation(const observe::Collector::Entry& entry,
                         const observe::Completion& completion) {
  if (!entry.enabled) {
    return;
  }
  // End returns scalar copies, releasing its mutex before any
  // formatting/logging.
  auto event = InputObserver().End(entry, completion, InputEpochMillis());
  if (event.row) {
    XELOGI(
        "INPUT_COMPLETE row={} api={} entry_sequence={} entry_epoch_ms={} "
        "entry_elapsed_ms={} completion_epoch_ms={} thread={:08X} "
        "lr={:016X} route={} result={:08X} user_valid={} user={} flags={:08X} "
        "ui_checked={} ui_active={} driver_calls={} output_initialized={} "
        "output_valid={} a={} packet={} vk={:04X} edges={:04X}",
        event.row_number, uint32_t(entry.api), entry.sequence, entry.epoch_ms,
        entry.epoch_ms - entry.anchor_ms, event.through_epoch_ms, entry.thread,
        entry.lr, uint32_t(completion.route), completion.result,
        completion.user_valid, completion.user, completion.flags,
        completion.ui_checked, completion.ui_active, completion.driver_calls,
        completion.output_initialized, completion.output_valid, completion.a,
        completion.packet, completion.virtual_key, completion.edge_flags);
  }
  if (event.cap_notice) {
    XELOGI("INPUT_COMPLETE changed-row cap=64 reached; counters continue");
  }
  if (event.summary) {
    for (uint32_t i = 0; i < event.counts.size(); ++i) {
      const auto& counts = event.counts[i];
      XELOGI(
          "INPUT_COMPLETE summary api={} through_completion_epoch_ms={} "
          "trigger_entry_sequence={} entries={} completions={} ui_gates={} "
          "driver_calls={} (snapshot through this call; not final totals)",
          i, event.through_epoch_ms, entry.sequence, counts.entries,
          counts.completions, counts.gates, counts.driver_calls);
    }
  }
}

// Invoked once per OUTER export; the non-Ex export does not re-enter the Ex
// export, preserving the original public ABI and avoiding nested trace counts.
uint32_t InputGetKeystroke(observe::Api api, lpdword_t user_index_ptr,
                           dword_t flags,
                           pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  const auto entry = BeginInputObservation(api);
  observe::Completion completion;
  const auto result = observe::GetKeystroke(
      user_index_ptr, flags, keystroke,
      [] { return kernel_state()->xam_state()->IsUIActive(); },
      [](auto&& operation) {
        auto input_system = kernel_state()->emulator()->input_system();
        auto lock = input_system->lock();
        return operation(*input_system);
      },
      completion);
  if (entry.enabled) {
    observe::Snapshot(completion, keystroke);
  }
  EndInputObservation(entry, completion);
  if (auto* capture = paired_capture::Active(); capture &&
      result == X_ERROR_SUCCESS && keystroke && keystroke->virtual_key) {
    paired_capture::Words words{};
    words[0] = result;
    words[1] = uint32_t(completion.route);
    words[2] = completion.user;
    words[3] = completion.flags;
    words[4] = keystroke->virtual_key;
    words[5] = keystroke->unicode;
    words[6] = keystroke->flags;
    words[7] = keystroke->user_index;
    words[8] = keystroke->hid_code;
    capture->RecordEvent(paired_capture::Kind::kKeystroke,
                         keystroke->user_index, words);
  }
  return result;
}
}  // namespace

dword_result_t XAutomationpUnbindController_entry(dword_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return 0;
  }

  return 1;
}
DECLARE_XAM_EXPORT1(XAutomationpUnbindController, kInput, kStub);

void XamResetInactivity_entry() {
  // Do we need to do anything?
}
DECLARE_XAM_EXPORT1(XamResetInactivity, kInput, kStub);

dword_result_t XamEnableInactivityProcessing_entry(dword_t inactivity_index,
                                                   dword_t enable) {
  // Enables/disables screen saver and auto shutoff
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableInactivityProcessing, kInput, kStub);

dword_result_t XamInputGetCapabilitiesEx_entry(
    dword_t unk, dword_t user_index, dword_t flags,
    pointer_t<X_INPUT_CAPABILITIES_EX> caps) {
  if (unk > 1) {
    return X_ERROR_NOT_SUPPORTED;
  }

  // Fail-safe check
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  caps.Zero();

  if ((flags & X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER) != 0) {
    // should trap
  }

  if ((flags & 4) != 0) {
    // should trap
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & XUserIndexAny) == XUserIndexAny ||
      (flags & X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  uint32_t actual_flags = flags;
  if (!flags) {
    actual_flags = X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD |
                   X_INPUT_FLAG::X_INPUT_FLAG_KEYBOARD;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  auto lock = input_system->lock();
  return input_system->GetCapabilities(actual_user_index, actual_flags, caps);
}
DECLARE_XAM_EXPORT1(XamInputGetCapabilitiesEx, kInput, kSketchy);

// https://learn.microsoft.com/en-gb/windows/win32/api/xinput/nf-xinput-xinputgetcapabilities
dword_result_t XamInputGetCapabilities_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_CAPABILITIES> caps) {
  X_HRESULT result;
  memset(caps, 0x0, sizeof(X_INPUT_CAPABILITIES));
  X_INPUT_CAPABILITIES_EX caps_ex = {};
  result = XamInputGetCapabilitiesEx_entry(1, user_index, flags, &caps_ex);
  if (!result) {
    std::memcpy(caps, &caps_ex, sizeof(X_INPUT_CAPABILITIES));
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamInputGetCapabilities, kInput, kSketchy);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
dword_result_t XamInputGetState_entry(dword_t user_index, dword_t flags,
                                      pointer_t<X_INPUT_STATE> input_state) {
  const auto entry = BeginInputObservation(observe::Api::kState);
  observe::Completion completion;
  const auto result = observe::GetState(
      user_index, flags, input_state,
      [] { return kernel_state()->xam_state()->IsUIActive(); },
      [](auto&& operation) {
        auto input_system = kernel_state()->emulator()->input_system();
        auto lock = input_system->lock();
        return operation(*input_system);
      },
      completion);

  if (completion.route == observe::Route::kDriver && input_state &&
      result == X_ERROR_SUCCESS) {
    if (auto patch = kernel_state()->xmp_volume_patch()) {
      patch->OnInputPoll(input_state->packet_number);
    }
  }

  if (entry.enabled) {
    observe::Snapshot(completion, input_state);
  }
  EndInputObservation(entry, completion);
  if (auto* capture = paired_capture::Active()) {
    paired_capture::Words words{};
    words[0] = result;
    words[1] = uint32_t(completion.route);
    words[2] = user_index;
    words[3] = flags;
    words[4] = input_state && result == X_ERROR_SUCCESS ? 1 : 0;
    if (words[4]) {
      words[5] = input_state->packet_number;
      words[6] = input_state->gamepad.buttons;
      words[7] = input_state->gamepad.left_trigger;
      words[8] = input_state->gamepad.right_trigger;
      words[9] = uint32_t(int32_t(input_state->gamepad.thumb_lx));
      words[10] = uint32_t(int32_t(input_state->gamepad.thumb_ly));
      words[11] = uint32_t(int32_t(input_state->gamepad.thumb_rx));
      words[12] = uint32_t(int32_t(input_state->gamepad.thumb_ry));
    }
    capture->RecordEvent(paired_capture::Kind::kInputState, user_index,
                         words, true);
  }
  return result;
}
DECLARE_XAM_EXPORT2(XamInputGetState, kInput, kImplemented, kHighFrequency);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
dword_result_t XamInputSetState_entry(
    dword_t user_index,
    dword_t flags, /* flags, as far as i can see, is not used*/
    pointer_t<X_INPUT_VIBRATION> vibration) {
  if (user_index >= XUserMaxUserCount) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  auto lock = input_system->lock();
  return input_system->SetState(user_index, vibration);
}
DECLARE_XAM_EXPORT1(XamInputSetState, kInput, kImplemented);

// https://learn.microsoft.com/en-gb/windows/win32/api/xinput/nf-xinput-xinputgetkeystroke
// Same as non-ex, just takes a pointer to user index.
dword_result_t XamInputGetKeystrokeEx_entry(
    lpdword_t user_index_ptr, dword_t flags,
    pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  return InputGetKeystroke(observe::Api::kKeystrokeEx, user_index_ptr, flags,
                           keystroke);
}
DECLARE_XAM_EXPORT1(XamInputGetKeystrokeEx, kInput, kImplemented);

dword_result_t XamInputGetKeystroke_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  uint32_t actual_user_index = user_index;
  return InputGetKeystroke(observe::Api::kKeystroke, &actual_user_index, flags,
                           keystroke);
}
DECLARE_XAM_EXPORT1(XamInputGetKeystroke, kInput, kImplemented);

X_HRESULT_result_t XamUserGetDeviceContext_entry(dword_t user_index,
                                                 dword_t device_type,
                                                 lpdword_t out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (kernel_state()->xam_state()->IsUserSignedIn(user_index) ||
      (user_index & XUserIndexAny) == XUserIndexAny) {
    if (device_type == X_USER_DEVICE_CLASS::DEVICE_CLASS_MIC &&
        cvars::allow_mic_initialization) {  // Microphone
      *out_ptr = X_USER_DEVICE_TYPE::DEVICE_TYPE_MIC_2;
    } else {
      *out_ptr = (uint32_t)user_index;
    }
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}
DECLARE_XAM_EXPORT1(XamUserGetDeviceContext, kInput, kStub);

X_HRESULT_result_t XamInputNonControllerGetRawEx_entry(
    dword_t device_id, lpdword_t buffer_ptr, lpdword_t buffer_length_ptr,
    lpword_t state_ptr) {
  if (device_id != 5 && device_id != 6) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!state_ptr || !buffer_length_ptr || !buffer_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (*buffer_length_ptr == 0 || *buffer_length_ptr > hid::kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto portal = kernel_state()->emulator()->input_system()->GetPortal();
  if (!portal) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t bytes_read = *buffer_length_ptr;
  uint16_t state = 0;

  const auto result = portal->Read(
      {kernel_memory()->TranslateVirtual(buffer_ptr.guest_address()),
       *buffer_length_ptr},
      bytes_read, state);

  if (XSUCCEEDED(result)) {
    *buffer_length_ptr = bytes_read;
    *state_ptr = state;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamInputNonControllerGetRawEx, kInput, kSketchy);

X_HRESULT_result_t XamInputNonControllerSetRawEx_entry(dword_t device_id,
                                                       lpdword_t buffer_ptr,
                                                       dword_t buffer_length) {
  if (device_id != 5 && device_id != 6) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!buffer_ptr || !buffer_length || buffer_length > hid::kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto portal = kernel_state()->emulator()->input_system()->GetPortal();
  if (!portal) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return portal->Write(
      {kernel_memory()->TranslateVirtual(buffer_ptr.guest_address()),
       buffer_length});
}
DECLARE_XAM_EXPORT1(XamInputNonControllerSetRawEx, kInput, kSketchy);

X_HRESULT_result_t XamInputNonControllerGetRaw_entry(
    lpword_t state_ptr, lpdword_t buffer_length_ptr, lpdword_t buffer_ptr) {
  return XamInputNonControllerGetRawEx_entry(5, buffer_ptr, buffer_length_ptr,
                                             state_ptr);
}
DECLARE_XAM_EXPORT1(XamInputNonControllerGetRaw, kInput, kSketchy);

X_HRESULT_result_t XamInputNonControllerSetRaw_entry(dword_t buffer_length,
                                                     lpdword_t buffer_ptr) {
  // Normally there are handled separatelly with different first param, but
  // whatever.
  return XamInputNonControllerSetRawEx_entry(5, buffer_ptr, buffer_length);
}
DECLARE_XAM_EXPORT1(XamInputNonControllerSetRaw, kInput, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Input);
