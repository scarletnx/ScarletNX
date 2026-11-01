#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Do not rank a fallback backend or invalid descriptor timing as a faster path."""
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bench_gpu_descriptors import measurement


class DescriptorMeasurementTest(unittest.TestCase):
    def sample(self, **changes):
        row = dict(sets=4096, descriptors=8, elapsed_ns=1000000, images=0, vk_descriptors=1, vk_record=1)
        row.update(changes)
        return 'startup output\nDESCRIPTOR_BENCH ' + json.dumps(row) + '\n'

    def test_valid_measurement(self):
        self.assertEqual(measurement(self.sample(), 4096, 8, 1, 1)['elapsed_ns'], 1000000)

    def test_backend_fallback_is_not_a_valid_heap_result(self):
        with self.assertRaises(ValueError):
            measurement(self.sample(vk_descriptors=0), 4096, 8, 1, 1)

    def test_wrong_workload_or_recording_mode_is_rejected(self):
        for changes in ({'sets': 1}, {'descriptors': 32}, {'images': 8}, {'vk_record': 0}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                measurement(self.sample(**changes), 4096, 8, 1, 1)

    def test_nonfinite_or_nonpositive_timing_is_rejected(self):
        for value in (0, -1, float('nan'), float('inf'), '100', True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                measurement(self.sample(elapsed_ns=value), 4096, 8, 1, 1)

    def test_missing_or_duplicate_measurement_is_rejected(self):
        for text in ('startup only', self.sample() + self.sample()):
            with self.assertRaises(ValueError):
                measurement(text, 4096, 8, 1, 1)


if __name__ == '__main__':
    unittest.main()
