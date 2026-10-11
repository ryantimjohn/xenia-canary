/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_STATE_H_
#define XENIA_KERNEL_XAM_NUI_STATE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/nui_abi.h"
#include "xenia/kernel/xam/nui_guide_gesture.h"
#include "xenia/kernel/xam/nui_guide_session.h"
#include "xenia/kernel/xam/nui_keepalive.h"
#include "xenia/kernel/xam/nui_messages.h"
#include "xenia/kernel/xam/nui_runtime_state.h"
#include "xenia/kernel/xam/nui_tilt_status.h"
#include "xenia/nui/nui_system.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class NuiCamera;
struct NuiCameraStats;

// What the last HUD skeleton frame (message 0x2B002) said, kept so that the
// log and the depth preview overlay can report what the title's own tracker is
// finding without having to read guest memory themselves.
struct NuiHudSkeletonFrame {
  // How many frames have arrived since the title started.
  uint64_t frames = 0;
  // Per skeleton slot: NUI_SKELETON_NOT_TRACKED, _POSITION_ONLY or _TRACKED,
  // and the id the tracker gave whoever is in that slot.
  uint32_t tracking_states[kNuiSkeletonCount] = {};
  uint32_t tracking_ids[kNuiSkeletonCount] = {};
  // The two engaged-id fields as the handler read them out of the guest's
  // frame, before anything was written back. xam writes them only while the
  // Kinect Guide has taken the frame's skeleton, the one case in which the
  // title reads them out again.
  uint32_t engaged_tracking_ids[2] = {};
  // The plane the tracker fitted to the floor, in the frame's own order: the
  // three components of its unit normal and the sensor's distance from it in
  // metres.
  float floor_clip_plane[4] = {};
};

// Guest-visible state of the NUI (Kinect) device. The host side of the sensor
// lives in xe::nui::NuiSystem; this class holds everything that only makes
// sense to the guest, such as the HUD state a title hands over and how long it
// wants the camera kept awake, and translates the app 0xFE messages in the
// 0x0002Bxxx and 0x0002Cxxx ranges into calls on the host system.
class NuiState {
 public:
  explicit NuiState(KernelState* kernel_state);
  ~NuiState();

  // Stops the camera's frame feed. Must run before either the host NUI system
  // or the guest goes away, because the feed touches both.
  void Shutdown();

  // XNuiTitleShutdown ends a runtime registration, not the host sensor or
  // camera transport. A running title may initialize the runtime again.
  void ResetRuntime(bool broadcast_resume);

  NuiRuntimeState& runtime_state() { return runtime_state_; }
  const NuiRuntimeState& runtime_state() const { return runtime_state_; }

  // The title is ending and its threads are still running: refuse every new
  // camera request and stop and join the frame feed, so that a completion
  // routine which is in flight finishes against a title that is still whole.
  // Called from KernelState::TerminateTitle, before it kills those threads.
  void OnTitleTerminating();

  // The title's threads are gone and its modules are not unloaded yet: drop
  // what it left queued without calling into it, and put back everything that
  // belonged to it, so that the next title in this process starts from a clean
  // NUI. The host NuiSystem and the device-status callback are the emulator's
  // and are left alone. Called from KernelState::TerminateTitle, after the
  // kill loop and before the unload, and always after OnTitleTerminating().
  void OnTitleTerminated();

  // Handles an app 0xFE message in the 0x0002Bxxx or 0x0002Cxxx range, or one
  // of the two identity I/O requests in the 0x0002Axxx range.
  X_HRESULT DispatchNuiMessage(uint32_t message, uint32_t buffer_ptr,
                               uint32_t buffer_length);

  // Handles a camera request from the xboxkrnl export PsCamDeviceRequest;
  // request_ptr points at a NuiCameraRequest.
  X_STATUS HandleCameraRequest(uint32_t request_ptr);

  // What the camera transport is doing, for the depth preview overlay.
  NuiCameraStats GetCameraStats() const;

  // What the last HUD skeleton frame carried, for the same overlay.
  NuiHudSkeletonFrame GetHudSkeletonFrame() const;

