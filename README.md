# Xenia Canary Kinect

Xenia Canary Kinect is an independent experimental fork of Xenia Canary for
researching and developing Xbox 360 Kinect support. It is based on the
[Xenia Canary project](https://github.com/xenia-canary/xenia-canary), and is
not an official release or support channel for that project.

This fork is under active development. The work is intended to be contributed
upstream as an 11-part series in response to
[issue #1290](https://github.com/xenia-canary/xenia-canary/issues/1290), with
[the first pull request #1291](https://github.com/xenia-canary/xenia-canary/pull/1291)
open. The series is pending; standard Xenia Canary releases should not be
assumed to include this work.

## What works

The live backend is Windows-only and targets Kinect sensors through the Kinect
for Windows 1.8 runtime. The verified hardware setup is an Xbox 360 Kinect with
its PC adapter and the SDK 1.8 installation. Other sensor models have not had
the same end-to-end verification. Linux builds support recorded-frame replay
and the no-sensor backend, but not live hardware.
The implemented protocol, current settings, test evidence, and limitations are
documented in [docs/nui.md](docs/nui.md).

There is no fork-specific binary release yet. See [docs/kinect-setup.md](docs/kinect-setup.md)
for safe setup and testing instructions, or [docs/building.md](docs/building.md)
to build from source. The [fork plan](docs/kinect-fork-plan.md) describes the
first rollout.

The opt-in [Kinect launch preset](config-presets/kinect.config.toml) enables
avatar initialization for body-controlled play. Copy it into a fresh test
storage folder and select that copy with `--config`; the emulator saves
settings to the selected file. The general default remains `false`. The setup
guide gives live and replay commands with isolated storage.

A tester reported live completion of all five activities for title ID
`4D5308ED`. Body-controlled play requires avatar initialization, supplied by
the opt-in preset or `--allow_avatar_initialization=true`. The obstacle activity
needs a matching external title patch, which is not bundled.
The emulator package alone does not provide that complete five-activity setup.
The loading figure is a translucent stand-in.
Other known gaps include slow tutorial movies, black color frames, incomplete
microphone support, and a rendering freeze in Debug builds after using
File > Close and reopening a title in the same emulator process. Restart the
emulator after that close/reopen path.

## Issues and contributions

The issue tracker will be enabled with the first rollout. Use this repository's
issue form for reproducible bugs in this fork once it is available. Include
the fork commit, platform, sensor, backend, title ID (or `N/A` for a startup
failure before a title loads), settings, and relevant logs.

For source contributions, read [CONTRIBUTING.md](.github/CONTRIBUTING.md) and
the [style guide](docs/style_guide.md). The project follows Xenia's source and
information-sourcing rules. Please use title IDs rather than game trademarks
when describing compatibility, and do not submit copyrighted game data, Kinect
drivers, or SDK files.

## License and credits

This fork retains the Xenia codebase's 3-clause BSD license; see
[LICENSE](LICENSE) and the notices in source files for copyright attribution.
Third-party components remain under their respective licenses. Xenia Canary is
the upstream project on which this experimental fork is based.
