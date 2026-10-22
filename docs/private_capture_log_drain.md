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
The dependency commit is local and unpublished; its gitlink is not fetchable
from the unchanged upstream URL. Isolated validation must explicitly overlay a
pinned archive of the private dependency into `third_party/disruptorplus` after
extracting the parent source archive (Git parent archives omit gitlink contents).
Never substitute upstream headers silently or touch the frozen staging headers.

Pinned dependency ancestry: original `bf9c70c0cc92b6435f7edec75e12751ea37e5c2f`,
private commit `d52c0bf5f71b1b1275ff34b8725d75de8b1ebac4`, private tree
`402654611f15cd672b6805a202af551a8e123e77`. The modified
`include/disruptorplus/multi_threaded_claim_strategy.hpp` SHA256 is
`06A8011BF9B134ACDC76932752AEA1BCD8FC2B297BD397373A8F5461D04461D2`.
Future fork integration must preserve this commit in a private fetchable source
or vendor the reviewed bytes through a separately reviewed integration change;
an otherwise unreachable gitlink is not a complete distributable checkout.

Six `[logging_drain]` host cases exercise the actual isolated ring/writer/sink:
pending output, blocked sink flush with later recovery, a full ring, stopped
writer, an unpublished predecessor and writer reentry. Fixtures release gates
and reservations before assertions/joins. No process logger is replaced.
Source review only: configure, build, tests and replay are UNRUN until separately
granted. Validation should build xenia-base-tests and run only this filter, then
build the application and perform a separately granted bounded recorded replay
with unchanged capture ACK requirements. None of these checks retroactively
qualifies the earlier failed artifacts.
