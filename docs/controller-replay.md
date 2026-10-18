# Controller state replay

This opt-in virtual gamepad backend plays recorded host XInput states through
the normal InputSystem. No physical driver, virtual bus or administrator access
is used. Select it explicitly; `hid=any` never selects replay.

Convert an existing `xinput-state-jsonl-v1` capture offline with Python 3:

```
python tools/controller_replay.py input.jsonl output.xctrl --duration-seconds 785.266
```

Supply the actual full recording duration from its receipt. A state-change log's
last event is not its end time. The converter never overwrites output. Do not
check private recordings into Git. It seeds four preflight states at time zero,
then uses `(perf_counter_ns - header.perf_counter_ns_anchor) // 1000`; wall UTC
and approximate floating elapsed timestamps do not control playback. It rejects
unsupported statuses, invalid ranges, reordered timestamps and partial input.

Launch with `--hid=replay --hid_replay=<absolute output.xctrl path>`.
An invalid recording logs an error and enables no input driver; it never falls
back to physical input. All four recorded ports are preserved; unconnected or
unrecorded ports return device-not-connected. Ordinary InputSystem deadzone and
connection notification handling remains in effect. The driver exposes generic
gamepad capabilities and acknowledges/discards vibration without hardware output.
Keystrokes were not captured: GetKeystroke returns EMPTY for connected ports.
This cannot reproduce a title that requires recorded GetKeystroke events.

The monotonic wall clock starts on the first valid-port GetState call. Capability
discovery, vibration and keystroke queries do not start it. State remains held
until the next recorded timestamp. Repeated polling does not advance the event
cursor past wall time; polls can naturally miss a complete short tap. Wall time
continues during guest/debugger pauses, like live physical input. At explicit
duration, connected ports become neutral once, with packet number incremented
modulo 2^32; disconnected ports stay disconnected. No automatic loop, pause/step
control, button stretching or guest-specific timing is provided. Setup resets the
timeline for a new session. NUI replay has an independent clock/restart policy;
startup and depth alignment are approximate, not deterministic navigation.

## Binary format v1

All fields are little-endian, packed without padding. Header (24 bytes): magic
`XECTRL1\0` (8), duration in microseconds (u64), event count (u32), reserved zero
(u32). Events (32 bytes): time_us (u64), port (u32), status (u32: 0 or 1167),
packet (u32), buttons (u16), left/right triggers (u8 each), left_x/left_y/right_x/
right_y (i16 each). Disconnected state fields are zero. Events are nondecreasing,
the first is at zero, and all precede duration. Equal timestamps retain file
order; the last event at a timestamp wins for that port. Maximum one million
events and 24 hours. Trailing bytes, partial records and unknown versions fail.

Focused validation target: `xenia-hid-replay-tests`, Catch filter
`[hid][replay]`. Converter tests use Python unittest and synthetic captures only:
`python -m unittest discover -s tools/testing -p test_controller_replay.py`.
