<!-- Private fork only. SDK-derived ancestry must never be upstreamed. -->
# Undefined native-timer raw-register observation

Default-off `--native_timer_undefined_trace=true` observes only the Windows x64
backend's existing UndefinedCallExtern path. It installs no shims, implements
no timer or KDPC behavior and changes no export status. The original
ignore_undefined_externs fatal/log branch and return 0 remain verbatim after
the observer. Existing ordinary undefined-export messages remain unchanged;
only the additional diagnostic records are capped. No debug, first-chance,
context, source-annotation, timing or capture option is changed. The frozen
XPOLL001 collector and KeWait failure observer are untouched.

The three exact candidates are xboxkrnl.exe KeCancelTimer ordinal 0x54,
KeSetTimer 0xA6, and KeSetTimerEx 0xA7. Selection requires matching case-sensitive
name/ordinal, function kind, unimplemented metadata and no extern handler, then
pointer identity against the processor's registered xboxkrnl.exe export table.
That existing resolver lookup is read-only and has no mutex or allocation.
No generic undefined-export logging is added. There are 16 irreversible atomic
admissions for each export, at most 48 total, so one ordinal cannot consume
the others' evidence. Strong-CAS admission cannot overshoot under contention.
After an export is exhausted, no register/thread/QPC inspection or additional
diagnostic formatting/logging occurs for it. Calls already admitted can finish
later. Logger filtering/failure can yield fewer retained records than claims.

Each admitted warning records library/name/ordinal, import address, per-export
sequence/limit, saved guest LR, guest thread ID/read flag, native Windows TID,
raw QPC (zero means unavailable), and **all 64 bits of raw r3..r8**. The hook
reads only owned context and trusted host export metadata. It does not read a
guest stack, translate/dereference a timer or DPC VA, touch a pointed-to object,
read a due-time pointer, flush a log, change a context register or return value,
or change the existing ignore/fatal policy. Extra early logging can perturb
those instants and cannot support a performance comparison. Emission may be
out of order; sequence numbers identify each export's admission order.

The log explicitly says `abi_uninterpreted=true`. Do not claim DPC=r5, due=r4,
or even a null DPC from one raw zero without resolving Xbox aggregate ABI.
`shim_utils.h` loads scalar uint64_t arguments from one full GPR; that does not
prove the convention for a by-value LARGE_INTEGER union. The installed private
SDK public winnt.h:458-481 gives the union's big-endian HighPart/LowPart field
order, not this function's argument classification. No verified Xbox
KeSetTimer prototype/call-site ABI was found. The public
[WDM KeSetTimer declaration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kesettimer)
is a parameter-role reference, not proof of the Xbox register mapping. Preserve
the raw values and qualify any later interpretation independently.

Relevant local source: x64_emitter.cc::UndefinedCallExtern and CallExtern;
xex_module.cc::SetupImports attaches the resolver Export to GuestFunction;
ExportResolver::GetExportByOrdinal performs table identity lookup;
XThread creates ThreadState with its guest thread ID. PPCContext r3..r8 and LR
are copied exactly as saved at the existing external-call boundary, without
changing compiler or context settings. They are not a stack trace or a guest
call-site disassembly. Source basis is private 6ecd5f5, retaining all private
ancestry restrictions. No PR1295 changes are merged or cherry-picked.

Focused CPU Catch filter `[native_timer_trace]` contains **three cases**:
exact candidate selection/rejection, full-bit immutable raw register copying,
and concurrent independent per-export caps. These pure tests use fabricated
register arrays/budgets and do not invoke UndefinedCallExtern, guest code,
timers, logger, QPC, or native app behavior. Canonical library identity and
unchanged fallback are source-review obligations. Tests/build/formatter/runtime
remain UNRUN during source preparation.
