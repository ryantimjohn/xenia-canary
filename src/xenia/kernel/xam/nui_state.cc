/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_state.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_range.h"
#include "xenia/kernel/xam/nui_camera.h"
#include "xenia/kernel/xam/nui_guide_frame.h"
#include "xenia/kernel/xam/nui_messages.h"
#include "xenia/kernel/xam/nui_player_mask.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/nui/nui_flags.h"
#include "xenia/nui/nui_gpu_readback.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

// Smallest hex dump a message gets, so that a message whose payload size we do
// not know still shows something useful.
constexpr uint32_t kMinimumDumpBytes = 32;

// This answer to a HUD skeleton frame tells the title's NUI runtime that the
// system HUD has taken the skeleton (0x82d35ad8).
constexpr X_HRESULT kNuiHudTookSkeleton = 0x8301000B;

const char* GuideArmName(NuiGuideArm arm) {
  switch (arm) {
    case NuiGuideArm::kLeft:
      return "left";
    case NuiGuideArm::kRight:
      return "right";
    default:
      return "no";
  }
}

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Formats a point in the sensor's camera space, in metres to the millimetre,
// for the play-frame log.
std::string FormatNuiPoint(const NuiGuideVector& point) {
  return fmt::format("({:.3f}, {:.3f}, {:.3f})", point.x, point.y, point.z);
}

// Returns value when the frame showed what it measures, and otherwise not a
// number, which the trace prints as nan.
float ShownOrNan(bool shown, float value) {
  return shown ? value : std::numeric_limits<float>::quiet_NaN();
}

// Logs, at Debug, what the guide gesture's recogniser measured of a skeleton
// on frame frame_number: a line for each arm that is in the band or whose hold
// started, broke or completed on the frame. See "The Kinect Guide" in
// docs/nui.md for how to read it.
void LogGuideTrace(uint64_t frame_number, const NuiGuideTrace& trace) {
  for (size_t i = 0; i < trace.arms.size(); ++i) {
    const NuiGuideArmTrace& arm = trace.arms[i];
    if (!arm.in_band && !arm.hold_started && !arm.hold_broken &&
        !arm.completed) {
      continue;
    }
    const NuiGuideArmMeasurement& measurement = arm.measurement;
    std::string events;
    if (arm.hold_broken) {
      events += "; hold broken";
    }
    if (arm.hold_started) {
      events += "; hold started";
    }
    if (arm.completed) {
      events += "; completed";
    }
    if (!arm.armed) {
      events += "; disarmed";
    }
    XELOGD(
        "NUI: guide trace frame {}: id {} {} arm: angle {:.1f} extension "
        "{:.2f} sideways {:.2f} other arm {:.1f} other hand {:.3f} lean {:.1f} "
        "arm turned {:.1f} hip moved {:.3f} held {} ms; fails {}{}",
        frame_number, trace.tracking_id,
        GuideArmName(i == 0 ? NuiGuideArm::kLeft : NuiGuideArm::kRight),
        ShownOrNan(measurement.seen, measurement.angle_degrees),
        ShownOrNan(measurement.seen, measurement.extension),
        ShownOrNan(measurement.seen, measurement.sideways),
        ShownOrNan(measurement.other_arm_seen, measurement.other_angle_degrees),
        ShownOrNan(measurement.other_hand_seen, measurement.other_hand_height),
        trace.lean_degrees, arm.arm_turned_degrees, arm.hip_moved,
        arm.holding ? arm.held_us / 1000 : 0, FormatNuiGuideRules(arm.failed),
        events);
  }
}

const char* DeviceStatusName(xe::nui::NuiSystem::DeviceStatus status) {
  switch (status) {
    case xe::nui::NuiSystem::DeviceStatus::kNotPresent:
      return "not present";
    case xe::nui::NuiSystem::DeviceStatus::kInitializing:
      return "initializing";
    case xe::nui::NuiSystem::DeviceStatus::kReady:
      return "ready";
    case xe::nui::NuiSystem::DeviceStatus::kError:
      return "error";
    default:
      return "unknown";
  }
}

}  // namespace

NuiState::NuiState(KernelState* kernel_state) : kernel_state_(kernel_state) {
  // Note that the kernel's xconfig does not exist yet at this point, so the
  // constructor must not read any settings.
  camera_ = std::make_unique<NuiCamera>(this, kernel_state);
  auto* system = nui_system();
  if (system) {
    system->SetDeviceStatusCallback(
        [this](xe::nui::NuiSystem::DeviceStatus status) {
          OnDeviceStatusChanged(status);
        });
  }
}

NuiState::~NuiState() {
  // The emulator shuts the camera down before it tears the host NUI system
  // down; this is only the backstop for anything that did not.
  Shutdown();
  // Unregistering waits for any status callback that is already running to
  // come back out, so the backend cannot be inside this object once this
  // returns. See the contract on NuiSystem::SetDeviceStatusCallback.
  auto* system = nui_system();
  if (system) {
    system->SetDeviceStatusCallback(nullptr);
  }
}

void NuiState::Shutdown() { camera_->Shutdown(); }

void NuiState::OnTitleTerminating() {
  // Only the camera has anything to quiesce: it is the part with a thread of
  // its own, and that thread is the only one here that runs guest code. The
  // rest of this object is data the title left behind, which cannot be put
  // back until the title's threads have stopped reading it - that is the
  // second half, OnTitleTerminated() below. Nothing here calls into the guest,
  // and no lock of this class is held while the camera waits for its feed.
  camera_->OnTitleTerminating();
}

