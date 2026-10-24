<!-- Private fork-only work over SDK-derived ancestry; never upstream. -->

# Private capture close log drain

The retained post-timer attribution replay failed qualification: process exit 0
and finalized-looking frame/poll files did not have their mandatory close log
acknowledgments. That failure remains failed. This patch changes no analyzer,
wire format, timer, guest execution, protected-zero or first-chance behavior.

The source defect is a missing queue completion barrier. `FlushLog` sleeps 10 ms
and flushes sinks from its caller without waiting for the logging writer to
consume published records. Normal GUI shutdown then uses `quick_exit`, bypassing
the Logger destructor's terminator/join. The actual scheduling cause of the lost
tail is inferred, not proved; the writer's literal 50 ms idle branch is not
established as reachable in this ordinary path.

Only normal GUI close with frame or poll capture enabled calls the new drain,
after the existing sidecar closes and before `quick_exit`. Ordinary `FlushLog`
and capture-disabled exit remain unchanged. A fixed one-block marker is ordered
behind earlier queue reservations, including any unpublished gap. The writer
recognizes it without emitting a newline, calls sink Flush, then release-publishes
its completion sequence. Concurrent records reserved after the marker are not
covered. A timeout leaves only self-contained queue bytes, no borrowed caller
state. Absent/stopped logger and writer-thread reentry refuse completion.

The requested wait is clamped to two seconds. This is a queue/sink-processing
acknowledgment, not durable storage or sink I/O error qualification: the existing
LogSink interface returns void. Initialization, destruction, sink replacement
and Stop must not race the drain. A non-complete result gives a fixed direct
stderr failure indication and exits nonzero; stderr and sink OS I/O can block,
so no hard process wall-time guarantee is claimed. External owned-Job limits
remain required. Other quick_exit paths do not gain capture-close qualification.

The private disruptorplus dependency adds only `try_claim_once`: one strong CAS
attempt with no contention retry or full-ring reservation wait. The outer drain
checks its steady deadline between attempts. Existing vendor `try_claim` remains
unchanged and is deliberately not used here because its CAS loop is not bounded.
The original isolated validation used a pinned private dependency archive
overlaid after extraction of the parent source archive because its gitlink
was not fetchable from the upstream URL. That archive and the original
validation receipts remain historical evidence. The current fork tree tracks
the exact dependency source and MIT license in `third_party/disruptorplus`;
a clean clone needs no private archive overlay or dependency gitlink fetch.
See [the vendoring provenance and per-file manifest](vendor_disruptorplus_fork.md).

Pinned dependency ancestry: original `bf9c70c0cc92b6435f7edec75e12751ea37e5c2f`,
private commit `d52c0bf5f71b1b1275ff34b8725d75de8b1ebac4`, private tree
`402654611f15cd672b6805a202af551a8e123e77`. The modified
`include/disruptorplus/multi_threaded_claim_strategy.hpp` original Windows
checkout SHA256 is `06A8011BF9B134ACDC76932752AEA1BCD8FC2B297BD397373A8F5461D04461D2`.
The vendored Git/LF byte SHA256 is
`8D44E72F960F1DE597BB6251FB544D5222D7E5F8F329B3C42F2D81C5618E94FD`;
its Git blob OID matches the private child tree exactly.

Six `[logging_drain]` host cases exercise the actual isolated ring/writer/sink:
pending output, blocked sink flush with later recovery, a full ring, stopped
writer, an unpublished predecessor and writer reentry. Fixtures release gates
and reservations before assertions/joins. No process logger is replaced.
At the initial source review these cases, the app build and replay were UNRUN.
Later isolated qualification passed six focused cases with 23 assertions and
used the reviewed dependency bytes for a mixed app build. That evidence does
not replace a clean build and test of the full integrated fork tree, and it
does not retroactively qualify earlier failed artifacts.