  // True when a sensor is attached, ready, and not hidden by the user.
  bool IsDevicePresent() const;

  void FillDeviceStatus(X_NUI_DEVICE_STATUS* out) const;

  X_RESULT GetTiltDegrees(int32_t* out_degrees);
  // Reads the angle without requesting the backend's tilt thread.
  X_RESULT PeekTiltDegrees(int32_t* out_degrees);
  X_RESULT SetTiltDegrees(int32_t degrees);
  // The accelerometer legs the tilt status block carries for the sensor as it
  // is now, and the angle it reports: from the gravity its accelerometer
  // measured when there is a reading, else from the angle, which is
  // nui_tilt_override's when that is set (the backends hand out no reading
  // then). False, with nothing written but a zero angle, when the angle
  // cannot be read, which is when XamNuiCameraTiltGetStatus fills nothing.
  bool GetTiltStatusLegs(NuiTiltStatusLegs* legs, int32_t* degrees);
  // The gravity vector the sensor's accelerometer last measured, in g on the
  // host runtime's axes (xe::nui::NuiSystem::GetGravityVector). False when
  // there is none to be had, and the tilt angle then stands in for it.
  bool GetGravityVector(float* xyz) const {
    return IsDevicePresent() && nui_system()->GetGravityVector(xyz);
  }
  // True while the sensor's tilt motor is on its way somewhere
  // (xe::nui::NuiSystem::IsTiltMotorMoving).
  bool IsTiltMotorMoving() const {
    return IsDevicePresent() && nui_system()->IsTiltMotorMoving();
  }

  // Hides an attached sensor from the guest for as long as it is set.
  void SetForceDeviceOff(bool off);

  uint32_t hud_init_flags() const { return runtime_state_.snapshot().flags; }
  uint64_t st_version_major() const {
    return runtime_state_.snapshot().xam_version;
  }
  uint64_t st_version_build() const {
    return runtime_state_.snapshot().xtl_version;
  }

  // The floor the title last handed XamNuiCameraRememberFloor while a sensor
  // was present (see NuiRememberedFloor), which XamNuiCameraTiltGetStatus
  // hands back in the tilt status block's tail. Empty until the title has
  // remembered one, and emptied again when the title ends.
  NuiRememberedFloor remembered_floor() const;
  // Remembers the plane and the flag the title handed over, with the legs
  // GetTiltStatusLegs gives at this moment, so that the tail pairs the plane
  // with the gravity it was fitted under. The title calls it 700-2,500 times
  // a session, each time right after a poll of the block.
  void RememberFloor(const std::array<float, 4>& plane, uint32_t flag);

  uint32_t engaged_tracking_id() const {
    return runtime_state_.snapshot().engaged_tracking_id;
  }
  void set_engaged_tracking_id(uint32_t id) {
    runtime_state_.SetEngagedTrackingId(id);
  }

  // The HUD is only usable once a title has initialized it (message 0x2B003)
  // and a device is actually present. The pair of file-static gates this
  // replaces behaved the same way.
  bool hud_enabled() const;

  // True while an unexpired 0x2B001 camera keep-alive request is outstanding.
  bool IsCameraKeepAliveActive() const;

  // The Kinect Guide, xam's system HUD. A player opens it by holding the
  // guide gesture (NuiGuideGesture) in the skeleton frames the title hands
  // over with 0x2B002, which pauses the title; it closes on the same player's
  // gesture again, or through CloseGuide. Nothing else opens or closes it.

  // Returns whether the guide is up.
  bool guide_open() const;

  // Returns the tracking id of the player whose gesture opened the guide, or
  // 0 while it is closed.
  uint32_t guide_tracking_id() const;

  // Writes where that player's higher hand points on the guide's overlay,
  // each of x and y from 0 to 1 (see MapNuiGuideHand), as of the last frame.
  // Returns false while the guide is closed, while the last frame did not
  // show the player, and while neither of the player's hands can be seen.
  bool GuideHandPosition(float* x, float* y) const;

