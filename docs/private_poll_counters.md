<!-- Private fork only. SDK-derived ancestry must never be upstreamed. -->
# Bounded private poll counters

`--poll_counters=<fresh absolute file>` enables a fixed aggregate snapshot of
only `XNotifyGetNext` and `KeWaitForMultipleObjects`. The default empty string
does no file/thread work and returns before thread/context lookup. This source
is a private diagnostic experiment, not a behavior fix or a performance claim.
No wait, return value, event operation, guest clock, debug/context setting or
source annotation option is changed. The existing timeout conversion runs once;
the observer copies its effective Windows DWORD milliseconds.

The sidecar uses exclusive create-new, no directory creation, and a single
close-only writer. Its hard size is **745600 bytes**, asserted at compilation,
under 1 MiB. Startup writes an unfrozen header prefix. Ordinary GUI shutdown
requests close before quick_exit. A positive same-process log acknowledgement
`Poll counters close: complete=true frozen_snapshot_only=true` is required in
addition to an exact-size file with `frozen=1`. Partial files, write/flush/close
errors, or a close timeout are unqualified. Normal close waits at most its
supplied timeout (currently two seconds); the observer is retained through
process exit if I/O or a producer is stalled. This is not a hard I/O cancellation
guarantee. Production retains the session even after successful close so TLS and
late API completions cannot dangle. Test owners must finish Operations before
destroying a Session. Startup/Close are single lifecycle-owner operations.

Guest paths perform no collector mutex acquisition, heap allocation, file I/O,
per-call logs or stackwalk. A fixed 64-slot array is claimed once per combined
guest thread ID/native Windows TID; slots are never reassigned. A TLS cache
avoids repeated slot scans. Native TIDs come from current_thread_system_id,
not the guest-overridable current_thread_id. These are IDs, not thread birth
certificates: reuse of the same pair is not independently detected. Slots are
single host-thread-owned; nested calls on that thread remain sequential.
Each API retains four saved guest LR values with counts, then caller_overflow.
LR is the saved context value, not a verified instruction/stack or flow mapping.

Entry and completion independently acquire a bounded short mutation admission.
No admission is held while executing the observed API or waiting in the guest.
Close atomically bars new admissions and drains only admitted short mutations,
then freezes bucket storage. For each API, unless saturation is flagged:

`started = completed + abandoned + outstanding_at_cutoff`.

An infinite guest wait can therefore coexist with a complete frozen snapshot.
Outstanding here means no admitted completion/abandonment, not proof the API
is still executing: a completion excluded during cutoff remains outstanding
in the frozen accounting relation.
Late completions update only retained atomic exclusion counters, never buckets.
`excluded_entry` and `excluded_completion` are sampled between `cutoff` and
`exclusion_sample_end_qpc`; they are not final process totals. Contention can
exclude an admission after eight CAS attempts and is included in these counts.
`global_saturation=1` means an atomic counter saturated or its bounded increment
could not be committed; counts are then lower bounds. Per-slot `saturation=1`
similarly qualifies all slot counts/sums. Slot overflow counts calls for which
no slot was available. `bucket_overflow` counts invalid bucket categories;
normal finite categories cannot run out of buckets. Caller overflow does not
discard the aggregate call. A default-disabled observer produces no sidecar.

The layout is the exact sequence in `poll_counters.h`: little-endian 64-bit
scalar fields, including all unused slots/buckets. Compile-time size assertions
exclude hidden padding in the qualified format. No pointers/atomic objects are
serialized. Range has `min,max`; untouched ranges are `UINT64_MAX,0`. Elapsed
has `samples,ticks(min,max),sum`. Slot `key` is guest ID in its high 32 bits and
native TID in its low 32 bits; zero denotes unused. Header includes QPF, raw
host QPC bounds and format limits. QPC is independent of guest clock scaling.

Notify bucket order is invalid required-id-output, invalid handle, any dequeue,
any empty, filtered dequeue, filtered empty, filtered miss with pending
nonmatching notifications. Null optional param output remains valid. Buckets
contain count, filter min/max, queue-before/after min/max, sampled elapsed.
Queue sizes are copied inside the existing dequeue critical region. Invalid
output/handle buckets have no queue ranges. A nonempty queue implies expected
signaled state by the current queue contract; **the event was not probed**.
No notification payloads, listener addresses or output memory are recorded.

Wait bucket index is `(timeout_class * 3 + wait_type_class) * 6 + result_class`.
Timeout classes: null, zero, negative, positive, unavailable. The unavailable
class preserves invalid-object early return without introducing a timeout read.
Wait type classes: all(0), any(1), other. Result classes: success/index0..63,
timeout0x102, APC0xC0, abandoned0x80..0xBF, invalid/failed(C000000D/C0000001),
other. Each bucket contains count, object-count range, raw signed timeout range,
effective host DWORD-ms range, exact result range, sampled elapsed, alertable
count and raw-timeout-present count. Raw timeout range uses sign-bit-xor ordered
encoding: XOR each stored endpoint with 0x8000000000000000 to recover its signed
two's-complement value. Null/unavailable timeouts have no raw range. Effective
host range is absent for early invalid-object return; 0xFFFFFFFF denotes the
Windows infinite DWORD. WaitAny success/abandon index follows from exact result;
all/any remain separate. Existing status quirks are observed without correction.

Elapsed measures every 1024th admitted start per thread/API, not every call.
Notify spans entry work through output assignments. Wait spans only the actual
XObject::WaitMultiple call, including its callbacks/yield/boost, excluding object
lookup and subsequent guest APC dispatch. Early invalid-object returns are not
timed. Missing QPC or unfinished samples have no elapsed sample. Counts/min/max/
sum describe sampled calls only; their ratio is not an all-call latency mean.
Observer admission/cache/counter overhead is unmeasured and may perturb this
poll-heavy workload. The aggregates cannot prove main-thread flow, causality,
notification correctness, lock contention time, or productive guest execution.

Focused Catch tests cover frozen accounting with a blocked outstanding wait,
entry/completion racing close, range/outcome aggregation, slot/caller/bucket
overflow, saturation, exclusive create errors, disabled lifecycle, and an
injected writer error. The injection exists only in the explicit Session test
API; production uses fwrite/fflush/fclose. Source preparation does not imply
these tests, a build, or any runtime experiment have been performed.
