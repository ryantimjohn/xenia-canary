/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/nui_log.h"
#include "xenia/kernel/xam/nui_messages.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/kernel/xam/nui_tilt_status.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/nui/nui_flags.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
// https://web.cs.ucdavis.edu/~okreylos/ResDev/Kinect/MainPage.html

namespace {

// Shorthand for the guest-visible Kinect state, which every export below reads
// or writes. X_NUI_DEVICE_STATUS lives alongside it in nui_state.h.
NuiState* nui_state() { return kernel_state()->xam_state()->nui_state(); }

// Reports what went into the tilt-status block. The title polls it about fifty
// times a second - 7,759 calls in a 150 s run - so a line per call would bury
// the rest of a NUI trace; only a change of angle, or a second's silence, is
// worth one.
//
// The rate limit's state is function-local and static, which means two things
// worth knowing. It is shared by every thread that polls, and the exchange and
// the timestamp are not one atomic step, so two simultaneous polls can both
// decide to print; the race only ever costs an extra line, never a suppressed
// one, which is the harmless direction. And it outlives a title launch, so the
// first poll of a second title in the same process is rate-limited against the
// first one's last line. Neither is worth a lock or a home in NuiState for a
// diagnostic that only runs under nui_log_messages.
void LogNuiTiltStatus(int32_t degrees, int32_t accel_y, int32_t accel_z) {
  if (!cvars::nui_log_messages) {
    return;
  }
  static std::atomic<int32_t> last_degrees = {
      std::numeric_limits<int32_t>::min()};
  static std::atomic<uint64_t> last_millis = {0};
  const uint64_t now_millis = Clock::QueryHostUptimeMillis();
  const bool angle_changed =
      last_degrees.exchange(degrees, std::memory_order_relaxed) != degrees;
  if (!angle_changed &&
      now_millis - last_millis.load(std::memory_order_relaxed) < 1000) {
    return;
  }
  last_millis.store(now_millis, std::memory_order_relaxed);
  XELOGI(
      "NUI: XamNuiCameraTiltGetStatus: angle {}, accelerometer y {} z {} "
      "(atan2 -> {} degrees)",
      degrees, accel_y, accel_z,
      static_cast<int32_t>(
          std::lround(std::atan2(static_cast<double>(accel_z),
                                 static_cast<double>(accel_y)) /
                      kNuiTiltRadiansPerDegree)));
}

}  // namespace

// Get
dword_result_t XamNuiGetDeviceStatus_entry(
    pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  /* Notes:
     - it does return a value that is not always used
     - returns values are X_ERROR_SUCCESS, 0xC0050006, and others
     - 1) On func start *status_ptr = 0, status_ptr->tilt_status = 0,
     status_ptr->mic_status = 0, and status_ptr->status_bits = 0
     - 2) calls XamXStudioRequest(6,&var <- = 0);
     - if return is greater than -1 && var & 0x80000000 != 0 then set
     status_ptr->tilt_status = 0xC000009D,
     status_ptr->mic_status = 0xC000009D, and status_ptr->status_bits = 0x20
     - lots of branching functions after
  */
  LogNuiCall("NUI: XamNuiGetDeviceStatus({:08X})", status_ptr.guest_address());

  if (!status_ptr ||
      !nui_state()->IsGuestRangeWritable(status_ptr.guest_address(),
                                         sizeof(X_NUI_DEVICE_STATUS))) {
    return X_E_INVALIDARG;
  }
  nui_state()->FillDeviceStatus(status_ptr);
  return nui_state()->IsDevicePresent() ? X_ERROR_SUCCESS : 0xC0050006;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kImplemented);

