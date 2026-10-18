# Bounded disc read completion diagnostic

This private diagnostic has fork-only ancestry and must not be upstreamed.
It uses the existing log sink and is disabled by default:

```
--trace_disc_read_completions=true
--trace_disc_read_delay_ms=45000
--trace_disc_read_path_prefix=<optional device-relative path prefix>
```

Only the guest VFS mount `\Device\Cdrom0` is eligible, compared without case.
The optional prefix is also compared without case against the device-relative
path. It is a literal prefix; separators are not normalized by this diagnostic.
Other mounts (including profiles and content storage) are excluded before any
path formatting. No host filesystem path, XUID, profile metadata, APC value,
buffer contents, key, guest stack or host address is emitted.

Time zero is the first enabled, Info-loggable disc read completion reaching the
trace point, before the optional path-prefix filter. Elapsed milliseconds use
`std::chrono::steady_clock`; wall time continues during guest/debugger pauses.
The delay is measured from that anchor, not process creation, title launch or
the first prefix match. Both anchor and budget last for the process lifetime;
changing flags, reopening files or loading another title does not reset them.

The process-wide saturating atomic budget permits at most 64 completion rows
and one cap notice, including concurrent reads on different files. Each printed
guest path is limited to 256 UTF-8 bytes, avoids splitting a UTF-8 sequence and
replaces ASCII controls. `path_truncated` reports truncation. Sequence numbers
reflect budget reservation; scheduling may reorder rows and the cap notice.
The logger's usual level/filter settings still apply.

The trace point is in `XFile::ReadInternal`, after `out_bytes_read` is assigned
and before completion-port/event notification. Rows report guest destination
or `unmatched`, requested
count, resolved offset, actual count, actual completion status and notification
flag. No handle or alias identity is captured; use sequence/time/path together
when correlating. The trace takes no additional global/object-table lock.
NtReadFile may subsequently return PENDING for a nonsynchronous handle even
though this actual completion already happened. The trace status is therefore
not labeled as the syscall return status.

Guest destinations are emitted only when the existing heap/range/access checks
reach the normal guest-to-host translation used by ReadSync. No host pointer is
converted or truncated into a fabricated guest address. Earlier failures and
zero-length requests are unmatched. The address is the original guest request,
including a physical alias if applicable, rather than a canonical physical
address. Direct VFS reads into host temporary buffers bypass this layer and
remain unobserved/unmatched; no guest address is inferred for them.

ReadScatter invokes ReadInternal per segment with `notify_completion=false`;
these rows are segment completions, not the aggregate scatter status. An asset
inside an archive may appear only as the containing file and offset. A buffer
match associates kernel I/O, not automatically a particular song or loading
dependency; an absent match is inconclusive if guest buffers/copies intervene.

Before runtime use, meaningful focused checks should cover 64 unique row
reservations and exactly one cap notice under concurrent completions; disc-only
and prefix filtering without spending budget; delay anchoring before prefix
matching; controls/UTF-8 path limits; zero-length and rejected-range unmatched
destinations; and successful/EOF/error counts remaining identical with tracing
disabled/enabled. No payload comparison or private recording is needed for
these checks. Build/test/runtime execution remains separately authorized.
