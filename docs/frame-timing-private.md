# Private frame timing sidecar

This is a fork-only diagnostic with private SDK-derived ancestry. Do not
upstream the format or observations. It does not change guest timing, GPU
scheduling, presentation, or any existing paired input/audio format.

`--frame_timing=<fresh absolute path>` enables it. The default empty value
does nothing. Startup creates the file exclusively and fails if the path
already exists or its parent is missing. Keep the ordinary Xenia log from the
same process. Normal GUI exit waits up to two seconds for the writer before
`quick_exit`; abnormal exits may leave an incomplete file. Producers try a
short admission mutex once and never wait on it or perform file I/O. A failed
try-lock is counted as contention loss. All writes run on a writer thread.
The queue holds at most 4096 records, and the session accepts at most 524288
records (16,777,352 maximum file bytes including header and footer).
Queue, contention, session-limit, closing-exclusion, and write failures are
counted and disqualify the file.

The independent little-endian v1 format has a 64-byte header (`XFRAM001`,
version 1, header/record sizes, queue/record limits, QPF, start QPC), 32-byte
records, and a 72-byte footer (`XFRAME01`, accepted/written/queue-drop/
limit-drop/contention-drop/closing-exclusion/write-error counts and cutoff
QPC). A record is `kind/u32`,
`code/u32`, `sequence/u64`, `begin_qpc/u64`, `end_qpc/u64`:

| Kind | QPC fields | Code | Meaning |
| --- | --- | --- | --- |
| 1 guest swap | equal | zero | GPU command processor reached a valid `XE_SWAP` command. |
| 2 D3D12 host present | before/after `IDXGISwapChain::Present` | raw HRESULT bits | Duration of the host API call, including a failed return. |

Sequences are assigned on successful admission and are independent per kind.
The records from two threads may interleave; keep file order within each kind.
The structural validator requires contiguous sequences and nondecreasing QPC
within each kind. For a contiguous window, guest swap rate is
`(guest swaps - 1) * QPF / (last QPC - first QPC)`.
The distribution of consecutive guest-swap intervals exposes stalls even
when average FPS is stable. Compute host-present call intervals separately;
the host can present the same guest image multiple times, draw the UI at a
different refresh rate, or drop queued images. A `Present` call returning does
not prove display scanout or unique game progression. The trace does not
contain GPU completion timestamps or guest simulation-clock values.

Close withdraws admission, lets already entered producer calls finish, then
takes the cutoff QPC. A call that observes the closed gate is outside the
capture, even if it sampled QPC just before cutoff. A host `Present` begun
before the gate closes but returning after it closes is also excluded. The
observed first-to-last interval for each kind is therefore narrower than the
header-to-footer interval. Menus, startup, pauses, and shader work may still
appear; analyze a marked play segment separately. Compare QPC with audio
sidecar anchors only after confirming both files and logs came from the same
owned process and use the same raw QPC epoch. The diagnostic never calls
`Clock::QueryGuestTickCount`, which would update the clock being investigated.

`ValidateFileStructure` checks magic, sizes, hard limits, per-kind intervals
and sequences, and zero drops/errors. It does not prove clean shutdown. A
qualified file also needs one positive `Frame timing close` acknowledgment
from the flushed log of the same process, with matching start/cutoff QPC and
all counts. A missing footer, any loss, or `complete=false` leaves the interval
unqualified.
An empty but well-formed file proves only recorder lifecycle; require positive
guest and host record counts before reporting either FPS series.
