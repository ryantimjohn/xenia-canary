# Source-only tracking observer manifest

- Base: `348cb6ff8f9ee4087a8e4dd169e1bcf7a93d73ae` in the private fork.
- Scope: `nui_camera.cc`, `nui_state.cc`, the new private
  `nui_tracking_observer.h`, `nui_flags.cc/.h`, the Catch test source, and the
  two documentation files.
- Translation units affected: `nui_camera.cc`, `nui_state.cc`,
  `nui_flags.cc`, and `nui_tracking_observer_test.cc`. The new observer header
  is included only by the first two and the test. No public or guest struct
  layout changes.
- Static/source review: `git diff --check` passed. Source inspection checked
  the observer's bounded storage, separate mutex, validated HUD read point,
  and feed-only aggregate logging after callback returns.
- Focused Catch source contains eight cases under `[nui-tracking-observer]`:
  extraction bounds, disabled path, delivery and HUD correlation, row cap and
  ambiguity, concurrent collection, session reset, slot movement versus set
  change, and delayed interval plus first-end snapshot cap.
- Native compile, tests, game, and sensor run: intentionally deferred to the
  coordinator's separately granted execution stage.