void NuiState::OnTitleTerminated() {
  // The camera goes first: it is the part with the guest's addresses in it.
  // Its feed was already stopped and joined by OnTitleTerminating(), while the
  // title's threads were still alive, so nothing below can be racing a frame
  // delivery or a completion routine.
  const NuiCameraTermination termination = camera_->OnTitleTerminated();

  ResetRuntime(false);

  // A feed that stopped and requests that were deliberately never answered are
  // exactly what a later "the title froze waiting for a completion" question
  // needs, so a title that used the camera gets a line in any log. This hook
  // runs for every title, Kinect or not, and a title that never touched the
  // camera has nothing to say at Info.
  if (termination.feed_stopped || termination.depth || termination.colour) {
    XELOGI(
        "NUI: title terminated: frame feed stopped, {} depth and {} colour "
        "requests dropped.",
        termination.depth, termination.colour);
  } else {
    XELOGD("NUI: title terminated: the camera was never used.");
  }
}

void NuiState::ResetRuntime(bool broadcast_resume) {
  // Serialize reset with initialization and frames so an in-flight frame
  // cannot reopen the Guide or restore stale tracking metadata after reset.
  std::lock_guard<std::mutex> transition(runtime_transition_mutex_);
  // Clear runtime-owned data while leaving the emulator's sensor and status
  // callback intact. XNuiTitleShutdown can be followed by another initialize
  // in the same title; full title termination quiesces the camera separately.
  runtime_state_.Reset();
  {
    std::lock_guard<std::mutex> lock(remembered_floor_mutex_);
    remembered_floor_ = NuiRememberedFloor();
  }
  {
    std::lock_guard<std::mutex> lock(keep_alive_mutex_);
    keep_alive_ = NuiKeepAlive();
  }
  {
    std::lock_guard<std::mutex> lock(hud_skeleton_frame_mutex_);
    hud_skeleton_frame_ = NuiHudSkeletonFrame();
  }
  {
    std::lock_guard<std::mutex> transition(guide_transition_mutex_);
    CloseGuideLocked("runtime shutdown", broadcast_resume);
    {
      std::lock_guard<std::mutex> lock(guide_mutex_);
      guide_gesture_.reset();
    }
    guide_taking_ = false;
    guide_frame_ = {};
  }
  RestoreSkeletalTrackerReadback();
}

xe::nui::NuiSystem* NuiState::nui_system() const {
  auto* emulator = kernel_state_->emulator();
  return emulator ? emulator->nui_system() : nullptr;
}

bool NuiState::IsDevicePresent() const {
  auto* system = nui_system();
  if (!system || !system->IsDevicePresent()) {
    return false;
  }
  if (force_off_.load(std::memory_order_relaxed)) {
    return false;
  }
  // The user can hide an attached sensor from the guest with the dashboard's
  // Kinect switch, which lives in the retail flags. The kernel builds XamState
  // before it builds the xconfig, so there is a window where there is nothing
  // to ask.
  auto* xconfig = kernel_state_->xconfig();
  if (!xconfig) {
    return false;
  }
  const uint32_t retail_flags = xconfig->ReadSetting<uint32_t>(
      X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS);
  return (retail_flags & X_RETAIL_FLAGS::KinectDisabled) == 0;
}

void NuiState::FillDeviceStatus(X_NUI_DEVICE_STATUS* out) const {
  FillNuiDeviceStatus(out, IsDevicePresent());
}

