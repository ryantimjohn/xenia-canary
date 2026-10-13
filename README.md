# Xenia Canary Kinect

Xenia Canary Kinect is an independent experimental fork of Xenia Canary for
researching and developing Xbox 360 Kinect support. It keeps the existing
[ryantimjohn/xenia-canary repository](https://github.com/ryantimjohn/xenia-canary)
and develops on the `kinect` branch. It is not an official release or support
channel for [Xenia Canary](https://github.com/xenia-canary/xenia-canary).

The Kinect work is also being contributed upstream in an eleven-part series in
response to [issue #1290](https://github.com/xenia-canary/xenia-canary/issues/1290).
[The first pull request #1291](https://github.com/xenia-canary/xenia-canary/pull/1291)
is open; the series is pending, so standard Xenia Canary releases should not be
assumed to include this work.

## What works

Live Kinect support is Windows-only and uses the Kinect for Windows 1.8
runtime. The verified setup uses an Xbox 360 Kinect with its PC power/USB
adapter and the SDK 1.8 installation. Linux supports recorded-depth replay and
the no-sensor backend, not live hardware. See [the setup guide](docs/kinect-setup.md)
for prerequisites and isolated test commands, and [the NUI notes](docs/nui.md)
for protocol details, evidence and limitations.

Title ID `4D5308ED` boots, tracks a player and supports hand-cursor selection.
A tester reported live completion of all five activities. The obstacle activity
requires a matching external patch; an optional separate patch skips forced
introductory activity selection. Both patches are disabled by default, and the
two have not been verified enabled together. Follow the version-matched
[patch instructions](docs/kinect-setup.md#optional-title-patches) for the Rev 2
image (`FEF1C9FF272B3BD9`). The patches are maintained separately and are not
bundled with this emulator.

The translucent loading figure stands in for unavailable avatar assets.
Tutorial movies can run slowly, color frames are black and microphone support
is incomplete. See the setup guide and NUI notes before testing.

Experimental Windows builds are distributed from the durable
[releases index](https://github.com/ryantimjohn/xenia-canary/releases). Each
candidate is prepared as a draft and published after its archive, checksum and
startup have been checked. You can also [build from source](docs/building.md).

The opt-in [Kinect launch preset](config-presets/kinect.config.toml) enables
avatar initialization for body-controlled play. Copy it into a fresh test
storage folder and select that copy with `--config`; the emulator saves
settings to the selected file. The general default remains `false`.

## Issues and contributions

Use this repository's issue form for reproducible bugs in this fork. Include
the fork commit, platform, sensor, backend, title ID (or `N/A` for a startup
failure before a title loads), settings and relevant logs. The issue tracker is
for bug reports and work items, not general support or discussion.

For source contributions, read [CONTRIBUTING.md](.github/CONTRIBUTING.md) and
the [style guide](docs/style_guide.md). Follow Xenia's source and
information-sourcing rules. Use title IDs rather than game trademarks when
describing compatibility, and do not submit copyrighted game data, Kinect
drivers or SDK files.

## License and credits

This fork retains the Xenia codebase's 3-clause BSD license; see
[LICENSE](LICENSE) and the notices in source files for copyright attribution.
Third-party components remain under their respective licenses. Xenia Canary is
the upstream project on which this experimental fork is based.
