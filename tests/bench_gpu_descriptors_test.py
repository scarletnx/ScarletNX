#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Do not rank a fallback backend or invalid descriptor timing as a faster path."""
import json
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bench_gpu_descriptors as descriptor_bench
from bench_gpu_descriptors import COUNTERS, measurement, pattern_measurements, renderer_gate, rankings


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


class DescriptorPatternTest(unittest.TestCase):
    def row(self, pattern='repeated', backend=2, images=0, indexed=0, **changes):
        elements = images + (512 if indexed else 0)
        row = dict.fromkeys(COUNTERS, 0)
        row.update(pattern=pattern, sets=4096, descriptors=8, images=images, indexed=indexed,
                   elapsed_ns=1000000, vk_descriptors=int(backend != 0), vk_descriptor_heaps=int(backend == 2),
                   backend=backend, vk_record=1, fallbacks=0, counters_available=int(backend != 0))
        if backend:
            row.update(buffer_requests=32768, buffer_driver_calls=8, buffer_hits=32760,
                       image_requests=4096 * elements, image_driver_calls=int(bool(elements)),
                       image_hits=max(0, 4096 * elements - 1), arena_allocated_bytes=4 << 20,
                       cache_allocated_bytes=4096, peak_used_bytes=1024, workload_buffer_bytes=8192)
        if backend == 2:
            lookups = 4096 * (images + (2 if indexed == 1 else 512 if indexed else 0))
            replicas = 4096 * elements - lookups
            row.update(image_hits=max(0, lookups - 1), image_replicas=replicas, heap_image_lookups=lookups)
            row.update(heap_sampler_requests=4096 * elements, heap_sampler_driver_calls=int(bool(elements)),
                       heap_sampler_lookups=lookups, heap_sampler_replicas=replicas,
                       heap_sampler_hits=max(0, lookups - 1), heap_table_bytes=144 * 4096,
                       heap_resource_range_bytes=2 << 20, heap_sampler_range_bytes=131072, heap_reserved_bytes=97280)
        row.update(changes)
        return row

    def text(self, rows):
        return ''.join('DESCRIPTOR_PATTERN_BENCH ' + json.dumps(row) + '\n' for row in rows)

    def pair(self, **changes):
        return [self.row(pattern, **changes) for pattern in ('repeated', 'unique')]

    def test_all_backends_accept_complete_actual_counters(self):
        for backend in (0, 1, 2):
            with self.subTest(backend=backend):
                rows = pattern_measurements(self.text(self.pair(backend=backend)), 4096, 8, backend, 1)
                self.assertEqual([row['pattern'] for row in rows], ['repeated', 'unique'])

    def test_heap_output_can_also_advertise_buffer_fallback(self):
        self.assertEqual(pattern_measurements(self.text(self.pair()), 4096, 8, 2, 1)[0]['backend'], 2)
        old = DescriptorMeasurementTest().sample(vk_descriptor_heaps=1)
        self.assertEqual(measurement(old, 4096, 8, 2, 1)['vk_descriptor_heaps'], 1)

    def test_missing_duplicate_or_unknown_patterns_are_rejected(self):
        for rows in ([self.row()], self.pair() + [self.row()], [self.row(), self.row()],
                     [self.row(), self.row('other')]):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                pattern_measurements(self.text(rows), 4096, 8, 2, 1)

    def test_backend_fallback_recording_and_workload_mismatch_are_rejected(self):
        for changes in ({'vk_descriptor_heaps': 0}, {'backend': 1}, {'backend': True}, {'vk_record': 0},
                        {'fallbacks': 1}, {'counters_available': 0}, {'images': 8}, {'indexed': 1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                pattern_measurements(self.text(self.pair(**changes)), 4096, 8, 2, 1)

    def test_counter_types_counts_and_memory_are_checked(self):
        for changes in ({'buffer_hits': -1}, {'arena_bytes': True}, {'heap_table_bytes': 1.5},
                        {'buffer_requests': 32767}, {'buffer_driver_calls': 32769}, {'image_requests': 1},
                        {'buffer_hits': 32759}, {'image_hits': 1}, {'heap_image_lookups': 1},
                        {'heap_sampler_lookups': True}, {'heap_sampler_replicas': 1},
                        {'heap_sampler_requests': 1}, {'arena_allocated_bytes': 1}, {'cache_allocated_bytes': 0},
                        {'heap_sampler_range_bytes': 0}, {'heap_reserved_bytes': 1 << 30}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                pattern_measurements(self.text(self.pair(**changes)), 4096, 8, 2, 1)

    def test_indexed_tail_requests_include_both_complete_arrays(self):
        rows = pattern_measurements(self.text(self.pair(images=8, indexed=16)), 4096, 8, 2, 1, 8, 16)
        self.assertEqual(rows[0]['image_requests'], 4096 * 520)
        with self.assertRaises(ValueError):
            pattern_measurements(self.text(self.pair(images=8, indexed=16, image_requests=4096 * 24)),
                                 4096, 8, 2, 1, 8, 16)

    def test_identical_indexed_runs_keep_all_requests_but_resolve_once_per_run(self):
        rows = pattern_measurements(self.text(self.pair(indexed=1)), 4096, 8, 2, 1, 0, 1)
        self.assertEqual(rows[0]['image_requests'], 4096 * 512)
        self.assertEqual(rows[0]['heap_sampler_requests'], 4096 * 512)
        self.assertEqual(rows[0]['heap_image_lookups'], 4096 * 2)
        self.assertEqual(rows[0]['heap_sampler_lookups'], 4096 * 2)
        self.assertEqual(rows[0]['image_replicas'], 4096 * 510)
        self.assertEqual(rows[0]['heap_sampler_replicas'], 4096 * 510)
        for changes in ({'image_replicas': 4096 * 510 - 1}, {'heap_image_lookups': 4096 * 2 + 1},
                        {'heap_sampler_lookups': 4096 * 2 - 1}, {'heap_sampler_replicas': 4096 * 510 - 1},
                        {'heap_sampler_driver_calls': 4096 * 2 + 1}, {'heap_sampler_hits': 4096 * 2 + 1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                pattern_measurements(self.text(self.pair(indexed=1, **changes)), 4096, 8, 2, 1, 0, 1)

    def test_heap_buffer_api_calls_count_bounded_batches_without_relabeling_elements(self):
        for calls in (1, 2, 8):
            rows = pattern_measurements(self.text(self.pair(heap_buffer_native_calls=calls)), 4096, 8, 2, 1)
            self.assertEqual(rows[0]['buffer_driver_calls'], 8)
            self.assertEqual(rows[0]['heap_buffer_native_calls'], calls)
        for calls in (0, -1, 9, True, 1.5):
            with self.subTest(calls=calls), self.assertRaises(ValueError):
                pattern_measurements(self.text(self.pair(heap_buffer_native_calls=calls)), 4096, 8, 2, 1)
        with self.assertRaises(ValueError):
            pattern_measurements(self.text(self.pair(buffer_driver_calls=9, buffer_hits=32759,
                                                      heap_buffer_native_calls=1)), 4096, 8, 2, 1)
        with self.assertRaises(ValueError):
            pattern_measurements(self.text(self.pair(backend=1, heap_buffer_native_calls=1)), 4096, 8, 1, 1)
        old = pattern_measurements(self.text(self.pair()), 4096, 8, 2, 1)
        self.assertNotIn('heap_buffer_native_calls', old[0])

    def test_sampler_driver_calls_and_byte_interning_hits_can_overlap(self):
        rows = self.pair(images=8, heap_sampler_driver_calls=3, heap_sampler_hits=4096 * 8)
        measured = pattern_measurements(self.text(rows), 4096, 8, 2, 1, 8)
        self.assertGreater(measured[0]['heap_sampler_driver_calls'] + measured[0]['heap_sampler_hits'],
                           measured[0]['heap_sampler_lookups'])

    def test_renderer_gate_requires_pass_and_actual_backend(self):
        text = 'vulkan: graphics descriptors use heaps with buffer fallback\nPASS headless Vulkan: all fixtures\n'
        self.assertTrue(renderer_gate(text, 2)['renderer_exact'])
        for invalid in (text.replace('heaps', 'buffers'), text.replace('PASS headless Vulkan:', 'SKIP'),
                        text + 'vulkan validation: bad descriptor\n', text + 'graphics descriptors use heaps\n'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                renderer_gate(invalid, 2)

    def test_paired_rankings_keep_unique_and_repeated_separate(self):
        rows = []
        for repeat in range(3):
            for pattern in ('repeated', 'unique'):
                for backend in (0, 2):
                    elapsed = 1000 * (repeat + 1)
                    if backend == 2:
                        elapsed *= 0.5 if pattern == 'repeated' else 2
                    rows.append(self.row(pattern, backend, elapsed_ns=elapsed, round=repeat))
        result = rankings(rows, [(0, 1), (2, 1)], 3)
        by_pattern = {item['workload']['pattern']: item for item in result}
        self.assertEqual(by_pattern['repeated']['ranking'][0]['backend'], 2)
        self.assertEqual(by_pattern['unique']['ranking'][0]['backend'], 0)
        self.assertEqual(by_pattern['repeated']['ranking'][0]['paired_speedup_median'], 2)
        self.assertEqual(by_pattern['unique']['ranking'][1]['paired_speedup_median'], 0.5)
        self.assertEqual(by_pattern['repeated']['ranking'][0]['memory_bytes']['arena_allocated_bytes'], 4 << 20)
        missing = [r for r in rows if not (r['backend'] == 2 and r['round'] == 2 and r['pattern'] == 'unique')]
        incomplete = {item['workload']['pattern']: item for item in rankings(missing, [(0, 1), (2, 1)], 3)}
        self.assertEqual(len(incomplete['unique']['ranking']), 1)


class DescriptorExecutableTest(unittest.TestCase):
    def rewrite(self, path):
        before = path.stat()
        path.write_bytes(b'changed binary!')
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        after = path.stat()
        self.assertEqual((before.st_ino, before.st_size, before.st_mtime_ns),
                         (after.st_ino, after.st_size, after.st_mtime_ns))

    def test_content_guard_rejects_in_place_rewrite_with_unchanged_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'benchmark'
            binary.write_bytes(b'initial binary!')
            signature = descriptor_bench.binary_identity(binary)
            descriptor_bench.unchanged(binary, signature)
            self.rewrite(binary)
            with self.assertRaises(descriptor_bench.BinaryChangedError):
                descriptor_bench.unchanged(binary, signature)

    def test_process_guard_checks_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'benchmark'
            binary.write_bytes(b'initial binary!')
            signature = descriptor_bench.binary_identity(binary)
            self.rewrite(binary)
            with mock.patch.object(descriptor_bench, 'execute') as run:
                with self.assertRaises(descriptor_bench.BinaryChangedError):
                    descriptor_bench.execute_checked([str(binary)], Path(directory) / 'run.log', signature)
                run.assert_not_called()

    def test_process_guard_checks_after_pass_skip_and_failure(self):
        for result in ('PASS', None, RuntimeError('process failed')):
            with self.subTest(result=result), tempfile.TemporaryDirectory() as directory:
                binary = Path(directory) / 'benchmark'
                binary.write_bytes(b'initial binary!')
                signature = descriptor_bench.binary_identity(binary)

                def run(*unused):
                    self.rewrite(binary)
                    if isinstance(result, Exception):
                        raise result
                    return result

                with mock.patch.object(descriptor_bench, 'execute', side_effect=run):
                    with self.assertRaises(descriptor_bench.BinaryChangedError):
                        descriptor_bench.execute_checked([str(binary)], Path(directory) / 'run.log', signature)

    def test_final_measurement_rewrite_cannot_produce_a_ranking(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'benchmark'
            binary.write_bytes(b'initial binary!')
            output = Path(directory) / 'out'
            measurements = 0

            def run(command, *unused):
                nonlocal measurements
                startup = ('vulkan: Test GPU, Vulkan 1.4.351, driver 99d38200, graphics queue family 0, '
                           'compute 2 (separate family)\nvulkan: graphics descriptors use heaps\n')
                if any(arg.startswith('--bench-descriptors=') for arg in command):
                    measurements += 1
                    if measurements == 6:  # three rounds, two recording modes: the last process
                        self.rewrite(binary)
                    recording = int(next(arg.split('=')[1] for arg in command if arg.startswith('--vk_record=')))
                    rows = DescriptorPatternTest().pair(vk_record=recording)
                    return startup + DescriptorPatternTest().text(rows)
                return startup + 'PASS headless Vulkan: all fixtures\n'

            argv = ['bench_gpu_descriptors', '--bench', str(binary), '--out', str(output), '--rounds', '3',
                    '--sets', '4096', '--counts', '8', '--images', '0', '--indexed', '0', '--backends', '2']
            with mock.patch.object(sys, 'argv', argv), mock.patch.object(descriptor_bench, 'execute', side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(descriptor_bench.BinaryChangedError):
                    descriptor_bench.main()
            self.assertEqual(measurements, 6)
            self.assertFalse((output / 'winner.json').exists())

    def test_measured_device_must_match_the_renderer_gate(self):
        startup = ('vulkan: GPU A, Vulkan 1.4.351, driver 99d38200, graphics queue family 0, '
                   'compute 2 (separate family)\n')
        self.assertEqual(descriptor_bench.check_device(startup, 'GPU A'), 'GPU A')
        # The fixture's final line does not occur in timed runs and must not replace the primary device line.
        self.assertEqual(descriptor_bench.check_device(startup + 'Headless Vulkan device: GPU A\n', 'GPU A'), 'GPU A')
        self.assertIsNone(descriptor_bench.check_device('legacy log without a device\n', None))
        for text in (startup.replace('GPU A', 'GPU B'), 'vulkan: GPU A\n', startup + startup,
                     'Headless Vulkan device: GPU A\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                descriptor_bench.check_device(text, 'GPU A')


if __name__ == '__main__':
    unittest.main()
