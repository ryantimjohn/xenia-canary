/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/nui_log.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamVoiceIsActiveProcess_entry() {
  // Returning 0 here will short-circuit a bunch of voice stuff.
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceIsActiveProcess, kNone, kStub);

dword_result_t XamVoiceCreate_entry(dword_t user_index,
                                    dword_t max_attached_packets,  // 0xF
                                    lpdword_t out_voice_ptr) {
  // Null out the ptr.
  out_voice_ptr.Zero();
  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamVoiceCreate, kNone, kStub);

dword_result_t XamVoiceClose_entry(lpunknown_t voice_ptr) { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceClose, kNone, kStub);

dword_result_t XamVoiceHeadsetPresent_entry(lpunknown_t voice_ptr) { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceHeadsetPresent, kNone, kStub);

dword_result_t XamVoiceSubmitPacket_entry(lpdword_t unk1, dword_t unk2,
                                          lpdword_t unk3) {
  // also may return 0xD000009D
  return 0x800700AA;
}
DECLARE_XAM_EXPORT1(XamVoiceSubmitPacket, kNone, kStub);

dword_result_t XamVoiceGetMicArrayStatus_entry() {
  // Returning 0 here tells caller mic is not connected
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayStatus, kNone, kStub);

// The microphone array is the Kinect's four-element mic. None of it is
// emulated, so the array never has audio to hand over and the control calls
// quietly succeed. The arguments are logged for the same reason the rest of
// the NUI surface is: to see what a title actually asks for.
dword_result_t XamVoiceSetMicArrayIdleUsers_entry(dword_t user_mask) {
  LogNuiCall("NUI: XamVoiceSetMicArrayIdleUsers({:08X})", user_mask.value());
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayIdleUsers, kNone, kStub);

dword_result_t XamVoiceMuteMicArray_entry(dword_t mute) {
  LogNuiCall("NUI: XamVoiceMuteMicArray({})", mute.value());
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceMuteMicArray, kNone, kStub);

dword_result_t XamVoiceDisableMicArray_entry(dword_t disable) {
  LogNuiCall("NUI: XamVoiceDisableMicArray({})", disable.value());
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceDisableMicArray, kNone, kStub);

dword_result_t XamVoiceSetMicArrayBeamAngle_entry(dword_t angle) {
  LogNuiCall("NUI: XamVoiceSetMicArrayBeamAngle({})", angle.value());
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayBeamAngle, kNone, kStub);

dword_result_t XamVoiceGetMicArrayUnderrunStatus_entry() {
  LogNuiCall("NUI: XamVoiceGetMicArrayUnderrunStatus()");
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayUnderrunStatus, kNone, kStub);

dword_result_t XamVoiceGetMicArrayAudio_entry(lpvoid_t buffer_ptr,
                                              dword_t buffer_size,
                                              lpdword_t bytes_read_ptr) {
  LogNuiCall("NUI: XamVoiceGetMicArrayAudio({:08X}, {}, {:08X})",
             buffer_ptr.guest_address(), buffer_size.value(),
             bytes_read_ptr.guest_address());
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayAudio, kNone, kStub);

dword_result_t XamVoiceGetMicArrayAudioEx_entry(lpvoid_t buffer_ptr,
                                                dword_t buffer_size,
                                                lpdword_t bytes_read_ptr,
                                                dword_t flags) {
  LogNuiCall("NUI: XamVoiceGetMicArrayAudioEx({:08X}, {}, {:08X}, {:08X})",
             buffer_ptr.guest_address(), buffer_size.value(),
             bytes_read_ptr.guest_address(), flags.value());
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayAudioEx, kNone, kStub);

dword_result_t XamVoiceSetAudioCaptureRoutine_entry(lpvoid_t routine_ptr,
                                                    lpvoid_t context_ptr) {
  LogNuiCall("NUI: XamVoiceSetAudioCaptureRoutine({:08X}, {:08X})",
             routine_ptr.guest_address(), context_ptr.guest_address());
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamVoiceSetAudioCaptureRoutine, kNone, kStub);

dword_result_t XamVoiceGetDirectionalData_entry(lpvoid_t data_ptr) {
  LogNuiCall("NUI: XamVoiceGetDirectionalData({:08X})",
             data_ptr.guest_address());
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamVoiceGetDirectionalData, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Voice);
