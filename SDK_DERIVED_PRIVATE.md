<!-- SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect branch after
validation. MUST NOT enter upstream PRs or cherry-pick series targeting
xenia-canary:canary_experimental, or any other upstream contribution.
.github/CONTRIBUTING.md excludes XDK-derived information, not just copied code. -->

# SDK-DERIVED FORK-ONLY WORK

These SDK-derived changes are eligible for integration into the `kinect` branch
of the user's own fork after validation. They **MUST NOT** be included in an
upstream PR or cherry-pick series targeting `xenia-canary:canary_experimental`,
or in any other upstream contribution. The project's `.github/CONTRIBUTING.md`
excludes information derived from the official XDK, including independently
written code using that information. Keep these files, these changes, and any
SDK-derived follow-ups out of upstream-bound contribution series.

The local user explicitly authorized this fork-only work. The retained
`SDK_DERIVED_PRIVATE.md` filename records its origin as a private experiment;
the current integration policy permits validated use in this fork's `kinect`.
This notice does not change the upstream project's policy or license, and
removing a warning would not remove the SDK-derived provenance.

The only behavior added is a dependency-light kernel wire codec for exactly one
0xA40-byte Xbox 360 CONTEXT record. It accepts explicit caller-owned integer
values and reads/writes big-endian bytes manually. It has no runtime hook,
export, cvar, PPCContext mapping, SDK include/link dependency, capture, restore,
handler invocation, or exception-dispatch integration.

The codec preserves every field, arbitrary ContextFlags bits and Fill included.
It does not admit state according to flags. FPR/FPSCR payloads are uint64_t bits;
Vscr and each of 128 Vr registers are four uint32_t words in increasing wire
address order, with no host-vector lane interpretation. Exact-size rejection
leaves the destination unchanged. Staged output permits source/destination
overlap and unaligned byte spans. Flag validity and state precision belong to a
future caller; successful conversion does not make a record safe to resume.

## SDK-derived layout provenance

Source: local Microsoft Xbox 360 SDK 21256,
`C:\Program Files (x86)\Microsoft Xbox 360 SDK\include\xbox\winnt.h`,
SHA256 `FB0E67409D672FB2C2475C7BD2A02F99914834D8EE4EB04314650E8E5D60EDB4`.
The `_PPC_` declaration is at lines 1366–1649; `xtl.h:39–41` selects that branch
for Xbox applications. No header was copied wholesale. The source's field
sequence was converted to explicit wire offsets using 4-byte DWORD/float and
8-byte ULONGLONG/double storage and the intended target alignment model. No SDK
compiler or host-Windows CONTEXT was used to establish these offsets.

| Fields | Wire offsets (hex) |
| --- | --- |
| ContextFlags, Msr, Iar, Lr | 000, 004, 008, 00C |
| Ctr; Gpr0–31 | 010; 018 + 8*n |
| Cr, Xer; Fpscr; Fpr0–31 | 118, 11C; 120; 128 + 8*n |
| UserModeControl, Fill; Vscr[4] | 228, 22C; 230 |
| Vr0–127, four words each | 240 + 10*n, 4 bytes per word |
| Record end | A40 |

The size and IAR offset agree with the previously observed opaque title copy,
but this does not prove object identity or the title's compiler/SDK version.
Dispatcher layout, unwind/restore contracts, capture validity and PC semantics,
fault precision, and JIT resumption remain unverified. This codec implements
none of those contracts.

## Validation scope

Seven standalone Catch2 cases specify literal endian/offset examples, raw NaN
and signed-zero payloads, all 128 vector registers, arbitrary-record round trips,
transactional wrong-size failures, unaligned buffers with adjacent canaries,
and overlapping object/byte storage. The codec and tests depend only on the C++20
standard library and Catch2; no SDK tools or libraries are required.

Builds, tests, game execution and SDK binaries are **UNRUN** for this source-only
change. Formatting is restricted to the three new C++ files. Existing runtime
and build-registration files are untouched.