dword_result_t XamNuiSetForceDeviceOff_entry(dword_t off) {
  LogNuiCall("NUI: XamNuiSetForceDeviceOff({})", off.value());

  nui_state()->SetForceDeviceOff(off != 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiSetForceDeviceOff, kNone, kImplemented);

dword_result_t XamUserNuiGetUserIndex_entry(unknown_t unk, lpdword_t index) {
  LogNuiCall("NUI: XamUserNuiGetUserIndex({:08X}, {:08X})", unk.value(),
             index.guest_address());
  return X_E_NO_SUCH_USER;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

dword_result_t XamUserNuiGetEnrollmentIndex_entry() {
  LogNuiCall("NUI: XamUserNuiGetEnrollmentIndex()");
  return X_E_NO_SUCH_USER;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kStub);

dword_result_t XamUserNuiBind_entry() {
  LogNuiCall("NUI: XamUserNuiBind()");
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiBind, kNone, kStub);

dword_result_t XamUserNuiUnbind_entry() {
  LogNuiCall("NUI: XamUserNuiUnbind()");
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiUnbind, kNone, kStub);

dword_result_t XamUserNuiGetUserIndexForSignin_entry(lpdword_t index) {
  LogNuiCall("NUI: XamUserNuiGetUserIndexForSignin({:08X})",
             index.guest_address());

  if (!index || !nui_state()->IsGuestRangeWritable(index.guest_address(),
                                                   sizeof(uint32_t))) {
    return X_E_INVALIDARG;
  }

  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    auto profile = kernel_state()->xam_state()->GetUserProfile(i);
    if (profile) {
      *index = i;
      return X_E_SUCCESS;
    }
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForSignin, kNone, kImplemented);

dword_result_t XamUserNuiGetUserIndexForBind_entry(lpdword_t index) {
  LogNuiCall("NUI: XamUserNuiGetUserIndexForBind({:08X})",
             index.guest_address());
  if (!index || !nui_state()->IsGuestRangeWritable(index.guest_address(),
                                                   sizeof(uint32_t))) {
    return X_E_INVALIDARG;
  }
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

dword_result_t XamNuiGetDepthCalibration_entry(
    pointer_t<X_NUI_DEPTH_CALIBRATION> depth_data) {
  LogNuiCall("NUI: XamNuiGetDepthCalibration({:08X})",
             depth_data.guest_address());
  if (!depth_data ||
      !nui_state()->IsGuestRangeWritable(depth_data.guest_address(),
                                         sizeof(X_NUI_DEPTH_CALIBRATION))) {
    return X_E_INVALIDARG;
  }
  ClearNuiDepthCalibration(depth_data);
  return X_STATUS_NO_SUCH_FILE | X_FACILITY_NT_BIT;
}
DECLARE_XAM_EXPORT1(XamNuiGetDepthCalibration, kNone, kStub);

// Skeleton
qword_result_t XamNuiSkeletonGetBestSkeletonIndex_entry(int_t unk) {
  LogNuiCall("NUI: XamNuiSkeletonGetBestSkeletonIndex({})", unk.value());
  return 0xffffffffffffffff;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetBestSkeletonIndex, kNone, kStub);

/* XamNuiCamera Notes
   - most require message calls to xam in 0x0002Bxxx area
*/

// XamNuiCameraTiltGetStatus(X_NUI_TILT_STATUS* block).
//
// The NUI runtime's auto-tilt driver polls this about fifty times a second and
// reads the camera's elevation out of the block's accelerometer rather than
// out of XamNuiCameraElevationGetAngle: atan2(block[+0x28], block[+0x24]) in
// degrees (0x82d2dd40-0x82d2dd94), compared against the angle it last
// commanded with a two-degree tolerance (0x82d2df54-0x82d2df84 and
// 0x82d2e048-0x82d2e078). Handing it a blank block, as xenia used to, makes
// that measurement atan2(0, 0) = 0 for every angle, so the driver never
// settles at a non-zero target: 4D5308ED re-commanded the motor nine
// times in a 150 s run, five of those to the clamped 27 degrees, and grew the
// "PRESS (X) ... TO LAUNCH THE KINECT TUNER, OR (Y) TO DISABLE AUTO-TILT" hint.
// On a console that is the motor being driven into its end stop over and over.
//
// The block's tail hands back the floor the title last remembered
// (XamNuiCameraRememberFloor below), and the driver's targets depend on it.
// Its target routine (0x82d2c6f8) branches on the tail's d: with a floor it
// aims absolutely, the top of its view on the head of a 1.8 m player 2.2 m
// away, and never searches again; with a d of zero it sweeps the motor
// blind, from -25 up in steps of ten while the head reads below level, and
// then adds +4 or +8 to a base the moving bits keep resetting to zero. A
// tail of zeros is what gave 4D5308ED its endless searches and its
// +4, +8, +12 and +16 degree targets, which put a standing player's feet out
// of view.
//
/* Notes:
   - Used by XamNuiCameraElevationGetAngle, and XamNuiCameraSetFlags
   - if it returns anything greater than -1 then both above functions continue
   - Both funcs send in a param of *unk = 0x50 bytes to copy
   - Ghidra decompile fails
*/
dword_result_t XamNuiCameraTiltGetStatus_entry(
    pointer_t<X_NUI_TILT_STATUS> block_ptr) {
  LogNuiCall("NUI: XamNuiCameraTiltGetStatus({:08X})",
             block_ptr.guest_address());

  if (!block_ptr ||
      !nui_state()->IsGuestRangeWritable(block_ptr.guest_address(),
                                         sizeof(X_NUI_TILT_STATUS)) ||
      !IsValidNuiTiltStatus(block_ptr)) {
    return X_E_FAIL;
  }
  if (!nui_state()->IsDevicePresent()) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  int32_t degrees = 0;
  NuiTiltStatusLegs legs;
  if (!nui_state()->GetTiltStatusLegs(&legs, &degrees)) {
    // A sensor whose angle xenia cannot read is, as far as the block goes, a
    // sensor that is not there: nothing is written and the caller keeps what
    // it put in the block itself.
    return X_E_DEVICE_NOT_CONNECTED;
  }
  // The range, size and signature were validated above. A sensor whose
  // accelerometer has been read reports where
  // it really points; otherwise, and whenever nui_tilt_override is set, the
  // block is made from the angle, as the backend reported it just above.
  // Either way a motor still on its way says so, so that the driver waits
  // for the move instead of judging an angle the head has not reached yet.
  // Then the tail gets the remembered floor, when there is one the driver
  // can aim from; otherwise it stays as the fill cleared it.
  FillNuiTiltStatusLegs(block_ptr, legs, nui_state()->IsTiltMotorMoving());
  FillNuiTiltStatusFloor(nui_state()->remembered_floor(), block_ptr);
  LogNuiTiltStatus(degrees, block_ptr->accel_y, block_ptr->accel_z);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltGetStatus, kNone, kImplemented);

// XamNuiCameraElevationGetAngle(LONG* angle_degrees, DWORD* status_flags).
// The two arguments are told apart by what 4D5308ED does with them:
// 0x82a5fbd0 reads the first back and compares its magnitude against a
// configured threshold, which only makes sense for an angle, while 0x82a5f85c
// and 0x82a596ac read the second back and test single bits of it, which only
// makes sense for a status. Writing the angle into the second argument put the
// title's engagement HUD into "check the sensor" for every angle that is not a
// multiple of four and kept it there, skeletons and all. Either pointer may be
// null: the title's own forwarder rejects a null before it calls, but the
// other call sites do not all pass both.
dword_result_t XamNuiCameraElevationGetAngle_entry(lpdword_t angle_ptr,
                                                   lpdword_t status_ptr) {
  if ((angle_ptr && !nui_state()->IsGuestRangeWritable(
                        angle_ptr.guest_address(), sizeof(uint32_t))) ||
      (status_ptr && !nui_state()->IsGuestRangeWritable(
                         status_ptr.guest_address(), sizeof(uint32_t)))) {
    return X_E_INVALIDARG;
  }
  int32_t degrees = 0;
  const X_RESULT result = nui_state()->GetTiltDegrees(&degrees);
  // The backend's moving state also supplies the tilt block's moving bit.
  // A stalled/error condition is not available from the host backend yet.
  const uint32_t status =
      NuiTiltStatusWord(nui_state()->IsTiltMotorMoving(), false);
  if (XSUCCEEDED(result)) {
    if (angle_ptr) {
      *angle_ptr = NuiTiltAngleWord(degrees);
    }
    if (status_ptr) {
      *status_ptr = status;
    }
    LogNuiCall(
        "NUI: XamNuiCameraElevationGetAngle({:08X}, {:08X}) = {:08X}, angle "
        "{}, status {:08X}",
        angle_ptr.guest_address(), status_ptr.guest_address(), result, degrees,
        status);
  } else {
    LogNuiCall(
        "NUI: XamNuiCameraElevationGetAngle({:08X}, {:08X}) = {:08X}, nothing "
        "written",
        angle_ptr.guest_address(), status_ptr.guest_address(), result);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kImplemented);

dword_result_t XamNuiCameraElevationSetAngle_entry(dword_t degrees) {
  LogNuiCall("NUI: XamNuiCameraElevationSetAngle({})",
             static_cast<int32_t>(degrees.value()));
  return nui_state()->SetTiltDegrees(static_cast<int32_t>(degrees.value()));
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kImplemented);

// The remaining tilt entry points have no observed call sites yet, so they only
// report whether the sensor is there to move at all.
dword_result_t XamNuiCameraElevationStopMovement_entry() {
  LogNuiCall("NUI: XamNuiCameraElevationStopMovement()");
  return nui_state()->IsDevicePresent() ? X_ERROR_SUCCESS
                                        : X_E_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kStub);

dword_result_t XamNuiCameraElevationAutoTilt_entry() {
  LogNuiCall("NUI: XamNuiCameraElevationAutoTilt()");
  return nui_state()->IsDevicePresent() ? X_ERROR_SUCCESS
                                        : X_E_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationAutoTilt, kNone, kStub);

dword_result_t XamNuiCameraElevationReverseAutoTilt_entry() {
  LogNuiCall("NUI: XamNuiCameraElevationReverseAutoTilt()");
  return nui_state()->IsDevicePresent() ? X_ERROR_SUCCESS
                                        : X_E_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationReverseAutoTilt, kNone, kStub);

dword_result_t XamNuiCameraAdjustTilt_entry() {
  LogNuiCall("NUI: XamNuiCameraAdjustTilt()");
  return nui_state()->IsDevicePresent() ? X_ERROR_SUCCESS
                                        : X_E_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraAdjustTilt, kNone, kStub);

// XamNuiCameraRememberFloor(uint64_t plane_ab, uint64_t plane_cd,
// uint32_t flag). The runtime's tilt driver calls it 700-2,500 times a session
// from 0x82d2cc1c and 0x82d2cc84, both as `lwz r5, 48(r31); ld r3, 0(r30);
// ld r4, 8(r30); bl`, and stores 0 to 48(r31) afterwards, so the export takes
// two 8-byte words and a word: the two loads read the 16 bytes at r30, which
// hold the floor plane as four big-endian floats (see DecodeNuiFloorPlane), and
// the third argument is the word at r31+48 that the driver clears once it has
// made the call. Taking none, as this stub did, ignored all three. What the
// word means is not known; xenia logs it with the plane. The plane is kept,
// with the accelerometer legs the tilt status block carries at this moment,
// and XamNuiCameraTiltGetStatus hands both back in the block's tail, which
// is where the runtime's tilt driver looks for the floor it remembered.
dword_result_t XamNuiCameraRememberFloor_entry(qword_t plane_ab,
                                               qword_t plane_cd, dword_t flag) {
  std::array<float, 4> plane;
  DecodeNuiFloorPlane(plane_ab, plane_cd, plane.data());
  LogNuiCall(
      "NUI: XamNuiCameraRememberFloor({:.3f}, {:.3f}, {:.3f}, {:.3f}; flag {})",
      plane[0], plane[1], plane[2], plane[3], flag.value());

  if (!nui_state()->IsDevicePresent()) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  nui_state()->RememberFloor(plane, flag);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

// XamNuiCameraTiltSetCallback(void (*cb)(uint32_t id, float* data)).
// The NUI runtime registers its tilt callback here: 0x82d2d524 calls this with
// 0x82D2D150, so the export takes one argument and taking none read the
// title's callback pointer as nothing at all. The channel runs the other way -
// xam calls the title when something outside it moves the sensor - so xenia,
// which never moves a sensor behind the title's back, records the registration
// but does not invoke it yet.
dword_result_t XamNuiCameraTiltSetCallback_entry(dword_t callback) {
  LogNuiCall("NUI: XamNuiCameraTiltSetCallback({:08X})", callback.value());

  kernel_state()->nui()->SetTiltCallback(callback);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

dword_result_t XamNuiCameraElevationSetCallback_entry() {
  LogNuiCall("NUI: XamNuiCameraElevationSetCallback()");
  return nui_state()->IsDevicePresent() ? X_ERROR_SUCCESS
                                        : X_E_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetCallback, kNone, kStub);

dword_result_t XamNuiCameraTiltReportStatus_entry(dword_t message,
                                                  lpdword_t data) {
  // The title reports tilt progress back to xam as a plain 0x2Bxxx message.
  LogNuiCall("NUI: XamNuiCameraTiltReportStatus({:08X}, {:08X})",
             message.value(), data.guest_address());
  return nui_state()->DispatchNuiMessage(message, data.guest_address(),
                                         sizeof(uint32_t));
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kImplemented);

dword_result_t XamNuiCameraGetTiltControllerType_entry() {
  /* Notes:
     - undefined unk[8]
     - undefined8 local_28;
     - undefined8 local_20;
     - undefined8 local_18;
     - undefined4 local_10;
     - local_20 = 0;
     - local_18 = 0;
     - local_10 = 0;
     - local_28 = 0xf030000000000;
     - calls DetroitDeviceRequest(unk) -> result
     - returns (ulonglong)(LZCOUNT(result) << 0x20) >> 0x25
  */
  LogNuiCall("NUI: XamNuiCameraGetTiltControllerType()");
  return nui_state()->IsDevicePresent() ? 1 : 0;
}
DECLARE_XAM_EXPORT1(XamNuiCameraGetTiltControllerType, kNone, kImplemented);

dword_result_t XamNuiCameraSetFlags_entry(qword_t unk1, dword_t unk2) {
  /* Notes:
     - if XamNuiCameraGetTiltControllerType returns 1 then operation is done
     - else 0xffffffff8007048f
  */
  LogNuiCall("NUI: XamNuiCameraSetFlags({:016X}, {:08X})", unk1.value(),
             unk2.value());

  if (XamNuiCameraGetTiltControllerType_entry() != 1) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  int32_t degrees = 0;
  const X_RESULT result = nui_state()->GetTiltDegrees(&degrees);
  if (XFAILED(result)) {
    return result;
  }
  // The flags are not applied yet; do not claim success for an unsupported
  // change merely because the sensor is present.
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

dword_result_t XamIsNuiUIActive_entry() {
  return kernel_state()->xam_state()->is_xam_dialog_present_.load();
}
DECLARE_XAM_EXPORT1(XamIsNuiUIActive, kNone, kImplemented);

dword_result_t XamNuiIsDeviceReady_entry() {
  /* device_state Notes:
   - used with XNotifyBroadcast(kXNotificationSystemNUIHardwareStatusChanged,
   device_state)
   - known values:
     - 0x0001
     - 0x0004
     - 0x0040
  */
  LogNuiCall("NUI: XamNuiIsDeviceReady()");
  return nui_state()->IsDevicePresent() ? 1 : 0;
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamIsNuiAutomationEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNuiAutomationEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamIsNatalPlaybackEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNatalPlaybackEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamNuiIsChatMicEnabled_entry() {
  /* Notes:
     - calls a second function with a param of uint local_20 [4];
     - Second function calls ExGetXConfigSetting(7,9,local_30,0x1c,local_40);
     - Result is sent to *local_20[0] = ^
     - Once sent back to XamNuiIsChatMicEnabled it looks for byte that
     correlates to NUI mic setting
     - return uVar2 = (~(ulonglong)local_20[0] << 0x20) >> 0x23 & 1;
     - unless the second function returns something -1 or less then
     XamNuiIsChatMicEnabled 1
  */
  LogNuiCall("NUI: XamNuiIsChatMicEnabled()");
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiIsChatMicEnabled, kNone, kImplemented);

/* HUD Notes:
   - XamNuiHudGetEngagedTrackingID, XamNuiHudIsEnabled,
   XamNuiHudSetEngagedTrackingID, XamNuiHudInterpretFrame, and
   XamNuiHudGetEngagedEnrollmentIndex all utilize the same data address
   - engaged_tracking_id set second param of XamShowNuiTroubleshooterUI
*/

dword_result_t XamNuiHudSetEngagedTrackingID_entry(dword_t id) {
  LogNuiCall("NUI: XamNuiHudSetEngagedTrackingID({:08X})", id.value());

  if (!id) {
    return X_STATUS_SUCCESS;
  }

  if (nui_state()->IsDevicePresent() &&
      nui_state()->runtime_state().EngageTrackingId(id)) {
    return X_STATUS_SUCCESS;
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiHudSetEngagedTrackingID, kNone, kImplemented);

qword_result_t XamNuiHudGetEngagedTrackingID_entry() {
  LogNuiCall("NUI: XamNuiHudGetEngagedTrackingID()");

  if (nui_state()->hud_enabled()) {
    return nui_state()->engaged_tracking_id();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);

dword_result_t XamNuiHudIsEnabled_entry() {
  /* Notes:
     - checks if XamNuiIsDeviceReady false, if nui_unknown_1 exists, and
     nui_unknown_2 is equal to null terminated string
     - only returns true if one check fails and allows for other NUI functions
     to progress
  */
  LogNuiCall("NUI: XamNuiHudIsEnabled()");
  return nui_state()->hud_enabled();
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

dword_result_t XamNuiHudGetInitializeFlags_entry() {
  /* HUD_Flags Notes:
     - set by 0x2B003
     - set to 0 by unnamed func alongside version_id
     - known values:
       - 0x40000000
       - 0x200
  */
  LogNuiCall("NUI: XamNuiHudGetInitializeFlags()");
  return nui_state()->hud_init_flags();
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

void XamNuiHudGetVersions_entry(lpqword_t unk1, lpqword_t unk2) {
  /* version_id Notes:
     - set by 0x2B003
     - set to 0 by unnamed func alongside HUD_Flags
  */
  LogNuiCall("NUI: XamNuiHudGetVersions({:08X}, {:08X})", unk1.guest_address(),
             unk2.guest_address());

  if (unk1) {
    *unk1 = nui_state()->st_version_major();
  }
  if (unk2) {
    *unk2 = nui_state()->st_version_build();
  }
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

/* XamNuiIdentity Notes:
   - most require message calls to xam in 0x0002Cxxx area
*/
qword_result_t XamNuiIdentityGetSessionId_entry() {
  LogNuiCall("NUI: XamNuiIdentityGetSessionId()");

  return nui_state()->runtime_state().GetOrCreateSessionId();
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kImplemented);

dword_result_t XamNuiIdentityEnrollForSignIn_entry(dword_t unk1, qword_t unk2,
                                                   qword_t unk3, dword_t unk4) {
  /* Notes:
     - Decompiler issues so double check
  */
  LogNuiCall(
      "NUI: XamNuiIdentityEnrollForSignIn({:08X}, {:016X}, {:016X}, {:08X})",
      unk1.value(), unk2.value(), unk3.value(), unk4.value());

  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [2]
  // buffer[0] = unk
  // var = unk4
  // return func(0xfe,0x2c010,buffer,0xc,unk3);
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityEnrollForSignIn, kNone, kStub);

dword_result_t XamNuiIdentityAbort_entry(dword_t unk) {
  LogNuiCall("NUI: XamNuiIdentityAbort({:08X})", unk.value());

  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [4]
  // buffer[0] = unk
  // return func(0xfe,0x2c00e,buffer,4,0)
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityAbort, kNone, kStub);

// Other
dword_result_t XamUserNuiEnableBiometric_entry(dword_t user_index,
                                               int_t enable) {
  LogNuiCall("NUI: XamUserNuiEnableBiometric({}, {})", user_index.value(),
             enable.value());
  return X_E_INVALIDARG;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kStub);

dword_result_t XamReadBiometricData_entry(
    qword_t xuid, lpdword_t stamp_ptr, lpvoid_t buffer_ptr, lpdword_t size_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // The NUI runtime reads a profile's stored biometric data with this once the
  // profile is signed in (0x82d34318, and 0x82d329f8 on the enrolment path),
  // into a 0x32000-byte buffer whose size it passes in and out, with no
  // overlapped. It zeroes the stamp first and, when the read succeeds and
  // leaves it non-zero, sends it to xam as the stamp of 0x2A007. Xenia stores
  // none. Any failure makes the runtime give xam a one-byte placeholder with
  // 0x2A007 and carry on, so the outputs are left alone and the answer is
  // "not found".
  LogNuiCall(
      "NUI: XamReadBiometricData({:016X}, {:08X}, {:08X}, {:08X}, {:08X}): no "
      "biometric data",
      xuid.value(), stamp_ptr.guest_address(), buffer_ptr.guest_address(),
      size_ptr.guest_address(), overlapped_ptr.guest_address());
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_NOT_FOUND);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamReadBiometricData, kNone, kStub);

void XamNuiPlayerEngagementUpdate_entry(
    lpunknown_t unk1, unknown_t unk2,
    pointer_t<X_NUI_PLAYER_ENGAGEMENT_UPDATE> engagement_data) {
  LogNuiCall("NUI: XamNuiPlayerEngagementUpdate({:08X}, {:08X}, {:08X})",
             unk1.guest_address(), unk2.value(),
             engagement_data.guest_address());
  if (engagement_data && nui_state()->IsGuestRangeWritable(
                             engagement_data.guest_address(),
                             sizeof(X_NUI_PLAYER_ENGAGEMENT_UPDATE))) {
    ClearNuiPlayerEngagement(engagement_data);
  }
}
DECLARE_XAM_EXPORT1(XamNuiPlayerEngagementUpdate, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
