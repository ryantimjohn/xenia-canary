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
The public development and default branch is `kinect`; the repository will not
be renamed. The implementation includes the eleven-part contribution series
and an integration of upstream `canary_experimental`.

The first public build channel is experimental Windows x86-64 releases. Linux
remains useful for building, tests and depth replay; live sensor support is
currently Windows-only. Keep executable and existing configuration names
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

The first candidate, `kinect-v0.1.0-preview.1`, is currently a draft. It is not
a published download. After publication, use the durable releases index; do not
use a `latest` alias or tag link until the release is publicly available.

### 3. Repository rollout and release verification

The repository rollout is complete: `kinect` is pushed and is the default
branch, Issues are enabled, and the bug-report template is present on that
branch. Public documentation links have been checked. The issue setting and
template file are separate repository features; confirm both when setting up
another repository.

The manual workflow created the first draft candidate. Its hosted Windows build,
automated suites, archive contents, checksum, extraction, and startup checks are
recorded below. A menu-depth replay check was interrupted by a Windows firewall
prompt and remains pending. Do not publish until that check is resolved or its
limitation is explicitly accepted and the final release notes are reviewed.
Keep future releases as drafts until their actual archive and runtime results
have been checked. Publish only after reviewing the candidate's results and
limitations; keep the previous usable version available.

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
   checkpoint. Keep the current candidate unpublished while menu-depth replay
   and final release review remain pending.
7. Update this plan's status and record the next concrete action before stopping.

## Status

- Identity and documentation: **published on the public `kinect` default branch**.
- Repository URL: **`ryantimjohn/xenia-canary` retained; no rename planned**.
- Issues: **enabled; `.github/ISSUE_TEMPLATE/bug_report.yaml` is present on the
  default branch**.
- First release candidate: **`kinect-v0.1.0-preview.1` is a draft from source
  commit `360ab79482c5f10b4827624fd2cfb64579c87ee2`; it is not published**.
- Hosted workflow: **passed** ([run 38331249988](https://github.com/ryantimjohn/xenia-canary/actions/runs/38331249988)).
- Windows archive: **97 files; archive and contents verified**. Archive
  SHA-256: `277db959bad04165de3685fc1a068f51b4cdf271b3fbb4c04316031cbf25a85c`;
  executable SHA-256: `812bfb628ce150a7ffcd492853f99697e1e05eb94052ec43530a07e25dafacf6`.
- Release application builds and APU, kernel, NUI and CPU suites: **passed on
  Windows and Linux** at the candidate source revision.
- Downloaded artifact startup: **passed**; branding, Help menu, avatar-enabled
  preset and clean exit were verified. Menu-depth replay: **pending**, blocked
  by a Windows firewall prompt.
- Publication: **pending final replay decision and release-note review**.
- Upstream PR series: **continues separately**.

## References

- [Canary contribution guide](../.github/CONTRIBUTING.md)
- [Canary license](../LICENSE)
- [GitHub manual workflow events](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch)
- [GitHub CLI release creation](https://cli.github.com/manual/gh_release_create)

