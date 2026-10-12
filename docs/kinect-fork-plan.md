# Xenia Canary Kinect development plan

Xenia Canary Kinect is an independent experimental fork of Xenia Canary focused
on Kinect support. Maintain a usable, tested development build while contributing
general emulator improvements upstream. Upstream review can proceed at its own
pace without blocking further Kinect work.

This is the continuing plan for the fork. Update its status and decisions when
work changes, and read it before starting a new session.

## Current decision

Use **Xenia Canary Kinect** as the public identity. Keep the existing repository
at [ryantimjohn/xenia-canary](https://github.com/ryantimjohn/xenia-canary) for the
initial rollout. A later rename to `xenia-canary-kinect` is optional and requires
a coordinated update of remotes, links, workflows and the PR monitor.

The proposed public development and default branch is `kinect`. Local identity
preparation is on `codex/kinect-identity`; it contains the complete eleven-part
implementation and an integration of upstream `canary_experimental`.
This preparation must be reviewed by the owner before any identity changes are
pushed. No new branch, tag, release, repository rename or settings change has
been published as part of this preparation.

The first public build channel will be experimental Windows x86-64 releases.
Linux remains useful for building, tests and depth replay; live sensor support is
currently Windows-only. Keep the executable and existing configuration names
compatible while changing display names and downloadable archive branding.

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
submissions. Its publication authorization covers that series; it does not
authorize publishing this fork identity.

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
- Keep the current repository URL. Prepare the optional rename as a later
  decision, not a prerequisite for releasing.

### 2. Local release preparation

- Add a manually triggered workflow for the `kinect` branch.
- Build the exact selected revision and run relevant automated checks.
- Package a clearly named Windows archive with license, README, setup guidance,
  protocol documentation, third-party notices and source/upstream identity;
  include SHA-256 checksums.
- Create only a **draft prerelease**, never an automatic public release on push.
- Reject attempts to replace an existing tag or release's files. A revised
  candidate receives a new version.
- A failed upload or conflicting tag can leave an unpublished draft. Inspect
  it before retrying and use a new version; retries never replace its assets.
- Keep hosted workflow execution separate from local checks: a local syntax or
  packaging check is not evidence that GitHub Actions completed successfully.

Initial version example: `kinect-v0.1.0-preview.1`. This is a naming proposal;
the tag does not exist as part of local preparation.

### 3. Owner review and repository rollout

Present the plan, README, setup guide, UI changes, workflow diff and validation
results to the owner. After explicit approval of the rollout:

1. Push the approved integration to the new `kinect` branch.
2. Set `kinect` as the default branch so the fork's landing page and manual
   workflow use the new identity.
3. Enable this fork's issues and confirm its issue form works. Review Actions
   settings without copying personal credentials into workflows.
4. Check all rendered links and the visible relationship to upstream.
5. Before tagging, update the README's initial no-release/closed-tracker status
   to match the rollout and provide the intended release and patch links. Run
   the manual build workflow to prepare a draft prerelease.
6. Inspect the actual downloaded archive and release notes, verify checksums,
   and verify startup using separate settings and storage.
7. Publish only after the release candidate's results and limitations are
   reviewed. Keep the previous usable version available.

The existing repository has issues disabled at the time of preparation; local
issue templates do not change that setting. Manual workflows must be present
on the default branch to be dispatched. Changing the default branch, enabling
issues, running hosted workflows and publishing a release are external rollout
steps, not completed by a local commit.

## Release evidence and compatibility

The initial hardware evidence is an Xbox 360 Kinect with its PC power/USB
adapter on Windows, using the Kinect for Windows SDK 1.8 installation for
drivers and `Kinect10.dll`. Building does not require Kinect SDK headers or
import libraries. A runtime-only installation and other sensor models have not
received the same end-to-end verification. Depth replay needs no Kinect runtime.

For title `4D5308ED`, existing evidence includes live completion of all five
activities reported by the tester, depth-based player tracking, cursor input and
a visibly verified Guide open/close. Body-controlled gameplay requires
`allow_avatar_initialization=true`. The obstacle activity requires a separate
external loading workaround; the repeated first-activity startup has a separate
optional workaround. These are not included in the emulator package.

Before describing an activity as usable from a public download, provide an
accessible, version-matched path to any required external patches and their
instructions. Do not claim a patch is publicly released merely because a local
copy was tested.

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
6. Check the current permission for foreground use and publication. Local
   preparation may continue while release approval is pending.
7. Update this plan's status and record the next concrete action before stopping.

## Status

- Identity and release preparation: **implemented locally for owner review**.
- Hosted release workflow and downloaded release candidate: **not yet run**.
- Identity push, default-branch/settings changes and release: **await owner review**.
- Upstream PR series: **continues separately**.
- Repository rename: **deferred; current URL retained**.

## References

- [Canary contribution guide](../.github/CONTRIBUTING.md)
- [Canary license](../LICENSE)
- [GitHub manual workflow events](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch)
- [GitHub CLI release creation](https://cli.github.com/manual/gh_release_create)

