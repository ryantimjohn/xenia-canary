# XAudio2 queue timing trace

`--audio_queue_trace=true` enables a private, default-off observer for XAudio2
drivers. It changes no PCM samples, queue capacity, pacing, callbacks,
guest-facing results, or host API calls. Each driver receives a process-unique
numeric ID. `client=-1` identifies an independent media-player driver;
nonnegative client values identify registered audio client slots. The ID
distinguishes reused slots and addresses are never printed. The flag is
captured once for the process; the disabled submit and callback paths take
only a pointer branch, with no diagnostic timestamp or allocation.

At orderly driver shutdown, the trace scans occupied bins for the first 600
seconds since the first submit attempt plus one overflow bin. It prints at
most **eight selected bins per driver**, ranking queue-zero samples, API
errors, voice errors, and long gaps ahead of healthy bins. Ties prefer later
bins. The cumulative summary includes occupied, emitted, and omitted bin
counts; omitted bins still contribute to that summary. A process-wide budget
limits **all drivers combined** to 600 selected rows and 16 cumulative
driver-shutdown summaries. A depleted budget suppresses later lines without
changing the counters while a driver exists. A driver emits selected rows
only if its companion summary fits the budget. Each selected bin prints its
first and last observed steady-clock microseconds, its index, and submission
counts. The overflow bin may cover longer than one second. `min_queued_before=-1`
means no post-priming queue sample was taken in that bin.

`attempts` counts `SubmitFrame` calls reaching the existing XAudio2 `GetState`
and submit path. `submitted` and `submit_errors` classify the result of
`SubmitSourceBuffer`. `ratio_errors` counts failed `SetFrequencyRatio` calls
after a successful submit. Queue depth is the existing `BuffersQueued` value
before submission. `queue_zero`, `queue_one`, and `min_queued_before` exclude
the first successful submission, when an empty queue is normal. They also
exclude paused submissions and the first successful submission after an
explicit driver pause/resume or system pause/resume. `max_success_gap_us`
compares **pre-conversion, pre-API timestamps** of successful submit attempts
in the same unpaused priming period; it is not the gap between API completion
times. A failed
submit cannot start a new gap. The XAudio2 callback only increments atomic
`buffer_ends` and `voice_errors` counters. Those are sampled at the next
submit or shutdown and assigned to that sample's bin, so their bin time is
approximate; the cumulative summary has the exact callback counts through
voice shutdown.
An explicit driver pause updates the trace just after the existing XAudio2
`Stop` call, so a concurrent submission at that boundary may be classified
on either side of the pause. Treat that narrow boundary as approximate.

Registered driver shutdown holds the AudioSystem's existing client and global
locks while these selected lines are written. The cumulative summary prints
`teardown_steady_us` from shutdown entry to just before its final log call and
`flush_steady_us` from selection completion to that same point. These are
approximate **lower bounds** because the final summary write is not included.
When comparing concurrent drivers, exclude bins overlapping that teardown
range and nearby log time from causal conclusions; the observer's own output
may delay another client. The eight-row cap keeps this effect bounded.

A queue-zero sample after priming plus a long successful-submit gap supports
a producer starvation hypothesis. It does not prove an audible gap, and this
trace cannot attribute a stall to the guest callback, worker deadline,
semaphore slot, PCM content, resampling, or host output device. The worker
scheduler is not instrumented in this first patch because slot reuse and
misses outside the client lock require a separate correlation design. Queue
depth is sampled only when a submission occurs; an empty queue between samples
may go unseen. A process that exits without orderly voice shutdown may emit
no trace. No PCM, profile data, callback arguments, or pointers are logged.
