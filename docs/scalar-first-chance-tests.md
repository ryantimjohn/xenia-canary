# Scalar first-chance fixture boundaries

The Windows AMD64 CPU suite runs each intentional LWZ/STW zero-access case in
its own child process. A CPU-only private command-line branch runs before
`Catch::Session`; the child setup and callback contain no Catch assertions.
The CPU-only console startup hook selects a child-local log before logging is
initialized. Other test executables retain their existing startup path.

The child exclusively creates one 96-byte versioned record with a parent nonce,
read/write case, callback count, thread identity equality, guest PC, instruction,
guest address, operation and modeled r10/r0. It flushes and closes the record
inside the ordinary pre-access callback, then returns into the synthetic zero
access. There is no native exception handler or observer, host CONTEXT access,
exception recovery, protection change, instruction skip, or forced AV exit.

The parent starts the child suspended, assigns it to a nested kill-on-close Job
before resuming, and waits at most 15 seconds. Error paths terminate only the
retained child/Job and wait at most five seconds for cleanup. No handles are
inherited. Setup failure, timeout, nonempty Job, missing/truncated/extra record
bytes, repeated callback, wrong nonce/case/fields, normal return or any exit
other than `0xC0000005` fails. Each case has a fresh exclusively created temp
directory; both successful and failed evidence is retained, with no recursive
temporary-directory cleanup. The nonce combines a fixed parent PID prefix with
a strictly increasing nonzero 32-bit per-parent serial; serial wrap fails setup.
The directory name separately includes GetTickCount64 to avoid collisions with
retained evidence when Windows later reuses a parent PID.
The child suppresses only its own error
UI. The parent Catch runner and its exception handling remain intact.

This establishes the pre-access callback values plus child AV termination. It
does **not** identify the native fault RIP or address, inspect state at that
fault, prove tail suppression, or qualify native unwind metadata. The previous
in-process SEH fixture depended on native unwinding through a JIT thunk; this
fixture does not make that dependency a passing condition.

A separate ordinary direct-site call checks same-thread and different-thread
nested suppression, handler replacement, intentional live-context mutation and
whole PPCContext restoration on normal return. A subsequent independent call
checks that the TLS nesting guard reset. Existing host-exception restoration,
nonzero native access and CALL_EXTERN operand-reload controls remain separate.
These checks do not turn the death-test record into state-at-fault evidence.

Validation is recorded separately with exact source identities and build/test
receipts; this description does not itself assert a validation result.
