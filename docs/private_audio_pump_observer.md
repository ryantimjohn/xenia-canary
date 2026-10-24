# Private audio pump observer

This SDK-derived checkout is private and must not be sent upstream. The
default-off `--audio_pump_observer=true` flag observes the existing audio
worker; it does not change selected deadlines, semaphore capacity or wait
outcomes, callback count, guest execution, XAudio submission, frequency ratio,
pause/resume, save/restore, or fallback behavior.

The worker emits `APUMP001` lines in the ordinary Xenia log. A start line gives
the owner address, raw Windows QPC frequency and start QPC. Each *completed*
active five-second window has one aggregate line and up to four detail lines:
worst selection lateness, worst wake lateness, longest callback, and worst
token-timeout lateness. Roles may name the same attempt. The aggregate counts
all selected clients and separates no callback, interrupted wait, zero-timeout
semaphore timeout, other semaphore result, missing driver after token, and
executed callback. Details include slot, callback and wrapped argument,
existing selected/next deadlines in steady-clock microseconds, and separate
selection, wake, callback-start and callback-end QPC/steady-clock samples.
An initial zero `next_pump_us` is an unarmed deadline: its attempt is counted
but excluded from lateness maxima; a real callback duration remains eligible.
Query failures produce zero QPC and an invalid-stamp count, not a mapped time.
The semaphore result is from the existing nonblocking wait. A callback duration
includes the enabled stamp probes beside the guest call; it is not pure guest
work.

This is a sparse trace: only four selected attempts per completed window retain
individual timestamps. Absence of a detail near an audio empty edge is not
evidence that its pump was on time. The first selection starts window zero;
attempts are assigned by selection time and may finish after that window's
end, so outcome counts are not wall-clock callback throughput for that window.
Time before it, skipped empty windows, pauses with no selection, and the last
partial window are not qualified. A completed window is logged only when a
later selection closes it, so normal GUI `quick_exit` can censor the tail.
There are at most 16 start lines, 120 aggregate lines and 480 detail lines
across all workers in the process. Later worker sessions are unobserved. The
final admitted report marks `cap_reached`; workers then do
no further observer clock sampling (an already active attempt may finish), and
cannot support later negative claims. At most 64 lifecycle rows (register,
unregister, or whole-owner invalidate) plus one cutoff row are logged.
Registration serials distinguish a reused slot only inside an intact ordinary
RegisterClient/UnregisterClient interval of one worker session. Shutdown and
Restore emit capped invalidate rows because they clear or recreate slots
without corresponding registration rows. Restore has no added synchronization
with worker selection; overlapping selections remain ambiguous. After
invalidation, loss/cap, or QPC failure, slot identity is unknown until a new
intact registration interval. Callback addresses and guest heap pointers must
not substitute for a serial. Lifecycle rows and details use the same owner
address to separate AudioSystem instances, but an address alone is not a
lifetime key.

Each enabled stamp executes two explicit QPC queries and one steady-clock read
(which may itself call QPC in this MSVC runtime). `explicit_qpc_queries`
counts only the direct calls. `stamp_bracket_ticks` sums elapsed spans from
the first to the second direct QPC call, including the steady-clock read and
any intervening scheduling; it is not exclusive observer CPU time. A failed
or regressing QPC bracket is invalid, with zero QPC and zero measured span.
The selection stamp is taken under the existing registration lock so
lifecycle and selection timestamps retain their lock order. This is
measurement overhead, not a claim of zero timing impact.
The synchronous enqueue of a completed window and its details is measured in
QPC ticks and reported in the *next* window as
`previous_window_log_enqueue_span_ticks`; the final enqueue cost can be
censored by `quick_exit`. Selection immediately after a previous enqueue is
marked `logging_adjacent`. `lifecycle_log_enqueue_span_ticks` is cumulative
at report time. These are elapsed brackets, not sink-flush time or exclusive
CPU work. Initial start logging and collector arithmetic are not isolated
timing measurements. The emitted QPC and the window's last selected QPC
expose delayed reporting.

For a qualified GUI-close capture, also set `--audio_queue_trace=true` and the
existing bounded frame/pacing captures. The current app closes the log with a
bounded drain only for its existing capture flags; this ticket deliberately
does not alter that lifecycle. Observer-only quick-exit retention is therefore
unqualified. Validate a fresh same-launch PID/birth, app and log hash, positive
log drain, QPF, and source caps before comparing QPCs with queue/frame/pacing
records. Even a matching late pump cannot by itself prove audible doubling or
the cause of a visual gap.

The focused `[audio-pump-observer]` cases exercise distinct worst roles,
completed-window rollover, skipped and capped coverage, and registration
serial/cutoff behavior. They are source-only until separately built and run.
The flag is cached for the process; changing it requires a restart.
