# Native mutant implementation

`KeInitializeMutant` creates a native `X_KMUTANT` backed by the same recursive
host mutex used for handle-based mutants. `KeWaitForSingleObject` and multiple
object waits record successful acquisitions; `KeReleaseMutant` releases one
acquisition and returns the previous signed signal state. Native owner and
signal-state fields change under the same lock as host release, so a newly
woken owner's state cannot be overwritten by the previous owner.
Guest storage is checked and translated at each native update; the host does
not retain a pointer to guest memory between calls. Initialization checks the
entire 0x20-byte writable output before clearing it.
Reinitializing storage that already carries a wrapper marker fails visibly
before any fields change, preserving the original mutex and handle mapping.

The guest startup path in title `373307D9`, version `0.0.0.5`, initializes a
0x20-byte mutant unowned at `0x8281B2F8`, waits indefinitely on the same object
at `0x82816C3C`, and releases it with increment, abandon, and wait all zero at
`0x82816CA8`. The object is at identity-global `+0x24AB0`. The surrounding guest
memsets preserve those 0x20 bytes. Without the initializer, its zeroed header
was interpreted as an unsignaled notification event. This evidence establishes
the required initialization and use, rather than inferring them from an import
warning.

Recursive acquisition and release use the public NT mutex model as an
approximation; see Microsoft's
[KeReleaseMutex documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kereleasemutex).
Initially owned initialization eagerly creates the host wrapper on the owning
thread. A late wrapper cannot take over an abandoned, recursively owned, or
foreign-owned native object. Guest exception delivery for invalid Ke releases,
explicit abandonment, the wait-next scheduler protocol, and guest per-thread
mutant-list linkage remain unsupported.
Requests with abandon or wait-next set are rejected before releasing ownership.
Runtime depth is bounded to `INT32_MAX` rather than allowing the guest signal
state to wrap. The header size byte remains zero, matching the current event
initialization convention; its console-specific value has not been verified.

When `NtSignalAndWaitForSingleObjectEx` signals a mutant, it uses the accounted
mutant release followed by the normal object wait. A failed release returns
without waiting; a successful release updates recursive state before the wait.
This follows the public Win32
[SignalObjectAndWait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-signalobjectandwait),
which does not guarantee atomicity, and the existing POSIX host approximation.
Console atomicity has not been verified.

Automatic host-abandoned waits grant ownership to the waiter. Single waits,
signal-and-wait, and wait-any record that acquisition for the acquired mutant;
wait-all records every acquired mutant while preserving current-owner recursion.
Known abandoned single/indexed waits start at depth one. For wait-all, a private
process-unique host-thread lifetime cookie distinguishes a new owner from an
already-current owner, even if the opaque guest owner address was reused.
The abandoned guest status is preserved. Failed waits, timeouts, and APC returns
do not change ownership; this follows the public Windows
[wait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects).
This differs from an explicit Ke abandon request, which
remains unsupported. The snapshot owner lookup still retains the existing raw
thread pointer; its lifetime and guest per-thread mutant-list linkage require
separate work. The cookie adds no guest fields or snapshot bytes.

Snapshots retain their existing layout: `SaveObject` followed by one owner
handle, with no recursive-depth field. The existing restored-owner startup
reacquires once; runtime metadata accounts for that single acquisition. Saving
depth greater than one logs the unsupported fidelity, but cannot safely reject
the snapshot: `KernelState::Save` currently drops objects whose `Save` returns
false, and `Emulator::SaveToFile` ignores that kernel result. Recursive snapshot
representation and save-failure propagation require separate work. Existing
restore resume/reacquisition ordering also remains unchanged. A snapshot of a
recursively owned mutant must not be treated as preserving its recursive depth.

`native_mutant_test.cc` exercises real host mutex exclusion, recursive releases,
failed non-owner releases, signed previous states, exact native output bounds,
and ownership transfer under contention. These tests do not claim full emulator
snapshot support.
The direct `XObject` regression checks the non-waitable guard. The Windows host
abandonment tests use real recursively owned mutexes and thread exit, followed
by the shared acquisition callback logic. They cover recycled owner addresses
and a mixed wait-all that preserves live recursion while replacing abandoned
ownership. These tests do not instantiate a full guest thread/kernel fixture.
