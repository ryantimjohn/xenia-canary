# Private selected PPC source observation

`--private_delay_guest_source_trace=true` is a default-off, private diagnostic
for a fresh compilation of guest function `829E6360`. It changes no guest
instructions, branch policy, waits, protection, or scheduler choice. The flag
selects only the first compilation attempt for that address in the process;
failure consumes the attempt. It is intended for private diagnostic media and
must not be enabled as a general emulator option.

The PPC builder already reads and byte-swaps each word while translating the
scanned function. On the selected attempt, a fixed-size callback copies the
first at most 64 consecutive words from that same traversal. The ordinary
two-argument `PPCHIRBuilder::Emit` symbol and all shared class layouts remain
unchanged. With the flag off, translation creates no capture or source text.
The focused overload test checks source-level addressability; a later native
link is needed to verify the emitted symbol, and no HIR byte-equivalence claim
is made without a separate comparison.
The captured function range is the scanner's actual inclusive address range;
`partial=1` means more than 64 words were present, so no claim is made about
branches beyond the captured prefix. A failed scan, emission, or carrier
validation does not yield guest-source evidence.

After successful HIR emission, the bounded tagged words occupy an otherwise
empty `FunctionDebugInfo::source_disasm` field. The object is created after
the HIR builder's debug-comment decision, avoiding an annotation change.
Existing all-function disassembly is refused for this selected capture; its
source field is never overwritten. The existing assembler ownership handoff
then places the function, establishes its host begin and actual emitted code
length, and invokes the source-map observer. That observer validates the
complete bounded tag and emits one `XGUESTWORDS001 source` line with the
current host range, module token, guest start/end, word count, and partial
status. Its `XDELAYMAP001 match` lines retain the 16 per-byte host-to-guest
source-map fields. When this flag is on, unrelated functions are filtered
before the existing scan budget, so they cannot consume it first.

The tagged record is limited to 64 words and 640 text bytes; no guest bytes
are reread by this diagnostic. The selected compiler path can still add
compile-time work and logging latency. The source-map byte selector recognizes
eight adjacent `F3 90` pairs, not a decoded guest instruction. A qualified
same-compilation association requires the same host begin/code length and a
`P` source-map field for the sampled byte; a missing, truncated, or malformed
record cannot support it. The PPC words can describe local branch targets and
CTR setup, but do not identify a dynamic caller, prove why the function is
reentered, establish exact executed PPC at a sampled host RIP, or explain
uneven displayed frames.
