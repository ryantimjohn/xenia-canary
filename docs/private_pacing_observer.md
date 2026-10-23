<!-- SDK-DERIVED FORK-ONLY WORK: never upstream. -->
# Private Windows pacing observation (source-only)

`--pacing_observer=<fresh absolute path>` is empty/default-off. It observes only
Windows vsync limiter deliveries, not unlimited/other sleeps, presentation or
physical scanout. No guest clock read, scheduling fix, resolution request,
protection/context change or guest memory read is added. Existing MarkVblank
and NanoSleep symbols remain; the unobserved MarkVblank body is unchanged.
NanoSleep shares the literal original conversion (ns/100, nonzero sub100 ->1,
negate), including unusual negative sub100 behavior. Observed NTSTATUS is raw.

Each admitted ordered limiter tick records five QPCs: mark entry, immediately
before/after synchronous interrupt dispatch (including guest Execute), and
immediately before/after the existing NtDelayExecution call. These distinguish
callback duration, NT wait duration, and consecutive delivery gaps. They do not
measure requested-vblank deadline, scheduler ready time or display FPS. Enabled
observation adds five QPCs, short atomic operations and queue-copy overhead.

Timer metadata is copied from the existing startup query/set calls, before the
session starts. Missing calls have flags=0. Query success validates min/max and
query-current; set success separately validates set-current. Requested is known
only with query success. There is no fresh post-set query. Initializing the old
output locals to zero makes failure metadata deterministic; the same query/set
calls remain, including a zero desired value if the query unexpectedly fails.

The session is retained until process exit. Begin/Complete admissions cover only
short mutations, never guest execution or NT waiting. Close shuts admissions,
drains those short mutations, freezes counters/cutoff, and lets the writer drain
a fixed SPSC ring. One ordered production limiter thread is required (no nested
Begin); contending misuse producers are rejected. Sequence is the admitted start
ordinal, so queue/contention losses can leave gaps. Cap exhaustion suppresses
further QPC capture. No per-event heap allocation, file I/O, logging or blocking
producer lock. The writer uses independent I/O and a bounded queue; OS sink I/O
itself has no hard wall-time guarantee. Close waits at most its supplied deadline;
a timed-out production session remains alive and cannot give a positive ACK.
Isolated test destruction joins after gates have been released; it is not the
bounded application shutdown API.

Normal GUI close emits `Pacing close` after footer fflush/fclose and uses the
existing bounded published-sequence logger drain before quick_exit. A positive
ACK means frozen/flushed output, not zero loss or all callbacks finished. Require
same-owned-launch flushed log, complete=true, exact start/cutoff/QPF/timer/count
match and separately require at least one written record and zero queue/limit/contention
losses, write errors and outstanding ticks for qualified pacing coverage. Other abrupt exits remain
unqualified. XFRAM001 and XPOLL001 bytes and semantics are unchanged.

All fields little endian, maximum 2,621,664 bytes:
* Header128: XPACE001 at0; u32 version1, header128, record80, queue1024,
  limit32768 at8..24; reserved28; u64 QPF/start at32/40; eight u32 timer fields
  flags/query-status/set-status/min/max/query-current/set-current/requested
  at48..76; zeros80..127. Timer flag bits: query attempted/success,set attempted/success.
* Record80: u64 sequence,mark,callback-begin,callback-end,wait-begin,wait-end at0..40;
  signed64 requested-ns/NT-interval at48/56; u32 nativeTID/status/flags/reserved
  at64/68/72/76. Flags exactly1 means NT call occurred; reserved0. Ordered QPCs
  and exact interval conversion required. No context/guest-PC attribution.
* Footer96: XPACEEND then u64 started/completed/accepted/written/queue-drops/
  limit-drops/contention-drops/excluded-at-cutoff/outstanding/write-errors/cutoff.
  started=completed+outstanding; completed includes queue/contention rejections.
  Contention combines failed Begin (not started) and failed Complete admissions.
  Therefore completed-accepted-queue-drops <= contention, not equality.
  Limit drops are attempted starts after32768, not completed operations.
  Excluded is sampled once during close; later completions are not final totals.
  Outstanding is a frozen count, not evidence of a permanently blocked callback.

ValidateFile is bounded structural validation and requires matching external ACK;
corrupt reserved fields, ordering, count relations, conversion and file extents
are refused. It cannot prove timestamps belong to an owned launch on its own.
Seven focused cases (58 assertion sites, Windows) cover conversion extremes,
format/ACK refusal, one-record production publication, pending drain and
queue/cap losses, a callback outstanding across close, timeout sticky failure,
and disabled token behavior. Source-only: builds/tests/game UNRUN. This private
ancestry is eligible only for the user's fork, never upstream.