  // Sets the function called with true when the guide opens and false when it
  // closes, on whatever thread opened or closed it; null removes it. Setting
  // it waits for a call that is already running to return. The observer must
  // not call back into this object.
  void SetGuideObserver(std::function<void(bool open)> observer);

  // Closes the guide, if it is up, for the reason given, such as "escape" or
  // "hover": broadcasts kXNotificationSystemNUIPause with 0, forgets the
  // player and logs the reason.
  void CloseGuide(const char* reason);

  // Guest memory helpers, shared with the camera transport. A request or a
  // message arrives with pointers the title chose, and an address inside a
  // heap can still sit on pages that are not committed or not readable.

  // True when length bytes can really be read from the guest at address.
  bool IsGuestRangeReadable(uint32_t address, uint32_t length) const;

  // True when length bytes at address can be written as well as read.
  bool IsGuestRangeWritable(uint32_t address, uint32_t length) const;

  // Hex dump of length bytes of guest memory, cut short at the end of the
  // page or heap they start in.
  std::string DumpGuestBytes(uint32_t address, uint32_t length) const;

 private:
  // The host NUI system, or null when the emulator was created without one.
  xe::nui::NuiSystem* nui_system() const;

  // Invoked by the backend on its own thread whenever the sensor's status
  // changes. Only does work that is safe from a foreign thread.
  void OnDeviceStatusChanged(xe::nui::NuiSystem::DeviceStatus status);

  // Sets the xconfig retail flag that marks Kinect as set up, if it is clear.
  void MarkKinectInitialized();

  // Turns xenia's resolve and memory-export read-back on, once, so that a
  // title's skeletal tracker can read its own GPU results back out of guest
  // memory. A setting that is not at its compiled default was chosen by
  // somebody and is left as it is.
  void EnableSkeletalTrackerReadback();

  // Puts back exactly the settings the call above raised, so that a title
  // opened into this process after a Kinect one is emulated the way it would
  // have been on its own. A setting somebody chose was never raised and is
  // not touched here either.
  void RestoreSkeletalTrackerReadback();

  X_HRESULT HandleCameraKeepAlive(uint32_t buffer_ptr, uint32_t buffer_length);
  X_HRESULT HandleHudInitialize(uint32_t buffer_ptr, uint32_t buffer_length);
  X_HRESULT HandleHudSkeletonFrame(uint32_t buffer_ptr, uint32_t buffer_length);
  X_HRESULT HandleTitleShutdown();

  // Feeds a HUD skeleton frame, number frame_number, to the Kinect Guide: to
  // the gesture recogniser when nui_guide_gesture is on, which may open or
  // close the guide, and to the guide's hand position while it is up. With
  // nui_log_messages set it logs the recogniser's trace of the frame. Returns
  // whether the guide takes the frame, and then writes the tracking id it has
  // taken to taken_id.
  bool UpdateGuideFromFrame(const NuiMsg2B002& guest_frame,
                            uint64_t frame_number, uint32_t* taken_id);

  // Logs what the title's own tracker found in a HUD skeleton frame, for
  // nui_log_play_frames: a line for each tracked skeleton, one for the floor
  // and the tilt, and one each for the player-index and depth textures, all
  // for frame number frame_number. The frame is the guest's own buffer, and
  // the textures are read out of the guest as they are at that moment.
  void LogPlayFrame(const NuiMsg2B002& guest_frame, uint64_t frame_number);

  // Logs one line summarising a HUD texture, which is one of two streams of
  // 16-bit words that its pitch tells apart (nui_player_mask.h). It is the
  // player-index texture when player_mask is set and the depth texture
  // otherwise. Says so instead when the texture cannot be read.
  void LogPlayTexture(uint64_t frame_number, bool player_mask,
                      uint32_t bits_ptr, uint32_t pitch);

  // Returns where length bytes at a guest address the title chose lie in host
  // memory, or null when they cannot all be read. Unlike IsGuestRangeReadable
  // it also sees pages of graphics memory that were allocated through another
  // view of the physical memory than the one the address is in.
  const uint8_t* TranslateReadableGuestRange(uint32_t address,
                                             uint64_t length) const;

