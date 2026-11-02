#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Reject misleading GPU tuning results before they can win the search."""
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import tune_gpu as tuner


def log(samples):
    return '\n'.join(f'[{t:.2f}] perf: 30.0 frames a second over 5.0 s ({n} shown in all)'
                     for t, n in samples)


class TunerTest(unittest.TestCase):
    def test_excludes_fast_boot_and_incomplete_warmup_interval(self):
        text = log([(5, 5000), (10, 10000), (15, 10150), (20, 10300), (25, 10450)])
        result = tuner.measure(text, 12, 1000 / 30)
        self.assertEqual(result['frames'], 300)
        self.assertAlmostEqual(result['fps'], 30)

    def test_stalled_or_truncated_runs_do_not_rank(self):
        for samples in ([(5, 0), (10, 0), (20, 0)], [(5, 1)], [(5, 5000), (10, 5000), (20, 5000)]):
            with self.subTest(samples=samples), self.assertRaises(ValueError):
                tuner.measure(log(samples), 0, 1000 / 30)

    def test_hitch_penalty_survives_same_average_rate(self):
        text = log([(5, 150), (10, 300), (15, 450)])
        baseline = tuner.measure(text, 0, 1000 / 30)
        hitched = tuner.measure(text + '\n[12.00] vulkan: a 200 ms frame:', 0, 1000 / 30)
        self.assertGreater(hitched['score'], baseline['score'])

    def test_frozen_intervals_are_not_removed_from_timing(self):
        result = tuner.measure(log([(5, 150), (10, 300), (15, 300), (20, 450)]), 0, 33)
        self.assertEqual(result['measured_s'], 15)
        self.assertEqual(result['fps'], 20)

    def test_reset_frame_counter_is_not_accepted(self):
        with self.assertRaises(ValueError):
            tuner.measure(log([(5, 150), (10, 300), (15, 10), (20, 160)]), 0, 33)

    def test_state_load_is_required_and_sets_time_origin(self):
        text = log([(5, 5000), (10, 10000), (15, 10150), (20, 10300), (25, 10450)])
        with self.assertRaises(ValueError):
            tuner.measure(text, 0, 33, state=True)
        text = '[10.00] state: loaded slot\n' + text
        result = tuner.measure(text, 2, 33, state=True)
        self.assertEqual(result['frames'], 300)

    def test_one_failed_confirmation_disqualifies_fast_candidate(self):
        self.assertTrue(math.isinf(tuner.aggregate([{'valid': True, 'score': 1}, {'valid': False}])))
        self.assertTrue(math.isinf(tuner.aggregate([])))

    def test_search_never_disables_graphics_work_or_quality(self):
        baseline = {key: values[0] for key, values in tuner.AXES.items()}
        for candidate in tuner.candidates(baseline):
            self.assertEqual(set(candidate), set(tuner.AXES))
        for setting in ('compute=0', 'async_pipelines=1', 'vk_gpu_profile=1'):
            with self.assertRaises(ValueError):
                tuner.settings_args([setting])

    def test_unavailable_or_equivalent_strategies_are_not_searched(self):
        text = ('guest buffers copied (vk_memory=auto); VK_EXT_external_memory_host no\n'
                'jit: 0 batches compiled 0 pcs')
        axes = tuner.active_axes(text, {'vk_memory': '0'})
        self.assertEqual(axes['vk_memory'], ['0'])
        self.assertNotIn('pipeline_threads', axes)
        self.assertNotIn('jit_threads', axes)

    def test_storage_ordering_only_competes_when_nvn_loader_is_active(self):
        baseline = {'vk_memory': '0'}
        self.assertNotIn('nvn_storage_order', tuner.active_axes('', baseline))
        for prefix in ('nvn: ', 'nvn: nvn_direct: '):
            active = tuner.active_axes(prefix + 'the runtime implements 1 NVN functions', baseline)
            self.assertEqual(set(active['nvn_storage_order']), {'0', '1', '2'})

    def test_timeout_stops_only_its_owned_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = tuner.run_command([sys.executable, '-c', 'import time; time.sleep(30)'], tmp,
                                       Path(tmp) / 'console', .05)
            self.assertEqual(result, 'timeout')

    def test_blank_and_resolution_changes_fail_image_gate(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow is required for image checks')
        with tempfile.TemporaryDirectory() as tmp:
            blank = Path(tmp) / 'blank.png'
            Image.new('RGB', (32, 32)).save(blank)
            with self.assertRaises(ValueError):
                tuner.image_check(blank)
            # A visible two-tone frame, at two different resolutions.
            image = Image.new('RGB', (32, 32), 'white')
            image.paste('black', (0, 0, 16, 32))
            ref, wrong = Path(tmp) / 'ref.png', Path(tmp) / 'wrong.png'
            image.save(ref)
            image.resize((64, 64)).save(wrong)
            with self.assertRaises(ValueError):
                tuner.image_check(wrong, ref)
            self.assertEqual(tuner.image_check(ref, ref), 0)


if __name__ == '__main__':
    unittest.main()
