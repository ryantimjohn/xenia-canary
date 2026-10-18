# Guest GPR checkpoints (private diagnostic)

Disabled by default. This observes scalar register metadata at two user-selected
guest instruction addresses without pausing execution. It neither reads guest
memory nor changes guest context, API results, or control flow. The private
dependency ancestry remains fork-only and is not eligible for upstream submission.

Set both `guest_gpr_capture_pc` and `guest_gpr_observe_pc` (uint32, distinct,
four-byte aligned). Both zero disable the feature. A partial or malformed
configuration logs an error and disables it without failing emulator startup.
The ordinary compilation profile is required by the existing frontend; there
are no additional optimization/debug flag requirements. The diagnostic supports
optimized compilation via an explicit context barrier and builtin call.

Other CPU options:

| Option | Meaning | Default |
| --- | --- | --- |
| `guest_gpr_capture_key` | Capture key GPR, 0–31 | 0 |
| `guest_gpr_observe_key` | Observe key GPR, 0–31 | 0 |
| `guest_gpr_capture_mask` | Register bitmask, at most six set bits | 0 |
| `guest_gpr_observe_mask` | Register bitmask, at most six set bits | 0 |
| `guest_gpr_predicate_gpr` | Observe equality predicate register, low32; -1 disables | -1 |
| `guest_gpr_predicate_value` | Predicate comparison value, uint32 | 0 |
| `guest_gpr_delay_ms` | Delay observation output from first checkpoint execution | 0 |

Configuration is snapshotted once in frontend initialization. Register masks are
decoded in ascending register-number order; six logged value positions are padded
with zero. Values and cache keys retain all 64 bits. The PC label is the immutable
configured address, not a sampled PPC context PC. Hooks execute before that
instruction and after its incoming branch label.

The frontend owns sixteen total `(thread_id, key)` FIFO slots, including unmatched
observations. The seventeenth identity replaces the oldest inserted slot;
updates do not change FIFO position. There is no cross-thread matching. Capture updates the
slot and its sequence/time from startup, regardless of observation delay or
predicate, and never spends output rows. Each capture occurrence advances the
capture version. New identities fit by eviction, and rows report the cumulative
`cache_evictions` count. A miss can mean eviction or an unmatched thread,
key reuse or a capture that never occurred; it does not prove absent I/O.

Output is limited to 64 rows per frontend lifetime plus one cap notice. Within
a slot, output requires changed selected observation values or capture version.
All state, including delayed output, resets with a new frontend,
not with a title change. There is no claim of title ownership/reset behavior.

`steady_epoch_ms` is `std::chrono::steady_clock::now().time_since_epoch()` in
milliseconds, also emitted by the disc read-completion diagnostic. It is only
comparable inside the same host process, is not UTC, and continues across guest
debugger pauses. `elapsed_ms` starts at the first capture or observe hook, even
if a predicate rejects that observation. `capture_age_ms` is meaningful only
when matched. Sequence order is mutex reservation order; output may interleave
across threads. Formatting/logging occurs outside the collector mutex and the
handler takes no global critical-region lock.

The logger is generic: the caller chooses PCs/registers appropriate to a verified
instruction listing. An identical buffer address alone is insufficient if it
has been reused; use timing, sizes, and capture sequence. Different intermediate
buffer pools require a separate transfer observation. No payload is emitted.

Validation is in normal `xenia-cpu-tests`, filter `[guest-gpr-checkpoint]`.
Collector tests cover full-width/thread identity, delay, low32 predicate,
deduplication, cache saturation, concurrent row saturation and validation. A
compiled synthetic PPC program branches into a selected point, overwrites the
observed GPR in that very instruction, and checks preinstruction values and
enabled/disabled architectural results with optimization/store elimination on.
These tests have not been run as part of source preparation.
