# Guest exception metadata inventory

`cpu/exception_metadata` reads bounded runtime-function metadata and describes
caller-supplied frame PCs. The result owns copied ranges, import identities,
records and scope rows. It retains no reader, guest-memory pointer, CPU function
or thread. It does not unwind registers, invoke filters, dispatch an exception,
choose a continuation or change guest execution.

The pure parser accepts declared virtual section extents and an exact-read
callback. It rejects malformed supplied usable ranges before any reader call;
only explicitly unavailable declarations are retained without granting reads.
The XexModule adapter classifies declaration availability, checks the actual
allocation, sums bounded XEX page descriptors in 64 bits and checks current
committed/readable pages before copying. It consumes the callback synchronously
while holding the global region for heap access. The caller must retain module
ownership and exclude concurrent Load/Unload/setup at a stable boundary. The
region alone does not provide module lifetime retention or make running guest
self-modification atomic. No metadata cache or exception-route integration is
installed.

Every declared section is preserved. An extent outside the loaded allocation or
an inaccessible range is marked unavailable with a diagnostic. It grants no
reads and is never silently clipped. An unavailable `.pdata` fails inventory;
an unrelated unavailable section does not prevent reading valid `.pdata`.
This distinction is necessary for supplied images whose declared `.reloc`
extends past their loaded allocation. The adapter does not infer why that section
is unavailable or claim that the loader discarded it. Overlapping usable sections
are rejected.

Runtime-function records use the loaded `.pdata` virtual size, excluding file
padding. Each eight-byte record retains its big-endian start and raw second word.
All bounded records are inventoried; zero or non-executable starts are diagnosed
rather than treated as terminators. Invalid, unordered, duplicate or overlapping
records disable indexed lookup. The existing function-discovery scanner is
unchanged.

For the evidenced bit30-set packed form, the parser reports candidate instruction
length and prologue count and bounds them against an executable section. It does
not claim a complete console unwind format. Bit31 in this form can describe the
preceding handler/data pair. Bit30-clear helper forms remain opaque: some restore
helpers have bit31 set while their preceding bytes are instructions. No prefix
is read for those forms. Lookup never extends an encoded range into padding and
does not invent a range for an opaque helper.

An unknown executable guest handler and its data address stay opaque. A validated
ordinal `0x1A5` import from exactly `xboxkrnl` or `xboxkrnl.exe`, compared without
ASCII case sensitivity, identifies the evidenced C-specific scope shape.
`XexModule::SetupLibraryImports` stores the basename returned by
`utf8::find_base_name_from_guest_path`, so the loaded adapter supplies `xboxkrnl`.
The parser retains the supplied raw library name. This recognition describes the
loader-stored identity; the original path and extension discarded by that
producer are not reconstructed. Other supplied suffixes, paths and whitespace
are not accepted. Multiple identities for the same thunk must all
agree on this kernel identity and ordinal; a conflicting entry keeps the handler
opaque, while equivalent basename/full-name aliases are retained. The scope
shape is a big-endian count followed by sixteen-byte rows. Count, byte budget,
readability, range containment and ordinary executable pointer forms are checked.
Zero and small/sentinel role values remain raw and uninterpreted. Matching scope
ranges use an exclusive end. A matching row is a metadata observation, not a
filter invocation or a chosen exception continuation. Invalid handler tables do
not produce matching scopes.

`exception_metadata_test.cc` contains small reconstructed metadata fixtures,
including relocated packed records, the supplied World caller chain, six opaque
helper forms, malformed ranges, padding, resource limits, reader lifetime and a
real Memory allocation with protected/decommitted pages. It contains no game
binary or proprietary SDK code. Compiled verification and actual module adapter
integration remain separate validation steps.

`cpu/ppc/ppc_virtual_unwind` is a separate pure, offline partial-frame operation.
It accepts a valid packed metadata function and the complete, same-generation,
caller-owned code copy, then admits only two exact nonleaf prologue/epilogue
shapes. Both save LR at entry-SP minus 8 and allocate a constant aligned frame
with `stwu`. The second also saves full-width r31 at entry-SP minus 16 and uses
r31 as a frame base. The packed prologue count must equal the decoded shape.
Every instruction in the complete function is checked. The currently admitted
body subset is non-updating `addi`, `addis`, `lwz`, `stw`, the exact `mr` alias and
relative direct linked calls to outside the function. Stores need a statically
derived constant address, and a frame use declines if one overlaps its frame.
Unknown instructions, internal branches, indirect calls, helper-based
prologues or epilogues, dynamic stack changes and other frame shapes decline.
Admitted external direct callees are assumed to return normally and preserve
SP, nonvolatile r31 and this frame's save slots according to the normal calling
convention. Local code inspection does not prove arbitrary callees or exclude
an external alternate entrance to the function. The caller must establish that
control entered through the recognized prologue.

For an exact, aligned body PC before that instruction's effects, frame use needs
explicit valid SP low 32 bits, and the r31-base shape also needs valid r31 low
32 bits equal to SP. Full-width validity includes usable low 32 bits. It checks
the 32-bit big-endian backchain at SP against a wide checked SP plus allocation,
then reads the saved 32-bit LR at caller-SP minus 8. The r31-base shape reads a
full 64-bit big-endian saved r31 at caller-SP minus 16. Read count and bytes
have hard ceilings of four reads and 32 bytes; the supplied owned stack extent
can cover the larger 96/112-byte frames. Every slot must fit within that frame
and the supplied extent. The output contains only caller SP as a 32-bit address,
the exact saved return address, and, for the r31-base shape, full-width r31.
It does not carry any other register validity into another frame. All prologue
and epilogue PCs, including the first SP restore, decline. Failure publishes no
partially recovered values.

The caller must independently guarantee that metadata and code came from the
same retained module instance and code generation, and that registers and stack
are one coherent, precise capture of the same stable thread and memory epoch.
Unknown provenance declines. Bookkeeping tags alone cannot establish these
guarantees; there is currently no production adapter satisfying them. An owned
`HostFaultRecord` is insufficient. A missing `.pdata` entry does not imply a
leaf frame. The saved LR is returned exactly: no universal PC subtraction,
scope selection, handler or dispatch decision follows from it. Later phase-aware
work must use effects strictly before the exact PC, not a source-map or native
predecessor. The synthetic tests reconstruct bounded instruction words and
big-endian stacks; they provide no live game unwind evidence.

The admitted shapes were derived from bounded observed words: a three-word
prologue, 96-byte frame and four-word epilogue at `82311A68..82311AAC`, and a
five-word prologue, 112-byte frame and five-word epilogue at
`82313028..82313074`. Those addresses are examples for the fixtures, not
title-specific production rules. The source was a bounded in-memory decode of
`default.xex` from the supplied ISO for title ID `373307D9`, version `0.0.0.5`;
the complete function extents and words were checked against their packed
lengths. No XDK material was used. Helper-save forms and wider PPC virtual
unwind, optimized register reconstruction, console exception/context layouts,
language-handler arguments and search/unwind/return semantics remain separate
work.
