# Xenia Canary Kinect development plan

Xenia Canary Kinect is an independent experimental fork of Xenia Canary focused
on Kinect support. Maintain a usable, tested development build while contributing
general emulator improvements upstream. Upstream review can proceed at its own
pace without blocking further Kinect work.

This is the continuing plan for the fork. Update its status and decisions when
work changes, and read it before starting a new session.

## Current decision

Use **Xenia Canary Kinect** as the public identity and keep the existing
[ryantimjohn/xenia-canary repository](https://github.com/ryantimjohn/xenia-canary).
The owner approved `kinect` as the public development and default branch; do not
rename the repository. Local identity preparation is on `codex/kinect-identity`;
it contains the complete eleven-part implementation and an integration of
upstream `canary_experimental`.

The owner approved the saved rollout plan. Pushes, repository settings and
release work remain to be completed and verified during rollout. The first
public build channel is experimental Windows x86-64 releases. Linux remains
useful for building, tests and depth replay; live sensor support is currently
Windows-only. Keep executable and existing configuration names compatible while
changing display names and downloadable archive branding.

## Principles

- Keep Canary as the upstream foundation. Review its updates for behavioral
  impact and run relevant checks before accepting them into a release.
- Keep general emulator changes suitable for upstream review. Follow
  [CONTRIBUTING.md](../.github/CONTRIBUTING.md) and the
  [style guide](style_guide.md), including source evidence and title IDs.
- Keep title-specific patches in the separate game-patches work. Document which
  patches are required or optional without hiding their effect.
- Preserve upstream credits, copyright notices and licenses. Make the independent
  experimental status clear; do not imply endorsement by upstream maintainers.
- Describe tested hardware and behavior precisely. A successful build, depth
  replay and a live playthrough answer different questions.
- Preserve working saves and recordings. New test builds use separate settings,
  caches and storage, never the only copy of a user's working setup.
- Favor a small maintained difference from upstream. Evaluate Edge or other fork
  changes individually instead of importing a second integration baseline.

## Three parallel tracks

| Track | Contents | How it changes |
| --- | --- | --- |
| Kinect development | Complete working implementation, fork identity and new improvements | Focused feature branches merge into `kinect` after review and checks. |
| Tested releases | Immutable source tags and matching Windows packages | Promote a tested development revision to an experimental release. |
| Upstream contributions | One focused emulator feature per PR | Submit in dependency order after the preceding PR merges. |

The initial upstream series is eleven parts responding to
[xenia-canary#1290](https://github.com/xenia-canary/xenia-canary/issues/1290).
[The first PR](https://github.com/xenia-canary/xenia-canary/pull/1291) is open.
The existing three-hour monitor handles merges, review comments and subsequent
submissions. Its publication authorization covers that series; separate owner
approval covers the fork identity rollout.

Fork branding, issue templates and release workflows stay out of the upstream
series. Keep its prepared source commits available. When maintainers revise a
change, carry the accepted behavior into Kinect development and adapt dependent
work rather than reintroducing the old implementation.

Reviewer answers are drafted in the working conversation for the owner to send
in their own voice. The monitor never posts replies as the owner.

## Implementation milestones

### 1. Local identity and documentation

- Write a fork-specific README explaining purpose, status and the upstream plan.
- Provide a short [setup guide](kinect-setup.md) and retain the detailed
  [NUI implementation and evidence](nui.md).
- Identify the application as Xenia Canary Kinect and link build information to
  the fork's source revision.
- Direct fork bug reports to this repository, including build revision, title ID,
  sensor/backend, settings, patches and reproduction steps.
- Retain binary names and storage behavior to avoid an incidental migration.
- Ship an opt-in Kinect config template enabling avatar initialization. Copy it
  to isolated test storage before selecting it with `--config`, because Xenia
  saves settings to the selected file. Keep the general default disabled.
- Keep the current repository URL and do not rename the repository.

### 2. Local release preparation

- Add a manually triggered workflow for the `kinect` branch.
- Build the exact selected revision and run relevant automated checks.
- Package a clearly named Windows archive with license, README, setup guidance,
  the Kinect config template, protocol documentation, third-party notices and
  source/upstream identity;
  include SHA-256 checksums.
- Create only a **draft prerelease**, never an automatic public release on push.
- Reject attempts to replace an existing tag or release's files. A revised
  candidate receives a new version.
- A failed upload or conflicting tag can leave an unpublished draft. Inspect
  it before retrying and use a new version; retries never replace its assets.
- Keep hosted workflow execution separate from local checks: a local syntax or
  packaging check is not evidence that GitHub Actions completed successfully.

The first candidate is planned as `kinect-v0.1.0-preview.1`. The release index is
the durable public link; do not use a `latest` alias or a tag link until it has
been confirmed that the release exists.

### 3. Owner review and repository rollout

The owner approved the rollout. Complete and verify these external steps before
calling the rollout complete:

1. Push the approved integration to the new `kinect` branch.
2. Set `kinect` as the default branch so the fork's landing page and manual
   workflow use the new identity.
3. Enable this fork's issues and confirm its issue form works. Issues are
   currently disabled; enabling them before publication is an approved step.
   Review Actions settings without copying personal credentials into workflows.
4. Check all rendered links and the visible relationship to upstream.
5. Before tagging, confirm the issue tracker is enabled and the README's release
   and patch links resolve. Run the manual build workflow for the planned
   `kinect-v0.1.0-preview.1` draft prerelease.
6. Inspect the actual downloaded archive and release notes, verify checksums,
   and verify startup using separate settings and storage.
7. Publish only after the release candidate's results and limitations are
   reviewed. Keep the previous usable version available.

Local issue templates do not change the repository's issue setting. Manual
workflows must be present on the default branch to be dispatched. Changing the
default branch, enabling issues, running hosted workflows and publishing a
release are external rollout steps, not completed by a local commit.

## Release evidence and compatibility

The initial hardware evidence is an Xbox 360 Kinect with its PC power/USB
adapter on Windows, using the Kinect for Windows SDK 1.8 installation for
drivers and `Kinect10.dll`. Building does not require Kinect SDK headers or
import libraries. A runtime-only installation and other sensor models have not
received the same end-to-end verification. Depth replay needs no Kinect runtime.

For title `4D5308ED`, existing evidence includes live completion of all five
activities reported by a tester, depth-based player tracking, cursor input and
a visibly verified Guide open/close. Body-controlled gameplay requires
`allow_avatar_initialization=true` through the opt-in Kinect launch preset (or
the equivalent command-line flag). The obstacle activity needs the external
loading workaround matched to Rev 2 module hash `FEF1C9FF272B3BD9`. The exact
patch payload is publicly readable at the pinned [game-patches file](https://github.com/ryantimjohn/game-patches/blob/5a675635196223bb48c0a3e0e7769d67eb36bc36/patches/4D5308ED%20-%20Kinect%20Adventures!.patch.toml);
its [pull request #521](https://github.com/xenia-canary/game-patches/pull/521)
is open. The setup guide explains how to place and enable the matching entry.
Both entries are disabled in the file, and enabling both together has not been
verified across all activities. Do not claim that upstream accepted or released
this patch. Neither patch is included in the emulator package.

Known limitations to keep visible:

- Tutorial animations may run slowly; investigation is deferred to separate
  game-patches work and no fix is claimed.
- Color camera output is a placeholder, and microphone support is incomplete.
- Debug title close/reopen can freeze rendering; the broader investigation is
  outside the initial Kinect contribution.
- Full activity coverage on Vulkan and broader Kinect-title compatibility are
  not established by the existing D3D12 checks.
- Existing unrelated base-test and Windows trace-tool issues must be described
  accurately when encountered, rather than silently counted as passing.

Run an app build and relevant tests for each changed executable revision.
Expand verification for changes to camera delivery, tracking, input, title
lifetime or GPU behavior. A display-name change does not by itself require
replaying every activity; its check should establish correct build and startup.
A new hardware or compatibility claim needs matching runtime evidence.

## Ongoing development

After the initial rollout, prioritize reproducible problems and requested
hardware/title coverage. Candidate work includes sensor setup diagnostics,
additional title testing, session close/reopen behavior and the deferred
tutorial investigation. Keep game-patch work and general emulator fixes in
their appropriate repositories or branches.

Integrate upstream regularly into the development branch. Record conflicts and
meaningful behavior changes, validate them, and carry upstream review fixes
forward. Avoid changing already published source tags or repeatedly rewriting
branches others may use.

For each release, record:

- tag, source commit and included upstream revision;
- supported platform and tested sensor/runtime setup;
- builds, automated tests and runtime observations;
- required external patches, settings and known limitations;
- archive checksum and a link to the matching source.

## Resume checklist

1. Read this plan and the current work-session checkpoint.
2. Inspect Git state and active work before changing files or starting a build.
3. Identify whether the task belongs to development, release or upstream review.
4. Preserve original saves, recordings, binaries and diagnostic work.
5. Serialize builds, tests and game runs; use focused delegated work as needed.
6. Confirm the current rollout step and external status in the work-session
   checkpoint. The saved identity and rollout plan is approved.
7. Update this plan's status and record the next concrete action before stopping.

## Status

- Identity and documentation: **prepared locally; rollout approved**.
- Repository URL: **retained**; `kinect` is the approved public/default branch.
- Issues: **currently disabled; enable before publication**.
- Planned first release: **`kinect-v0.1.0-preview.1`, draft then inspect and
  publish after checks**; no fork-specific binary release exists yet.
- Hosted workflow, downloaded release candidate and runtime checks on that
  candidate: **not yet confirmed**.
- Upstream PR series: **continues separately**.
- Repository rename: **not planned; current URL retained**.

## References

- [Canary contribution guide](../.github/CONTRIBUTING.md)
- [Canary license](../LICENSE)
- [GitHub manual workflow events](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch)
- [GitHub CLI release creation](https://cli.github.com/manual/gh_release_create)

