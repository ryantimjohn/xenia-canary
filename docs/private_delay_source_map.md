# Private bounded PAUSE/source-map diagnostic

`--private_delay_source_map_trace=true` records a small, compiler-owned sample
after x64 emission has copied the source map and after `X64Function::Setup` has
set the actual emitted code length. The default is false and exits before any
scan, allocation, budget lock, or diagnostic log. It changes no emitted bytes,
guest time, scheduling, or exception behavior. This private fork must not be
published upstream with SDK-derived work.

`XDELAYMAP001 begin` records the **effective** `delay_via_maybeyield` value at
the first acquired diagnostic budget. When that option is true, db16cyc emission uses
`MaybeYieldForwarder` instead of the inline PAUSE hint; absence of a PAUSE-run
match in that mode is expected and does not establish absence of guest delay.
The ordinary config-file dump prints raw file content before applying CLI
overrides, so it is not an effective-value check.

The selector finds eight contiguous `F3 90` byte pairs inside the function's
actual emitted code length. A byte match is a *host candidate*: it is not an
instruction decode and may include bytes of another instruction. Every match
reports its host code begin, actual code length, function guest entry, byte
offset, and the preceding and next source-map markers. For the final marker,
`next_marker_offset` is the emitted code length sentinel, not another marker.
`map=preceding` means
the marker is the one the existing reverse lookup would select. `fallback`
means the match precedes the first marker; `unmapped` means the map is missing
or invalid. `truncated` means the map exceeds 65,536 entries. Only `preceding` carries an associated guest marker, and even that
is a source-map association, not proof of the exact guest instruction executing
at a sampled host RIP. The function entry alone is never an exact guest-PC
claim. To correlate a new host RIP, require a fresh same-launch code range and
an offset in a recorded match; do not reuse addresses from previous launches.

The enabled diagnostic admits at most 4096 functions, scans at most 16 MiB of
emitted code, and emits at most 16 match lines plus one metadata and one
truncation line. It emits at most four matches per function. Those fixed-format
lines have a total bound below 8 KiB. A contended budget lock skips a function
without scanning; therefore a capture never proves that unmatched functions
had no PAUSE run. Truncation and partial coverage prohibit negative claims.

The diagnostic is unbuilt and untested until a separately approved private
validation run. It does not decode instruction boundaries, inspect a running
process, walk the shared code cache, or enable broad source annotations.
