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
This example selects the tested D3D12 graphics path. The opt-in
[Kinect launch preset](../config-presets/kinect.config.toml) enables avatar
initialization without changing the general default. Xenia writes settings to
the file selected by `--config`, so copy the shipped template into a new test
folder before launching. Use a fresh folder for each test: an existing config
can set separate content or cache paths, affecting storage isolation. These
commands assume the extracted archive directory. For a source build, run from
the repository root and replace `.\xenia_canary.exe` with the path to your built
executable; the preset path is relative to the repository root.

```powershell
$testRoot = ".\kinect-test-data"
if (Test-Path -LiteralPath $testRoot) { throw "Choose a fresh test folder." }
New-Item -ItemType Directory -Path $testRoot -ErrorAction Stop | Out-Null
Copy-Item ".\config-presets\kinect.config.toml" "$testRoot\kinect.config.toml" -ErrorAction Stop
.\xenia_canary.exe "<path-to-game-image>" --storage_root="$testRoot" --config="$testRoot\kinect.config.toml" --gpu=d3d12 --nui=kinect --mute=true
```

The preset sets `allow_avatar_initialization=true`, which is needed for
body-controlled gameplay. You can instead pass
`--allow_avatar_initialization=true` explicitly if you do not use the preset.
Title ID `4D5308ED` is the only title with established live gameplay
in the current test record; one tester reported live completion of all five
activities. D3D12 is the recommended first graphics backend because it is the
route used for that gameplay evidence. For diagnostics, add
`--nui_log_messages=true`. See [nui.md](nui.md) for additional flags and
placement advice.

## Replay a recording on Windows

Replay needs no Kinect hardware or runtime. Point `--nui_replay` at a depth
recording you created yourself, and copy the same preset into a different fresh
folder so replay settings, cache and saves stay isolated:

```powershell
$testRoot = ".\kinect-replay-data"
if (Test-Path -LiteralPath $testRoot) { throw "Choose a fresh test folder." }
New-Item -ItemType Directory -Path $testRoot -ErrorAction Stop | Out-Null
Copy-Item ".\config-presets\kinect.config.toml" "$testRoot\kinect.config.toml" -ErrorAction Stop
.\xenia_canary.exe "<path-to-game-image>" --storage_root="$testRoot" --config="$testRoot\kinect.config.toml" --nui=replay --nui_replay="<path-to-recording.xdepth>" --mute=true
```

On Linux, copy the preset into a fresh storage folder and run your built
`./xenia_canary` with `--config` pointing to that copy, alongside the same
`--nui=replay`, `--nui_replay=<path>`, and `--storage_root=<new-directory>`
options. The preset does not select a sensor backend; the `--nui` flag does.

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