  // Does what opening or closing the guide entails beyond the state: the
  // engaged tracking id, the log line, kXNotificationSystemNUIPause when
  // broadcast is set, and the observer. Called with guide_transition_mutex_
  // held.
  void AnnounceGuideChange(bool open, uint32_t tracking_id, const char* reason,
                           bool broadcast);

  // Closes the guide, if it is up, and announces it for the reason given,
  // broadcasting the pause notification only when broadcast is set. Called
  // with guide_transition_mutex_ held.
  void CloseGuideLocked(const char* reason, bool broadcast);

  X_HRESULT HandleTiltStatusReport(uint32_t message, uint32_t buffer_ptr,
                                   uint32_t buffer_length);
  X_HRESULT HandleIdentityIoRequest(uint32_t message, uint32_t buffer_length);
  X_HRESULT HandleIdentityRegisterCallback(uint32_t buffer_ptr,
                                           uint32_t buffer_length);
  X_HRESULT HandleIdentityRegisterFeature(uint32_t buffer_ptr,
                                          uint32_t buffer_length);

  // Logs a message and the code it returned. Known messages are only logged
  // when nui_log_messages is set; unknown ones are always logged as warnings,
  // because they are what Kinect support is still missing.
  void LogMessage(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
                  X_HRESULT result, bool known) const;

  // Reads a string of the given length out of guest memory, cut short at a
  // sane maximum. Empty when the range cannot be read.
  std::string ReadGuestString(uint32_t address, uint32_t length) const;

  // Hex dump of the message buffer, at least 32 bytes long.
  std::string DumpMessageBuffer(uint32_t buffer_ptr,
                                uint32_t buffer_length) const;

  KernelState* kernel_state_;

  // The depth and colour camera transport behind PsCamDeviceRequest.
  std::unique_ptr<NuiCamera> camera_;

  std::atomic<bool> force_off_ = {false};

  // Runtime reset, initialization and frame publication happen in order.
  std::mutex runtime_transition_mutex_;
  NuiRuntimeState runtime_state_;
  std::atomic<bool> readback_enabled_for_tracker_ = {false};
  // Which of the two read-back settings this title's registration raised, so
  // that the title's end puts back those and only those.
  std::atomic<bool> readback_raised_resolve_ = {false};
  std::atomic<bool> readback_raised_memexport_ = {false};

  // Guards remembered_floor_, whose plane and legs are one call's worth of
  // state and are only ever read and written together.
  mutable std::mutex remembered_floor_mutex_;
  NuiRememberedFloor remembered_floor_;

  // Guards keep_alive_, which guest threads both read and write.
  mutable std::mutex keep_alive_mutex_;
  NuiKeepAlive keep_alive_;

  // Guards hud_skeleton_frame_, which a guest thread writes once per frame and
  // the UI thread reads.
  mutable std::mutex hud_skeleton_frame_mutex_;
  NuiHudSkeletonFrame hud_skeleton_frame_;

  // Held for the whole of anything that can open or close the Kinect Guide,
  // its announcement included, so that the pause notifications and the
  // observer's calls go out in the order the changes happened. It also guards
  // guide_observer_, guide_frame_ and guide_taking_. Taken before
  // guide_mutex_, never after it.
  std::mutex guide_transition_mutex_;
  std::function<void(bool open)> guide_observer_;
  // This is a copy of the frame being handled: the title may be building the
  // next one in the guest's buffer while the guide reads this one.
  NuiMsg2B002 guide_frame_ = {};
  // This records whether the last frame was taken, for logging the changes.
  bool guide_taking_ = false;

  // Guards the guide's state, which the guest thread handling the frames and
  // the UI thread both read and write.
  mutable std::mutex guide_mutex_;
  NuiGuideSession guide_session_;
  // The recogniser is made on the first frame it sees, with the settings the
  // cvars hold then, and dropped when the title ends.
  std::unique_ptr<NuiGuideGesture> guide_gesture_;
  bool guide_hand_valid_ = false;
  float guide_hand_x_ = 0.0f;
  float guide_hand_y_ = 0.0f;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_STATE_H_
