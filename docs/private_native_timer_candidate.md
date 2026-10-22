# Private native timer candidate

SDK-derived fork-only research. Keep this change and its evidence out of
upstream contributions. See `SDK_DERIVED_PRIVATE.md`.

`--private_native_timer_candidate=true` selects a Windows-only experimental
dispatcher timer path. The default is false. Restart the process to change it:
export mappings are selected once during kernel registration, after CLI/config
parsing (`windowed_app_main_win.cc`, `XeniaApp::OnInitialize`, Emulator setup).
Hot toggling and restoring export metadata in a running process are unsupported.
Runtime `SetFunctionMapping` modifies the existing export entries, preserving
the resolver's by-name pointers. With the flag off, A6/54 remain the original
undefined exports and timer first-use resolution remains unsupported. Existing
handle-based XTimer/APC timers and all XPOLL001 fields/layout are unchanged.

The candidate supports timer dispatcher types 8 (manual reset) and 9 (automatic
reset), relative **negative**, representable one-shot due times, a null DPC,
and guest time scalar exactly 1. Zero, absolute, INT64_MIN, overflowing host
deadlines, non-null DPC, unavailable associations and unsupported scalar values
are rejected with the existing fatal-error mechanism, without reporting a
successful timer arm. There is no periodic/KeSetTimerEx or KDPC implementation,
sleep/yield substitute, title/PC special case, or global clock change. A fatal
rejection is a terminal diagnostic, not an implemented BOOLEAN return contract.
The only repeated diagnostic (unavailable checked event query/identity after
wait) is capped at 16 process-wide warnings.

## ABI qualification and retained limitations

Private callsite evidence and a compile-only SDK compiler experiment support
the narrow inference that r3 contains the timer VA, r4 the entire signed due
value, and r5 the DPC pointer for the observed KeSetTimer call. The game
instructions construct r4=-150000000 and r5=0 immediately before the import
call. A research prototype using the actual SDK LARGE_INTEGER type generated
the same constant register sequence and forwarded dynamic r3/r4/r5 unchanged.
This is not an independently verified Xbox kernel prototype or hardware result.
The formal research prototype used the Windows WDK KeSetTimer signature:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kesettimer

Qualified saved callsite log SHA256:
`51B61AFF1396DF3E9D2E3337EA2D1EE9557F30EF996CB08DAB9A8DE15B22B7CB`.
Compiler assembly SHA256:
`8EF22477EF895884674CEE523DDBA53739DEC1A54808995DCCD87C24BCA86C54`.
Compiler command receipt:
`E7D5F805F848577A09D4E0D5EEEE73CFF769E8B1D2FCD3752555554812CD7003`.
The compiler emitted D9002 for ignored `/errorReport:none`; no error-report
suppression or retry is claimed.

This candidate stores the requested relative value in KTIMER.due_time; it does
not claim the full kernel's absolute interrupt-time header representation.
The host steady deadline is authoritative. Header inserted/signal_state, null
dpc and zero period are maintained. No guest timer-list or KDPC queue is built.
Thus guest code inspecting other timer bookkeeping is not qualified. Native
timers continue in host steady time during an emulator pause; this experiment
does not establish pause/resume compatibility.

## Ownership, ordering and reinitialization

Each native XTimer has shared state owning a stable Event. WaitMultiple retains
the XTimer and its Event; reinitialization creates a different association and
never replaces an Event used by an existing waiter. TimerQueue callbacks capture
only weak state, never XTimer. Expiry, cancel, arm and identity/header/Event
transitions serialize with the existing global critical region. An expiry
checks the expected generation, guest allocation's writable range, timer type,
signature and handle before touching guest storage or signaling. There is one
expiry path. Cancel returns the previous inserted state and does not reset an
already signaled Event. Rearm resets its signal and invalidates older callbacks.

Queue publication and Disarm can block and always run outside global. Publisher
accounting covers the entire publication and stale-publication disarm. An expiry
before publication is preserved; a concurrent cancellation/retirement causes
the resulting item to be disarmed after publication. Queue allocation exceptions
roll back armed/inserted and return failure to the entry's fatal rejection.
This failure may already have reset the prior signal; no recovery is claimed.

Explicit KeInitializeTimerEx retires old associations before rewriting the
header and drains after leaving global. First-use resolution also scans the
existing object table for an old association at the same VA, including when
inline initialization erased its signature. That resolver path cannot drain
because callers may already hold global; it invalidates immediately and releases
the internal association handle once. Canceled TimerQueue nodes may remain until
their original due time under existing queue behavior, but their weak captures
do not retain the object, Event or guest memory. No new queue capacity/resource
guarantee is claimed for repeated long-deadline rearms.

Both TerminateTitle and KernelState destruction close admission, invalidate
native state, and drain outside global before clearing modules/object storage.
A later executable resets admission. Retired callbacks cannot write reused
storage after their state is invalidated. Allocation/protection checks serialize
with host heap bookkeeping; inline guest PPC writes themselves do not acquire
the host lock. Completed ordinary inline initialization is detected, but truly
concurrent unsynchronized header rewriting, copying a stashed signature/handle
into reused storage, or freeing storage outside normal heap bookkeeping is not
made atomic or otherwise qualified by this HLE candidate.

After a successful wait, additive Event::TryQuery uses checked NtQueryEvent on
Windows. Unsupported/failed queries leave the header unchanged, with no
unchecked Query fallback. Query and header update serialize against expiry;
an old wait callback cannot blindly clear a newer expiry. This query is still
a snapshot, not atomic with independent host waiters consuming an auto-reset
signal. SyncFromGuest never re-signals from a stale guest signal_state.

## Validation status

Source-only. No configure, compiler, formatter, tests, game or sensor execution
has been performed for this candidate. Proposed filter: `[native_timer]` in
`xenia-kernel-tests`, **12 Windows-only cases**:

- signed duration/deadline boundaries;
- real blocking WaitAny with timer at index 2;
- real manual/automatic Event consumption;
- cancellation/prior-inserted returns and stale generation callbacks;
- expiry before publication;
- invalidation/reused storage;
- shutdown while publication is in flight;
- publication exception accounting;
- failed checked query and newer expiry before an old wait callback;
- weak callback storage lifetime;
- production type/signature/handle association predicate;
- quiesce with a callback executing inside its guest-access section.

The tests use actual Events and a real TimerQueue for the blocking wait. Injected
publishers make ordering failures deterministic. Local-header fixtures exercise
the production state/predicate but do not instantiate a full KernelState, title
loader or emulator teardown; those integrations still need separately granted
validation. Existing poll/wait diagnostics and their focused regressions should
also pass before any separately granted replay. No performance or title-fix
claim follows from source review alone.