X_RESULT NuiState::GetTiltDegrees(int32_t* out_degrees) {
  *out_degrees = 0;
  auto* system = nui_system();
  if (!IsDevicePresent() || !system->GetTiltDegrees(out_degrees)) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NuiState::PeekTiltDegrees(int32_t* out_degrees) {
  *out_degrees = 0;
  auto* system = nui_system();
  if (!IsDevicePresent() || !system->PeekTiltDegrees(out_degrees)) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

bool NuiState::GetTiltStatusLegs(NuiTiltStatusLegs* legs, int32_t* degrees) {
  if (GetTiltDegrees(degrees) != X_ERROR_SUCCESS) {
    return false;
  }
  float gravity[3];
  *legs = GetGravityVector(gravity) ? NuiTiltStatusLegsFromGravity(gravity)
                                    : NuiTiltStatusLegsForAngle(*degrees);
  return true;
}

X_RESULT NuiState::SetTiltDegrees(int32_t degrees) {
  auto* system = nui_system();
  if (!IsDevicePresent() || !system->SetTiltDegrees(degrees)) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

void NuiState::SetForceDeviceOff(bool off) {
  force_off_.store(off, std::memory_order_relaxed);
  XELOGI("NUI: device forced {}", off ? "off" : "on");
}

bool NuiState::hud_enabled() const {
  return runtime_state_.snapshot().initialized && IsDevicePresent();
}

bool NuiState::IsCameraKeepAliveActive() const {
  std::lock_guard<std::mutex> lock(keep_alive_mutex_);
  return keep_alive_.IsActive(NowUs());
}

void NuiState::OnDeviceStatusChanged(xe::nui::NuiSystem::DeviceStatus status) {
  // Runs on the backend's thread, so nothing here may touch state that a guest
  // thread owns.
  const bool ready = status == xe::nui::NuiSystem::DeviceStatus::kReady;
  if (ready) {
    // Set the retail flag before the guest hears about the change, so that a
    // title reading it while handling the notification sees the new value.
    MarkKinectInitialized();
  }
  XELOGI("NUI: device status is now {}, notifying the guest with {}",
         DeviceStatusName(status), ready ? 1 : 0);
  kernel_state_->BroadcastNotification(
      kXNotificationSystemNUIHardwareStatusChanged, ready ? 1 : 0);
}

void NuiState::MarkKinectInitialized() {
  auto* xconfig = kernel_state_->xconfig();
  if (!xconfig) {
    return;
  }
  const uint32_t retail_flags = xconfig->ReadSetting<uint32_t>(
      X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS);
  if (retail_flags & X_RETAIL_FLAGS::KinectInitialized) {
    return;
  }
  const xe::be<uint32_t> new_flags =
      retail_flags | X_RETAIL_FLAGS::KinectInitialized;
  xconfig->WriteSetting(X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY,
                        XCONFIG_USER_RETAIL_FLAGS, &new_flags);
  XELOGI("NUI: marked Kinect as initialized in xconfig");
}

void NuiState::EnableSkeletalTrackerReadback() {
  if (readback_enabled_for_tracker_.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  const gpu::ReadbackResolveMode resolve_mode = gpu::GetReadbackResolveMode();
  const xe::nui::GpuReadbackPlan plan = xe::nui::PlanSkeletalTrackerGpuReadback(
      resolve_mode == gpu::ReadbackResolveMode::kDisabled,
      gpu::GetGPUSetting(gpu::GPUSetting::ReadbackMemexport));
  // Both settings are raised for this session only. Xenia is deciding this for
  // the user rather than being asked for it, so it must not end up in the
  // config the user keeps: a title that needs the stall should not leave every
  // other title paying it.
  if (plan.enable_resolve) {
    gpu::SetReadbackResolveModeForSession("full");
    readback_raised_resolve_.store(true, std::memory_order_relaxed);
  }
  if (plan.enable_memexport) {
    gpu::SetGPUSettingForSession(gpu::GPUSetting::ReadbackMemexport, 1);
    readback_raised_memexport_.store(true, std::memory_order_relaxed);
  }
  if (plan.enables_resolve_and_memexport()) {
    XELOGI(
        "NUI: enabling resolve and memexport read-back for the skeletal "
        "tracker");
  } else if (plan.enable_resolve) {
    XELOGI("NUI: enabling resolve read-back for the skeletal tracker");
  } else if (plan.enable_memexport) {
    XELOGI("NUI: enabling memexport read-back for the skeletal tracker");
  }
  if (plan.resolve_configured) {
    // The only two modes that are not the compiled default.
    XELOGI("NUI: resolve read-back is already configured as {}, leaving it",
           resolve_mode == gpu::ReadbackResolveMode::kFull ? "full" : "fast");
  }
  if (plan.memexport_configured) {
    XELOGI("NUI: memexport read-back is already on, leaving it");
  }
}

void NuiState::RestoreSkeletalTrackerReadback() {
  if (!readback_enabled_for_tracker_.exchange(false,
                                              std::memory_order_relaxed)) {
    return;
  }
  // Back to the compiled defaults, which is where they were: a value that was
  // anything else counted as chosen and was never raised in the first place.
  if (readback_raised_resolve_.exchange(false, std::memory_order_relaxed)) {
    gpu::SetReadbackResolveModeForSession("none");
  }
  if (readback_raised_memexport_.exchange(false, std::memory_order_relaxed)) {
    gpu::SetGPUSettingForSession(gpu::GPUSetting::ReadbackMemexport, 0);
  }
  XELOGI("NUI: skeletal tracker read-back settings put back");
}

X_HRESULT NuiState::DispatchNuiMessage(uint32_t message, uint32_t buffer_ptr,
                                       uint32_t buffer_length) {
  X_HRESULT result = X_E_FAIL;
  bool known = true;
  switch (message) {
    case 0x0002B001: {
      result = HandleCameraKeepAlive(buffer_ptr, buffer_length);
    } break;
    case 0x0002B002: {
      result = HandleHudSkeletonFrame(buffer_ptr, buffer_length);
    } break;
    case 0x0002B003: {
      result = HandleHudInitialize(buffer_ptr, buffer_length);
    } break;
    case 0x0002B004: {
      result = HandleTitleShutdown();
    } break;
    case 0x0002B006:
    case 0x0002B00C: {
      result = HandleTiltStatusReport(message, buffer_ptr, buffer_length);
    } break;
    case 0x0002A005:
    case 0x0002A006:
    case 0x0002A007:
    case 0x0002A008: {
      result = HandleIdentityIoRequest(message, buffer_length);
    } break;
    case 0x0002C009: {
      result = HandleIdentityRegisterCallback(buffer_ptr, buffer_length);
    } break;
    case 0x0002C00C: {
      result = HandleIdentityRegisterFeature(buffer_ptr, buffer_length);
    } break;
    default: {
      // Everything else, the rest of the 0x2Cxxx identity range included, is
      // still a mystery; fail it the way an unhandled XAM message would and
      // log the payload so that it can be worked out later.
      known = false;
    } break;
  }
  LogMessage(message, buffer_ptr, buffer_length, result, known);
  return result;
}

X_HRESULT NuiState::HandleCameraKeepAlive(uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  std::lock_guard<std::mutex> transition(runtime_transition_mutex_);
  uint32_t timeout_ms = 0;
  if (NuiMessageBufferFits(buffer_length, sizeof(NuiMsg2B001)) &&
      IsGuestRangeReadable(buffer_ptr, sizeof(NuiMsg2B001))) {
    const auto* args =
        kernel_state_->memory()->TranslateVirtual<const NuiMsg2B001*>(
            buffer_ptr);
    timeout_ms = args->timeout_ms;
  }

  {
    std::lock_guard<std::mutex> lock(keep_alive_mutex_);
    keep_alive_.Request(timeout_ms, NowUs());
  }

  if (cvars::nui_log_messages) {
    XELOGI("NUI: camera keep-alive requested for {} ms", timeout_ms);
  }

  return IsDevicePresent() ? X_E_SUCCESS : X_E_DEVICE_NOT_CONNECTED;
}

X_HRESULT NuiState::HandleHudInitialize(uint32_t buffer_ptr,
                                        uint32_t buffer_length) {
  if (!NuiMessageBufferFits(buffer_length, sizeof(NuiMsg2B003)) ||
      !IsGuestRangeReadable(buffer_ptr, sizeof(NuiMsg2B003))) {
    return X_E_INVALIDARG;
  }
  // Copy the guest's request once before validating and publishing it.
  const NuiMsg2B003 args =
      *kernel_state_->memory()->TranslateVirtual<const NuiMsg2B003*>(
          buffer_ptr);
  std::lock_guard<std::mutex> transition(runtime_transition_mutex_);
  if (!runtime_state_.Initialize(args.version_major, args.version_build,
                                 args.flags)) {
    return X_E_FAIL;
  }
  if (cvars::nui_log_messages) {
    XELOGI("NUI: HUD initialized with version {}.{}, flags {:08X}",
           args.version_major.get(), args.version_build.get(),
           args.flags.get());
  }
  // Enable only after a valid registration; invalid messages cannot change
  // another title's GPU read-back settings.
  EnableSkeletalTrackerReadback();
  return X_E_SUCCESS;
}

X_HRESULT NuiState::HandleHudSkeletonFrame(uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  std::lock_guard<std::mutex> transition(runtime_transition_mutex_);
  if (!runtime_state_.snapshot().initialized) {
    return X_ERROR_ACCESS_DENIED;
  }
  // The title hands this over once per frame it processes: the six skeleton
  // slots its own tracker has just filled in, the floor plane, and the two
  // locked textures a system silhouette would be drawn from. On a console it
  // is how the Kinect Guide watches for its gesture and draws over a game, and
  // the answer matters a great deal. The title reads 0x8301000B as "the system
  // HUD has taken the skeleton": it sets [g+96], which makes the next
  // published frame carry status 2 (0x82d23260), which makes
  // NuiSkeletonGetNextFrame return 0x8301000B (0x82d2788c), which makes the
  // game skip its own skeleton processing altogether (0x82a54e14). So xam
  // answers with that code only while the Kinect Guide is up and the frame
  // holds the player whose gesture opened it, and plain success otherwise,
  // which is what a console with the guide closed behaves like.
  //
  // An unreadable frame cannot be taken by the system HUD.
  if (!NuiMessageBufferFits(buffer_length, sizeof(NuiMsg2B002)) ||
      !IsGuestRangeReadable(buffer_ptr, sizeof(NuiMsg2B002))) {
    return X_E_INVALIDARG;
  }

  const auto* args =
      kernel_state_->memory()->TranslateVirtual<const NuiMsg2B002*>(buffer_ptr);

  NuiHudSkeletonFrame frame;
  for (size_t i = 0; i < kNuiSkeletonCount; i++) {
    frame.tracking_states[i] = args->skeletons[i].tracking_state;
    frame.tracking_ids[i] = args->skeletons[i].tracking_id;
  }
  frame.engaged_tracking_ids[0] = args->engaged_tracking_id[0];
  frame.engaged_tracking_ids[1] = args->engaged_tracking_id[1];
  frame.floor_clip_plane[0] = args->floor_clip_plane.x;
  frame.floor_clip_plane[1] = args->floor_clip_plane.y;
  frame.floor_clip_plane[2] = args->floor_clip_plane.z;
  frame.floor_clip_plane[3] = args->floor_clip_plane.w;

  bool tracking_changed = false;
  {
    std::lock_guard<std::mutex> lock(hud_skeleton_frame_mutex_);
    tracking_changed = !std::equal(
        std::begin(frame.tracking_states), std::end(frame.tracking_states),
        std::begin(hud_skeleton_frame_.tracking_states));
    frame.frames = hud_skeleton_frame_.frames + 1;
    hud_skeleton_frame_ = frame;
  }

  // Thousands of these arrive in a run, so only a change of what the tracker
  // is finding is worth a line.
  if (tracking_changed && cvars::nui_log_messages) {
    std::string states;
    std::string ids;
    for (size_t i = 0; i < kNuiSkeletonCount; i++) {
      fmt::format_to(std::back_inserter(states), "{}{}", i ? " " : "",
                     frame.tracking_states[i]);
      fmt::format_to(std::back_inserter(ids), "{}{}", i ? " " : "",
                     frame.tracking_ids[i]);
    }
    XELOGI(
        "NUI: HUD skeleton frame {}: tracking states [{}], tracking ids [{}], "
        "floor plane {} {} {} {}",
        frame.frames, states, ids, frame.floor_clip_plane[0],
        frame.floor_clip_plane[1], frame.floor_clip_plane[2],
        frame.floor_clip_plane[3]);
  }

  // The play-frame log is a diagnostic that changes nothing the title or the
  // guide sees, so it comes before the guide has its say and reads nothing at
  // all while nui_log_play_frames is off.
  const int32_t play_log_interval = cvars::nui_log_play_frames;
  if (play_log_interval > 0 &&
      frame.frames % static_cast<uint64_t>(play_log_interval) == 0) {
    LogPlayFrame(*args, frame.frames);
  }

  uint32_t taken_id = 0;
  if (!UpdateGuideFromFrame(*args, frame.frames, &taken_id)) {
    return X_E_SUCCESS;
  }
  // Having answered 0x8301000B, the runtime reads the two engaged ids back out
  // of the frame (0x82d35b08, 0x82d35b20) as the players the system HUD has
  // taken, so xam names the one the guide took. The ids in the frame's slots
  // already carry the runtime's renumbering base, which it takes off again.
  const uint32_t engaged_ids_ptr =
      buffer_ptr +
      static_cast<uint32_t>(offsetof(NuiMsg2B002, engaged_tracking_id));
  constexpr uint32_t kEngagedIdsLength =
      sizeof(NuiMsg2B002::engaged_tracking_id);
  if (IsGuestRangeWritable(engaged_ids_ptr, kEngagedIdsLength)) {
    auto* engaged_ids =
        kernel_state_->memory()->TranslateVirtual<uint8_t*>(engaged_ids_ptr);
    xe::store_and_swap<uint32_t>(engaged_ids, taken_id);
    xe::store_and_swap<uint32_t>(engaged_ids + sizeof(uint32_t), 0);
  }
  return kNuiHudTookSkeleton;
}

void NuiState::LogPlayFrame(const NuiMsg2B002& guest_frame,
                            uint64_t frame_number) {
  for (const X_NUI_SKELETON_DATA& slot : guest_frame.skeletons) {
    if (slot.tracking_state != kNuiSkeletonTracked) {
      continue;
    }
    const uint32_t tracking_id = slot.tracking_id;
    const NuiGuideVector position = {slot.position.x, slot.position.y,
                                     slot.position.z};
    const NuiGuideSkeleton skeleton = ReadNuiGuideSkeleton(slot);
    XELOGI(
        "NUI: play frame {}: id {} position {} hip {} head {} hand L {} R {} "
        "foot L {} R {}",
        frame_number, tracking_id, FormatNuiPoint(position),
        FormatNuiPoint(skeleton.joints[kHipCenter]),
        FormatNuiPoint(skeleton.joints[kHead]),
        FormatNuiPoint(skeleton.joints[kHandLeft]),
        FormatNuiPoint(skeleton.joints[kHandRight]),
        FormatNuiPoint(skeleton.joints[kFootLeft]),
        FormatNuiPoint(skeleton.joints[kFootRight]));
  }

  int32_t tilt_degrees = 0;
  const std::string tilt = PeekTiltDegrees(&tilt_degrees) == X_ERROR_SUCCESS
                               ? std::to_string(tilt_degrees)
                               : "unavailable";
  const X_NUI_VECTOR4& floor = guest_frame.floor_clip_plane;
  const X_NUI_VECTOR4& gravity = guest_frame.normal_to_gravity;
  XELOGI(
      "NUI: play frame {}: floor ({:.3f}, {:.3f}, {:.3f}, {:.3f}) gravity "
      "({:.3f}, {:.3f}, {:.3f}, {:.3f}) tilt {}",
      frame_number, static_cast<float>(floor.x), static_cast<float>(floor.y),
      static_cast<float>(floor.z), static_cast<float>(floor.w),
      static_cast<float>(gravity.x), static_cast<float>(gravity.y),
      static_cast<float>(gravity.z), static_cast<float>(gravity.w), tilt);

  LogPlayTexture(frame_number, true, guest_frame.player_index_bits,
                 guest_frame.player_index_pitch);
  LogPlayTexture(frame_number, false, guest_frame.depth_bits,
                 guest_frame.depth_pitch);
}

void NuiState::LogPlayTexture(uint64_t frame_number, bool player_mask,
                              uint32_t bits_ptr, uint32_t pitch) {
  const char* name = player_mask ? "mask" : "depth";
  if (!bits_ptr) {
    XELOGI("NUI: play frame {}: {} texture has no bits", frame_number, name);
    return;
  }
  const NuiHudTextureShape shape = NuiHudTextureShapeForPitch(pitch);
  if (!shape.valid()) {
    XELOGI(
        "NUI: play frame {}: {} bits {:08X} pitch {} is too narrow for either "
        "stream",
        frame_number, name, bits_ptr, pitch);
    return;
  }
  const char* shape_name = NuiHudTextureShapeName(shape);
  const uint8_t* bits = TranslateReadableGuestRange(
      bits_ptr, NuiHudTextureBytesRead(pitch, shape));
  if (!bits) {
    XELOGI("NUI: play frame {}: {} bits {:08X} pitch {} cannot be read",
           frame_number, name, bits_ptr, pitch);
    return;
  }

  NuiPlayerMaskSummary mask;
  SummariseNuiPlayerMask(bits, pitch, shape, &mask);
  if (player_mask) {
    if (!mask.pixel_count) {
      XELOGI("NUI: play frame {}: mask {} pitch {}: 0 pixels, no centroid",
             frame_number, shape_name, pitch);
      return;
    }
    XELOGI(
        "NUI: play frame {}: mask {} pitch {}: {} pixels, centroid ({:.1f}, "
        "{:.1f}), indices {} {} {} {} {} {} {}",
        frame_number, shape_name, pitch, mask.pixel_count, mask.centroid_column,
        mask.centroid_row, mask.histogram[0], mask.histogram[1],
        mask.histogram[2], mask.histogram[3], mask.histogram[4],
        mask.histogram[5], mask.histogram[6]);
    return;
  }

  // The depth texture's words are the depth, and may carry an index in their
  // low three bits as well, so its line shows the histogram of those too.
  NuiDepthSummary depth;
  SummariseNuiDepthTexture(bits, pitch, shape, &depth);
  XELOGI(
      "NUI: play frame {}: depth {} pitch {}: {} non-zero pixels, mean {:.1f}, "
      "indices {} {} {} {} {} {} {}",
      frame_number, shape_name, pitch, depth.pixel_count, depth.mean,
      mask.histogram[0], mask.histogram[1], mask.histogram[2],
      mask.histogram[3], mask.histogram[4], mask.histogram[5],
      mask.histogram[6]);
}

const uint8_t* NuiState::TranslateReadableGuestRange(uint32_t address,
                                                     uint64_t length) const {
  auto* memory = kernel_state_->memory();
  // The title chose the address and the length, and a range that runs off the
  // end of the address space would wrap in the 32-bit arithmetic below.
  if (!address || !length || uint64_t(address) + length > (uint64_t(1) << 32)) {
    return nullptr;
  }
  const uint32_t last_address = static_cast<uint32_t>(address + length - 1);
  const BaseHeap* heap = memory->LookupHeap(address);
  if (!heap || heap != memory->LookupHeap(last_address)) {
    return nullptr;
  }
  if (heap->heap_type() != HeapType::kGuestPhysical) {
    return IsGuestRangeReadable(address, static_cast<uint32_t>(length))
               ? memory->TranslateVirtual<const uint8_t*>(address)
               : nullptr;
  }
  // The graphics memory a title locks a texture in is a view of the physical
  // memory, and the heap of such an address does not always report the pages
  // of an allocation as readable: 4D5308ED' HUD textures, at
  // 0xD44C0000, are not. The physical heap is where every allocation is
  // recorded, so the range is asked of that, and read where it lies in
  // physical memory.
  const uint32_t physical = memory->GetPhysicalAddress(address);
  if (physical == UINT32_MAX ||
      memory->GetPhysicalHeap()->QueryRangeAccess(
          physical, static_cast<uint32_t>(physical + length - 1)) ==
          xe::memory::PageAccess::kNoAccess) {
    return nullptr;
  }
  return memory->TranslatePhysical<const uint8_t*>(physical);
}

bool NuiState::UpdateGuideFromFrame(const NuiMsg2B002& guest_frame,
                                    uint64_t frame_number, uint32_t* taken_id) {
  const bool recognise = cvars::nui_guide_gesture;
  std::lock_guard<std::mutex> transition(guide_transition_mutex_);
  if (!recognise && !guide_open()) {
    // Nothing can open the guide and it is not up: the frame is the title's.
    return false;
  }
  std::memcpy(&guide_frame_, &guest_frame, sizeof(guide_frame_));

  NuiGuideFrameResult result;
  {
    std::lock_guard<std::mutex> lock(guide_mutex_);
    if (recognise && !guide_gesture_) {
      guide_gesture_ =
          std::make_unique<NuiGuideGesture>(NuiGuideGestureSettingsFromCvars());
    }
    // The recogniser times holds with the host's clock. It predates the
    // kernel's frame stamp, and its hold and gap timing was tuned and tested
    // against that clock, so it stays there: the frame's own timestamp
    // (+2736, the guest's uptime in milliseconds that
    // NuiCamera::FeedDepthFrame writes) has not been tried in its place.
    result = AdvanceNuiGuide(guide_session_,
                             recognise ? guide_gesture_.get() : nullptr,
                             guide_frame_, NowUs());
    guide_hand_valid_ = result.hand_valid;
    guide_hand_x_ = result.hand_x;
    guide_hand_y_ = result.hand_y;
  }

  // The trace comes first, so that a completion's line precedes the guide's.
  if (cvars::nui_log_messages) {
    for (size_t i = 0; i < result.trace_count; ++i) {
      LogGuideTrace(frame_number, result.traces[i]);
    }
  }
  if (result.change != NuiGuideSession::Change::kNone) {
    AnnounceGuideChange(result.change == NuiGuideSession::Change::kOpened,
                        result.change_tracking_id, "gesture", true);
    if (cvars::nui_log_messages) {
      XELOGI("NUI: the Kinect Guide gesture was made with the {} arm",
             GuideArmName(result.change_arm));
    }
  }
  if (result.takes != guide_taking_ && cvars::nui_log_messages) {
    if (result.takes) {
      XELOGI(
          "NUI: the Kinect Guide takes the skeleton frames of tracking id {}",
          result.taken_tracking_id);
    } else {
      XELOGI("NUI: the Kinect Guide gives the skeleton frames back");
    }
  }
  guide_taking_ = result.takes;
  *taken_id = result.taken_tracking_id;
  return result.takes;
}

void NuiState::AnnounceGuideChange(bool open, uint32_t tracking_id,
                                   const char* reason, bool broadcast) {
  runtime_state_.SetEngagedTrackingId(open ? tracking_id : 0);
  if (open) {
    XELOGI("NUI: Kinect Guide opened by tracking id {}", tracking_id);
  } else {
    XELOGI("NUI: Kinect Guide closed by {} (tracking id {})", reason,
           tracking_id);
  }
  if (broadcast) {
    kernel_state_->BroadcastNotification(kXNotificationSystemNUIPause,
                                         open ? 1 : 0);
  }
  if (guide_observer_) {
    guide_observer_(open);
  }
}

void NuiState::CloseGuideLocked(const char* reason, bool broadcast) {
  uint32_t tracking_id = 0;
  bool closed = false;
  {
    std::lock_guard<std::mutex> lock(guide_mutex_);
    tracking_id = guide_session_.tracking_id();
    closed = guide_session_.Close();
    guide_hand_valid_ = false;
  }
  if (closed) {
    AnnounceGuideChange(false, tracking_id, reason, broadcast);
  }
}

bool NuiState::guide_open() const {
  std::lock_guard<std::mutex> lock(guide_mutex_);
  return guide_session_.open();
}

uint32_t NuiState::guide_tracking_id() const {
  std::lock_guard<std::mutex> lock(guide_mutex_);
  return guide_session_.tracking_id();
}

bool NuiState::GuideHandPosition(float* x, float* y) const {
  std::lock_guard<std::mutex> lock(guide_mutex_);
  if (!guide_session_.open() || !guide_hand_valid_) {
    return false;
  }
  *x = guide_hand_x_;
  *y = guide_hand_y_;
  return true;
}

void NuiState::SetGuideObserver(std::function<void(bool open)> observer) {
  std::lock_guard<std::mutex> transition(guide_transition_mutex_);
  guide_observer_ = std::move(observer);
}

void NuiState::CloseGuide(const char* reason) {
  std::lock_guard<std::mutex> transition(guide_transition_mutex_);
  CloseGuideLocked(reason, true);
}

X_HRESULT NuiState::HandleTitleShutdown() {
  // Camera streaming is stopped by the runtime's PsCamDeviceRequest calls.
  // This message clears its registration; it must not discard live requests
  // whose completion routines the runtime may still be waiting for.
  ResetRuntime(true);
  if (cvars::nui_log_messages) {
    XELOGI("NUI: runtime shut down");
  }
  return X_E_SUCCESS;
}

X_HRESULT NuiState::HandleTiltStatusReport(uint32_t message,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  uint32_t value = 0;
  if (NuiMessageBufferFits(buffer_length, sizeof(uint32_t)) &&
      IsGuestRangeReadable(buffer_ptr, sizeof(uint32_t))) {
    value = xe::load_and_swap<uint32_t>(
        kernel_state_->memory()->TranslateVirtual(buffer_ptr));
  }
  if (cvars::nui_log_messages) {
    XELOGI("NUI: camera tilt status report {:08X} = {:08X}", message, value);
  }
  return X_E_SUCCESS;
}

X_HRESULT NuiState::HandleIdentityIoRequest(uint32_t message,
                                            uint32_t buffer_length) {
  // The identity subsystem starts 0x2A005 and 0x2A006 while it sets itself up
  // and carries on whatever they answer. Later, for each profile whose
  // biometric data it keeps in step (0x82d34270), it asks with 0x2A008 whether
  // xam already holds that data: xam answers with a 64-bit stamp written
  // through a pointer in the buffer, which the runtime zeroes first. While the
  // stamp is 0 the runtime reads its own copy with XamReadBiometricData and
  // hands it over with 0x2A007, whose answer it ignores. Leaving the stamp
  // alone says that xam holds nothing, which is true here, so the runtime goes
  // on exactly as it did when both requests failed. What xam does with the
  // buffers is not known, so nothing is read out of them beyond the log's hex
  // dump.
  if (!NuiMessageBufferFits(buffer_length, sizeof(NuiMsgIdentityIoRequest))) {
    return X_E_INVALIDARG;
  }
  if (cvars::nui_log_messages) {
    XELOGI("NUI: identity I/O request {:08X}", message);
  }
  return X_E_SUCCESS;
}

X_HRESULT NuiState::HandleIdentityRegisterCallback(uint32_t buffer_ptr,
                                                   uint32_t buffer_length) {
  std::lock_guard<std::mutex> transition(runtime_transition_mutex_);
  // This is the last step of the runtime's identity initialization and its
  // result is that initialization's result, which in turn decides whether
  // NuiInitialize succeeds. The callback itself has no sender yet.
  if (!NuiMessageBufferFits(buffer_length, sizeof(NuiMsg2C009)) ||
      !IsGuestRangeReadable(buffer_ptr, sizeof(NuiMsg2C009))) {
    return X_E_INVALIDARG;
  }
  const auto* args =
      kernel_state_->memory()->TranslateVirtual<const NuiMsg2C009*>(buffer_ptr);
  const uint32_t callback = args->callback;
  runtime_state_.SetIdentityCallback(callback);
  if (cvars::nui_log_messages) {
    XELOGI("NUI: identity callback registered at {:08X}", callback);
  }
  return X_E_SUCCESS;
}

X_HRESULT NuiState::HandleIdentityRegisterFeature(uint32_t buffer_ptr,
                                                  uint32_t buffer_length) {
  // A feature registration names one of the classifiers the identity database
  // holds. Nothing is kept: the tracker runs inside the title, so xam only has
  // to accept the registration.
  if (!NuiMessageBufferFits(buffer_length, sizeof(NuiMsg2C00C)) ||
      !IsGuestRangeReadable(buffer_ptr, sizeof(NuiMsg2C00C))) {
    return X_E_INVALIDARG;
  }
  if (cvars::nui_log_messages) {
    const auto* args =
        kernel_state_->memory()->TranslateVirtual<const NuiMsg2C00C*>(
            buffer_ptr);
    XELOGI("NUI: identity feature '{}' registered with flags {:08X}",
           ReadGuestString(args->name_ptr, args->name_length),
           args->flags.get());
  }
  return X_E_SUCCESS;
}

X_STATUS NuiState::HandleCameraRequest(uint32_t request_ptr) {
  return camera_->HandleRequest(request_ptr);
}

NuiCameraStats NuiState::GetCameraStats() const { return camera_->GetStats(); }

NuiHudSkeletonFrame NuiState::GetHudSkeletonFrame() const {
  std::lock_guard<std::mutex> lock(hud_skeleton_frame_mutex_);
  return hud_skeleton_frame_;
}

NuiRememberedFloor NuiState::remembered_floor() const {
  std::lock_guard<std::mutex> lock(remembered_floor_mutex_);
  return remembered_floor_;
}

void NuiState::RememberFloor(const std::array<float, 4>& plane, uint32_t flag) {
  // Keep sampling and publication on the same side of a runtime reset. Tilt
  // queries request worker startup and read cached values, never guest code.
  std::lock_guard<std::mutex> transition(runtime_transition_mutex_);
  NuiRememberedFloor floor;
  floor.plane = plane;
  floor.flag = flag;
  // Read outside the floor lock: the backend has locks of its own. A sensor
  // whose angle cannot be read leaves the legs empty, and the plane is then
  // kept but not handed back, since the runtime would pair it with a gravity of
  // nothing.
  NuiTiltStatusLegs legs;
  int32_t degrees = 0;
  if (GetTiltStatusLegs(&legs, &degrees)) {
    floor.legs = legs;
  }
  std::lock_guard<std::mutex> lock(remembered_floor_mutex_);
  remembered_floor_ = floor;
}

// The checks themselves live in util/guest_range.h, where the XLIVEBASE app
// uses them too. These members only fill in this state's memory, and stay
// because the camera transport and the handlers above ask through the state
// they hold.
bool NuiState::IsGuestRangeReadable(uint32_t address, uint32_t length) const {
  return util::IsGuestRangeReadable(kernel_state_->memory(), address, length);
}

bool NuiState::IsGuestRangeWritable(uint32_t address, uint32_t length) const {
  return util::IsGuestRangeWritable(kernel_state_->memory(), address, length);
}

std::string NuiState::ReadGuestString(uint32_t address, uint32_t length) const {
  // The names a title registers are short; a longer one means the pointer or
  // the length is not what it looks like, so only a sane prefix is read.
  constexpr uint32_t kMaximumStringLength = 256;
  length = std::min(length, kMaximumStringLength);
  if (!IsGuestRangeReadable(address, length)) {
    return "";
  }
  const auto* chars =
      kernel_state_->memory()->TranslateVirtual<const char*>(address);
  // The lengths a title passes count the terminator, so stop at it.
  const auto* end = static_cast<const char*>(std::memchr(chars, '\0', length));
  return std::string(chars, end ? end - chars : length);
}

std::string NuiState::DumpMessageBuffer(uint32_t buffer_ptr,
                                        uint32_t buffer_length) const {
  // The dump is at least kMinimumDumpBytes long, because a pointer-style
  // message arrives with a length of zero.
  return DumpGuestBytes(buffer_ptr, std::max(buffer_length, kMinimumDumpBytes));
}

std::string NuiState::DumpGuestBytes(uint32_t address, uint32_t length) const {
  if (!address) {
    return "<null>";
  }
  auto* heap = kernel_state_->memory()->LookupHeap(address);
  if (!heap) {
    return "<unmapped>";
  }

  // The dump stops at the end of the heap and at the end of the page the
  // buffer starts on, which keeps a long buffer from needing several pages to
  // all be readable at once.
  const uint64_t heap_end =
      static_cast<uint64_t>(heap->heap_base()) + heap->heap_size();
  const uint64_t page_end =
      (static_cast<uint64_t>(address) / heap->page_size() + 1) *
      heap->page_size();
  const uint64_t limit = std::min(heap_end, page_end);
  const uint64_t available = limit > address ? limit - address : 0;
  length = static_cast<uint32_t>(std::min<uint64_t>(length, available));
  if (!length) {
    return "<empty>";
  }
  if (!IsGuestRangeReadable(address, length)) {
    return "<unreadable>";
  }

  const auto* bytes =
      kernel_state_->memory()->TranslateVirtual<const uint8_t*>(address);
  std::string dump;
  dump.reserve(length * 3);
  for (uint32_t i = 0; i < length; i++) {
    if (i) {
      dump.push_back(' ');
    }
    fmt::format_to(std::back_inserter(dump), "{:02X}", bytes[i]);
  }
  return dump;
}

void NuiState::LogMessage(uint32_t message, uint32_t buffer_ptr,
                          uint32_t buffer_length, X_HRESULT result,
                          bool known) const {
  if (known && !cvars::nui_log_messages) {
    return;
  }
  const std::string dump = DumpMessageBuffer(buffer_ptr, buffer_length);
  if (known) {
    XELOGI("NUI: message {:08X}(buffer {:08X}, length {}) = {:08X} [{}]",
           message, buffer_ptr, buffer_length, result, dump);
  } else {
    XELOGW(
        "NUI: unhandled message {:08X}(buffer {:08X}, length {}) = {:08X} [{}]",
        message, buffer_ptr, buffer_length, result, dump);
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
