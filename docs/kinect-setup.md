# Kinect setup

This guide covers the experimental Kinect support in Xenia Canary Kinect.
Live hardware is supported on Windows. Linux supports replaying a depth
recording or selecting the no-sensor backend. The detailed protocol, settings,
test results, and limitations are in [nui.md](nui.md).

## Live sensor on Windows

The live backend is Windows-only and targets Kinect sensors through the Kinect
for Windows 1.8 runtime. The verified hardware setup is a Windows build, an
Xbox 360 Kinect with its PC power/USB adapter, and the Kinect for Windows SDK
1.8 installation. That installation provides the runtime and drivers. A
runtime-only installation and other sensor models have not received the same
end-to-end verification. Building this emulator does not require Kinect SDK
headers or import libraries; it loads `Kinect10.dll` at runtime.

Use Microsoft's [Kinect for Windows SDK 1.8 distribution](https://www.microsoft.com/en-us/download/details.aspx?id=40278)
for the runtime and drivers. Do not get or redistribute driver or SDK files
from this repository. Close other software that may be using the sensor before
starting the emulator.

Place the sensor where its head can tilt freely, with the whole person visible
in the depth preview. The tested setup places the sensor about 0.6–1.2 m high
and the player about 2.0–2.5 m away. The motor can move the sensor head during
automatic tilt. Do not force it if it resists movement.

## Run a live test

Keep this build in its own directory, separate from an existing Xenia setup.
From PowerShell, use your own build and a game image you are authorized to use.
This example selects the tested D3D12 graphics path and keeps default
configuration, cache, and guest storage under a new folder in the current
directory. Use a fresh folder for each test. An existing configuration can set
separate content or cache paths, so do not reuse a test folder that contains an
older config if you need to isolate saves:

```powershell
.\xenia_canary.exe "<path-to-game-image>" --storage_root=".\kinect-test-data" --gpu=d3d12 --nui=kinect --mute=true --allow_avatar_initialization=true
```

The `--allow_avatar_initialization=true` flag is needed for body-controlled
gameplay. Title ID `4D5308ED` is the only title with established live gameplay
in the current test record; one tester reported live completion of all five
activities. D3D12 is the recommended first graphics backend because it is the
route used for that gameplay evidence. For diagnostics, add
`--nui_log_messages=true`. See [nui.md](nui.md) for additional flags and
placement advice.

## Replay a recording on Windows

Replay needs no Kinect hardware or runtime. Point `--nui_replay` at a depth
recording you created yourself, and keep the test storage isolated:

```powershell
.\xenia_canary.exe "<path-to-game-image>" --storage_root=".\kinect-replay-data" --nui=replay --nui_replay="<path-to-recording.xdepth>" --mute=true --allow_avatar_initialization=true
```

On Linux, run your built emulator executable (`./xenia_canary`) with the
same `--nui=replay` and `--nui_replay=<path>` options and a fresh
`--storage_root=<new-directory>`.

To create a local recording, run the live example with
`--nui_record=".\person.xdepth"`. Recordings can be large (about 18 MB/s).
Depth recordings contain sensor data and may capture people. Keep them local
unless you have permission to share them. A recording does not include sensor
tilt; replay may require
`--nui_tilt_override=<pitch>` to match the recorded session. The recording and
replay commands are described in more detail in [nui.md](nui.md).

## Current limits

One tester reported live completion of all five activities in title
`4D5308ED`, with these qualifications: the obstacle activity needs a matching
external title patch, which is not bundled; a separate optional startup
workaround has been used to avoid forced selection of the first rafting
activity. The emulator package alone does not provide the complete five-activity
setup; the matching patches and instructions are maintained separately.
The avatar is a translucent stand-in, the tutorial movies run slowly, color
frames are black, and microphone support is incomplete. In Debug builds, using
File > Close and reopening a title in the same process can freeze
rendering; exit and restart the emulator before another run. See [nui.md](nui.md)
for the full evidence and open questions.
