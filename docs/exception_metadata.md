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

Virtual unwind, optimized register reconstruction, console exception/context
layouts, language-handler arguments and search/unwind/return semantics require
later independent work. This inventory is the bounded foundation for that work.
