# Source-only XAudio2 queue trace manifest

- Base commit: `d9be2ca9962bb8c18d3f5f4bfcc12895dbeae5ea` in the private
  fork, with the prior tracking observer unchanged.
- Production translation units: `apu_flags.cc`, `audio_system.cc`,
  `xaudio2_audio_system.cc`, and `xaudio2_audio_driver.cc`. The new
  `audio_queue_trace.h` is header-only and included by `audio_system.cc`,
  `xaudio2_audio_driver.cc`, and the new test translation unit.
- `xaudio2_audio_driver.h` changes its constructor signature and adds a
  `unique_ptr` to the private diagnostic collector. Rebuild closure includes
  every translation unit including that header (`xaudio2_audio_driver.cc`,
  `xaudio2_audio_system.cc`) and all linked consumers of the XAudio2 driver;
  no guest ABI or `AudioSystem` layout changes.
- Six focused Catch cases use `[audio-queue-trace]` and cover priming/gap,
  submit errors, pause epochs, callback atomics/overflow, a shared budget,
  and late severe-bin selection over early healthy bins.
- Native compile, tests, emulator, game, and sensor execution are deferred to
  the coordinator's separately authorized stage.
