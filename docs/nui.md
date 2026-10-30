# NUI (Kinect)

Navigation: [Overview](#overview), [Running](#running), [Playing](#playing),
[Recording and replaying](#recording-and-replaying),
[Play-frame diagnostics](#logging-what-the-title-sees-during-play),
[Protocol reference](#protocol-reference), [Known limitations](#known-limitations),
[Open questions](#open-questions).

## Overview

xenia emulates the xam and xboxkrnl protocols used by Xbox 360 Kinect titles.
The title runs its own skeletal tracker; xenia supplies device status, tilt,
calibration and raw depth frames. Title `4D5308ED` boots, tracks a player,
supports hand-cursor selection and plays all five activities with the settings
and matching external title patches described under compatibility below.

The host subsystem, `xe::nui::NuiSystem` in `src/xenia/nui/`, publishes 640x480
16-bit millimetre depth frames. Its backends are:

* `kinect`: Windows, using the Kinect for Windows runtime 1.8 with an Xbox 360
  Kinect or Kinect for Windows sensor. It also controls tilt and reads the
  accelerometer.
* `replay`: depth frames from a recording, paced by their timestamps.
* `nop`: no sensor.

A title never touches USB. Its statically linked NUI runtime talks to xam via
`XamNui*` exports and app-0xFE messages (`0x2Axxx`, `0x2Bxxx`, `0x2Cxxx`),
mounts its tracking databases with `XamContentOpenFile`, and queues camera
requests through xboxkrnl's `PsCamDeviceRequest` (ordinal 0x30D).
`xe::kernel::xam::NuiState` and `NuiCamera` implement these interfaces. The
runtime also needs the kernel's `LDI*` LZX decompression exports and Ob support
for object types defined by guest code. Body-part classification and skeleton
fitting run inside the title, including Xenos shaders such as
`ShadePixelExemplarClassify*` (strings at 0x82040000-0x82049000). xam reads the
resulting skeletons only to recognise the Kinect Guide gesture and provide
diagnostics.

**Addresses in this document** are guest virtual addresses in the decrypted
image of title `4D5308ED` loaded at 0x82000000: Rev 2 disc,
module hash `FEF1C9FF272B3BD9`, NUI runtime 2.0.11775.7 and skeletal tracker
2.0.11775.5. They identify code and data a maintainer can check against the XEX.
The protocol reference comes from that image and observed behaviour, not Xbox
360 SDK headers. Deductions are marked **[inference]**; observations made only
on a replay say so.

## Running

### Requirements and backend selection

For a live sensor, use Windows, the Kinect for Windows runtime 1.8, and a Kinect
for Windows sensor or an Xbox 360 Kinect with its PC power/USB adapter. The Xbox
360 sensor has been verified with the SDK 1.8 installation, which includes the
runtime and drivers; a runtime-only installation has not been checked
separately. Building xenia needs no Kinect SDK headers or import libraries:
`src/xenia/nui/kinect/kinect_api.h` declares the interfaces resolved from
`Kinect10.dll` at runtime. Other platforms have `replay` and `nop`.

The host API is documented in Microsoft's public Kinect for Windows 1.8
[NUI functions reference][host-functions] and [INuiSensor reference][host-sensor].
The [accelerometer method reference][host-accelerometer] specifies its signature,
gravity units, axes and level reading. The public [SDK 1.8 distribution][host-sdk]
includes the Windows runtime, drivers and API/device interfaces. These are host
Windows interfaces; the guest protocol evidence is the title image identified
above. The per-sensor slot assumptions and runtime checks used here are described
under [the sensor's accelerometer](#where-the-angle-comes-from-the-sensors-accelerometer).

[host-functions]: https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/hh855370(v=ieb.10)
[host-sensor]: https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/hh855326(v=ieb.10)
[host-accelerometer]: https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/jj663844(v=ieb.10)
[host-sdk]: https://www.microsoft.com/en-us/download/details.aspx?id=40278

```powershell
.\xenia_canary.exe <iso> --nui=kinect --mute=true
```

`--nui=any` selects the first available backend, in this order: replay, kinect,
nop. Replay is available when `--nui_replay` names an existing file. The Kinect
availability probe checks whether `Kinect10.dll` loads, not whether a sensor is
attached, so `any` can select it with no hardware present. The backend then
probes for a sensor every second and reports readiness changes to the guest.
`--nui=kinect` explicitly requires the runtime: failure stops startup with
`NUI: cannot initialize nui_system!`. `--nui=nop` never opens the hardware.

### Flags

These flags are in the `Kinect` category of `xenia-canary.config.toml`, except
`allow_avatar_initialization`, which is in `Kernel`.

| flag | default | purpose |
|---|---|---|
| `--nui` | `any` | Backend: `any`, `kinect`, `replay` or `nop`. |
| `--nui_log_messages` | `false` | NUI calls, messages and buffer dumps, content opens and camera requests. Camera state queries log at Info when the answer changes and at Debug on every query. With `--log_level=3`, also logs the guide recogniser's trace. |
| `--nui_log_play_frames` | `0` | Log the title's skeletons and texture summaries every N-th skeleton frame; 0 disables it. |
| `--nui_show_depth_preview` | `false` | Show the Kinect Depth overlay. |
| `--nui_record` | empty | Record published host depth frames to this file. |
| `--nui_replay` | empty | File played by the replay backend. |
| `--nui_tilt_override` | `1000` (off) | Values from -27 to 27 replace the reported tilt; intended for replay and diagnostics. |
| `--nui_allow_tilt` | `true` | Accept the title's motor requests; `false` refuses them. |
| `--nui_accelerometer` | `true` | Read the sensor's real tilt; `false` uses the angle read at open, then the last accepted motor target. |
| `--nui_guide_gesture` | `true` | Recognise the Kinect Guide gesture; additional settings are below. |
| `--allow_avatar_initialization` | `false` | Allow xam's avatar initialisation, required for body-controlled gameplay. |

### Depth preview and dashboard state

`--nui_show_depth_preview=true` opens the Kinect Depth overlay. It shows the
backend, device status, latest sequence, tilt and host depth image. Its angle
read uses `NuiSystem::PeekTiltDegrees` and does not request a tilt thread.
During a title it also shows guest camera state, streams, keep-alive, queued and
fed frames, and a `Tracker:` line from the latest 0x2B002 message: six skeleton
states and ids, two engaged ids, and the floor plane. Before the first such
message it says `Tracker: no skeleton frame yet`.

`NuiState::MarkKinectInitialized` sets xconfig's `KinectInitialized` retail flag
when a backend first reports a ready sensor. This records setup; setting it does
not create a sensor. `KinectDisabled`, checked on every
`NuiState::IsDevicePresent` call, hides an attached sensor from the guest.

### Running without host audio

A title still needs its audio clients serviced when sound is muted. Title
`4D5308ED` otherwise stays in its intro movies. With `--apu=nop`, the audio
worker paces registered clients at its guest-time-scaled 256 / 48000 second
interval (5.333 ms at normal speed), and `NopAudioDriver` discards samples and
releases their semaphores immediately. Direct XMP music playback bypasses that
worker and uses `PacingAudioDriver` to release each frame at its playback rate.

```powershell
.\xenia_canary.exe <iso> --apu=nop --nui=kinect --mute=true
```

The pacing driver logs `APU: pacing client without a host device at 48000 Hz,
256 samples per frame` at Info for that frame format; registered nop clients do
not use this driver. It is also a fallback if a selected backend cannot create
an audio render driver, or cannot start the media player's driver for XMP music.
The fallback logs one Warning per failed client, or per failed media-player
setup, beginning `APU: XAudio2 could not create a driver`. The backend may also
log its own failure, such as `IXAudio2::CreateMasteringVoice failed`. These
messages mean the affected audio is silent; use `--apu=nop` to remove host audio
from a regression check.

### Testing the tilt motor

Close xenia first: only one process can own the sensor. The standalone
`tools/kinect_motor_test.ps1` opens it through `Kinect10.dll`, commands 0, +15,
-15, +27, -27 and 0 degrees, and prints the runtime result, call duration,
reported angle and accelerometer samples every 250 ms. `-Angles` changes the
sweep; `-LogDir` changes the transcript directory from TEMP.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\kinect_motor_test.ps1
```

To sample for 20 seconds without commanding a move:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\kinect_motor_test.ps1 -NoCommands -Watch 20
```

A successful command does not prove the motor moved. A failed tilt drive has
accepted the full sweep while both the accelerometer and runtime angle stayed
near -20 degrees. A working Xbox 360 sensor followed the targets to within about
a degree; calls took roughly 1-3 seconds because they block for movement. Do not
force a head that resists movement. If the motor does not work, adjust the
sensor's placement, inspect the depth preview, and use the title's (Y)
auto-tilt-disable hint. xenia can still report the head's measured angle.

## Playing

Start title `4D5308ED` with `--allow_avatar_initialization=true` and sign in
through xenia's Profiles menu to save progress.

### What works

On a live Xbox 360 Kinect through the Kinect for Windows 1.8 runtime:

* The title boots. Auto-tilt starts at -25 degrees, then settles on absolute
  targets after remembering the floor (about -12 and -7), with no further
  sweeps.
* Raising a hand joins a player; the hand cursor and hold-to-select work.
* The leak-plugging, introductory, ball-deflection and bubble activities played
  well with the body tracked and a translucent stand-in avatar visible.
* The obstacle activity has also been reported to play successfully live with
  its matching external patch, with the other four activities still playing
  successfully. Separate replay verification reached a score with the patch and
  stayed on the hand-grab prompt without it.
* The Kinect Guide gesture opens, closes and pauses as on a console. Walking out
  of view pauses the game, and the gesture resumes it.
* Saving works with a signed-in profile. With `storage_selection_dialog`
  disabled (the default), `XamShowNuiDeviceSelectorUI` supplies the first
  storage device, the hard drive.

### Placement and joining

Place the sensor level, 0.6-1.2 m high, with clearance for the tilting head's
full ±27-degree travel. Stand about 2.0-2.5 m away, facing it, with the whole
body and both feet visible in the depth preview. Stand upright with weight
evenly distributed and feet under the hips, and stay still while tilt settles.
If play-space detection refuses a join, check the feet and try adjusting the
sensor's aim a few degrees.

The runtime aims for a 1.8 m player 2.2 m away, using the remembered floor's
height and a margin of 0, 150 or 300 mm above the head. It does not aim at the
feet, so a taller or closer player may lose them below the image
**[inference]**. At a sensor height of 1.4 m its targets are approximately -12
degrees initially and -8 for the later 150 mm margin; at 1.0 m, -2 and +1.

The title's disengagement classifier (0x82b716b0) scores ten geometry curves;
with `[settings+556] = 1`, one failing cue rejects the join. The important cues
measure spine and head displacement from the midpoint of the feet, perpendicular
to gravity and divided by torso length. Their thresholds are 0.8 and 1.1 torso
lengths (about 38 and 52 cm for a 0.47 m torso). Leaning, feet displaced from
the torso, or a gravity axis inconsistent with the depth image can fail these
tests. Uniform depth-scale error cancels in the ratios. The gravity axis comes
from the tilt status block; a replay therefore needs the recording's pitch, as
described below.

Once a feed binds, the play-space page advances on its dwell timer. The periodic
"PRESS (X) ... KINECT TUNER, OR (Y) TO DISABLE AUTO-TILT" hint does not indicate
a tilt loop. A cursor moving against the hand indicates a missing guest
depth-row mirror; see "Pixel format".

### The avatar

Title `4D5308ED` drives an avatar pawn from the tracked skeleton, and the
activities read the pawn and its mesh. The introductory activity steers from the
pawn's position; the leak-plugging activity traces against per-bone collision
bodies, including wrist bodies `LF_W` and `RT_W` (avatar joints 33 and 36). Without
`--allow_avatar_initialization=true`, `XamAvatarInitialize` fails and the title
shuts down its avatar system. The raft does not steer. The pawn's avateering
engine also requires the avatar-ready byte at 0x82a4e3c0-0x82a4e4cc
**[inference]**: this also depends on the flag.

The poser, `UMCMDPoser` (0x82b33748), forwards the skeleton to its avateering
engine only when `NUI_SKELETON_FRAME.liTimeStamp` changes. xenia writes a
non-zero guest-uptime timestamp in milliseconds into camera request 5's result
at `+72`; the runtime copies it to the skeleton frame. The unit is
**[inference]** from the poser and engine scaling differences by 0.001 into
seconds: the poser clamps to 0.008-0.066 seconds (0x82b3442c-0x82b34470), the
engine to 1/60-1 second (0x82bfa6b8, 0x82c0d6e0). Only differences matter. A
fixed stamp leaves limbs still even while the tracker sees moving joints. The
engine call at 0x82b34400-0x82b34488 solves rotations for 71 avatar joints. The
poser also solves a camera-to-avatar pose from the 20 NUI joints, blends in the
engine's joints, puts the root at the NUI hip and writes the result to the
AnimTree's `AvateeringPose` node. Per-limb confidences select that pose or a
fallback for the root, torso, head, each arm and each leg; they rise with
tracked NUI joints and positive engine joint weights.

xenia supplies no avatar assets. The title uses its translucent loading figure,
`gbl_avt_p_loading.AvatarLoading_skelMesh`, a 104-bone rig; the poser uses the
title's default 71-joint skeleton at 0x82130900. Its limbs follow the player.
Hand collision plugging a leak was measured on replay; the live leak-plugging
activity result is that it played well.

**Compatibility caveat.** The obstacle activity requires a matching external
title patch for the image identified above while avatar assets are unavailable.
An optional external startup patch avoids forced introductory activity selection.
These title-specific workarounds belong in the separate
[xenia-canary/game-patches repository](https://github.com/xenia-canary/game-patches).
Compatibility with other images and patched activity variants needs
separate verification.

### The Kinect Guide

Hold the left arm straight out to the side and down about 45 degrees, with the
right arm hanging at the side, and stay still for about a second. xam recognises
this in the skeleton frames received through 0x2B002. The title teaches it on
its guide-gesture tutorial page.

`NuiGuideGesture` checks each tracked skeleton. A qualifying arm must:

* Be the configured arm, default left.
* Be 30-60 degrees from down, defined by the inverse floor normal or the
  camera's down axis when no floor is available.
* Extend the hand at least 0.9 of the arm's length from the shoulder, point
  within 60 degrees of the body's outward axis, and have tracked shoulder, elbow
  and hand joints rather than inferred ones.
* Have the other arm outside the band and at most 20 degrees from down, with its
  hand below the hip centre.
* Turn at most 10 degrees from its starting direction while the hip centre moves
  at most 0.15 m from its starting position during the hold.

The pose must last 1000 ms. A gap longer than 200 ms breaks the hold. After
completion, the arm must be seen out of the band for longer than the gap
tolerance before it can fire again; lost tracking or a stalled feed does not
rearm it. Direction, rather than hand position, measures stillness because
tracked joints can jitter along an otherwise stationary arm.

The other-arm and hip thresholds distinguish the gesture from gameplay poses. In
recorded deliberate gestures the resting arm stayed within 13.6 degrees (16.6
while continuing to hold afterwards), versus 22.4-36.8 during accidental holds;
the 20-degree default separates them. Deliberate hip movement was 0.02-0.08 m,
versus 0.18-0.30 m in accidental holds. Deliberate arm rotation reached 7.6
degrees, leaving 2.4 degrees below the limit. `--nui_guide_rest_angle_max=25`
can help if the trace shows deliberate holds failing only `other-arm-at-rest`;
on the recordings, accidental holds still lasted under 0.6 seconds with that
setting and were also stopped by hip motion.

| cvar | default | purpose |
|---|---|---|
| `nui_guide_gesture` | `true` | Enable recognition. |
| `nui_guide_arm` | `left` | `left`, `right` or `either`; another value logs one Warning and uses `left`. |
| `nui_guide_angle_min` | `30` | Minimum angle from down, in degrees. |
| `nui_guide_angle_max` | `60` | Maximum angle from down, in degrees. |
| `nui_guide_rest_angle_max` | `20` | Maximum other-arm angle from down, in degrees. |
| `nui_guide_hold_ms` | `1000` | Required hold duration. |
| `nui_guide_gap_ms` | `200` | Gap tolerance; values below 50 count as 50. |

All are in the Kinect category. Extension, heading and stillness limits are the
`NuiGuideGesture` constants `kMinimumExtension`, `kMinimumSidewaysFraction`,
`kMaximumArmTurnDegrees` and `kMaximumHipMovement`.

With `--nui_log_messages=true --log_level=3`, `NUI: guide trace` lines report
each arm that is in the band or whose hold changes. Fields are: HUD frame
number, tracking id, arm, angle, extension, sideways cosine, other-arm angle,
other-hand height above the hip, trunk lean, arm rotation, hip movement and held
milliseconds. Unavailable values are `nan`; lean is diagnostic only. `fails`
names broken rules (`seen`, `angle`, `extension`, `sideways`, `tracked`, `arm`,
`other-arm-out-of-band`, `other-arm-at-rest`, `other-hand-lowered`, `arm-still`,
`hip-still`) or `none`. Hold events are `hold started`, `hold broken`,
`completed` and `disarmed`. A successful hold reaches `held 1000 ms`; a few
failing frames within the gap tolerance are allowed. These frame numbers also
appear in `HUD skeleton frame` and `play frame` lines.

Opening records the player's tracking id for `XamNuiHudGetEngagedTrackingID`,
broadcasts `kXNotificationSystemNUIPause` (0x0006001A) with 1, and logs `NUI:
Kinect Guide opened by tracking id N`. While open, a 0x2B002 frame containing
that player receives `0x8301000B`, its first engaged id (`+2764`) is set to the
player, and its second (`+2768`) to 0. Other frames receive `X_E_SUCCESS`.

The centred Kinect Guide overlay shows device status, engaged id and a cursor
from the higher hand. It maps a shoulder-centred 0.5 by 0.4 m box onto the
overlay. Close it by repeating the gesture after lowering the arm, pressing
Escape, or hovering on "Return to game" for two seconds. Closing clears
engagement, broadcasts NUIPause with 0, and logs the reason (`gesture`,
`escape`, `hover`). Escape closes the guide before it can leave fullscreen.
Ending the title also closes it.

`AdvanceNuiGuide` permits at most one transition per frame. While open, only the
engaged player's completion counts; another player's simultaneous completion
cannot reopen it. While closed, the first completion in slot order opens it. A
player lost and found under a new id cannot close the existing guide by gesture
or hover; use Escape.

The overlay offers only "Return to game"; it does not draw the locked HUD
textures or provide dashboard/settings/sign-in functions. Hold timing uses the
host clock rather than the frame timestamp. Its ImGui layer consumes character
events, while key presses and releases still reach the keyboard driver.
`XamShowNuiGuideUI` remains a separate stub reached by the title's script,
observed as `(0, 255)` during normal play.

## Recording and replaying

Record a session to reproduce tracker behaviour without somebody repeating the
same movements at the sensor:

```powershell
.\xenia_canary.exe <iso> --nui=kinect --nui_record=person.xdepth --mute=true
```

```powershell
.\xenia_canary.exe <iso> --nui=replay --nui_replay=person.xdepth --mute=true
```

`--nui_record=<file>` writes every depth frame the host publishes, whatever the
backend, to `<file>`; it costs about 18 MB/s, so a two-minute session is a bit
over 2 GB. `--nui=replay` publishes the frames of `--nui_replay=<file>` back in
order, paced by their recorded timestamps and looping when the file ends, and
answers tilt requests as if a motor had obeyed them. The replay backend is only
available when `--nui_replay` names a file that exists, so `--nui=any` never
picks it by accident, and it refuses to start when both flags name the same file
(recording truncates, so that would destroy the recording being played).

The replay starts the recording over when the title starts consuming frames,
which is the first frame buffer its NUI runtime queues after it has configured
the camera. Without that the title would join the loop wherever it had reached
while the title was booting, and which frames it saw first would be a matter of
chance. A stop followed by a new configuration counts as a new stream and starts
the recording over again. One frame of the old position can still reach the
title: the frame feed hands over whichever frame was published last as soon as
the first buffer is queued, before the replay has read the recording's first
frame back.

Begin with an empty room for at least a second before somebody walks in. The
title seeds its background from the per-pixel minimum of the first fifteen
processed depth frames; later frames cannot move that background farther away. A
person present from the start can become part of the background and fail
detection. An existing recording can be prepared by prepending empty-room
frames, for example a copy of its first frame with the person's silhouette
replaced by interpolated wall/floor depth. Such a modified recording is
synthetic input, not another sensor measurement.

A recording (`.xdepth`) is a 24-byte file header (`XNUIDEP1`, then the frame
width and height as two `uint32` and two reserved zero `uint32`) followed, per
frame, by a 16-byte frame header (`uint64` sequence, `uint64` timestamp in
microseconds) and `width * height` `uint16` millimetre samples. Every field is
host endian (little endian), a sample of 0 means the sensor had no reading, and
the file is valid after any number of complete frames: a reader ignores a
trailing frame that was cut short when the emulator was killed.
`src/xenia/nui/depth_recording.h` is the format; the recorder itself lives in
`NuiSystem::PublishFrame`.

### A recording carries no tilt: pass `--nui_tilt_override`

`XNUIDEP1` stores depth samples and timestamps and nothing else, in particular
not the angle the sensor was pitched at. The replay backend answers
`XamNuiCameraTiltGetStatus` and `XamNuiCameraElevationGetAngle` with the angle
the title last commanded (`ReplayNuiSystem::GetTiltDegrees`), which is
reasonable, since there is no motor to read, but the depth frames cannot follow
that command. The title's join classifier measures its cues against the gravity
axis built from the reported angle (see "Placement and joining" and "The block's
second consumer"), so **a recording is faithful only while the replayed tilt
matches the pitch it was made at**.

Hold it there with `--nui_tilt_override=<deg>`, where the degrees are the
recording's own pitch: `atan2(-c, b)` in degrees of the floor clip plane
`(a, b, c, d)` the tracker fits, which the 0x2B002 message carries at `+16` and
the `NUI: HUD skeleton frame` log lines show. A sensor that reached a different
angle from the one the title commanded (a head that was aimed by hand, or one
whose motor does not move) shows in the recording as a floor pitch that stays
put while the commands change.

On replay, matching the override to the recorded pitch keeps the player bound
until they leave view. A mismatched pitch can lose the join even while the
tracker continues to fit a healthy skeleton; +27 degrees rejected the player
throughout a replay recorded with the sensor tilted down.

With the reported angle pinned, the runtime's auto-tilt driver never sees its
command obeyed, so it keeps re-issuing `XamNuiCameraElevationSetAngle` now and
then. That is harmless on a replay, where the backend only records the requests.
The override is a replay and diagnostic tool and should never be set for a live
sensor, where it would pin the axis while the motor moves.

### Checking a change against a recording

Record an empty room for one or two seconds, then walk in, stand still, move,
stand still again and leave. For menu or gameplay changes, also record the
relevant gesture or activity and retain the matching profile and saved-game
data. Use a new filename for the reference recording and record its log:

```powershell
.\xenia_canary.exe <iso> --nui=kinect --nui_log_messages=true --nui_record=<file> --mute=true
```

Take the stable floor pitch from that log's `NUI: HUD skeleton frame` lines
(`atan2(-c, b)` in degrees). Replay the same file before and after a change,
with the same profile/settings and that override:

```powershell
.\xenia_canary.exe <iso> --apu=nop --nui=replay --nui_replay=<file> --nui_log_messages=true --nui_tilt_override=<pitch> --mute=true
```

Add `--allow_avatar_initialization=true` for gameplay. Choose a portion recorded
at a stable tilt: one override cannot reproduce a moving sensor's entire
recording. Start each comparison with the same saved-game data and compare the
actual screen or level reached, since menu replay timing can vary.

Keep the window restored when a recording navigates the host Kinect Guide by
hovering: its hover timer advances in `NuiGuideDialog::OnDraw`, and a minimized
window can prevent the recorded hover from closing the guide.

What to look for needs no instrumented build: each item is a committed log line
or the screen.

1. **No unexpected GPU refusal markers**: `Unsupported resolve`,
   `not a ColorFormat`, `Draw refused by`, `Failed in backend`,
   `GPU packet failed` or `Wide 1D texture`. Also count
   `Resolve without a vertex buffer`: title `4D5308ED` has one expected
   one-shot Warning of this kind. The markers are for title logs:
   `xenia-gpu-tests` logs three `Unsupported resolve` lines on purpose, from its
   negative cases.
2. **`NUI: HUD skeleton frame N: tracking states [...]`** shows a slot at 2
   while the person is in view and all zeros after they leave.
3. **`NUI: XamNuiCameraTiltGetStatus: angle <pitch>`** on every such line (the
   poll is logged on a change of angle and otherwise at most once a second): the
   override is in force and the angle never follows the title's commands.
4. **The title leaves `IS ANYBODY THERE?`** while the person is in view.
5. **The first `NUI: XamNuiCameraRememberFloor` line follows the start-up
   `XamNuiCameraElevationSetAngle(-25)`**, and after it the driver neither
   searches nor steps up: no start-up 0 and no +4 or +12 of the no-floor branch
   (see "The block's tail: the remembered floor"). The framing's cut-at-top
   command, the pinned angle plus 34.2, can still repeat.
6. **With a Debug build, check title close and camera reinitialization** in the
   same process. File > Close and save-state restore reach the termination path
   only in a build with `DEBUG` defined. Check for `NUI: title terminated: frame
   feed stopped, N depth and M colour requests dropped.` and no frame delivery
   while the title is closed. On the second launch, the camera should be probed
   at state 4 again rather than at 2, then configured with a new depth table and
   feed. Check the rendered game separately: camera reinitialization succeeds in
   the tested File > Close case, but full game restart fails as described under
   "Camera stop and title termination" below.

Compare errors and warnings with a baseline of the same recipe before the
change. xenia prefixes errors with `!>` and warnings with `w>`; check for new
kinds or changed counts, allowing only differences explained by the change or
the actual screen reached. Unimplemented exports outside NUI can still log
errors. `XamContentOpenFile` suppresses missing-update-device messages for the
expected `update:` lookup before its `game:` fallback. Replay timing without a
matching tilt override is not a regression criterion.

## Logging what the title sees during play

Message 0x2B002 carries the skeletons, floor and player-mask textures the title
receives from its tracker. Logging them helps distinguish a missed movement in
tracking from a gameplay response. The Kinect Guide has its own recogniser
trace, described above.

`--nui_log_play_frames=<N>` (default 0, which is off) logs, on every N-th
0x2B002 frame, at Info, one group of lines; 30 is about one group a second. With
it off nothing is read or logged, and the answer to the message, the Kinect
Guide and the `HUD skeleton frame` lines are the same either way. With it on,
its read of the tilt angle uses `NuiState::PeekTiltDegrees` without requesting
the Kinect backend's tilt thread (see "Where the angle comes from"). The lines carry
the frame's number, the same one the `HUD skeleton frame` lines count. There is
one line for each skeleton the tracker holds as tracked, then the floor's, the
player mask's and the depth's:

```
NUI: play frame 2730: id 45 position (0.057, 0.116, 2.011) hip (0.046, 0.167, 2.067) head (0.049, 0.727, 1.979) hand L (-0.173, -0.114, 2.087) R (0.316, 0.563, 1.456) foot L (-0.143, -0.609, 2.293) R (0.115, -0.538, 2.214)
NUI: play frame 2730: floor (-0.021, 0.988, -0.152, 1.285) gravity (-0.000, 0.988, -0.156, 0.000) tilt -9
NUI: play frame 2730: mask 320x240 pitch 768: 12346 pixels, centroid (168.8, 100.7), indices 0 0 0 0 0 12346 0
NUI: play frame 2730: depth 80x60 pitch 256: 4340 non-zero pixels, mean 23736.3, indices 0 0 0 0 0 824 0
```

* **The skeleton line** gives the skeleton's own position and six joints, the
  hip centre, head, both hands and both feet, in the sensor's camera space and
  in metres as the runtime hands them: x to the right, y up, z away from the
  sensor. A joint the tracker did not find reads `(0.000, 0.000, 0.000)`.
* **The floor line** is the floor clip plane the tracker fitted, the unit normal
  and the sensor's height above the floor in metres, then the gravity normal and
  the tilt angle xenia reports to the title at that moment. The two normals come
  from different places, the floor's from the tracker's fit to the depth image
  and the gravity normal from the tilt angle, and they should nearly agree, as
  here (a tilt of -9 degrees gives y 0.988 and z -0.156). A floor that drifts
  far from the gravity normal means the tilt xenia reports is not the pitch the
  sensor had (see "A recording carries no tilt"). When the tracker has no floor
  in view, the runtime should publish the floor it remembered instead, turned to
  the current tilt, rather than zeros (see "The block's tail: the remembered
  floor"). That is **[inference]**: a replay cannot show it, because the
  recordings keep the floor in view.
* **The mask line** counts the pixels of the player-index texture that hold a
  player, gives the mean column and row of those pixels, and counts how many
  pixels hold each index from 1 to 7. With nobody in view it reads
  `0 pixels, no centroid`. A player the tracker only has a position for is in
  the mask too, so a mask line can count pixels in a frame that prints no
  skeleton line.
* **The depth line** does the same for the depth texture, but counts the pixels
  that are not zero, gives the mean of those pixels' words, and counts the
  pixels whose low three bits hold each index.

A line that names a texture but no summary says why there is none: the texture
has no bits, its pitch is too narrow for either stream, or its memory cannot be
read.

**How to read them.** Stand in front of the sensor and move.

* A step to one side should move `hip` x and the mask's centroid column
  together, both rising or both falling; a lean should move the head's x before
  the hip's.
* A jump should move `hip` y, and the head's and the feet's with it, and the
  centroid's row. The columns and rows count from the top left of the image, so
  a body that goes up has a smaller row.
* z is the distance from the sensor in metres. It grows as the person steps
  back, and the mask's pixel count falls.
* The mask's centroid is that of the whole silhouette, so it lies a little below
  the hip in the image; the hip projects to about `column = 160 + 290 * x / z,
  row = 120 - 290 * y / z` in the 320x240 image. What matters is that the two
  follow each other. If the joints move and the centroid does not, the mask is
  not following the body; if the mask moves and the joints do not, the skeleton
  is not. If neither moves, the tracker did not report movement.

**What the textures are.** Both are locked by the runtime and handed over as a
pointer and a pitch, and both hold 16-bit words, big endian, with each row
padded up to a multiple of 256 bytes. The pitch is all that says which stream a
texture is:

* a pitch of 640 or more (768 on a replay) is the 320x240 depth-plus-index
  image: the depth in millimetres in the high 13 bits and the player index in
  the low three, as a Kinect's own depth frame packs them;
* a pitch of 160 to 639 (256 on a replay) is an 80x60 image of the same kind of
  word, which also carries the index in its low three bits.

On a replay the player mask was the 320x240 stream and the depth texture the
80x60 one; nothing in the message says which texture takes which, so the lines
name the stream (`mask 320x240`, `depth 80x60`) each time. A pitch under 160
fits neither and the line says so. On a replay the index in a mask was the
skeleton's slot plus one, not its tracking id, in every play frame that had a
mask.

**The layout is linear.** On replay, row-by-row mask reads followed the
projected hip position (horizontal correlation 0.996, mean difference three
pixels), consistent with linear rather than tiled storage. The silhouette's
centroid is below the hip, so its vertical coordinate need not coincide.

**A caution.** The summary reads the title's texture memory as it is at that
moment, from the thread that handles the message, while the title's tracker and
the GPU may be writing it. A frame's mask can therefore be a frame behind or
half written, and one count that is off means nothing; look at a run of lines.
The textures are in graphics memory, at addresses such as `0xD44C0000`, a view
of the physical memory whose heap does not report those pages as readable, so
xenia asks the physical heap whether they can be read and reads them from
physical memory. For those views `NuiState::IsGuestRangeReadable`, which the
camera transport uses, is not asked; an address in any other heap falls back to
it.

## Protocol reference

What title `4D5308ED`'s NUI runtime asks of xam and the kernel, and how xenia
answers. `g` is the runtime's state block at 0x83896510, and `g+N` an offset
into it.

### Title startup sequence

The game's Kinect manager constructor (0x82a55b70) is the only place NUI is
brought up:

```
0x82a55be4: NuiInitialize(0x849, 5)                 ; 0x82d26c58
0x82a55bf4: XMsgSystemProcessCall(0xFE, 0x2B001, &120000, 4)   ; camera keep-alive
0x82a55c3c: g_KinectManager+20 = SUCCEEDED(hr) ? -1 : 0
```

**The title's whole notion of "Kinect available" is
`SUCCEEDED(NuiInitialize)`.** The keep-alive result is not even stored.
`NuiInitialize` (0x82d26c58):

1. `XamContentOpenFile(0xFF, "stexemplar", "update:\Database.xmplr", 0x43, 0, 0,
   0)`, falling back to `"game:\Database.xmplr"`; both failing returns
   `0x8301000D` (`E_NUI_DATABASE_NOT_FOUND`), and the title then sits on "Is
   anybody there?" for good, with no further NUI traffic apart from the
   keep-alive.
2. The same for `"nuiidentity"` and `NuiIdentity.bin.be`.
3. `NuiInitializeInternal(flags, maxPlayers, "nuiidentity:\", "stexemplar:\",
   0)` (0x82d26510), which in order:
   - validates `flags` (`flags & 0x33FFE704` must be 0; 0x849 = the flags this
     title uses; bit 0x40 selects 640x480 colour, bit 0x2/0x40 enable colour at
     all, bits 0x1/0x8 mean depth/player index and skeleton, 0x80000000 is
     audio) and `maxPlayers` (must be < 6, -1 means 5);
   - `EtxProducerRegister`, from 0x82d26630 (result ignored; see "Export table
     (as used by this title)");
   - `XamXStudioRequest(1, ...)` (`X_E_FAIL` in xenia, which is fine: success
     would route camera requests through XStudio playback instead);
   - `NtCreateEvent` for the worker's idle wait;
   - **camera open** (0x82d254f8): allocates the frame rings and request
     descriptors, see "Frame transport";
   - `ExCreateThread` of the camera worker (0x82d25e60), suspended, base
     priority 17, affinity 2;
   - loads the ST database `stexemplar:\database.gmsodf` (0x82d49a90 →
     0x82d65600): allocates a physical buffer for the decompressed data (the
     file's size, at least 35,579,018 bytes) and inflates the file block by
     block with the xboxkrnl LZX API (see "Block decompression"), 691 blocks and
     22,614,016 bytes for this title;
   - allocates three 8192-entry 16-bit lookup textures (`g+8648`, `g+8704`,
     `g+8760`);
   - sends **message 0x2B003** `{u64 5, u64 11775, u32 flags, u32 0}` (this and
     the two steps above are skipped when the database load fails);
   - identity init (0x82d35860, run when the title uses ST): reads
     `nuiidentity:\NuiIdentityDbVersion.txt`, registers about twenty identity
     features with **message 0x2C00C**, loads `NuiIdentityNN.bin.be` and
     `NuiIdentityPCA.bin.be`, calls `XamNuiIdentityGetSessionId`,
     `XamGetCurrentTitleId`, `XMsgStartIORequest(0xFE, 0x2A005 / 0x2A006, 0,
     buf, 0x18)`, starts an identity thread (0x82d35480) and finally sends
     **message 0x2C009** with its callback; the result of 0x2C009 is the result
     of the identity init (0x82d35948);
   - tilt init: `XamNuiCameraTiltSetCallback(0x82d2d150)`;
   - a first device probe (`PsCamDeviceRequest` code 0), sets
     `g_NuiInitialized`, resumes the worker, `XamXStudioRequest(769, {flags,
     maxPlayers})`.

   On failure it resumes the worker, sends `XamXStudioRequest(770, 0)` and
   **message 0x2B004** (no buffer), and tears down.
4. `XamContentClose("stexemplar")` and `XamContentClose("nuiidentity")`: the
   packages only need to be mounted for the duration of the call.

Once `NuiInitialize` has succeeded, the worker probes the camera, configures it
and queues its frame buffers (see "Frame transport"), the tracker publishes each
skeleton frame it fits to xam with message 0x2B002, and the title binds a
tracked skeleton to one of its two player feeds (see "The engagement chain").

### Message table (app 0xFE)

| id | direction / call | buffer | meaning | xenia |
|---|---|---|---|---|
| 0x2B001 | `XMsgSystemProcessCall(0xFE, id, &ms, 4)` from the title (wrapper 0x831aef60) | `u32 timeout_ms` | keep the camera awake for `timeout_ms`; 0 releases it. Call sites use 120000 (startup, 0x82a55bf4), 60000 (0x82a6019c), 30000 (0x82b5d214), a value from a settings table (0x82a574a8) and 0 (release, e.g. 0x82b05540). The result is never used by this title. | `NuiState::HandleCameraKeepAlive`: `X_E_SUCCESS` with a sensor, `X_E_DEVICE_NOT_CONNECTED` without |
| 0x2B002 | `XMsgInProcessCall(0xFE, id, buf, 0)` (wrapper 0x831aef98, sent from 0x82d35a40) | 2784 bytes (0xAE0): `+0 u64 5`, `+8 u64 11775`, `+16..+48` floor clip plane and normal-to-gravity (two Vector4), `+48..+2736` six 448-byte `NUI_SKELETON_DATA`, `+2736 u64` timestamp, the skeleton frame's `liTimeStamp` (copied from `NUI_SKELETON_FRAME+0` at 0x82d3599c): the depth frame's stamp from request code 5, on xenia the guest's uptime in milliseconds; `+2744 u32` 0 in every frame sampled on replays, so not the frame number, `+2748` depth texture `pBits`, `+2752` its pitch, `+2756` player-index texture `pBits`, `+2760` its pitch | hands the Kinect guide/HUD the latest skeleton frame plus the two locked textures it can draw a silhouette from; sent from the frame-processing path (0x82d23610) only once frames flow and the runtime's HUD flag is set. The runtime treats a return of **0x8301000B** as "HUD engaged" (`g+96 = 1`, 0x82d35ad8) and anything else as not engaged. | `NuiState::HandleHudSkeletonFrame` parses it as `NuiMsg2B002` (`nui_messages.h`: `X_NUI_VECTOR4` 16 bytes, `X_NUI_SKELETON_DATA` 448, the message 2784, every offset `static_assert`ed), keeps the six tracking states and ids for the log and the overlay, feeds the tracked skeletons to the Kinect Guide's gesture recogniser, and returns **`X_E_SUCCESS`**, or, while the Kinect Guide is up and the frame holds the player who opened it, **0x8301000B** with that player's id written to `+2764` and 0 to `+2768` (see "The engagement chain" and "The Kinect Guide") |
| 0x2B003 | `XMsgInProcessCall(0xFE, id, buf, 0)` (wrapper 0x831aefb0, sent from 0x82d359f0 during init) | 24 bytes `{u64 version_major = 5, u64 version_build = 11775, u32 NuiInitialize flags, u32 0}` | registers the runtime version and init flags with xam (HUD init) | `NuiState::HandleHudInitialize` stores them; `XamNuiHudGetVersions` / `XamNuiHudGetInitializeFlags` read them back. It also turns on the GPU read-back the tracker needs (see "GPU read-back") |
| 0x2B004 | `XMsgInProcessCall(0xFE, id, 0, 0)` (wrapper 0x831aefc8, from the shutdown path 0x82d22868) | none | NUI shut down / release | `NuiState::HandleCameraRelease` |
| 0x2B005–0x2B00B | **xam → title**: ids passed to the tilt callback registered with `XamNuiCameraTiltSetCallback` (`callback(id, float* data)`, 0x82d2d150 switches on `id - 0x2B005`) | 0x2B005: `data[0]` = requested angle (0x82d2d2f8); 0x2B00A: `data[0..2]` floats and an index (0x82d2d3fc) | tilt requests from the system to the title's tilt driver | never sent by xenia |
| 0x2B006, 0x2B00C | `XamNuiCameraTiltReportStatus(id, block)` from the title (0x82d2c49c, 0x82d2d20c, ...) | block `{u32 status (values 3, 5, 6, 7 seen in the code), ..., +12/+16 float angle}` at 0x83898e70 / 0x83898e80 | the title reporting tilt progress/completion back to xam | `NuiState::HandleTiltStatusReport` logs the first dword |
| 0x2A004, 0x2A005, 0x2A006 | `XMsgStartIORequest(0xFE, id, 0, buf, 0x18)` (wrappers 0x831b6988, 0x831b6918, 0x831b68c8; sent during identity init) | 24 bytes `{u32, u64 at +8, u32 at +16, u32 at +20}` | identity/biometric profile data | `NuiState::HandleIdentityIoRequest` answers 0x2A005 and 0x2A006 with `X_E_SUCCESS` without reading the buffer; 0x2A004 has not been seen |
| 0x2A008, 0x2A007 | `XMsgStartIORequest(0xFE, id, 0, buf, 0x18)` (wrappers 0x831b67f0 and 0x831b6860), sent in that order, with `XamReadBiometricData` between them, for each profile whose biometric data the runtime brings up to date (0x82d34270, once a profile is signed in) | 24 bytes. 0x2A008: `{u64 XUID, u64 address of a u64 stamp (a 32-bit pointer, sign-extended), u32 0, u32 address of a u32}`; 0x2A007: `{u64 XUID, u64 stamp, u32 data address, u32 data size}` | 0x2A008 asks whether xam already holds the profile's biometric data: a non-zero stamp written through the pointer at +8 ends the exchange. While the stamp is 0 the runtime reads its own copy with `XamReadBiometricData` and hands it to xam with 0x2A007 (stamp 0x80000000 and one byte when that read fails), ignoring the answer | `NuiState::HandleIdentityIoRequest` answers both with `X_E_SUCCESS` without reading the buffer. The runtime zeroes the stamp before asking, so leaving it alone says that xam holds nothing, and the runtime goes on to its placeholder. On the enrolment path (0x82d32a70) the answer to 0x2A007 is checked before `XamUserNuiEnableBiometric`; that path has not been seen taken. 0x2A009 (wrapper 0x831b67a0, `{u64 XUID}`, sent from 0x82d32a58 on the same path) has not been seen |
| 0x2C001 | `XMsgSystemProcessCall` (0x831b31c0) | 16 bytes `{u32 a, ...}` | identity | unhandled |
| 0x2C003, 0x2C004 | `XMsgSystemProcessCall` (0x831b31f0, 0x831b3238) | 24 bytes `{u32, u32, u32, u32, u64}` | identity | unhandled |
| 0x2C005 | `XMsgSystemProcessCall` (0x831b3280) | 12 bytes `{u32, u32, u32}` | identity | unhandled |
| 0x2C009 | `XMsgInProcessCall(0xFE, id, buf, 0)` (0x831b32c0, sent from 0x82d35948 at the end of the identity init) | `{u32 callback}` (0x82d31f18 in this title) | registers the identity event callback; **its result is the identity init's result**, so it must return `S_OK` for `NuiInitialize` to succeed | `NuiState::HandleIdentityRegisterCallback` stores the callback and returns `X_E_SUCCESS`; nothing calls it back yet |
| 0x2C00C | `XMsgInProcessCall(0xFE, id, buf, 0)` (0x831b32f8; ~20 calls during identity init from 0x82d2f140.., 0x82d3692c.., 0x82d3749c..) | 32 bytes `{u32 2, u32 flags (0x40000180, 0x11, 0x13, ...), u32 0, u32 0x0101, char* name, u32 name length, ptr, ptr}` with names such as `NuiV1FaceHiRes`, `NuiV1FaceSample`, `NuiV1BHSample` | registers an identity feature/classifier | `NuiState::HandleIdentityRegisterFeature` logs the name and returns `X_E_SUCCESS` |
| 0x2C00D | `XMsgInProcessCall` (0x831b3348) | 32 bytes `{u32, u64 at +8, u32 at +16, u32 at +20}` | identity | unhandled |

xam routes the 0x2Bxxx and 0x2Cxxx ranges and 0x2A005-0x2A008 to
`NuiState::DispatchNuiMessage` (`IsNuiXamMessage`, `nui_messages.h`). An
unhandled message there is answered `X_E_FAIL` and logged at Warning with a dump
of its buffer, whatever `nui_log_messages` says; a handled one is logged, with
its dump, only under `nui_log_messages`.

### Export table (as used by this title)

| export | signature (from the call sites) | behaviour |
|---|---|---|
| `XamNuiGetDeviceStatus(X_NUI_DEVICE_STATUS* out)` | called once at startup (0x82a5f8ec / 0x82b04320 via 0x831aefe0) | xenia: `status = 1` and `X_ERROR_SUCCESS` with a sensor, `0xC0050006` with a zeroed struct without. The title only looks at bits 0x2 and 0x4 of `status` (see "`X_NUI_DEVICE_STATUS`"), which xenia never sets, so the call can land before or after the sensor becomes ready with the same outcome. |
| `XamContentOpenFile(0xFF, root_name, path, 0x43, 0, 0, 0)` | see "Title startup sequence" | mounts the package at `path` under `root_name:`; xenia stages a copy of the guest file under `cache_root/content_files/<title>/<root>/<name>` and mounts it with the content manager (`X_ERROR_SUCCESS`, disposition 2, licence from the package header). Every failure is `X_ERROR_FILE_NOT_FOUND`: a missing root name or path, a root that is already in use, a path that does not resolve, a directory, a root name that is not a plain identifier (`IsSafeContentRootName`, since it becomes a host directory name), a copy that could not be staged, or a file that is not a mountable package (`xeXamContentOpenFile` in `xam_content.cc`). |
| `XamContentClose(root_name, 0)` | after init | closes the mount |
| `XamNuiCameraTiltSetCallback(void (*cb)(uint32_t id, float* data))` | 0x82d2d528, `cb = 0x82d2d150` | registers the title's tilt callback (see 0x2B005–0x2B00B). xenia takes the one argument, logs it under `nui_log_messages` without keeping it, and reports whether a sensor is present. It never calls it: the callback is xam's channel for telling the title that something outside it moved the sensor, and xenia never does. |
| `XamNuiCameraTiltGetStatus(X_NUI_TILT_STATUS* block)` | 0x82d2d4b8 and the auto-tilt driver's poll at 0x82d2dd3c; the block is 0x50 bytes with `+0 = 0x50` and `+4 = 'XtSs'` (0x58745373) written by the caller before the call | fills the block with the host sensor's own accelerometer reading when the Kinect backend has one, and otherwise with an accelerometer synthesised from the reported tilt angle; the flags word says the motor is moving while a tilt is in progress; the tail (`+0x30..+0x4C`) hands back the floor the title last gave `XamNuiCameraRememberFloor`, with the accelerometer legs of that moment, once there is one, and is zero before. All 0x50 bytes are written, the size and signature with the two constants the caller put there (see "The tilt status block"). `X_ERROR_SUCCESS` with a sensor, `X_E_DEVICE_NOT_CONNECTED` without one or when the angle cannot be read, in which case nothing is written |
| `XamNuiCameraTiltReportStatus(id, block)` | see "Message table (app 0xFE)" | routed into `NuiState::DispatchNuiMessage` |
| `XamNuiCameraElevationGetAngle(LONG* angle_degrees, DWORD* status_flags)` | the runtime's tilt driver (0x82d2cd4c) through the forwarder 0x82d2c648, and the title's engagement state machine every frame (0x82a5f85c, 0x82a596ac, 0x82a5fbd0) | on success writes the host sensor's angle to the **first** argument and the tilt status word to the **second** (see "The tilt status word"); `X_ERROR_SUCCESS` with a sensor, `X_E_DEVICE_NOT_CONNECTED` without, in which case neither is written. Either pointer may be null. |
| `XamNuiCameraElevationSetAngle`, `XamNuiCameraElevationStopMovement` | tilt driver (0x82d2c408, ...) | implemented against the host sensor (set angle) or presence stubs |
| `XamNuiCameraRememberFloor(uint64_t plane_ab, uint64_t plane_cd, uint32_t flag)` | the tilt driver at 0x82d2cc1c and 0x82d2cc84, as `lwz r5, 48(r31); ld r3, 0(r30); ld r4, 8(r30); bl` followed by a store of 0 to `48(r31)`; hundreds to thousands of calls a session | the two 64-bit arguments are the title's floor plane as four big-endian floats: `a` and `b` in the high and low halves of the first, `c` and `d` in the second (`xe::kernel::xam::DecodeNuiFloorPlane`). `a`, `b` and `c` are the unit normal, and `d` is the sensor's distance from the floor in **millimetres**, where the HUD skeleton frame's `floor plane` carries metres (the runtime converts it for the frame): on replay, sampled planes match the skeleton frames' floor planes to six digits after dividing `d` by 1000; intervening calls may carry another fit. The flag was 1 on the first call and 0 on every later one on a replay; its meaning is unknown. xenia logs `NUI: XamNuiCameraRememberFloor(a, b, c, d; flag N)` with three decimals under `nui_log_messages`, keeps the last plane and flag in `NuiState` (`remembered_floor()`) while a sensor is present, together with the accelerometer legs the tilt status block carries at that moment (`NuiState::RememberFloor`), and answers `X_ERROR_SUCCESS` with a sensor and `X_E_DEVICE_NOT_CONNECTED` without one. `XamNuiCameraTiltGetStatus` hands the plane and those legs back in the block's tail, which is where the runtime's tilt driver reads the floor it remembered: see "The block's tail: the remembered floor". The store is emptied when the title ends. |
| `XamXStudioRequest(code, &args)` | codes 1, 769, 770, 0x1003, 0x1004 from the runtime | `X_E_FAIL` (the runtime treats failure as "no XStudio") |
| `XamNuiIdentityGetSessionId()` | identity init | returns a fixed session id |
| `XamShowNuiDeviceSelectorUI(tracking_id, user_index, content_type, content_flags, total_requested, u32* device_id, XAM_OVERLAPPED*)` | one call site, 0x82aab9a4, through the bare jump 0x831af950: `r3` the tracking id (0, 2, 11 and 14 have been seen), `r4` the user index (it also indexes the title's per-user record at `base + (user + 50) * 28`), `r5 = 1` (content type 1, saved games), `r6` the content flags (0x100 or 0x300, built from two booleans), `r7` the total bytes requested (a qword the caller stores at sp+88), `r8` that record as the device id pointer, `r9` a 28-byte overlapped the caller zeroes first. The title accepts 0 or `ERROR_IO_PENDING` and treats anything else as failure; it asks once a profile is signed in | logs the seven arguments and forwards the last six to `XamShowDeviceSelectorUI`; with `storage_selection_dialog` off (the default) that completes the overlapped, writes the first storage device (the hard drive) into the device id, and returns `ERROR_IO_PENDING` (0x3E5). The title then goes on to `XamContentGetDeviceData(1, ...)` and `XamContentCreateEx(savedrive, ...)` and saves `savedrive:\GameProgress` (an `NtWriteFile` of 0x7E35 bytes), live and on replays. |
| `XamShowNuiAchievementsUI(tracking_id, user_index, title_id)` | one call site, 0x82b5eec4, through 0x831af938 (which adds `li r5, 0`): `r3` the tracking id, `r4` the user index (checked to be 0-3) | logs the three and forwards `(user_index, title_id)` to `XamShowAchievementsUI`. Not seen called. |
| `XamShowNuiMarketplaceUI(tracking_id, user_index, ui_type, offer_id, offer_type, content_category, title_id)` | two call sites through 0x831af940, which supplies the offer type (-1) and the title id (0) behind the five arguments the callers give: 0x82aabb58 (`ui_type` 1 when an offer is given, else 0; the offer id is `0x4D5308ED << 32 \| id`) and 0x82aabbc0 (`ui_type` 2, offer id 0, category -1) | logs the seven and forwards the last six to `XamShowMarketplaceUI`. Not seen called. |
| `XamShowNuiDirtyDiscErrorUI(tracking_id, user_index)` | eight call sites through 0x831af958 (0x8234d730 and others), all with `r3 = 0`, `r4 = 255` | logs both and forwards the user index to `XamShowDirtyDiscErrorUI`, which puts up its message and exits xenia. Not seen called. |
| `XamShowNuiFriendsUI(tracking_id, user_index)`, `XamShowNuiControllerRequiredUI(tracking_id, user_index)` | 0x82aaba6c through 0x831af928 (user index checked to be 0-3), and 0x82b7a478 through 0x831af918 (`r3` the tracking id, 0 when the title has none looked up, `r4 = 255`) | log both arguments. Friends returns success; controller-required puts up an OK message box unless headless and returns success. Neither has a controller export to forward to, and neither has been seen called. |
| `XamShowNuiGamerCardUIForXUID(tracking_id, user_index, xuid, 0)` | 0x82aabac8 through 0x831af930, which adds `li r6, 0` (meaning unknown); `r5` is the XUID, loaded with `ld` from a pointer the caller holds | logs the four and returns success; there is no controller export to forward to. Not seen called. |
| `XamReadBiometricData(u64 xuid, u32* stamp, void* data, u32* size, XAM_OVERLAPPED*)` | 0x82d34318, between 0x2A008 and 0x2A007 (see "Message table (app 0xFE)") once a profile is signed in, and 0x82d329f8 on the enrolment path: the profile's XUID, a 0x32000-byte buffer with its size in and out, and no overlapped. Both callers treat any non-zero result as "no data" | xenia stores no biometric data: it logs the arguments at Info under `nui_log_messages` and answers `X_ERROR_NOT_FOUND` (or completes the supplied overlapped with that error and returns `X_ERROR_IO_PENDING`) without writing the biometric outputs, so the runtime hands xam its one-byte placeholder with 0x2A007 |
| `LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression`, `LDIResetDecompression` (xboxkrnl) | ST database loader (0x82d65600, 0x82d64d90) and the game's own asset code (0x82c08060, 0x82c1f3c4) | implemented on mspack's LZX decoder, see "Block decompression" |
| `EtxProducerRegister(provider id, description, 3, 0, 64, registration block)` (xboxkrnl) | `NuiInitializeInternal` (0x82d26510), the call at 0x82d26630: a 16-byte provider id at 0x82017cf4, the runtime's event description at 0x8381e258 and the 64-byte block at `g+8424` (0x838985f8), which it zeroes just before; the result is ignored. The runtime's logging helper (0x82d27410) tests the enabled mask at `+8` of that block before every `EtxProducerLog` | logs the arguments at Debug and answers success without writing the block, so nothing is ever enabled and `EtxProducerLog` is never reached. `EtxProducerUnregister`, which xenia does not implement, is called only from the runtime's teardown (0x82d22a1c), which has not been seen reached |
| `PsCamDeviceRequest(request)` | see "Frame transport" | camera transport |

The title imports these as well, and has not been seen to call them:
`XamUserNuiGetUserIndex`, `XamUserNuiGetEnrollmentIndex`,
`XamUserNuiEnableBiometric`, `XamWriteBiometricData` (which xenia does not
implement), and of the `XamShowNui*UI` family `XamShowNuiHardwareRequiredUI`,
`XamShowNuiFriendsUI`, `XamShowNuiControllerRequiredUI`,
`XamShowNuiGamerCardUIForXUID`, `XamShowNuiAchievementsUI`,
`XamShowNuiMarketplaceUI` and `XamShowNuiDirtyDiscErrorUI`. Seen called:
`XamShowNuiGuideUI(0, 255)`, `XamShowNuiTroubleshooterUI(255, 0, 0)`,
`XamShowNuiSigninUI(2, 255, 0)`, `XamShowNuiDeviceSelectorUI`, and
`XamReadBiometricData`, once a profile is signed in. The title imports no
`XamNuiHud*` export.

#### The `XamShowNui*UI` family: the leading argument

The exports of the family, apart from the Troubleshooter and the
hardware-required export, have an argument in front of the user index that the
controller export they mirror does not, and the title's wrappers
(0x831af900-0x831af950, and 0x831af958 for the disc error) leave `r3`, where it
arrives, untouched. It is not a user index, since the user index follows it: it
is 0, 2, 11 or 14 where it has been seen (Guide 0; Signin 2; the device selector
0, 2, 11 and 14), the Signin call site maps -1 to 0 for it, the Guide and Signin
call sites read it from a per-player record's +156 field, and the disc error
call sites pass 0. It is most likely the NUI tracking id of the skeleton that
drives the UI **[inference]**; xenia logs it and nothing uses it. xenia declares
it on every export of the family that has it; an export declared without it
takes every argument one register early (the device selector that way left the
title with no storage device, so it could not save). The Troubleshooter takes
the user index first (`XamShowNuiTroubleshooterUI(255, 0,
0)`). The disc error's `(0, 255)` is named on the strength of the pattern alone.

Checked against the call sites:

* `XamShowNuiSigninUI(tracking_id, user_index, flags)`: seen as `(2, 255, 0)`.
* `XamShowNuiGuideUI`: two arguments, `r3` the tracking id (0x82b5de6c,
  0x82b74a24) and `r4 = 255` from the wrapper; declared as two unknowns; seen as
  `(0, 255)`.
* `XamShowNuiTroubleshooterUI(255, 0, 0)` (0x82d2eb30): matches the declaration
  `(user_index, tracking_id, flags)`.
* `XamShowNuiHardwareRequiredUI`: both call sites (0x82aaa8f8, 0x82b04334) pass
  `r3 = 1` and ignore the result, while xenia refuses any argument other than 0
  with `X_ERROR_INVALID_PARAMETER`, so it never reaches the troubleshooter
  dialog it would show. That guard comes from a decompile, not from a call site,
  and is left alone: the export has not been seen called, and answering it would
  put up xenia's blocking Troubleshooter message box.

### `X_NUI_DEVICE_STATUS`

The 24-byte block `XamNuiGetDeviceStatus` fills
(`src/xenia/kernel/xam/nui_state.h`) is six big-endian dwords: `unk0`, `unk1`,
`unk2`, `status` (+12), `unk4`, `unk5`. xenia zeroes it and sets `status = 1`
when a sensor is present. The title's wrapper (0x831aefe0) discards the return
value and returns only `status`; its two callers test `status & 0x2`
(0x82a5f8f0: takes an alternate path through 0x82a5fd30 and returns without
setting up NUI) and `status & 0x4` (0x82b04324:
`XamShowNuiHardwareRequiredUI(1)` unless the `NOCAMERA` setting is present). No
other field is read by this title. By xenia's notes on the xam decompile (in
`XamNuiGetDeviceStatus_entry` and at `X_NUI_DEVICE_STATUS`), `unk1` and `unk2`
receive the NTSTATUS of xam's camera (`PsCamDeviceRequest`) and microphone-array
(`McaDeviceRequest`) probes, `status` is set to 0x20 when XStudio is active, and
one branch ors it with 0x40 or 0x44. The meaning of `unk0`, `unk4`, `unk5` and
of the status bits other than 0x2/0x4 is unknown.

### The tilt status word

The second output of `XamNuiCameraElevationGetAngle` is a bitfield about the
tilt motor, not a second copy of the angle. Title `4D5308ED` reads the two
apart at three call sites, which is how the order was settled: 0x82a5fbd0
compares the **first** output's magnitude against a configured threshold, while
0x82a5f85c and 0x82a596ac test single bits of the **second**. The two bits the
title tests are named in `src/xenia/kernel/xam/nui_tilt_status.h`:

| bit | name | what the title does with it |
|---|---|---|
| 0x1 | `kNuiTiltStatusBusy` | 0x82a596bc: the tilt driver gives up for this frame |
| 0x2 | `kNuiTiltStatusNeedsAttention` | 0x82a5f864: the engagement state machine is forced into state 4 ("check the sensor") and the timer that would let it decay back to 0 is reset every frame, so a tracked skeleton is discarded at 0x82a5f6f0 for as long as the bit is set |

xenia reports `kNuiTiltStatusOk` (0). The Kinect backend does know when a tilt
is in progress, and says so in the tilt status block's own flags word (see "The
tilt status block"), but not here: bit 0x1 would make the title give up for the
frame on every poll of a move, and nothing in xenia knows when the motor has run
out of travel, which is all 0x2 could honestly say, and 0x2 parks the engagement
machine in "check the sensor". Without a sensor the status is not written, so
the guest reads back whatever it put there, which is the zero it cleared the
stack slot to (0x82a5f850).

### The tilt status block

The 0x50 bytes `XamNuiCameraTiltGetStatus` fills (`X_NUI_TILT_STATUS` in
`src/xenia/kernel/xam/nui_messages.h`) are the only place the NUI runtime's
auto-tilt driver looks to find out where the camera is actually pointing, and
where the floor is. It does not trust the angle `XamNuiCameraElevationGetAngle`
reports; it reads two accelerometer longs back out of this block instead. And it
reads the block's tail, `+0x30..+0x4C`, as the floor the title handed
`XamNuiCameraRememberFloor`, which decides every target it aims the motor at
(see "The block's tail: the remembered floor").

| offset | field | read at | use |
|---|---|---|---|
| +0x00 | `cb_size` | written by the caller (0x82d2dd28/0x82d2dd38) | always 0x50 |
| +0x04 | `signature` | written by the caller (0x82d2dd24/0x82d2dd34) | `'XtSs'` = 0x58745373 |
| +0x20..+0x2B | `accel_x/y/z` | 0x82d2d4c8, handed on to the runtime's float4 accelerometer helper 0x82d2c938 | the sensor's accelerometer |
| +0x24, +0x28 | `accel_y`, `accel_z` | 0x82d2dd44 and 0x82d2dd40, both `extsw` | `atan2(accel_z, accel_y)` |
| +0x2C | `status_flags` | 0x82d2dd90, and the public "is the motor moving" accessor 0x82d2c6d8 | `& 3` moving, `& 4` the driver returns 25, `& 8` it returns 31, `& 0x10` its tilt call searches again (0x82d2cdf0); the rest are unknown, and 0 is idle |
| +0x30..+0x3F | `floor_plane[4]` | as two doublewords at 0x82d2d584/0x82d2d588 (the frame path), and by address at 0x82d2c548, 0x82d2e1a4 and 0x82d2e648 (the player-extents routine 0x82d49940) | the remembered floor, exactly as `XamNuiCameraRememberFloor` received it: the unit normal's `a`, `b`, `c`, then `d`, the sensor's height above the floor in **millimetres** |
| +0x3C | `floor_plane[3]`, the floor's `d` | 0x82d2c71c (the target routine), 0x82d2ca9c (the search), 0x82d2ce9c (the start), 0x82d2defc (the poll), 0x82d2cbc4 (the 0x2B006 report, x 0.001) and 0x82d2da30 (the title's result, x 0.001) | zero means "no floor": a blind search and relative targets; anything else, absolute targets |
| +0x40 | `floor_flags` | 0x82d2cd20 | bit 0 makes the runtime's tilt call return 5 before it starts, **[inference]** "auto-tilt is off"; xenia writes 0 |
| +0x44..+0x4F | `floor_accel_x/y/z` | 0x82d2c4f4 and 0x82d2c514 (`atan2(+0x4C, +0x48)`), and by address at 0x82d2d5ac, 0x82d2e180 and 0x82d2e624 | the accelerometer at the moment the floor was remembered, in the encoding and scale of +0x20..+0x2B |

The measured elevation is `atan2(block[+0x28], block[+0x24]) * 57.2957763671875`
(the constant at 0x82018068, rounded through the 0.5 at 0x82001178 and an
`fctiwz` at 0x82d2dd94). The driver compares that with the angle it last
commanded and stops moving the motor when the difference is at most the 2.0 it
keeps at 0x82001168 (0x82d2df54-0x82d2df84 and 0x82d2e048-0x82d2e078); otherwise
it re-issues `XamNuiCameraElevationSetAngle` through the wrapper at 0x82d2c668,
which refuses anything beyond ±27 with `E_INVALIDARG` without calling xam; its
callers clamp first. A blank block would make the measured angle
`atan2(0, 0) = 0` whatever the sensor did, so the driver could never settle at a
non-zero target and would drive the motor into its end stop again and again.

xenia therefore always fills the block, in one of two ways
(`src/xenia/kernel/xam/nui_tilt_status.h`), writing all 0x50 bytes with the size
and signature re-stamped as the constants the caller put there:

* **From the sensor's measured accelerometer** (`NuiTiltStatusLegsFromGravity`)
  whenever the Kinect backend has a reading (see "Where the angle comes from"):
  `accel_y = round(981 * -y)` and `accel_z = round(981 *
  kAccelerometerElevationSign * z)` for the host reading `(x, y, z)` in g, which
  points at the floor. The driver's `atan2` then yields the elevation xenia
  reports, to a fraction of a degree.
* **From the reported angle** (`NuiTiltStatusLegsForAngle`) otherwise (no
  accelerometer, `--nui_accelerometer=false`, `--nui_tilt_override` set, or a
  replay): `accel_y = round(981 * cos(angle))`,
  `accel_z = round(981 * sin(angle))`.

Either way `accel_x = 0`: the gravity axis the title builds from the block
(below) does use it, but which way the guest counts a sideways roll is not
known, and zero leaves the axis upright across the sensor. Only the ratio of the
two legs reaches the driver's `atan2`, so the 981 is arbitrary: it is a chosen
scale large enough that rounding costs under a tenth of a degree, and a level
reading gives a block byte-identical to the one for an angle of 0. The one thing
that could be wrong is a sign, which the unit tests pin by running the driver's
own `atan2` over every angle in [-27, 27] for both kinds of legs, and which live
readings confirm against the depth image.

`status_flags` is `kNuiTiltBlockMoving` (`& 3`) while the Kinect backend has a
tilt request waiting for its tilt thread or a `NuiCameraElevationSetAngle` in
flight, and 0 otherwise, so the driver waits for a move instead of judging an
angle the head has not reached yet. The `& 4` and `& 8` bits are never set: they
make the driver return 25 and 31, and what those mean is not known, so nothing
in the block can tell the driver "cannot get there, stop asking".

**[inference]** Which leg is which, and what a non-zero `status_flags` means,
are read out of the driver's arithmetic rather than measured on a console:
`accel_y` is the cosine leg at +0x24 and `accel_z` the sine leg at +0x28 because
that is what the driver's `atan2(block[+0x28], block[+0x24])` needs to yield the
elevation it then compares with its own command, and the flag values above come
from the constants the driver returns, not from a sensor that was seen to set
them. xenia sets only the `& 3` moving bits. The legs and their sign are
confirmed on replays (at the recording's own pitch the join cues collapse and
the player binds; at +27 it is refused every time) and agree live with the
tracker's floor fit; the flag values remain inferred. So does the tail's layout:
no console has been seen to fill it, and it is read off the runtime's use of it,
which is consistent at every read site in the table.

#### The block's tail: the remembered floor

The driver keeps the floor in xam. On every poll that finds the motor still it
checks the floor plane its tracker has fitted (0x82d2cae8), and when the plane
is within 10 degrees of the block's gravity (the cosine at 0x82018078, tested at
0x82d2c9e8) and the driver is either searching or the floor has held steady for
60 frames, it hands the plane to `XamNuiCameraRememberFloor`, hundreds to
thousands of times a session. Everything that needs the floor afterwards reads
it out of the block's tail, which a console's xam presumably fills with it
**[inference]**: the tail has not been captured on a console for this
implementation. A tail of zeros means to the driver that no floor was ever
remembered. xenia's `XamNuiCameraRememberFloor` keeps the plane with the
accelerometer legs the block carries at that moment (`NuiState::RememberFloor`,
the same computation as `+0x20..+0x28`), and `XamNuiCameraTiltGetStatus` writes
both into the tail after the fill (`FillNuiTiltStatusFloor`): the plane as it
was received, `d` in millimetres, a flags word of 0, and the legs. Before the
title's first `XamNuiCameraRememberFloor`, and for a plane whose `d` is not a
finite positive number, the tail stays zero.

Every tilt target comes from one routine, 0x82d2c6f8, and it branches on the
tail's `d` (0x82d2c71c):

* **With a floor**, the target is absolute: `floor(atan((margin + H - d)
  / 2200) * 180 / pi - 22.3654 + 0.5)`, clamped to ±27. That puts the top of the
  view, 22.3654 degrees above the axis (0x82018064, the runtime's half vertical
  field of view, `atan(240 / 583.3)`), on the head of a player `H` = 1800 mm
  tall (0x82018070) standing 2200 mm away (1/2200 at 0x8201806c), with `margin`
  millimetres to spare above the head. `H` can come from the console's
  `XCONFIG_CONSOLE_NUI` setting, which xenia does not have (its xconfig has no
  such field, so the read leaves the runtime's zeroed buffer as it was), so it
  is always 1800. The margin comes from the title's request: 0 for the start-up
  target and 150 mm for every later request seen on replays, where each command
  from the title's own thread equals the 150 mm column at the tail's `d` (-6 and
  -5 at `d` = 1271-1287 mm, where 300 mm would give -2). 0, 150 and 300 mm are
  the three entries of the runtime's margin table at 0x8381e324; no request was
  seen to use 300. The search is skipped (0x82d2ca9c): a request with flag 8 is
  aimed by the formula at once (0x82d2ce30-0x82d2ce80) and then goes to state 6,
  and any other goes straight to its framing (0x82d2ce9c).
* **Without a floor** (`d` = 0), the driver searches blind, -25 (0x82d2cac8) and
  then steps of +10 to -17, -7, +3, +13 and +23 for as long as the measured
  angle is below 0, and then adds a **relative** offset,
  `target += round(atan2(margin, 2200))`: +4 for 150 mm and +8 for 300 mm. The
  base it adds to is reset to 0 on every poll that has the block's moving bit
  0x2 set (0x82d2cb60), so after any move the next request searches again from
  -25; while nothing moves, as on a replay, the steps pile up instead (0, +4,
  +12, +20, ...).

| sensor height `d` | margin 0 | 150 mm | 300 mm |
|---|---|---|---|
| 1400 mm | -12 | -8 | -5 |
| 1360 mm | -11 | -7 | -4 |
| 1238 mm | -8 | -4 | -1 |
| 1000 mm | -2 | +1 | +4 |

`d` has to be millimetres, as the title handed it over: the same floor in
metres, 1.36, would give +17, +19 and +21. The unit tests pin the tail's bytes
and run the driver's formula over them (`nui_tilt_status_test.cc`). With the
tail handed back, the first `XamNuiCameraRememberFloor` comes right after the
start-up -25 and the driver never searches again: on a live sensor the commands
are -25 at start-up and then the absolute targets (about -12, then about -7),
with no further sweeps. With the tail left zero, the no-floor search and its
relative steps (+4, +8, +12, +16 degrees) were what the title commanded, and
they tilted a standing player's feet out of view.

Not every command is a target from 0x82d2c6f8. With a player in view the framing
aims the tracked head about 22.8 degrees above the axis (0x82d2e300-0x82d2e518),
and a head cut off at the top of the view makes it command the measured angle
plus 34.2, truncated (`fctiwz`) and at most 27 (0x82d2e284-0x82d2e2fc). That
rule acts only while the measured angle is below 27 (0x82d2e290), and at most
five times per request: the request's start sets a step counter at T+0x58 to 5
(0x82d2cef0), and the rule tests it (0x82d2e2a0) and takes one off each time
(0x82d2e2f0). On a replay with the reported angle pinned the head never arrives,
so the rule repeats its pinned angle plus 34.2 (+22 at -12) up to its five steps
per request.

The tail feeds three more things. The frame path, when the tracker has no floor
in view and `NuiInitialize` was given flag 0x800 (this title passes 0x849),
turns the remembered plane from the remembered accelerometer (`+0x44`) to the
current one (`+0x20`) and publishes it as the frame's floor clip plane
(0x82d2d540, 0x82d231c8-0x82d2320c) **[inference]** for the vector arithmetic,
so a frame without a floor in view need not carry `(0, 0, 0, 0)`. The framing's
player-extents routine 0x82d49940 takes the plane as the floor it measures the
player against. And the 0x2B006 status report and the result the title gets back
carry `d / 1000`.

Nothing checks that the pair still describes the room. The title remembers the
floor again whenever it can see one, so the pair follows the sensor, and a head
tilted by the motor or by hand is what the rotation above is for. If the stand
were moved (raised, lowered, or set down at a slant) while no floor was in view,
the tail would go on describing the old place until the tracker fitted a floor
again: the driver would aim for a floor at the old height and the frame path
would publish a floor where there is none. A console may make the driver search
again after such a move using `status_flags & 0x10` at +0x2C **[inference]**;
xenia does not detect that move.

#### The block's second consumer: the skeleton frame's gravity vector

The auto-tilt driver is not the only thing that reads this block, and the other
reader matters much more to whether a title accepts a player. `0x82d2d460` turns
the same three accelerometer longs into `normalize(-accel_x, accel_y, accel_z)`,
and the frame-processing path writes it into every published skeleton frame as
`vNormalToGravity` (`frame+32`, `0x82d23234`), the message 0x2B002 field listed
above as "normal-to-gravity". Title `4D5308ED`'s own join classifier
(`0x82b716b0`) measures its body- and head-offset cues perpendicular to that
vector (see "Placement and joining"). So the angle xenia reports does not merely
tell the runtime where to stop the motor: it defines which way is down for the
title's skeleton geometry.

For a real sensor that is exactly right once the angle is the measured one: the
gravity vector then comes from the sensor's own accelerometer, so it matches the
depth image whether or not the motor went where it was told, which on a sensor
whose tilt drive has failed it does not. With the requested-angle model it is
right only while the motor obeys. For a **replay** it is a trap: the recorded
depth frames carry the pitch the sensor had when they were recorded and cannot
follow a motor, while the replay backend answers "where are you?" with the angle
the title last commanded (`ReplayNuiSystem::GetTiltDegrees`). A recording is
therefore faithful only while the replayed tilt matches the pitch it was made
at, and `--nui_tilt_override=<deg>` is how to hold it there (see "A recording
carries no tilt").

#### Where the angle comes from: the sensor's accelerometer

With a real sensor, the angle xenia reports and the block's accelerometer come
from the sensor's own accelerometer, which sits in the head the motor moves.
They say where the head really points, measured against gravity like the
elevation the title sets, rather than where it was last told to go.

* **The source.** `Kinect10.dll` has no flat entry point for the accelerometer;
  the 1.8 runtime offers it only as a method of the per-sensor interface. The
  Kinect backend resolves `NuiCreateSensorByIndex` as an optional ninth export,
  declares the interface's table up to the accelerometer's slot (`ISensor` in
  `src/xenia/nui/kinect/kinect_api.h`, slot 33), and uses it only when the
  runtime also exports `NuiCreateDepthFilter`, whose methods the interface lays
  out after the accelerometer, so the table is known to reach that far. Before
  the first read it asks two questions with known answers, the instance index
  (0) and the flags the flat `NuiInitialize` gave the sensor (0x20), which catch
  a separate, uninitialised instance and a table whose slot 27 answers something
  else. The accelerometer's neighbour is the slot that switches the infrared
  emitter off. Without the export, or when either answer is wrong, one line says
  so and the requested-angle model below stays for the session.
* **The angle read at open.** When the sensor opens, the depth pump makes one
  `NuiCameraElevationGetAngle` call, independently of which title is running. A
  second call of a session has been seen to block in the motor driver for good,
  so the backend makes no other: every later angle is the measured one, or the
  angle the motor last accepted.
* **Its own thread.** Everything else the tilt motor does runs on the
  `NUI Kinect Tilt` thread, and only once something has asked for it: the first
  xam tilt call that finds a sensor present (`XamNuiCameraTiltGetStatus`,
  `XamNuiCameraElevationGetAngle`, `XamNuiCameraElevationSetAngle` or
  `XamNuiCameraRememberFloor`). The depth preview overlay and
  `--nui_log_play_frames` read the angle without asking
  (`NuiSystem::PeekTiltDegrees`). The calls only ask, because they run on the
  title's threads; starting the thread, like watching it, is the pump's, which
  starts it once the sensor is ready and depth is flowing, says so once at Info
  (`NUI: tilt thread started on the title's first tilt call`), and starts it
  again each time the sensor opens after that. The request lasts for the
  process: a title opened after one that asked, in the same running xenia, finds
  the thread running. Until something asks there is no tilt thread, so the motor
  is not moved and the accelerometer is not read; before any request in the
  process, a title making no tilt call gets only the angle read at open. Until
  the thread's first reading the calls are answered as without an accelerometer
  (below), from the angle read at open: title `4D5308ED`'s first status poll,
  just before its first command (-25), gets a block made from that angle, and
  the first reading follows once the thread has applied the command. The thread
  applies the title's tilt requests as they come (it looks every 50 ms, and a
  command blocks it, not the depth frames, for the second or so the motor takes)
  and reads the accelerometer every 250 ms. A reading points at the floor in g
  on the sensor's axes, y up and z the way it looks, so level is (0, -1, 0), and
  the elevation is `atan2(-z, -y)`, positive for "look up"
  (`kAccelerometerElevationSign` in `src/xenia/nui/nui_accelerometer.h` is the
  one sign). A reading that is not about one gravity pulling toward the sensor's
  underside is ignored. The whole-degree angle reported is held until the
  measurement is 0.75 degrees away from it, so a head resting near a half degree
  does not flicker. The first reading is logged at Info (`NUI: the Kinect
  accelerometer reads (x, y, z) g, an elevation of N ...`), and every whole
  degree of travel after it at Debug.
* **What wins.** `--nui_tilt_override` first, then the measured angle, then the
  angle the motor last accepted (or the one `NuiCameraElevationGetAngle`
  returned when the sensor opened). The block follows the same order: the
  measured vector, else the angle. The replay backend has no accelerometer and
  keeps answering with the angle the title asked for.
* **The hang guard.** The motor commands and the accelerometer reads go to the
  same device as that angle read, so every call the tilt thread makes into the
  sensor carries a due time: 10 s for a motor command, which blocks for a second
  or so by design and has been seen past 3 s beside the pump's frame fetches,
  and 2 s for a read. The pump, which never waits for it, takes over a call that
  is overdue. A command slower than 2 s is logged at Info with its duration, and
  should a call the pump gave up on ever come back, one Warning says so with its
  duration. The Warning `NUI: <call> has not returned within <N> ms. The Kinect
  tilt thread is left inside it ...` means: the tilt thread is left inside the
  driver and makes no other call; tilt requests are refused from then on, the
  accelerometer is no longer read, and the angle reported is the last one the
  motor accepted; the depth stream keeps running while it can, but when the
  sensor session ends the runtime is neither shut down nor initialised again and
  the device goes to error, and that backend will not reopen the sensor in this
  process; and `Kinect10.dll`, the backend's events and the backend object
  itself are all kept alive. Closing the sensor gives up on the tilt thread the
  same way, with the same consequences, when a call is still in flight 3 s after
  the close asked it to stop (`NUI: <call> did not return within 3000 ms of the
  sensor closing. ...`) or when a thread taken over between calls still has not
  left a second later (`NUI: the Kinect tilt thread would not stop. ...`).
* **The switch.** `--nui_accelerometer=false` means no accelerometer, not no
  tilt thread: the thread still starts and applies tilt requests, but never asks
  for the sensor's interface (no `NuiCreateSensorByIndex`) and leaves the
  requested-angle model: every question is answered with the angle the motor
  last accepted, and the block is synthesised from it. It is read each time the
  thread starts, and is the thing to reach for if an accelerometer read is the
  call the Warning above names.
* **The tilt loop.** A measured block settles the runtime's auto-tilt driver
  only when the head really is within 2 degrees of its target. The block's
  moving bits keep the driver waiting while a move is in progress, and the
  backend drops a repeat of the target it last gave the motor for 3 s after that
  command returned (`TiltRequestGate`), reporting a refused command at most once
  in that window.

On a live sensor the first reading agreed with the runtime's own read at open
(an elevation of -20.0 degrees against -20), the floors the tracker fitted
agreed with the measured angle to within about 3 degrees, and with the motor
commands on the tilt thread the depth feed held about 30 frames a second through
every command, where each command made from the depth pump had cost 2-7 frames.
The separate tilt thread preserves depth delivery while motor commands block it.

### Block decompression: the `LDI*` exports

`database.gmsodf` is one LZX stream cut into blocks, and the ST loader inflates
it with four xboxkrnl exports. The argument lists below are read off the two
call sites in the title (`0x82d659d4` and `0x82c08120` for the create; the
game's own asset loader at `0x82c08060` is the more talkative of the two):

```
int LDICreateDecompression(u32* pcbDataBlockMax,   // in: 0x8000 in both callers
                           void* pvConfiguration,  // in: +0 = LZX window size
                           void* pfnAlloc,         // both callers pass NULL
                           void* pfnFree,          //   "
                           void* pvWorkBuffer,     // caller-owned decoder memory
                           u32* pcbSrcBufferMin,   // out: source buffer needed
                           u32* pmdcContext);      // out: the context handle
int LDIDecompress(u32 context, const void* src, u32 cbSrc,
                  void* dst, u32* pcbDst);         // in: room, out: produced
int LDIResetDecompression(u32 context);
int LDIDestroyDecompression(u32 context);
```

All four return 0 on success; the callers only test for zero. The ST loader
passes a window size of 0x80000 and a work buffer of 0x83210 bytes (0x80000 of
window plus 12,816 of state); the asset loader passes 0x8000 and checks after
the call that `*pcbDataBlockMax <= 0x8000` and `*pcbSrcBufferMin <= 0x9800`.
Because xenia decompresses on the host, `pvWorkBuffer` is ignored and a context
is just a handle into a table in `xboxkrnl_lzx.cc`; the decoder itself is
`xe::kernel::xboxkrnl::LzxDecompressor` (`lzx_decompressor.{h,cc}`) on top of
mspack's `lzxd`.

The stream framing, as the loader walks it (0x82d64d90): a 443-byte header
(`u16 0x5C42`, `u16 version`, `u16 header size`, `u32 uncompressed size`, ...)
followed by blocks of `[u16 big-endian compressed size][compressed bytes]`. Each
block holds exactly one LZX frame, so `*pcbDst` going in is
`min(remaining, 0x8000)` and the loader fails the whole load unless exactly that
many bytes come back. The decoder state carries over from block to block
(`LDIResetDecompression` is never called on this stream; the game's asset loader
does call it after every block, so each of its blocks is its own stream).
The database in title `4D5308ED` is 690 full frames plus a final 4096-byte one.

### Guest object types: the Ob layer

A title can define kernel object types of its own. An `X_OBJECT_TYPE`
(`src/xenia/kernel/kernel.h`, 0x1C bytes) is a table of guest procedures
(`allocate_proc` at +0, `free_proc` at +4, `close_proc` at +8, `delete_proc` at
+0xC) and a pool tag at +0x18. `ObCreateObject(type, attributes, size, &object)`
allocates the object through `allocate_proc` and puts an `X_OBJECT_HEADER`
(`src/xenia/kernel/xobject.h`, 0x10 bytes: pointer count, handle count,
`object_type_ptr` at +8, flags) in front of it. `ObOpenObjectByPointer` hands
out a handle to the object, `ObReferenceObjectByHandle(handle, type, &object)`
checks the handle's object against the type the caller names, and `NtClose`
closes the handle. On the console the kernel runs the type's procedures along
the way: `close_proc(object, handle_count)` on every close, with the object's
handle count as it stood before the close (so 1 means "this was the last
handle"), and `delete_proc(object)` followed by `free_proc` when the last
pointer reference goes.

**What xenia does.** xenia keeps a host-side `XObject` for every object a title
holds a handle to, with a type of xenia's own. A guest object whose body starts
with a dispatch header is wrapped on first use by `XObject::GetNativeObject`,
which records the object's guest address on the wrapper and stashes the
wrapper's handle in the object's dispatch header. On top of that,
`ObOpenObjectByPointer` records the guest type from a bounded read of the
object's preceding header. `xboxkrnl_ob.cc` handles a *title-defined* type: a
cached type naming a readable `X_OBJECT_TYPE` that lies outside the block of
ten types xenia itself
keeps in the kernel's guest globals (`ExThreadObjectType` to
`ObSymbolicLinkObjectType`) and that has an `allocate_proc`, as every type
`ObCreateObject` can create objects of must. The last test is also what tells a
header xenia allocated itself (`XObject::CreateNative`, whose private type is
zero-filled) from one a title's `ObCreateObject` wrote. The decisions are plain
functions in `src/xenia/kernel/xboxkrnl/guest_object_type.{h,cc}`, tested in the
kernel suite.

- **Allocate:** `ObCreateObject` runs `allocate_proc`.
- **Reference:** `ObReferenceObjectByHandle` checks the cached guest type first.
  When one is recorded, only that type matches; the host wrapper's type cannot
  override it. Without a cached guest type, the host type determines the match.
  Reference and close do not read a preceding Object Manager header.
- **Close:** `NtClose` (and XAM's `CloseHandle` and `XamTaskCloseHandle`, which
  close through it) runs `close_proc(object, count)` on the calling guest thread
  with no kernel lock held, on every close from a guest thread. `count` is the
  number of guest opens across the object's live handles before this close;
  pointer references are excluded and duplicate handles are counted together.
  It then releases the handle (a Debug line,
  `NtClose(<handle>): close procedure <proc>(<object>, <count>) of type
  <type>`). A close from a host thread has no guest thread to run the
  procedure on, so it skips it (a Debug line, `NtClose(<handle>) on a host
  thread: close procedure <proc> of type <type> is not run`) but releases the
  handle and sees to the dispatch header just the same (see "After the last
  handle" below); none does, since xenia's own two callers of `NtClose` are
  exports that run on the title's threads. A `close_proc` that is not guest code
  (not aligned, not readable, or outside every loaded module's code) is not
  called and is reported once at Warning, since a header read in front of an
  object `ObCreateObject` did not make can name anything.
- **Open:** `ObOpenObjectByPointer` hands the wrapper's own handle to the title
  on the first open of such an object, whoever made the wrapper, and takes a
  reference of its own only on later opens. A nonzero cached guest type before
  the open records that the wrapper's handle has already been handed out;
  savestates retain that cache in the upstream object layout. Thus the title's
  first matching `NtClose` can bring the count to zero.
- **After the last handle:** the object's dispatch header still names the closed
  handle, and xenia can give a freed handle slot to the very next object it
  creates. So, under the same lock as the release, the header is pointed at a
  handle the object still has, left alone when that slot already belongs to a
  new wrapper of the same object, or otherwise given back an empty wait list,
  and the next kernel call on the object wraps it afresh.
- **Delete:** not run (see "Known limitations").

Every other object (xenia's own events, semaphores, mutants, threads, files, and
every object with no header xenia can read) is referenced, opened and closed as
xenia always has, with one addition: the wrapper `GetNativeObject` makes for an
event, mutant or semaphore in guest memory carries the object's guest address.
So a host-typed `ObReferenceObjectByHandle` of it returns that address, and the
matching `ObDereferenceObject` finds the wrapper and releases its reference,
where the reference would otherwise return 0 and the dereference log `Null
native ptr in ObDereferenceObject!`.

**Why it matters beyond one title.** A title's runtime can keep state that only
its own close procedure restores. Without it, a single open-close cycle of such
an object disables it for the rest of the process: the state the close should
have reset stays set, and nothing in the log says so, because `NtClose` answers
success.

**The worked example: the NUI image streams.** The NUI runtime linked into
title `4D5308ED` creates its stream objects with its own type at `0x8381E234`
(pool tag `Nuio`: `allocate_proc 0x82D29DB0`, `free_proc 0x82D29DB8`,
`close_proc 0x82D2AB00`, `delete_proc 0`); xenia wraps each as an event, because
the stream object begins with a synchronisation event the runtime signals when
it parks a frame. `NuiImageStreamOpen` hands the title a handle with
`ObOpenObjectByPointer`; `NuiImageStreamGetNextFrame` (0x82d29f88) and
`NuiImageStreamReleaseFrame` (0x82d2a298) begin with
`ObReferenceObjectByHandle(handle, 0x8381E234, &object)` (0x82d29fd4,
0x82d2a2dc); and the title closes a stream with `NtClose`. The close procedure
keeps the object in `r26` (`mr r26, r3` at 0x82d2ab0c) and its second argument
in `r31` (0x82d2ab10), returns at once unless the process is a title and that
argument is exactly 1 (`cmplwi cr6, r31, 1` at 0x82d2ab20), finds the object
among the runtime's stream slots (0x82d2ab50), drains its request queue and
calls the stream teardown in mode 0 (0x82d2ac8c), the only code that puts the
stream's slot count `[obj+24]` back to its idle 2 (320x240 stream) or 3 (80x60
stream) and re-allocates the idle ring. The title's menus open two of these
streams, a 320x240 depth-plus-player-index stream and an 80x60 one, and its hand
cursor's position writer (`0x82a67a38`) runs only after a frame has been fetched
from each. A kernel that compares the caller's type only with its own host types
refuses every one of those references with `STATUS_OBJECT_TYPE_MISMATCH`, so no
frame is ever fetched or released and no cursor is drawn; one that never runs
the close procedure leaves the slot counts of the menu's two streams at 3 and 4
after their first close, and every later open answers `0x83010003` ("already
open"). With both handled, on a replay every reference is accepted by the
object's own header, and each close, every one with a count of 1, puts the slot
counts back to 2 and 3.

The title's main thread does not wait on the stream event. The runtime replaces
the caller's wait only when it is infinite or above 8,000 ms
(0x82d2a028-0x82d2a034), and the UI asks with 0 ms (`li r4, 0` at 0x82a54060),
so each tick is a poll; on a replay the main thread ticks at the rate the
runtime publishes frames (20 to 29 a second) whether the streams are open or
not.

### Frame transport: `PsCamDeviceRequest`

`NTSTATUS PsCamDeviceRequest(NuiCameraRequest* request)` (xboxkrnl ordinal
0x30D). The runtime calls it through a function pointer at 0x8381e214 that is
initialised to the import thunk (0x835af854) and only replaced by an XStudio
playback shim when `XamXStudioRequest(1, ...)` succeeds. The common header
(`src/xenia/kernel/xam/nui_camera_request.h`):

```
+0  u32 code
+4  u32 arg0        code 0: receives the device state
+8  u32 arg1
+12 u32 arg2
+16 u32 context     opaque to the kernel
+20 u32 completion  guest routine(request, NTSTATUS status, u32 bytes)
+24 u32 image_type  code 5: 4 = depth, 2 = colour (YUV)
+28 u32 buffer      code 5
+32 u32 length      code 5
```

Return conventions (0x82d20e68, the runtime's synchronous submit helper): a
negative NTSTATUS fails the request; `0x103` (STATUS_PENDING) means the
completion routine will be called later with the final status; any other
non-negative value is treated as completed synchronously. The runtime maps
`0xC000009D` (STATUS_DEVICE_NOT_CONNECTED) and `0xC00000A3`
(STATUS_DEVICE_NOT_READY) to "camera went away" (0x82d28c78).

The completion routine is guest code, so xenia has to call it from a thread that
can run guest code: the audio system's pattern is the model
(`src/xenia/apu/audio_system.cc`: an `XHostThread` worker calling
`processor_->Execute(worker_thread_->thread_state(), callback, args)`), with
`request`, `status` and `bytes` as the three arguments and the frame bytes, its
sequence number and its timestamp already in guest memory when the call is made.

#### Request codes

| code | who | block | meaning (what the runtime does with the answer) |
|---|---|---|---|
| 0 | probe 0x82d24030 from the worker every 30 ms, **and re-probes from the completion routines** (0x82d24528 depth-failure, 0x82d24a48 colour, 0x82d242f4 stop) | `{0, out state}` | `arg0` = device state: 1 not present (keep polling), 2 ready, 3/5 present but unusable, 4 present and needing configuration, 6/7 send code 2 and retry. **4 and 2 are a sequence, not alternatives** - see below. |
| 27 | configuration step 1 | `{27, ...}` with `+28` = 1 plus 2 when the flags have 0x40 | enable streams: bit 0 depth, bit 1 640x480 colour |
| 35 | exposure manager 0x82d283f0 / 0x82d28d28 (async, completion 0x82d28ad0) | 92-byte block at `g+2484` copied from the per-stream settings (`g+1804 + 388*i`: `+4` = 2 for 640x480 else 1, `+32/+36` 0x1999, `+40/+44` 0xCCCC, `+48..+56` 0x10000, 16.16 fixed point; `+40` negated before sending); completion reads `+24` back as a byte counter; the block as observed is quoted below the table | colour auto-exposure control; not needed for depth |
| 36 | exposure manager, synchronously (submit helper 0x82d20e68): 0x82d28508 for stream `i`, issued by the getters at 0x82d28974, 0x82d28ff4, 0x82d29074, 0x82d290a8 and 0x82d290dc whenever the value they want is not cached, and 0x82d28844 (inside 0x82d286c8) | 144 bytes on the sender's stack with only `+0 = 36` and `+20 = 0` set (the submit helper installs completion 0x82d20e40 there); the answer is the 92-byte code-35 payload at `block+28` (`NuiCameraGetExposureRequest` and `NuiCameraExposure`, `nui_camera_request.h`) | the exposure read-back. 0x82d28508 copies the low nibble of `+28` into the four "cached" bits of `[g+1712 + 388*i]` and `[g+1804 + 388*i]` and `+76..+88` into `+48..+60` of both (bits 1-3 mark `+48..+56`, bit 0 marks `+60`; 16.16 fixed point); 0x82d28844 reads the single word at `+92`. A failure sets no state: every getter asks again and hands its caller `0x8007048F`, which the title itself sees at 0x82a58b40 and 0x82a84020. xenia (`NuiCamera::HandleGetExposure`) answers with the payload the title's own code-35 block sends, all four cache bits set: `+28 = 0x8000000F`, `+32 = 2`, `+36..+56 = 0` (the title sends 1 and 2 at `+52`/`+56`, which neither reader uses), `+60/+64 = 0x1999`, `+68/+72 = 0xCCCC`, `+76..+88 = 0x00010000` (1.0; the title sends 0 at `+88`, which would read back as no exposure at all), `+92 = 0`, and `X_STATUS_SUCCESS`; without a device it gets `X_STATUS_DEVICE_NOT_CONNECTED` like every other non-zero code. The runtime asks for it twice after its configuration and then never again, because the answer marks all four values valid |
| 13 | 0x82d23e78 | `{13, out}`; `+4` byte major, `+5` byte minor | firmware version: major >= 5 and minor >= 1 selects code 14, else code 11 |
| 24 | 0x82d23e78 | `{24, 1, buffer (g+8760), 8192}` | fill an 8192-entry u16 table with **the depth in millimetres each raw value stands for**. The skeletal tracker reads it with an unsigned halfword load (0x82d4f0b4) and uses the entry as its metric depth, with no scaling of any kind; 0 is "no reading" and anything above 3999 becomes the sentinel 8000. The kernel fills it from `xe::nui::LutDepthTableEntry`; on failure the runtime keeps the identity ramp it pre-filled, which is what a camera whose raw value already is a depth would want. xenia calls this request `kNuiCameraRequestGetRawToDepthTable` and logs it as `get raw-to-depth table` (`nui_camera_request.h`, `NuiCamera::HandleGetRawToDepthTable`); it is the table the title's shaders call `g_SamplerShiftToDepth`, and it is not the depth calibration of codes 11/14 |
| 33 | 0x82d23e78 | `{33, ..., +28 = 1}` | issued after 24, before 13 (mode select) |
| 11 | 0x82d23658 (old firmware) | ~4.1 KB: `+8..+112` thirteen doubles, `+112` 2048 u16 entries, `+4212` u16 | depth calibration as a table; see "Calibration source" |
| 14 | 0x82d23880 (new firmware) | ~4.4 KB: doubles at `+4232`, `+4240`, `+4256`, `+4264`, u16 at `+4248`, `+4250`; registration integers at `+12..+28`, `+76`, `+84`, `+96..+120` (0x82d23a9c..0x82d23c80) | depth calibration as parameters; see "Calibration source" |
| 25 | 0x82d20d28 | 64 bytes, `+4..+56` fourteen u32, in microseconds | timing parameters, multiplied by 50 into `g+1160..g+1212` in order (0x82d20d90-0x82d20e34). **Only `+36` (`g+1192`) is ever tested**, and a zero there skips the tail of the state-4 configuration and with it code 15 (0x82d24118), the whole body of the per-frame graphics callback (0x82d259e0), the exposure scheduler (0x82d21580) and a branch of the colour completion (0x82d24b38). The kernel answers `+36` with 33,333 - one 30 Hz frame, x50 = 1,666,650 ticks, the runtime's own hard-coded modulus at 0x82d241f0 - and zero everywhere else: on those four paths every division is by a constant (10, 50, or that 1,666,666), so no other value is a divisor, a modulus or a loop count. `+20` and `+40` are read nowhere; `+4` and `+24` head two four-value groups, one per stream, that the scheduler walks in its two passes. |
| 15 | 0x82d21440 (async, completion 0x82d213a8) | 44 bytes `{15, .., +16 ctx, +20 completion, +24 u16, +28 u32, +32 u16, +36 u32, +40 u16}`; `+28` is code-25 `+4` and `+36` is code-25 `+24`, each multiplied by 50 and divided by 50 again (0x82d214c4-0x82d214d4), so both are zero as long as the kernel answers only code-25 `+36`. `+24` is 2 from the configuration | set timing; last step of the state-4 configuration (0x82d24268) and also sent by the exposure scheduler (0x82d21f38). Nothing acts on the values; the kernel completes it asynchronously, which is what clears the runtime's "a timing change is in flight" flag `g+1452` |
| 5 | 0x82d20fd0 (depth), 0x82d21298 (colour), re-queued from the completion routines | header above with `image_type`, `buffer`, `length`; `+40` points at `request+64`, where the frame's 16-byte result goes: `+64 u32` sequence number, `+68 u32` the kernel does not write, `+72 u64` timestamp; `+92` buffer again, `+96` offset of the slot in the ring. The block is 8 bytes into a 128-byte descriptor (see "Buffers and rings") | **queue a frame buffer.** Return STATUS_PENDING; when a frame has been written into `buffer`, write its sequence number to `request+64` and its timestamp to `request+72`, and call `completion(request, 0, bytes)`. The frame processing (0x82d22ef0) makes `+64` the skeleton frame's `dwFrameNumber` and `+72` its `liTimeStamp` (0x82d2324c-0x82d2325c), and the image frames take their timestamps from the same word. xenia writes the guest's uptime in **milliseconds**, big-endian and never 0, into depth and colour frames alike, and leaves `+68` as the runtime left it (`WriteNuiCameraFrameResult`, `nui_camera_request.h`); it checks that the 80 bytes through the timestamp are writable when the request is queued, and the 16-byte result area (and the frame buffer) again when a frame is delivered. The unit is inferred from what the title does with it, and a stamp that never changes keeps the avatar's limbs still (see "The avatar"). `bytes` must be 0x96000 (640x480x16-bit) or 0x67200 (640x480 packed 11-bit, which the runtime then asks the kernel to unpack with code 30); any other size is recorded as STATUS_INVALID_BUFFER_SIZE. A negative status makes the runtime re-probe with code 0. |
| 30 | 0x82d24598 | `{30, buffer, 0x67200, buffer, &out}` with `out = 0x96000` | unpack a packed 11-bit frame in place; success and `out == 0x96000` clears the packed flag |
| 2 | 0x82d242c8 | ~0x30 bytes | stop after device states 6/7 |
| anything else | not seen | -- | answered `X_STATUS_NOT_SUPPORTED` (0xC00000BB) with a device present, and `X_STATUS_DEVICE_NOT_CONNECTED` without one like every other non-zero code. Not `DEVICE_NOT_CONNECTED` while a device is there: the runtime maps that status to "camera went away" (above), so a request code xenia has not learned yet would tell the runtime its sensor had vanished. |

In state 4 the worker configures the camera with codes 27, 24, 33, 13, then 14
(or 11 on old firmware) and 25, and last 15; the exposure manager sends 35
asynchronously and reads it back with 36. Then the runtime queues its four depth
and four colour buffers (code 5), and the kernel completes each with a converted
host frame, at most one per stream per published host frame.

**Device state 4 means "present, configure me"; state 2 means "configured and
streaming", and the runtime expects to see 4 once and 2 from then on.** Three
routines re-probe code 0 while frames are flowing, and their state tables are
not the probe routine's: the colour completion (0x82d24708, table at
0x82d24a64-0x82d24ab4) maps **only state 2** to `[g+92] = 1` and maps 1, 3, 4,
5, 6 and 7 alike to `[g+92] = 0`, i.e. "the stream is not live". It takes that
re-probe on its ordinary success path whenever `[g+2608]`, the "a code-35
exposure change is in flight" flag that 0x82d28e80 sets from inside the state-4
configuration itself, is non-zero.

So a kernel that answers 4 forever tears its own pipeline down: the
configuration sets `[g+92] = 1`, the worker enqueues a frame, the first colour
completion re-probes, reads 4, clears `[g+92]`, and the next wake stamps the
queued item as dead, skips frame processing, unregisters the graphics callback
and parks the worker in the drain loop at 0x82d2648c, which waits without a
timeout for in-flight requests that every completion re-queues, so it never
comes back. Answering 2 once the configuration has been run (xenia latches it on
code 25, after the calibration queries) keeps `[g+92]` at 1 and the worker
processing frames. Answering 2 from the start is equally wrong: the runtime
would skip codes 27/24/33/13/14/25 and never build its depth table.

Because the runtime re-probes from inside its completion routines, every code-0
request is worth seeing: xenia logs the answer at Info (under
`nui_log_messages`) only when it changes, and at Debug every time.

The code-35 block as the runtime sends it (big-endian dwords; the completion
routine at +20, the 0x80000000 stream flag at +28, the 640x480 mode at +32, then
the 16.16 exposure limits):

```
00000023 00000000 00000000 00000000 00000000 82D28AD0 00000000 80000000
00000002 00000000 00000000 00000000 00000000 00000001 00000002 00001999
00001999 0000CCCC 0000CCCC 00010000 00010000 00010000 ...
```

#### Buffers and rings (camera open, 0x82d254f8)

- Depth: one physically contiguous 0x258000-byte allocation (`g+104`, via
  0x82ae0658 → 0x831b2638 with attributes 0x8C9C0000, then
  `MmSetAddressProtect(..., 4)`), split into **four 640x480 16-bit frames of
  0x96000 bytes**; the whole allocation is also bound as **one 1-D
  `k_16_16_16_16` texture 307,200 texels wide** (0x82d25578, format word
  0x1A22005A) so the ST shaders can sample any slot. That width is what makes
  xenia's wide-1D row cap matter: at 8192 texels per row it needs 38 rows, and a
  cap below that leaves the tail of the ring unmaterialised
  (`kTexture1DWideMaxRows`, `src/xenia/gpu/xenos.h`). Four 128-byte descriptors
  at `g+128 + 128*i` hold the request block at `+8` (so
  `request = descriptor + 8`), the completion routine 0x82d24360 at `+28`, image
  type 4 at `+32`, the slot address at `+36` and `+100`, 0x96000 at `+40`, and
  the slot offset at `+104`.
- Colour (only when the flags ask for it; this title does): a second ring of
  four slots of `n*0x4B000` bytes (`n` = 2 for 640x480), bound as a `k_8_8_8_8`
  texture (0x1A220086); descriptors at `g+648 + 128*i` with completion
  0x82d24708 and image type 2. 0x96000 = 640x480x2, i.e. YUV 4:2:2.
- All four depth requests (and four colour requests) are queued at once when the
  probe reports the camera usable; the runtime keeps one "latest frame"
  (`g+124`), re-queues the previous latest frame as soon as a new one lands
  unless a consumer holds it, and counts drops from the sequence numbers.
- Signalling: the completion routine sets the KEVENT at `g+36`; the worker
  (0x82d25e60) waits on `{g+52 frame returned, g+36 new frame, g+68 stop}`,
  takes the latest frame (0x82d25ca8), unpacks it if needed, and feeds it to ST
  (0x82d497d8 with the depth slot, the raw-to-depth table's texture and the
  session id). There is no polling of a shared status block and no notification
  involved: **the transport is request/completion on `PsCamDeviceRequest`.**

#### Pixel format

The ring holds 16-bit values that the ST shaders turn into depth through the
8192-entry lookup table whose data pointer sits at `g+8760` and whose texture
header at `g+8764` is bound to sampler 3: the **code-24 raw-to-depth table**,
which the title's own shaders call `g_SamplerShiftToDepth` (`ShadePixelUpfilter`
and `ShadePixelDepthOnly` are the only two that declare it; the sampler
assignment is read out of the image's CTABs). It is **not** `g+8648`, which the
title declares as `g_SensorDepthToRegistrationXShift` on the colour-registration
path. So a raw value is a **table index below 8192**: the sensor's raw disparity
("shift"), which is 11 bits (0..2047) on a Kinect. The packed 11-bit frame size
(0x67200 = 640 x 480 x 11 / 8) confirms 11 significant bits per pixel. How the
11 bits sit inside the 16-bit slot (low-aligned, or shifted) has not been
observed; the table lookup makes low-aligned the natural reading, and the
runtime's own remap loop indexes the table with the raw value itself, so xenia
writes the index low-aligned (`xe::nui::kLutIndexSlotShift`). Orientation is
row-major 640x480 (the texture width is 307,200 texels of 4 pixels).

**Left to right: the guest gets each row reversed.** The host holds a depth
frame (`xe::nui::DepthFrame`) in the orientation the Kinect for Windows runtime
delivers its 640x480 depth in, and that is what the recordings hold and what the
depth preview overlay draws. The title's runtime expects the other one, so the
frame feed writes the conversion of the pixel at column `c` of a row to column
`639 - c` of the same row (`mirror_columns` in `xe::nui::ConvertDepthFrame`,
passed by `NuiCamera::FeedDepthFrame`, which is the only place the guest is
given depth). On a live sensor, rows fed unreversed made the title's hand cursor
move against the hand on X while Y was right, and the raft game answered jumps
but not a sideways lean. The cursor is `hand.x - hip.x + 0.5` in the title
(`0x82a67a38`), the lean is the torso's X and the jump is Y, so all three are
the runtime's skeleton X and Y, and one orientation explains them together; with
the rows reversed the cursor follows the hand. The guide overlay's cursor
(`MapNuiGuideHand`) grows with skeleton X in the same sense as the title's own.
Host frames are untouched, so the recordings stay valid and a replay is mirrored
on its way to the guest like a live sensor. The choice is one argument at the
call in `FeedDepthFrame`.

The ring is also bound as a `k_16_16_16_16` texture whose format word
(0x1A22005A) has endianness 1 = 8-in-16, i.e. the GPU byte-swaps every halfword
on its way in, so the ring holds **big-endian** 16-bit values; xenia writes it
with `xe::copy_and_swap_16_aligned` (`kRingIsBigEndian` in
`src/xenia/kernel/xam/nui_camera.cc`). That fetch constant also has
`sign = kUnsigned` and `num_format = 1`, so a raw value reaches the shader as a
plain integer 0..65535 with no player-index field in it; ST adds the player
index downstream.

**Two of the three 8192-entry lookup textures are fetched as *signed* integers,
and the third is not.** The two registration tables (`g+8652`, `g+8708`) are
created with format word 0x1A22AA58, whose four sign fields are all `kSigned`;
the raw-to-depth table the code-24 answer fills (`g+8764`) is created with
0x1A220058, whose sign fields are zero, i.e. **unsigned**, which is what the
tracker's own unsigned `lhzx` load of the same table expects. So an entry of the
*registration* pair arrives at the shader as a value in -32768..32767: **32767
is the ceiling there, not 65535**, and the runtime's own "no reading" marker
0x8000 (0x82d239a4, stored at 0x82d239f0) is simply the most negative integer
rather than a separate flag bit. The code-11 builder confirms it by
sign-extending each entry with `extsh` (0x82d237ec). That ceiling is the
constraint on the calibration unit below; it says nothing about the code-24
table, whose constraint is the tracker's: unsigned, 1..3999 millimetres, 0 = no
reading.

Both sides of the raw encoding are therefore decoded rather than open: the byte
order comes from the ring's own fetch constant and the index placement from the
table lookup the tracker performs, and xenia controls both ends of it: it
delivers the table index for a millimetre reading
(`xe::nui::LutIndexFromDepthMm`) and answers code 24 with the table that maps
that index back to the same millimetres (`xe::nui::LutDepthTableEntry`).

#### `k_DXT3A_AS_1_1_1_1`: the plane order, and the compare-and-pack resolve

The tracker's classification passes do not write an ordinary colour surface.
They resolve into `k_DXT3A_AS_1_1_1_1`, a DXT3 alpha block reused as **four
one-bit planes**: each 4x4 block is 64 bits, one nibble per pixel, one bit of
the nibble per plane. Getting the bit order wrong costs nothing visible and
everything functional, so it is worth stating exactly.

**Plane `c` is bit `c` of the pixel's nibble**: plane 0 in the LSB, planes 1, 2
and 3 at bits 1, 2 and 3. Plane 0's position is **measured**: the body-part
vertex shader `E6997924C033DF9E` reads the destination back as `k_16_16_16_16`
and runs sixteen `trunc(2 * frac(lane * c5.k))` bit tests over four 16-bit lanes
with `c5 = (2^-1, 2^-5, 2^-9, 2^-13)` (dumped live as
`3F000000 3D000000 3B000000 39000000`), i.e. bits 0, 4, 8 and 12 of each lane =
**bit 0 of every one of the block's sixteen nibbles**. Planes 1-3 are a strong
**[inference]** from the consumer: the classify pass forms
`label = x + 2y + 4z + 8w` out of the four planes, so the nibble read as a 4-bit
integer *is* the label, which is what "a 4-bit class id split into four one-bit
planes" means. Only the plane-to-bit column is pinned; the lane-to-row and
nibble-to-column arrangement is xenia's existing one, which no shader in this
title can see. With the planes in the opposite order (plane `c` at bit `3 - c`),
the vertex shader's population count is zero on every block and its `cndeq`
substitutes a QNaN into every vertex, killing all 600 primitives before the
clipper.

**How the planes get written: copy command 2 into a one-bit destination.** The
guest programs `RB_COPY_CONTROL.copy_command = kConvertTo1111` (2) with a
`k_DXT3A_AS_1_1_1_1` destination, which this title uses as a
**compare-and-pack** rather than a constant fill **[inference]** from its
producer and consumer: each source channel is compared against a reference byte
under a per-channel function and the boolean result becomes that channel's plane
bit. Three registers carry it, and only this copy programs them:

| register | this title's value | meaning |
|---|---|---|
| `RB_COPY_FUNC` | `0x00001111` | four `kLess`, one nibble per channel |
| `RB_COPY_REF` | `0x80808080` | reference byte 128 per channel |
| `RB_COPY_MASK` | `0xFFFFFFFF` | all eight source bits per channel |

The operand order is `reference function source`, inferred from this title
alone: its producer writes only 0.0 and 1.0 and its classifier needs a 1.0
channel to come out 1, which is `128 less-than 255` and not `128 less-than 0`.
xenia implements that reference-against-source compare in
`resolve_dxt3aas1111.xesli` (`XeResolve1111Component` / `XeResolve1111Pixel`).
`draw_util.cc` passes the three register values through
`ResolveCopyShaderConstants`. The shader packs RGBA into bits 0 through 3,
uses the selected MSAA samples and applies the destination's 64-bit endian
mode.

**Which blocks are written** depends on the resolve rectangle, not just the
copy registers: the destination addresses 4x4 blocks while
`RB_COPY_DEST_PITCH` carries the *padded storage* extent. Ordinary vertex-derived
resolves use eight-pixel expansion. When the resolve vertex fetch
is missing, xenia infers a rectangle from the scissor, destination extent and
EDRAM pitch **[approximation]**, rounds partial 4x4 blocks up, and bounds the
counts by the destination extent and source pitch. `copy_size_blocks` passes the
exact resulting counts to the resolve shader so dispatch padding cannot write
an extra block row or column; CPU destination-range calculation uses the same
counts. The inferred write can reach allocation padding (see "Known limitations"
for the FGDataHi case, and "Open questions").

#### Calibration source

The camera is asked for two quite different things, and they are worth keeping
apart.

**Code 24 fills the table the skeletal tracker's CPU reads** (`g+8760`): one
entry per raw ring value, each entry the depth in **millimetres** that value
stands for. The tracker's 80x60 pass loads an entry with an unsigned halfword
load (`lhzx` at 0x82d4f0b4) and hands it straight to its point cloud as `z`,
with no shift, no `fcfid` and no multiply between the two, and the tests it then
applies are in millimetres accordingly: a standing player at 1800..2920, a torso
50..300 deep, a silhouette of 0.2 to 2 square metres computed with the depth
camera's focal length of 583.3 pixels. It reads 0 as "no reading" and replaces
anything above 3999 with the sentinel 8000, the same rule the runtime applies
when it composes this table with the next one (0x82d239a8..0x82d239e4). So this
table is the sensor's conversion from a raw value to millimetres, which on real
hardware comes from the camera's firmware; the kernel fills it from
`xe::nui::LutDepthTableEntry`, inverting the ring encoding exactly: raw
2000..3999 carry 4000..400 mm, and every other raw value, 0 included, reads 0.

**Code 14 (or code 11) supplies the parameters the runtime builds its own
8192-entry tables from** (`g+8648`, and the derived `g+8704`). Those are bound
as shader samplers, not read by the tracker's CPU, and the shader that binds
`g+8652` declares it `g_SensorDepthToRegistrationXShift`, so this pair looks
like the depth-to-colour registration path rather than a depth source. That
reading is not yet acted on: the answer below stands as it is, and the unit
question it raises is open.

Two paths, chosen by the code-13 version answer:

- **Code 11 (table)**: the kernel returns thirteen doubles at `+8`, a 2048-entry
  u16 table at `+112` and a u16 `p` at `+4212`. The runtime builds `LUT[shift]`
  (0x82d23794..0x82d23828) by walking the table: entry `i` gives the shift at
  which depth step `i` starts, and all shifts up to the next entry get `value =
  (int16)(4 * f2 * ((i & ~3) - 4p))` where `f2` is the double at `+104`, i.e.
  `16 * f2 * (i/4 - p)`. Entries that are 0 or >= 0x2000 are skipped; shifts
  past the last entry get the last value.
- **Code 14 (parameters)**: with `a = +4240`, `b = +4232`, `c = +4256`,
  `d = +4264` (doubles), `w = +4248`, `h = +4250` (u16):
  `k = 1 / ((w/640) * h * a)`, then for `i` in 0..8191: `LUT[i] = (u16)
  trunc(16 * (d*k*h * (1 - b*k*h / (i*k)) + c))` (0x82d238d4..0x82d23994; the
  constant 16.0 is at 0x82017f20). The runtime then derives a second table
  `g+8704[i] = LUT[remap[i]]` for `0 < remap[i] < 4000`, else 0x8000
  (0x82d23998..0x82d239f8), and computes the shader constants at
  `g+8568..g+8644` from the remaining parameters (0x82d23a98..0x82d23e60 via
  0x82d20b98).

Both paths therefore produce their table in units of 1/16 of whatever unit the
parameters use, and an entry is read back signed, so the unit cannot be
millimetres: `mm * 16` caps the sensor at 2047.9 mm and makes 4000 mm arrive as
-1536. xenia uses **centimetres**, and the one line in the tree that expresses
that choice is `constexpr double kLutUnitsPerMm = 0.1;`
(`src/xenia/nui/nui_depth_convert.h`), which every one of `kLutCalibration`'s
parameters is derived from. It puts 4000 mm at 6400, well below the ceiling,
while still resolving 1/16 cm = 0.625 mm. Any unit of at least 1.96 mm would
fit; centimetres are deduced from the ceiling and from the millimetres the NUI
skeleton API works in (the runtime converts the floor plane's W from mm to m at
0x82d23230), not observed. `XamNuiGetDepthCalibration` is not imported by this
title.

xenia answers code 13 with version 5.1 and therefore takes the code-14 path.
Folding `k`, `w` and `h` into the expression leaves `LUT[i] = A - B/i`, a
hyperbola, so the parameters it picks (`xe::nui::kLutCalibration`, derived in
`src/xenia/nui/nui_depth_convert.h`) put the depth window 400..4000 mm on the
raw values 2000..3999: the far end halfway up the range is what makes the widest
table step as narrow as it can be, 3.6 mm at 4000 mm and 0.9 mm at 400 mm. Raw 0
and anything from 4000 up are "no reading" through the runtime's own remap rule;
raw values at or below 326 would overflow the 16-bit entry, and the encoder
never produces one, since its window starts at 2000. The values are `a = 1`,
`b = -30007.50375187594`, `c = -320.2800900450226`, `d = 0.1`, `w = 640`,
`h = 480` (`c` and `d` carry the centimetre unit; `b` does not, because it
multiplies `d`), and the registration integers the runtime turns into shader
constants are zeroed, because nothing is known about them. Zeroing them is safe:
the helper that turns them into the twenty constants at `g+8568`/`g+8608`
(0x82d20b98) is multiply-and-add only, with no division, so zeros cannot become
NaN or infinity, and the code-11 path deliberately `memset`s eight of those same
constants to zero on every old-firmware sensor (0x82d236a4, 0x82d236b4). They
feed `g_SensorDepthToRegistrationXShift`, which only appears in the `st\cdrp\`
depth-to-colour shaders, never in the `st\exemplar\` classifier.

### Notification bits

The title's notification handler (0x82aaa880) reacts to
`kXNotificationSystemNUIHardwareStatusChanged` (0x00060019) unless the
`NOCAMERA` setting is present: bit 0x4 of the parameter makes it call
`XamShowNuiHardwareRequiredUI(1)` (0x831af920 is a jump to that import), and the
parameter is then posted as an internal event (0x82aaa8fc..0x82aaaa28). So bit
0x4 means "the sensor needs attention"; xenia only ever sends 1 or 0.
`kXNotificationSystemNUIPause` (0x0006001A) is handled at 0x82aaa9c4 and
`kXNotificationSystemAvatarChanged` (0x00040017) at 0x82aaa924. xenia broadcasts
the hardware notification with 1 (ready) or 0 when the backend's status changes;
nothing in the title waits for it before initialising NUI (that is gated on
`NuiInitialize`, above).

### The engagement chain: from a tracked skeleton to an engaged player

Delivering frames is not enough for a title to accept a player. Title `4D5308ED`
runs a chain of its own on top of them, and three of its links pass
through xam. They are worth knowing because each of them can silently stop the
title on its attract page with nothing wrong in the log.

**1. 0x2B002 must be answered `X_E_SUCCESS` while the guide is closed.** The
runtime reads the return of the HUD skeleton-frame message as "has the system
HUD taken this skeleton?": `0x8301000B` sets `[g+96] = 1` (0x82d35ad8) and the
title's own skeleton processing stops for as long as it is set, because a
console in that state has the guide open over the game. xenia answers
`X_E_SUCCESS`, which is what a console with the guide closed behaves like, and
the title keeps its skeletons. The one exception is xenia's own Kinect Guide:
while it is up, frames that hold the player who opened it are answered
`0x8301000B`, exactly as a console's guide takes that player's skeleton (see
"The Kinect Guide"). Any other `0x8301000B` would cost the entire tracker.

**2. `XamNuiCameraElevationGetAngle`'s two outputs must not be swapped.** The
first is the angle, the second a tilt status bitfield (see "The tilt status
word"). The title's engagement state machine (0x82a5f578) polls it every frame
and tests **bit 0x2** of the second output: if it is set, the machine is forced
into state 4 ("check the sensor") and the decay timer that would let it fall
back to 0 is reset every frame, and the "a tracked skeleton was found"
transition is gated on the state being 0 (0x82a5f6dc-0x82a5f6f0), so a tracked
skeleton is discarded for as long as the bit is set. A second consumer tests bit
0x1 and gives up for the frame (0x82a596b8). An angle in the second output would
park the machine: -25 reads as `0xFFFFFFE7`, whose bits 0 and 1 are both set.
xenia writes the angle to the first and `kNuiTiltStatusOk` (0) to the second.

**3. The auto-tilt driver has to be able to settle.** It is the NUI runtime's,
not the title's, and it closes its loop on the tilt status **block**, not on the
reported angle (see "The tilt status block"). A block xenia left blank would
make the measured angle `atan2(0, 0) = 0` forever, so the driver would
re-command the motor without end. A block made from the sensor's measured
accelerometer settles it only when the head really gets within 2 degrees; one
that cannot get there leaves the driver asking again, which the Kinect backend
bounds (see "Where the angle comes from"). Nothing on the bind path reads the
driver's state directly, but the same block feeds the skeleton frame's gravity
vector, which the join classifier does read.

**What actually binds a player feed is title-internal.** The gate the engagement
object watches (`[obj+60]`, `[obj+61]` at 0x83ABA010) has exactly two writers in
the image: `USpringfieldPlayerManager::BindPlayerToFeed(idx)` and
`UnbindFeed(idx)`. Neither they nor their call sites read a `XamNui*` result, a
message answer, a notification or the tilt. A feed binds when

* the title's **own** per-feed skeleton copy (`0x83AD2790 + 480*i`, gated on the
  frame-valid byte at `0x83AD2750`) holds `eTrackingState == 2`
  (`FeedHasTrackedSkeleton`), **and**
* there is a join request on that feed, **and**
* the play-space test at `0x82b732d8` passes: a distance window applied to the
  feed's smoothed distance curve, followed by the disengagement classifier
  `0x82b716b0`, whose cues are described under "Placement and joining".

So xam's contribution is exactly these three: answer 0x2B002 with success while
the guide is closed, keep the tilt status word's bits clear, and report a tilt
the runtime can settle on. Everything after that is the title's own arithmetic
over the skeletons its tracker fits out of the depth frames. With the feed
bound, the title leaves "IS ANYBODY THERE?" and walks its play-space setup on
its own: `FACE THE SENSOR!`, a play-space diagram carrying a player token and a
dwell ring, `MOVE BACK AS FAR AS YOU CAN.`, and `YOU HAVE SELECTED:` with the
space it found.

### GPU read-back

The title's skeletal tracker classifies the depth image on the GPU and then
reads the results back on the CPU: its joint proposals come back through resolve
read-back and its HUD images through memory-export read-back. Both are off by
default in xenia (`readback_resolve = "none"`, `readback_memexport = false`),
and with them off the tracker locks and reads the bytes its own allocation left
behind, which is why it can report no player at all while nothing in the log
goes wrong.

Native, unscaled command-2 one-bit resolves write GPU shared memory, and the
D3D12 and Vulkan resolve read-back paths copy those packed bytes after GPU
completion. This also applies to native resolves under global resolution
scaling; it does not use a CPU packing bypass or a bits-per-pixel downscaler.
Genuinely scaled command-2 sources remain unsupported upstream: the copy is
skipped with a warning, though a requested clear can still occur. Tracking for
that case has not been established.

xenia therefore turns both on by itself, once, when a title registers its
skeletal-tracking runtime with message 0x2B003 (`readback_resolve` to `full`).
Nothing else triggers it, so a title that never sends that message is emulated
exactly as before.

**The change lasts as long as the title, and it is never written to the config
file.** So the next launch of xenia is back to whatever the config says, and
running a Kinect title once does not leave every other title paying the GPU-CPU
synchronisation these two settings cost. A title opened into the same running
process after a Kinect title does not inherit the raised values either:
`NuiState::OnTitleTerminated` puts back exactly the settings the registration
raised, and a setting somebody had chosen was never raised, so it is not touched
on the way out any more than it was on the way in.

**How to keep your own choice.** xenia cannot tell a setting somebody chose from
one nobody touched (no cvar records where its value came from), so what it tests
is whether the setting still holds its compiled default. A value that differs
from the default is treated as chosen and left exactly as it is, with a log line
saying so: `--readback_resolve=fast` or `--readback_resolve=full`, or
`--readback_memexport=true`, all survive, as do the same values written into
`xenia-canary.config.toml`. The two values that mean "off",
`--readback_resolve=none` and `--readback_memexport=false`, **are the compiled
defaults and cannot be told apart from them**, so they are raised anyway; there
is no way to ask for the tracker's read-back to stay off, and with it off the
tracker reports no player at all.

## Known limitations

* **Compatibility coverage.** Live gameplay is established for title `4D5308ED`
  on an Xbox 360 Kinect through runtime 1.8, using D3D12. It does not establish
  compatibility with other Kinect titles. The obstacle activity needs a matching
  external patch; its other variants and second-player path have not been
  separately verified. The avatar remains a translucent stand-in. Voice and
  Kinect ID sign-in are stubs.
* **Sensor lifetime.** Under `--nui=any`, an available Kinect backend opens an
  attached sensor for every title, publishes depth frames, reads its angle once
  at open, sets `KinectInitialized` and reports readiness even if the title
  never uses it. The tilt thread starts on demand, but its request remains set
  for the process: a later title inherits it. The depth preview and
  `--nui_log_play_frames` peek at the angle without requesting the thread.
  Use `--nui=nop` to keep the hardware closed.
* **Tilt fallback.** Real measurements require the Kinect backend and a
  compatible 1.8 per-sensor interface. Without it, with
  `--nui_accelerometer=false`, or after an abandoned tilt call, xenia uses the
  last accepted target (initially the angle read at open). A stalled motor is
  invisible to that model. With measurements, accelerometer reads pause during
  motor commands, and the block's moving bits cover that gap. Repeated targets
  are suppressed for three seconds; a head that cannot reach the target can
  still provoke later commands. A working live motor settles after the
  remembered floor supplies absolute targets.
* **Tilt shutdown.** Closing a sensor can wait up to three seconds for a motor
  call and one more second for a thread that fails to leave, within the pump's
  five-second bound. An abandoned call leaves its runtime and backend resources
  alive rather than shutting down underneath it; see the hang guard in the
  accelerometer section.
* **Floor and roll.** The tilt block's axes, non-zero flags and floor-tail
  layout are inferred from the title's arithmetic, without a console capture.
  The sideways accelerometer leg is zero because the guest roll sign is unknown.
  The remembered floor is not invalidated when the stand moves; if the tracker
  cannot see a floor, the old height persists until it can fit and remember
  another.
* **Replay fidelity.** A recording contains no tilt, so a mismatch between its
  pitch and the reported angle can repeatedly bind and lose a player. Use
  `--nui_tilt_override` at the recording's pitch. Menu timing remains
  nondeterministic: a replay may wait on START or enter a different level.
  Compare the actual screen or level reached, not elapsed time alone.
* **Training movies.** The introductory activity's demonstration drawing plays
  substantially slower than its source movie, with instruction text running ahead
  of it. One measured interval advanced at about 15 source frames per second
  against the
  authored 30 fps. Other samples show varying slowdown; a uniform exact
  half-speed error has not been established, and the text clock's rate has not
  been measured. The cause remains unresolved.
* **Colour and identity.** The colour ring receives constant black YUV 4:2:2.
  Identity messages provide the success answers needed for startup, without
  biometric enrolment, matching or callback delivery.
* **Guest object lifetime.** Xenia counts guest opens separately from pointer
  references. Before each close, a title-defined `close_proc` receives the
  number of guest opens across all live slots for the object, including
  duplicate handles and excluding pointer references. Title `4D5308ED`'s
  observed stream closes received 1. Guest `delete_proc` and `free_proc` are
  not called: xenia keeps no guest pointer count to determine the last
  reference, and does not reclaim that memory. A non-zero delete procedure is
  noted once at Debug; the NUI stream type's delete procedure is 0.
* **LZX coverage.** The `LDI*` implementation has one real-stream reference: the
  tracking database, 691 blocks and 22,614,016 decompressed bytes. Unit tests
  cover the shim's boundaries, not another title's asset stream.
* **Camera stop and title termination.** Title `4D5308ED` has not been seen to
  issue camera code 2; it sends it after states 6/7, which xenia does not
  report. The handler drains both rings with `TakeAll`, leaves their queues
  open, and completes requests with `X_STATUS_CANCELLED`; this is checked from
  code, not a title exercising the stop path. Window close calls
  `std::quick_exit` and bypasses orderly shutdown.

  A separate title-termination path brackets `KernelState::TerminateTitle`'s
  guest-thread kill loop. Before it, `NuiState::OnTitleTerminating` rejects new
  requests with `X_STATUS_DEVICE_NOT_READY` and stops and joins the feed while
  guest threads can still release locks. Internal feed poll/join waits use the
  host thread handle directly; the join has no timeout. After it,
  `NuiState::OnTitleTerminated` drops queued requests without calling into the
  dying module, resets camera/per-title state and restores raised GPU read-back
  settings. File > Close and save-state restore reach this path only in a Debug
  build. A Debug File > Close check completed the reset without an assertion and
  delivered no frames while the title was closed. Reopening in the same process
  reprobed camera state 4, configured the camera, rebuilt its depth table and
  delivered fresh frames. However, the rendered image remained frozen with
  repeated GPU interrupt callback lookup errors: full game restart failed.
  Restart the emulator process after File > Close. Save-state restore remains
  unverified.
* **GPU validation.** A Vulkan replay of title `4D5308ED` displayed
  play-space detection, a hand cursor, a tracked player and the Kinect
  Guide. This covers menu and tracking behavior; it does not establish
  native packed-byte correctness or
  shared-memory read-back visibility, which need targeted coverage. All five
  activities on Vulkan remain unverified. This title is the only exercised
  workload for wide 1-D texture sampling and preservation of memory-exporting
  pixel draws under multipass. Boot checks of unrelated titles did not enter
  those paths, so they do not validate them for another title.
* **GPU representation limits.** Wide 1-D sampling uses the guest coordinate
  and selected-row centre calculation. Native one-bit resolves use the GPU
  resolve shader and shared-memory read-back; packed bytes and visibility
  still need targeted checks.
  Plane 0's one-bit position is measured from this title's consumer, while planes 1-3
  remain inferred. Title `4D53085B` is another known user of
  `k_DXT3A_AS_1_1_1_1`, but its boot check issued no one-bit-plane resolve. The
  FGDataHi L1/L2 destinations are packed as 20x7 blocks: the scissor is 80x60
  and the destination register says pitch 32, height 7, although the texture
  fetch describes 10x8. The resolve never sees that fetch constant; whether
  hardware includes the eighth row is open.
* **Read-back cost.** In a packed one-bit read-back experiment, the measured
  NUI-loop rate fell by about 25%. The current shared-memory read-back path
  has not been measured, so its overhead is unquantified. Read-back remains
  enabled because three tracker `LockRect` textures have not been identified;
  they may need it.
* **HUD parsing coverage.** The 0x2B002 layout is pinned by `static_assert` and
  `nui_messages_test.cc`; the `KernelState`-dependent `HandleHudSkeletonFrame`
  parse has replay coverage but no direct unit test.

## Open questions

1. **Calibration units.** The signed registration tables' 32767 ceiling rules
   out millimetres, but centimetres remain a deduction. Their shader scale is in
   the Xenos microcode at 0x8203e033 and 0x8203e7e3. `kLutUnitsPerMm = 0.1`
   expresses the current choice. Also unknown are the thirteen code-11 doubles
   and the code-25 timing values other than `+36`.
2. **The console guide.** What does xam draw from 0x2B002's locked depth and
   player-index textures? Does it fill the second engaged id? The runtime saves
   both ids in globals this title does not read. What should the title's
   script-driven `ShowSystemGuide` receive from `XamShowNuiGuideUI`, observed as
   `(0, 255)`? Gesture-driven opening and closing already work, but this export
   is still a stub.
3. **Unmeasured tilt fields.** What do message 0x2B00A's three floats, reported
   statuses 3/5/6/7, tilt-block `status_flags & 4` and `& 8`, and floor flag bit
   0 mean on a console? A capture could also confirm the accelerometer axis
   assignment, tail layout and handling of an unreachable target or a moved
   stand.
4. **Colour and identity.** Must the colour ring be fed for progress, or is
   depth sufficient? What do 0x2C00C, 0x2A005-0x2A008 and 0x2C009 do beyond
   their present success answers, particularly the cached biometric stamp from
   0x2A008 and the callback registered at 0x82d31f18?
5. **One-bit resolves.** Does hardware include a partly covered block row in the
   FGDataHi case above? Which three textures does the tracker lock, and are any
   among its 22 one-bit masks? If none are, GPU-packed one-bit destinations may
   not need CPU read-back. Another title exercising planes 1-3 would test their
   inferred order.
6. **Avatar assets.** Serving real meshes, textures and animations would replace
   the stand-in and retire the obstacle activity's external patch. The title's
   avatar library parses a binary asset format through its twelve imports:
   `XamAvatarInitialize`, `XamAvatarShutdown`, `XamAvatarGetManifestLocalUser`,
   `XamAvatarGetAssetsResultSize`, `XamAvatarGetAssets`,
   `XamAvatarLoadAnimation`, `XamAvatarGenerateMipMaps`,
   `XamAvatarManifestGetBodyType`, `XamAvatarSetCustomAsset`,
   `XamAvatarGetMetadataRandom`, `XamAvatarGetMetadataSignedOutProfile` and
   `XamAvatarGetMetadataSignedOutProfileCount`. xenia supplies no system avatar
   assets; `GetAssetsResultSize` returns zeros and the title's overlapped
   `GetAssets` request completes with failure, while a call without an
   overlapped returns success without supplying assets. Console
   packages supplied by their owner, or independently created assets in the
   required format, would need a separate implementation.
7. **Backend lifetime and recording format.** Can sensor opening be deferred
   until needed without changing presence queries or startup timing? Can a
   recording carry its measured tilt so replay no longer needs an override?
