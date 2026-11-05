#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check sampler recovery after collector interruption and immediate process exit."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import profile_cpu as sample


class Recovery(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'samples.bin'
        self.records = [sample.RECORD.pack(n, 0x1000 + n, 41, 1) for n in (1, 2)]

    def state(self, raw=b'', attempted=2, dropped=0, head=2, tail=0):
        self.path.write_bytes(raw)
        state = bytearray(256)
        sample.HEADER.pack_into(state, 0, sample.MAGIC, 1, 2, 192, 1, 0, 1, 4000000, 1000000, 42, 0, 64)
        struct.pack_into('<IIQQQ', state, 64, head, 2, attempted, dropped, 0)
        struct.pack_into('<I', state, 128, tail)
        struct.pack_into('<II', state, 200, 41, 1)
        state[208:256] = b''.join(self.records)
        self.path.with_suffix('.bin.state').write_bytes(state)

    def test_recover_unflushed_and_idempotent(self):
        self.state()
        self.assertTrue(sample.recover(self.path)['valid'])
        self.assertEqual(self.path.read_bytes(), b''.join(self.records))
        second = sample.recover(self.path)
        self.assertTrue(second['valid'])
        self.assertEqual(second['recovered_samples'], 0)

    def test_partial_write_and_unpublished_tail(self):
        self.state(self.records[0] + self.records[1][:11])
        stats = sample.recover(self.path)
        self.assertTrue(stats['valid'])
        self.assertEqual(stats['recovered_samples'], 1)
        self.assertEqual(self.path.read_bytes(), b''.join(self.records))

    def test_interrupted_signal(self):
        self.state(attempted=3)
        stats = sample.recover(self.path)
        self.assertTrue(stats['valid'])
        self.assertEqual(stats['exit_incomplete_samples'], 1)

    def test_overflow_is_not_a_valid_capture(self):
        self.state(attempted=3, dropped=1)
        self.assertFalse(sample.recover(self.path)['valid'])

    def test_impossible_accounting(self):
        self.state(head=3)
        with self.assertRaises(ValueError):
            sample.recover(self.path)

    def test_invalid_layout(self):
        self.state()
        state_path = self.path.with_suffix('.bin.state')
        state = bytearray(state_path.read_bytes())
        struct.pack_into('<I', state, 12, 3)
        state_path.write_bytes(state)
        with self.assertRaises(ValueError):
            sample.recover(self.path)

    def test_truncated_header(self):
        self.state()
        self.path.with_suffix('.bin.state').write_bytes(bytes(16))
        with self.assertRaises(ValueError):
            sample.recover(self.path)

    def test_out_of_order_collector_records(self):
        self.state(raw=b''.join(reversed(self.records)), tail=2)
        self.assertFalse(sample.recover(self.path)['valid'])


if __name__ == '__main__':
    unittest.main()
