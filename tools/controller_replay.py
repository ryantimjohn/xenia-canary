#!/usr/bin/env python3
"""Convert xinput-state-jsonl-v1 to XECTRL1. Python standard library only."""
import argparse
from decimal import Decimal, InvalidOperation
import json
from pathlib import Path
import struct

MAX_JSON_BYTES = 32 * 1024 * 1024
MAX_EVENTS = 1_000_000
HEADER = struct.Struct('<8sQII')
EVENT = struct.Struct('<QIIIHBBhhhh')
FIELDS = (('packet', 0, 0xffffffff), ('buttons', 0, 65535),
          ('left_trigger', 0, 255), ('right_trigger', 0, 255),
          ('left_x', -32768, 32767), ('left_y', -32768, 32767),
          ('right_x', -32768, 32767), ('right_y', -32768, 32767))


def strict_json(text):
    def unique_object(pairs):
        result = {}
        for name, value in pairs:
            if name in result:
                raise ValueError(f'duplicate JSON name: {name}')
            result[name] = value
        return result

    def reject_constant(value):
        raise ValueError(f'nonstandard JSON constant: {value}')

    return json.loads(text, object_pairs_hook=unique_object,
                      parse_constant=reject_constant)


def integer(obj, name, low, high):
    value = obj.get(name)
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'invalid {name}')
    return value


def convert(text, duration_us):
    """Returns bytes without sorting, adjusting holds, or guessing end time."""
    if type(duration_us) is not int or not 0 < duration_us <= 86_400_000_000:
        raise ValueError('duration must be positive and at most 24 hours')
    if len(text.encode('utf-8')) > MAX_JSON_BYTES:
        raise ValueError('input too large')
    lines = text.splitlines()
    if not lines:
        raise ValueError('empty input')
    header = strict_json(lines[0])
    if (not isinstance(header, dict) or header.get('type') != 'header' or
            header.get('format') != 'xinput-state-jsonl-v1' or
            header.get('api') != 'XInputGetState'):
        raise ValueError('invalid header/version')
    anchor = integer(header, 'perf_counter_ns_anchor', 0, 2**64 - 1)
    events = []
    seeded = set()
    changes_started = False
    previous_ns = 0
    for line in lines[1:]:
        obj = strict_json(line)  # Blank/truncated lines are errors.
        if not isinstance(obj, dict):
            raise ValueError('record must be an object')
        port = integer(obj, 'port', 0, 3)
        status = integer(obj, 'status', 0, 0xffffffff)
        if status not in (0, 1167):
            raise ValueError('unsupported status')
        if obj.get('type') == 'preflight_state':
            if changes_started or port in seeded:
                raise ValueError('late/duplicate preflight')
            seeded.add(port)
            time_us = 0  # Preflight elapsed timestamps intentionally invalid.
        elif obj.get('type') == 'state_change':
            if seeded != {0, 1, 2, 3}:
                raise ValueError('missing four-port preflight')
            changes_started = True
            ns = integer(obj, 'perf_counter_ns', anchor, 2**64 - 1) - anchor
            if ns < previous_ns:
                raise ValueError('decreasing timestamp')
            previous_ns = ns
            time_us = ns // 1000
        else:
            raise ValueError('unknown record type')
        if time_us >= duration_us:
            raise ValueError('event outside explicit duration')
        if status == 0:
            values = [integer(obj, name, low, high) for name, low, high in FIELDS]
        else:
            for name, _, _ in FIELDS:
                if name in obj and (type(obj[name]) is not int or obj[name] != 0):
                    raise ValueError('disconnected state must be absent or zero')
            values = [0] * len(FIELDS)
        events.append(EVENT.pack(time_us, port, status, *values))
        if len(events) > MAX_EVENTS:
            raise ValueError('too many events')
    if seeded != {0, 1, 2, 3}:
        raise ValueError('missing four-port preflight')
    return HEADER.pack(b'XECTRL1\0', duration_us, len(events), 0) + b''.join(events)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--duration-seconds', required=True,
                        help='Explicit full recording duration, not last state change')
    args = parser.parse_args()
    try:
        duration = Decimal(args.duration_seconds) * 1_000_000
        if not duration.is_finite() or duration != duration.to_integral_value():
            raise ValueError('duration must resolve to whole microseconds')
        if args.input.stat().st_size > MAX_JSON_BYTES:
            raise ValueError('input too large')
        payload = convert(args.input.read_text(encoding='utf-8'), int(duration))
        with args.output.open('xb') as output:  # Never overwrite evidence.
            output.write(payload)
    except (ValueError, InvalidOperation, OSError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
