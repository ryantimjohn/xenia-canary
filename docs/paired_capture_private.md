# Private paired input capture sidecar

This is a fork-only diagnostic with private SDK-derived ancestry. Do not
upstream the format or observations. It is not a Kinect implementation or a
paired replay reader. It does not change `.xdepth`, `.xctrl`, replay scheduling,
or live sensor behavior.

`--paired_capture=<fresh absolute path>` enables capture. The default empty
value does nothing. Startup creates the file exclusively; it never overwrites
one and fails startup if creation fails. There is no directory creation. The
caller should use a fresh output directory and retain the ordinary log, which
reports the final counts. The normal GUI exit path gives the writer two seconds
to drain before `quick_exit`. Abnormal process exit can leave an incomplete
file. The producer hooks only enqueue scalar records under a short mutex; the
writer thread performs all record file I/O.

The little-endian v1 file has a 64-byte header (`XPAIR001`, version 1, header
and record sizes, queue and record limits, raw QPF, start QPC), zero or more
112-byte records, and a 64-byte footer (`XPAIREND`, accepted, written,
coalesced, queue drops, record-cap drops, write errors, end QPC). Each record is
kind/u32, key/u32, raw QPC/u64, then 24 u32 words. Floats are IEEE-754 bit
patterns, and signed integers use their u32 two's-complement bits. The hard
limits are 2048 queued and 131072 accepted records. Every overflow is counted.
`ValidateFile` rejects a missing footer, bad sizes/magic, any drops or write
errors, or unequal accepted/written counts. A timeout is also reported as an
unclean close in the ordinary log; retain that log with the file.

Kinds and used words (remaining words are zero):

| Kind | Key | Words |
| --- | --- | --- |
| 1 InputState | requested user | result, route, requested user, flags, output valid, packet, buttons, triggers L/R, sticks LX/LY/RX/RY |
| 2 Keystroke | returned user | result, route, requested user, flags, virtual key, Unicode, edge flags, returned user, HID code |
| 3 StreamStart | 0 | image type (1 depth, 2 colour) |
| 4 DepthReturn | 0 | published frame sequence low/high, published steady-clock microseconds low/high, guest sequence, guest milliseconds low/high, copy QPC low/high |
| 5 TiltStatus | 0 | result, angle degrees, block present, accelerometer XYZ, status flags, floor plane float bits A/B/C/D, floor flags, remembered accelerometer XYZ |
| 6 ElevationGet | 0 | result, degrees, status, angle written, status written |
| 7 ElevationSet | 0 | result, requested degrees |
| 8 RememberFloor | 0 | result, floor plane float bits A/B/C/D, flag |

The record QPC is taken when the event is enqueued. `DepthReturn` also carries
copy QPC sampled after the guest buffer/result write, and its record QPC is
taken after successful request completion. The published sequence and
steady-clock timestamp join the existing `.xdepth` frame header. These clocks
do not establish exact guest perception or audible song onset. A returned
frame may be an older published sequence. InputState, tilt, elevation and
floor observations emit first/on-change records. InputState packet number and
query flags alone do not trigger an extra record; emitted records retain both
values. Successful nonempty keystroke edges and each depth delivery are kept.

The sidecar contains no depth pixels, skeletons, profile bytes, or game media.
Even with complete scalar anchors, a new live human capture is required to
recover calibration history omitted from existing recordings. A future replay
implementation would require a separate design and validation stage.
