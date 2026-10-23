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

The dependency-light kernel wire codec represents exactly one
0xA40-byte Xbox 360 CONTEXT record. It accepts explicit caller-owned integer
values and reads/writes big-endian bytes manually. The codec itself has no
runtime hook, export, cvar, PPCContext mapping, SDK include/link dependency,
capture or restore operation. The separate bounded private dispatcher below
uses its valid storage with ContextFlags=0, without claiming captured state.

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

The codec's source-only implementation and the later pure walker/admission
validation are separate from runtime integration. No runtime correctness follows
from a successful codec or synthetic-stack test. This dispatcher integration is
source-only and its build/tests/game execution remain **UNRUN** in this commit.
No SDK binaries or tools are used.

## Bounded primary-executable first-chance experiment

`sdk_private_first_chance.h/.cc` installs a kernel callback at initial setup and
detaches before teardown. It is disabled by default. It requires both CPU
`zero_page_scalar_first_chance` and kernel
`private_zero_page_first_chance_dispatch`, as well as protected zero memory.
The CPU hook offers only modeled LWZ/STW literal accesses to the zero guard at
a regular CALL_EXTERN boundary; this does not establish full architectural
fault precision. The callback never runs from a native VEH.

Start a fresh process with `apply_patches=false` before setup and an empty
private patches directory. Installation refuses patch loading enabled and the
callback requires it to stay disabled. Disabling it after a PatchDB already
loaded rules is insufficient. The primary title's LoadContinue/title update and
normal setup precede guest launch. Guest DLL loading also calls the patcher;
the fresh empty PatchDB makes that path nonmutating. There must be no concurrent
host reload/setup/patch or self-modifying guest code during search/invocation.

The callback retains the primary UserModule, requires all frames/handlers/filter
code in its current CPU XEX allocation, and copies PE sections declared readable
and nonwritable, plus at most 64 KiB of the current thread's stack. Current
mappings must be readable; they may still be writable in the emulator. It rejects
PE-declared writable metadata/code sections, unknown frame/handler patterns, unavailable mappings,
and bounded-resource exhaustion (64 MiB image evidence, 256 sections, eight
frames and 16 matching scopes per frame). Metadata is reparsed from only owned
eligible bytes with limits of 65,536 functions, 256 scopes per handler and 8 MiB
of reads, so an excluded mutable table cannot still admit a copied handler.
The original stack bytes are checked before calls and after returning calls
before further walking. Copy/revalidation uses the existing
global region, released before every guest call. Current primary identity,
allocation, permissions and copied bytes are checked before calls. This is
ordinary guest execution under the stated preconditions, **not** a generation
lease or prevention of a concurrent write after a check. Normal unload removes
registry/handle references; the retained primary reference delays destruction.
Existing title teardown must stop guest execution before kernel destruction.

The production bounded walker supplies the exact saved LR and next SP. It carries
the admitted original low32 nonvolatile r31 across a frame that does not save it,
and accepts a full64 r31 only when recovered from an owned save slot. Recovered
SP has only low32 validity. It neither adjusts LR nor invents high SP bits.

The dispatcher retains that raw LR as return evidence and separately selects
the caller's suspended call instruction for its half-open scope lookup and
bounded unwind control PC. It uses raw LR minus four only after alignment and
owned executable-image bounds checks, an exact instruction read proving a
direct relative BL to the just-unwound function's entry, and metadata showing
that callee outside the caller's function. Unsupported call forms, targets, or
unavailable evidence decline. The initial fault PC remains exact. This narrow
selection rule does not establish a universal calling ABI, architectural
resumption, or runtime qualification.

For a non-C handler, the exact relocated no-catch C++ taken path must pass
`AdmitNoCatchCppFirstPhase`. The dispatcher invokes the actual guest wrapper
with four arguments and requires its actual return value 1; it does not skip
that handler. Only its proven dispatcher+4 FunctionEntry prefix is provided.
For a known imported C-specific handler, valid matching metadata scopes with
ordinary executable filter/jump addresses admit the two-argument guest filter
call (EXCEPTION_POINTERS*, frame SP). Sentinel/termination-only/unknown roles
decline. Returning filter 0 continues search. Returning -1, +1, other values,
unknown frames and unsupported unwind patterns decline to the original native
fault. A filter can instead enter an ordinary nonreturning guest loop. No title,
PC, callback address, or frame count is used as a title allowlist.

Private checked guest storage contains a 0x50-byte EXCEPTION_RECORD with access
violation code, flags=0, fault PC and two operation/address parameters; a full
0xA40 CONTEXT with flags=0 and otherwise zero fields; an eight-byte pointer pair;
and an eight-byte dispatcher prefix. Record/pointer offsets are calculated from
the supplied winnt.h:1690–1697 and 1724–1727 under four-byte guest pointer/type
assumptions; this is SDK-derived information. No complete dispatcher ABI is
claimed. Private storage is reset before calls and freed on ordinary scope exit.
Guest calls use the existing Processor::Execute stack contract and require a
writable 4 KiB reserve below the original modeled SP. The entire modeled live
PPCContext is restored between returning calls and on host stack unwinding;
it is not mapped into the guest CONTEXT. No RtlCaptureContext, RtlUnwind,
RtlRestoreContext, exception continuation, PC skip or protection bypass is added.
