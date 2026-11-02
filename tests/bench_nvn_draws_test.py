#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""An unsupported, incomplete, or different draw workload cannot win a performance search."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('bench', Path(__file__).resolve().parents[1] / 'tools/bench_nvn_draws.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
VALID = 'NVN_DRAW_BENCH ordering=1 descriptors=1 record=0 draws=256 rounds=8 ns=4096000 passes=16 exact=1'


class Measurements(unittest.TestCase):
    def test_exact_workload(self):
        row = bench.parse_sample(VALID, (1, 1, 0))
        self.assertEqual(row['ns_per_draw'], 2000)
        self.assertEqual(row['passes_per_batch'], 2)

    def test_requested_strategy_must_run(self):
        with self.assertRaises(ValueError):
            bench.parse_sample(VALID, (1, 0, 0))

    def test_rendering_or_workload_failures(self):
        for before, after in [('exact=1', 'exact=0'), ('exact=1', 'exact=2'), ('ns=4096000', 'ns=0'), ('draws=256', 'draws=128'),
                              ('rounds=8', 'rounds=1'), ('passes=16', 'passes=0')]:
            with self.subTest(after=after), self.assertRaises(ValueError):
                bench.parse_sample(VALID.replace(before, after), (1, 1, 0))

    def test_missing_or_duplicate_result(self):
        for text in ['', VALID + '\n' + VALID]:
            with self.assertRaises(ValueError):
                bench.parse_sample(text, (1, 1, 0))

    def test_recorder_fallback(self):
        with self.assertRaises(ValueError):
            bench.parse_sample('could not start the command recording thread\n' + VALID, (1, 1, 0))


if __name__ == '__main__':
    unittest.main()
