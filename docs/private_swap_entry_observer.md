# Private VdSwap construction-entry observation

This ticket inherits SDK-derived private fork ancestry. Never upstream.
Source-only candidate: build, tests and application execution are UNRUN.

`--swap_entry_observer=<fresh absolute path>` defaults to empty. The hook at
`VdSwap_entry` returns before XThread/TLS/context identity reads, QPC or queue
work when disabled. Enabled records are taken before the existing parameter
assertions and packet construction. They describe attempted construction-entry
calls, including any that later return without a packet. This is not actual
ring submission, packet consumption, frame completion, scanout or unique FPS.
Nothing reads guest content, computes a packet token or changes guest behavior.
XPACE001, XFRAM001, XPOLL001 and PM4 handling are unchanged.

Enabled calls copy guest thread ID and full-width guest PPC LR saved in the
host PPCContext when available, and the native thread ID. Guest ID zero is allowed; LR zero is not an
absence sentinel (flags bit0 explicitly records whether the context was read).
QPC is sampled inside the short collector admission, after identity capture.
No per-event allocation, logging, file I/O, mutex, wait or guest memory access
occurs. A single atomic_flag try-lock serializes potential producers; contention
drops the event instead of blocking. Sequence is accepted-record order, not an
attempt number. Even if sequences are contiguous, all loss counters must be
checked. A queue of 1024 records and a cap of 32768 accepted records bound the
file to 1,310,864 bytes. After the cap, calls only update admission/drop counters;
the hook still collects host identity, but the collector does not sample QPC.
Counters are 64-bit process-lifetime counts (not saturating); no claim extends
to an execution long enough to wrap a counter.

The writer is separate. Normal GUI close removes the active pointer, closes
new mutation admissions and drains only short in-flight collector sections.
There is no admission across guest work and no new attempt to stop guest
threads. A caller that already obtained the retained Session may arrive after
cutoff; it cannot publish a record. `excluded_at_cutoff` is only a frozen sample,
not a final count of late calls. The Session remains process-retained, including
on timeout, so late callers cannot access freed storage. Startup failure closes
previously started streams; no observer opens if its path is relative or exists.

Close waits to its deadline for the writer to drain, write the footer, flush and
close. It cannot bound an OS file operation itself. A timed-out attempt stays
failed even if the writer later finishes; no complete ACK is emitted. The
isolated-test destructor may join and is not the production bounded-exit path.
Normal capture close also uses the existing bounded published logger drain.
Other fatal/quick-exit paths have no promised close ACK.

Wire is little-endian, manually encoded (no C++ struct serialization):

* Header64: `XSWAP001` at0, u32 version1/header64/record40/queue1024/cap32768
  at8/12/16/20/24; zero28..31; u64 QPF/startQPC at32/40; zero48..63.
* Record40: u64 accepted sequence/QPC/savedLR at0/8/16; u32 guestTID/nativeTID/
  flags/reserved at24/28/32/36. Flags bit0=LR read; all other bits/reserved zero.
  Missing LR must be zero. NativeTID must be nonzero. QPCs are nondecreasing in
  accepted order and bounded by start/cutoff. Numeric TID is not a birth proof.
* Footer80: `XSWAPEND` at0; u64 attempted/accepted/written/queueDrops/limitDrops/
  contentionDrops/excludedAtCutoff/writeErrors/cutoffQPC at8..72.

Open-gate `attempted = accepted + queueDrops + limitDrops + contentionDrops`.
Accepted is bounded by32768. A successfully written file has written=accepted
and exactly that many records, with sequence1..N. Writer failure may leave
partial evidence; it never establishes coverage. Structural validation alone
allows explicit loss and empty files, so quantitative coverage additionally
requires positive records, zero drops/write errors, a matching positive
same-owned-launch ACK and unchanged saved-input/identity receipts.
An enabled QueryPerformanceCounter failure records QPC zero; structural
qualification then fails closed even if the writer completed and ACK says
complete. Close completion alone never qualifies timestamp accuracy.

The one-off ACK is exactly:

```
Swap entry close: complete={} qpf={} start={} cutoff={} attempted={} accepted={} written={} queue_drops={} limit_drops={} contention_drops={} excluded_at_cutoff={} write_errors={}
```

`complete` means the frozen snapshot was drained/flushed/closed, not all future
VdSwap calls were observed. Compare cadence/counts only within an explicitly
intersected same-QPC interval alongside XPACE/XFRAM. Do not pair entries with
consumed packets, report queue latency, or infer CPU/GPU causality. Slow entry
cadence could itself reflect upstream backpressure. Startup/cutoff censoring,
observer overhead and differing scenes remain limitations.

Focused tests cover disabled lookup, actual writer/scalar/ACK round trip and
tampering, exclusive creation, queue overflow, concurrent admission accounting,
late cutoff calls and blocked writer timeout with mandatory fixture release.
Isolated Windows build closure: new base observer and test TU, app main TU,
kernel xboxkrnl_video TU; rebuild corresponding base/kernel archives and relink
app. No PM4 header or D3D12/Vulkan consumer rebuild is required. Existing project
source globs cover new base/test files in an ordinary full build; isolated
qualification must explicitly include them and their header dependencies.
