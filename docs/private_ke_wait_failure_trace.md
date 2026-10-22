<!-- Private fork only. SDK-derived ancestry must never be upstreamed. -->
# Bounded KeWait resolver failure trace

On Windows, default-off `--ke_wait_failure_trace=true` permits at most 16 process-wide
one-shot warning log records. It is a diagnostic of early failures, not a
behavior fix or performance comparison. XPOLL001 layout, counters, lifecycle
and tests remain unchanged. No native timer behavior is implemented here.
Other hosts do not emit this trace, so a fallback host clock is not labeled QPC.

Each admitted record reports sequence/limit, exact resolver failure branch,
guest thread ID/native Windows TID, saved guest LR/read flag, raw host QPC (zero means
unavailable), wait-array guest address, count/type, failed index, current object
guest address, effective type, whether header type was read and its value,
signature comparison/value, and whether the stashed handle was read and its
value. A false read flag makes its accompanying zero value unavailable, not an
observed zero. At this Ke call site as_type is UndefinedObject, so header type
is obtained by the existing read. Other forced-type resolver callers do not
gain a header-type read. No timeout, other array entry, object prefix, signal
state, event state, or pointed-to object is newly inspected.

The optional stack-local output copies the existing resolver's decision values.
The signature is still read once, in its original position. Stashed handle is
read only in the existing matching-signature branch. Failure reasons are:

* `stashed_lookup_miss`: header signature matched, existing generic object-table
  lookup returned null. This does not itself prove lifetime corruption.
* `unsupported_first_use_type`: signature did not match and the existing
  first-use type switch returned null. Types 8/9 would implicate an unsupported
  raw timer object; they do not establish the correctness of a timer fix.

The shared atomic budget returns each sequence 1..16 once and cannot overshoot
under contention. Strong CAS failures imply another successful claim, so at
most 16 competing successful claims can delay admission. An admission is never
returned or reset. After exhaustion, subsequent entries perform only the flag
and capacity check for this trace: no tracing QPC, thread/context lookup,
formatting, allocation, logging or additional mutex. Already-running calls that
sampled capacity earlier still have to win the final admission; losers do not
log or query context/QPC. Disabled calls pass no diagnostic pointer. Existing
resolver behavior, read order and return paths are retained.

For an admitted record, the local global-critical-region lock is released after
the failed decision is copied and before logger allocation/locking. Previously
resolved objects remain retained in the original function-scope array until
return. If an outer recursive lock is held by the caller it may still exist;
this code does not release caller-owned locks. Logging can perturb these first
16 instants. Logger filtering/failure may yield fewer than 16 retained records:
the bound is on admission/log attempts, not guaranteed evidence delivery.
No claim of full failure coverage, stable object identity across all failures,
guest instruction mapping, contention duration or frame-rate causality follows.
No sleeps, status substitution, event/wait changes, fault bypass, debugger,
memory dump, or new capture format/writer is added.

Source remains based on private 3b93. The known overlap with public PR1295's
revised guest-address checks, first-use guest_object_ptr_ and UpdateNativeHandle
cleanup must be reconciled in later native work; this diagnostic does not merge
or cherry-pick those changes. XTimer's APC callback is not assumed to implement
KeSetTimer/KDPC semantics. Actual failure tuples are required before a fix.

Focused Catch filter `[ke_wait_failure_trace]` has **two test cases in
Release/NDEBUG**: concurrent bounded admission with unique sequences, and
direct unsupported-resolver diagnostics/no-output equivalence with unchanged
stack headers. The unsupported branch retains its existing assert_always;
that test is compile-guarded out in assert-enabled builds (one case there).
A real stashed lookup failure requires a live KernelState fixture; no large
kernel fixture was added merely for this diagnostic. That branch is covered by
source review. Tests, build and formatter are UNRUN during source preparation.
