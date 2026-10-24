"""Synthetic converter contracts; never reads private recordings."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'controller_replay', Path(__file__).resolve().parents[1] / 'controller_replay.py')
REPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPLAY)


def seed(port):
    return dict(type='preflight_state', port=port, status=1167,
                elapsed_seconds=None)


def state():
    return dict(type='state_change', port=0, status=0, packet=7, buttons=4096,
                left_trigger=255, right_trigger=0, left_x=-32768, left_y=32767,
                right_x=-1, right_y=1, perf_counter_ns=11000)


def records():
    return [dict(type='header', format='xinput-state-jsonl-v1',
                 api='XInputGetState', perf_counter_ns_anchor=1000),
            *(seed(i) for i in range(4)), state()]


def encode(rows):
    return '\n'.join(json.dumps(row) for row in rows)


class ConverterTests(unittest.TestCase):
    def test_qpc_anchor_and_extrema(self):
        data = REPLAY.convert(encode(records()), 100)
        self.assertEqual(REPLAY.HEADER.unpack_from(data), (b'XECTRL1\0', 100, 5, 0))
        event = REPLAY.EVENT.unpack_from(data, 24 + 4 * 32)
        self.assertEqual(event, (10, 0, 0, 7, 4096, 255, 0, -32768, 32767, -1, 1))

    def test_preflight_ignores_invalid_timestamps(self):
        rows = records()
        rows[1] = dict(state(), type='preflight_state',
                       perf_counter_ns=-100, elapsed_seconds=None)
        data = REPLAY.convert(encode(rows), 100)
        self.assertEqual(REPLAY.EVENT.unpack_from(data, 24)[0], 0)

    def test_reject_bad_input_without_sorting(self):
        for change in ('status', 'range', 'decreasing', 'missing', 'duplicate',
                       'version', 'late', 'duration', 'boolean', 'before_anchor'):
            with self.subTest(change=change):
                rows = records()
                duration = 100
                if change == 'status': rows[-1]['status'] = 5
                if change == 'range': rows[-1]['left_x'] = -32769
                if change == 'decreasing': rows += [dict(state(), perf_counter_ns=10999)]
                if change == 'missing': del rows[2]
                if change == 'duplicate': rows.insert(2, seed(0))
                if change == 'version': rows[0]['format'] = 'unknown'
                if change == 'late': rows += [seed(0)]
                if change == 'duration': duration = 10
                if change == 'boolean': rows[-1]['buttons'] = True
                if change == 'before_anchor': rows[-1]['perf_counter_ns'] = 0
                with self.assertRaises(ValueError):
                    REPLAY.convert(encode(rows), duration)

    def test_truncated_and_oversized(self):
        with self.assertRaises(ValueError):
            REPLAY.convert(encode(records())[:-1], 100)
        with self.assertRaises(ValueError):
            REPLAY.convert(' ' * (REPLAY.MAX_JSON_BYTES + 1), 100)

    def test_duplicate_names_and_nonstandard_constants(self):
        rows = records()
        for name in ('status', 'port', 'buttons', 'perf_counter_ns'):
            with self.subTest(duplicate=name):
                line = json.dumps(rows[-1])
                field = json.dumps(name) + ': ' + json.dumps(rows[-1][name])
                line = line.replace(field, field + ', ' + field, 1)
                text = encode(rows[:-1]) + '\n' + line
                with self.assertRaisesRegex(ValueError, 'duplicate JSON name'):
                    REPLAY.convert(text, 100)
        header = json.dumps(rows[0])
        field = '"perf_counter_ns_anchor": 1000'
        text = header.replace(field, field + ', ' + field) + '\n' + encode(rows[1:])
        with self.assertRaisesRegex(ValueError, 'duplicate JSON name'):
            REPLAY.convert(text, 100)
        for constant in ('NaN', 'Infinity', '-Infinity'):
            with self.subTest(constant=constant):
                # Even otherwise-unused metadata must conform to standard JSON.
                text = encode(rows).replace('"elapsed_seconds": null',
                                            '"elapsed_seconds": ' + constant, 1)
                with self.assertRaisesRegex(ValueError, 'nonstandard JSON constant'):
                    REPLAY.convert(text, 100)

    def test_cli_preserves_existing_output(self):
        with tempfile.TemporaryDirectory(prefix='xenia-controller-replay-') as directory:
            source = Path(directory) / 'synthetic.jsonl'
            output = Path(directory) / 'existing.xctrl'
            source.write_text(encode(records()), encoding='utf-8')
            original = b'existing output must remain unchanged\x00\xff'
            output.write_bytes(original)
            result = subprocess.run(
                [sys.executable, str(Path(REPLAY.__file__)), str(source), str(output),
                 '--duration-seconds', '0.000100'],
                capture_output=True, timeout=5, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
