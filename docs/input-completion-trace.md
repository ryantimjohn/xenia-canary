# Private input completion diagnostic

This inherits the private fork's SDK-derived ancestry policy and is not eligible
for upstream contribution. It observes input; it does not generate keystrokes,
change replay state, close dialogs, or override guest API results.

Enable with `--input_completion_trace=true`. The default is false. The setting is
snapshotted on the first actual GetState/GetKeystroke/GetKeystrokeEx shim entry
and remains fixed for the process. There are no guest memory reads for tracing
while disabled, no observer locks, and no clock or context sampling.

The shared monotonic anchor is the first of those shim entries, before its
original UI gate. Times use `std::chrono::steady_clock` epoch milliseconds, as
the private file/checkpoint diagnostics do. This is **not** the replay driver's
first-GetState anchor. The anchor row records its API, thread and entry LR;
actual A delivery within the window must be observed, not inferred from launch
or recording timestamps.

Changed completion rows cover entries in `[180000,240000)` milliseconds after
that anchor. At most 64 rows are emitted across all three APIs, plus one cap
notice. One previous signature per API bounds deduplication; packet number alone
does not count as a change. Alternating callers/threads may consume this budget.
API numbers are 0=GetState, 1=GetKeystroke, 2=GetKeystrokeEx. Route numbers are
0=invalid output, 1=invalid user, 2=UI gate, 3=input driver path.

Each row reports entry sequence/time/LR/thread, input flags and user only if
already read by the original path, the single original UI-active decision,
actual result and driver call count, output initialization/validity, and either
A/packet or virtual key/edge flags. Output is sampled only on initialized success
paths. Gated success with zeroed output is distinct from driver EMPTY or
disconnection. The Ex user pointer remains unread on its UI gate and null-output
return. No guest buffers, host pointers, profile data or skeletons are logged.
The driver-call count means actual calls to InputSystem's GetState/GetKeystroke
dispatch boundary; it does not count or identify the backends it selects.

The outer GetKeystroke calls a private shared implementation, rather than the Ex
export, so one guest call has one API identity and one entry/completion. Public
typed export signatures are unchanged. InputSystem's existing lock covers the
whole driver operation, including any-user iteration. Observation mutexes and
input locks are released before logging. The existing any-user return behavior
and outer user-index conversion are deliberately preserved.

Fixed per-API saturating entry/completion/UI-gate/driver-call counters accumulate
from the anchor independently of deduplication and the row cap. The first call
entered at or after 240 seconds emits one three-API counter snapshot **through
that completion**, not final process totals. In-flight calls may complete later;
no post-window call means no summary. No destructor or unwind logging occurs.
The separate Kinect Guide state is not sampled or changed; its existing INFO
open/close records remain the appropriate evidence for NUIPause.

Validation source is `xam_input_completion_test.cc`, tag `[input-completion]` in
`xenia-kernel-tests` (existing recursive test discovery). It exercises the exact
production dispatch helpers with fake UI/driver dependencies, including null
index on the UI gate, A, EMPTY, key-up and disconnection; disabled/enabled result
parity; window/dedup/cap/counters; and real export trampolines on safe early
returns using guest-format buffers. Full emulator UI/input lifecycle and live
caller consumption still require a separately authorized runtime observation.

Production compilation changes only `xam_input.cc`; the helper is header-only.
No class layout or other kernel archive members change. Recompile that object
with its actual include closure, replace that one canonical kernel member, and
validate a fresh archive/relink before any game test. No tests or build have been
run as part of source preparation.
