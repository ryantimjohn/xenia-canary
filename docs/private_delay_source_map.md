# Private bounded PAUSE/source-map diagnostic

`--private_delay_source_map_trace=true` records a small, compiler-owned sample
after x64 emission has copied the source map and after `X64Function::Setup` has
set the actual emitted code length. The default is false and exits before any
scan, allocation, budget lock, or diagnostic log. It changes no emitted bytes,
guest clock API, explicit scheduling policy, or exception handling. Enabled
compiler scanning and logging can still perturb runtime timing. This private fork must not be
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
offset, and `map16`, a fixed mapping for **each of the 16 candidate bytes**.
Each 36-character field is `iiSgggggggghhhhhhhhmmmmmmmmnnnnnnnn;`: byte
index, state, guest marker address, HIR offset, preceding marker code offset,
and next marker code offset, all hex. `P` means the marker is the one the
existing reverse lookup would select for that byte. `F` means the byte
precedes the first marker; `U` means a missing or invalid map; `T` means the
map exceeds 65,536 entries. Only `P` carries an associated guest marker.
`format_error` in place of `map16` is unqualified.
When no later source marker exists, `next` is the emitted code length sentinel;
otherwise it is the next marker's code offset.
The association is not proof of the exact guest instruction executing at a
sampled host RIP. To correlate a new host RIP, require a fresh same-launch code
range, compute `i = RIP - host_begin - host_offset`, require `0 <= i < 16`,
and use field `ii=i` only when its state is `P`;
do not reuse addresses from previous launches. The function entry alone is
never an exact guest-PC claim.

The enabled diagnostic admits at most 4096 functions, scans at most 16 MiB of
emitted code, and emits at most 16 match lines plus one metadata and one
truncation line. It emits at most four matches per function. Those fixed-format
lines have a total diagnostic payload bound below 16 KiB. A contended budget lock skips a function
without scanning; therefore a capture never proves that unmatched functions
had no PAUSE run. Truncation and partial coverage prohibit negative claims.

The diagnostic is unbuilt and untested until a separately approved private
validation run. It does not decode instruction boundaries, inspect a running
process, walk the shared code cache, or enable broad source annotations.
