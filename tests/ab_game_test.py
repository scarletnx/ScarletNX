#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The A/B comparison refuses a result that its own rounds do not support."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import ab_game


def log(samples):
    return '\n'.join(f'[{t:.2f}] perf: 30.0 frames a second over 5.0 s ({n} shown in all)' for t, n in samples)


def round_of(ms_a, ms_b):
    return {'a': {'mean_ms': ms_a}, 'b': {'mean_ms': ms_b}}


class MeasureTest(unittest.TestCase):
    def test_steady_window_uses_the_rate_lines(self):
        text = log([(5, 150), (10, 300), (15, 450)])
        result = ab_game.measure(text, 0)
        self.assertEqual(result['frames'], 300)
        self.assertEqual(result['measured_s'], 10)
        self.assertAlmostEqual(result['fps'], 30)
        self.assertAlmostEqual(result['mean_ms'], 1000 / 30)

    def test_warmup_intervals_are_not_timed(self):
        text = log([(5, 5000), (10, 10000), (15, 10150), (20, 10300), (25, 10450)])
        result = ab_game.measure(text, 12)
        self.assertEqual(result['frames'], 300)
        self.assertEqual(result['measured_s'], 10)

    def test_reset_frozen_and_short_windows_are_rejected(self):
        for samples in ([(5, 150), (10, 300), (15, 10), (20, 160)],
                        [(5, 5000), (10, 5000), (20, 5000)],
                        [(5, 1)]):
            with self.subTest(samples=samples), self.assertRaises(ValueError):
                ab_game.measure(log(samples), 0)

    def test_state_load_sets_the_time_origin(self):
        text = log([(5, 5000), (10, 10000), (15, 10150), (20, 10300), (25, 10450)])
        with self.assertRaises(ValueError):
            ab_game.measure(text, 0, from_state=True)
        result = ab_game.measure('[10.00] state: loaded slot\n' + text, 2, from_state=True)
        self.assertEqual(result['frames'], 300)

    def test_hitches_are_counted_apart_from_the_average(self):
        text = log([(5, 150), (10, 300), (15, 450)]) + '\n[12.00] vulkan: a 200 ms frame:'
        result = ab_game.measure(text, 0, pace=33)
        self.assertGreater(result['hitch_ms_per_s'], 0)
        self.assertEqual(result['slow_frames'], 1)


class CompareTest(unittest.TestCase):
    def test_four_rounds_of_b_faster_get_a_verdict(self):
        result = ab_game.compare([round_of(41.7, 40.0)] * 4, 3)
        self.assertTrue(result['verdict'])
        self.assertEqual(result['faster'], 'b')
        self.assertAlmostEqual(result['speedup_percent'], 4.25, places=2)
        self.assertEqual(result['confirmed_rounds'], 4)
        self.assertAlmostEqual(result['sign_p'], 0.0625)

    def test_a_faster_is_named_too(self):
        result = ab_game.compare([round_of(40.0, 42.0)] * 4, 3)
        self.assertTrue(result['verdict'])
        self.assertEqual(result['faster'], 'a')

    def test_three_rounds_cannot_clear_the_sign_test(self):
        result = ab_game.compare([round_of(42.0, 40.0)] * 3, 3)
        self.assertFalse(result['verdict'])
        self.assertIn('sign test', result['reason'])

    def test_every_round_must_clear_the_gate(self):
        result = ab_game.compare([round_of(42.0, 40.0), round_of(42.0, 40.0),
                                  round_of(42.0, 40.0), round_of(40.6, 40.0)], 3)
        self.assertFalse(result['verdict'])
        self.assertIn('under the', result['reason'])

    def test_one_round_the_other_way_is_not_a_win(self):
        result = ab_game.compare([round_of(42.0, 40.0)] * 3 + [round_of(40.0, 42.0)], 3)
        self.assertFalse(result['verdict'])
        self.assertEqual(result['confirmed_rounds'], 3)
        self.assertIn('agree', result['reason'])

    def test_a_failed_round_stops_the_verdict(self):
        result = ab_game.compare([round_of(42.0, 40.0)] * 3 + [{'a': {'mean_ms': 42}, 'b': None}], 3)
        self.assertFalse(result['verdict'])
        self.assertEqual(result['valid_rounds'], 3)
        self.assertIn('failed arm', result['reason'])

    def test_no_round_has_no_winner(self):
        result = ab_game.compare([{'a': None, 'b': None}], 3)
        self.assertFalse(result['verdict'])
        self.assertIn('no round', result['reason'])


class SettingsTest(unittest.TestCase):
    def test_key_value_pairs_are_checked(self):
        self.assertEqual(ab_game.settings_args(['vk_record=0', 'vk_memory=2']),
                         {'vk_record': '0', 'vk_memory': '2'})
        for bad in ('vk_record', '=1', 'Vk_record=1', 'vk_record=', 'vk-record=1'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                ab_game.settings_args([bad])

    def test_the_measurement_protocol_is_fixed(self):
        ab_game.check_fixed({'vk_record': '0'})
        for key, value in ab_game.FIXED.items():
            ab_game.check_fixed({key: value})
            with self.subTest(key=key), self.assertRaises(ValueError):
                ab_game.check_fixed({key: 'the other way'})

    def test_the_plan_runs_both_arms_with_the_protocol(self):
        class Args:
            a = {'vk_record': '1'}
            b = {'vk_record': '0'}
            common = dict(ab_game.FIXED)
            script, state = '15000:1', None
            a_exe = Path('/games/one')
            b_exe = Path('/games/two')
            seconds, warmup, rounds = 60, 25, 4
            min_improvement, image_limit = 3.0, 0.05
            headless = True

        current = ab_game.plan(Args)
        self.assertEqual(current['a']['vk_record'], '1')
        self.assertEqual(current['b']['vk_record'], '0')
        self.assertEqual(current['a']['fps_limit'], '0')
        self.assertEqual(current['scene'], ['--script=15000:1'])
        self.assertNotIn('_warmup', current['a'])
        self.assertTrue(current['headless'])


if __name__ == '__main__':
    unittest.main()
