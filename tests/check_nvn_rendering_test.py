#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A skipped, incomplete or changed rendering run must never claim complete coverage."""

from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_nvn_rendering as harness


def gpu_output():
    return '\n'.join([harness.GPU_MARKER] + [passed + ' exact=1' for _, passed, _ in harness.GPU_FEATURES[1:]])


class SourceSnapshot(unittest.TestCase):
    def test_gpu_files_have_relative_paths_and_content_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gpu = root / 'src/nx/gpu'
            (gpu / 'vk').mkdir(parents=True)
            (gpu / 'vk/render.c').write_bytes(b'gpu source\n')
            (gpu / 'draw_state.h').write_bytes(b'draw state\n')
            (root / 'unrelated.txt').write_bytes(b'outside the source snapshot\n')
            before = harness.source_snapshot(root)
            self.assertEqual(before, [
                {'path': 'src/nx/gpu/draw_state.h', 'sha256': hashlib.sha256(b'draw state\n').hexdigest()},
                {'path': 'src/nx/gpu/vk/render.c', 'sha256': hashlib.sha256(b'gpu source\n').hexdigest()},
            ])
            self.assertEqual(harness.changed_sources(root, before), [])

    def test_added_removed_and_modified_sources_are_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gpu = root / 'src/nx/gpu'
            gpu.mkdir(parents=True)
            (gpu / 'changed.c').write_bytes(b'before\n')
            (gpu / 'removed.h').write_bytes(b'removed\n')
            before = harness.source_snapshot(root)
            (gpu / 'changed.c').write_bytes(b'after\n')
            (gpu / 'removed.h').unlink()
            (gpu / 'added.comp').write_bytes(b'new source\n')
            self.assertEqual(harness.changed_sources(root, before), [
                'src/nx/gpu/added.comp', 'src/nx/gpu/changed.c', 'src/nx/gpu/removed.h'])


class Classification(unittest.TestCase):
    def test_validation_error_fails_even_with_all_markers(self):
        row = harness.classify(gpu_output() + '\nvulkan validation: invalid operation [VUID-test-00001]', 0, gpu=True)
        self.assertEqual(row['status'], 'fail')
        self.assertFalse(harness.all_features_passed([row]))

    def test_threading_validation_errors_fail_even_with_all_markers(self):
        for diagnostic in ('vulkan validation: THREADING ERROR: simultaneous object use',
                           'vulkan validation: [THREADING] object used by two threads',
                           'Vulkan Validation: threading error: simultaneous object use',
                           'vulkan validation: [UNASSIGNED-Threading-MultipleThreads] simultaneous use'):
            with self.subTest(diagnostic=diagnostic):
                row = harness.classify(gpu_output() + '\n' + diagnostic, 0, gpu=True)
                self.assertEqual(row['status'], 'fail')
                self.assertFalse(harness.all_features_passed([row]))
        row = harness.classify(gpu_output() + '\nrecording-thread mode selected; THREADING measurement', 0, gpu=True)
        self.assertEqual(row['status'], 'pass')
    def test_complete_gpu_output(self):
        row = harness.classify(gpu_output(), 0, gpu=True)
        self.assertEqual(row['status'], 'pass')
        self.assertEqual(len(row['features']), len(harness.GPU_FEATURES))
        self.assertTrue(harness.all_features_passed([row]))

    def test_each_missing_feature_fails(self):
        for _, marker, _ in harness.GPU_FEATURES:
            with self.subTest(marker=marker):
                text = '\n'.join(line for line in gpu_output().splitlines() if not line.startswith(marker))
                row = harness.classify(text, 0, gpu=True)
                self.assertEqual(row['status'], 'fail')
                self.assertFalse(harness.all_features_passed([row]))

    def test_explicit_optional_skip_preserves_incomplete_coverage(self):
        for feature, passed, skipped in harness.GPU_FEATURES:
            if skipped is None:
                continue
            with self.subTest(feature=feature):
                text = gpu_output().replace(passed + ' exact=1', skipped + ' unavailable')
                row = harness.classify(text, 0, gpu=True)
                self.assertEqual(row['status'], 'pass')
                self.assertEqual(next(item['status'] for item in row['features'] if item['name'] == feature), 'skip')
                self.assertEqual(row['skip_messages'], [skipped + ' unavailable'])
                self.assertFalse(harness.all_features_passed([row]))

    def test_driver_exit77_is_skip_only_for_gpu(self):
        row = harness.classify('No device', 77, gpu=True)
        self.assertEqual(row['status'], 'skip')
        self.assertFalse(harness.all_features_passed([row]))
        self.assertEqual(harness.classify('No device', 77)['status'], 'fail')

    def test_error_exit_cannot_pass_with_success_markers(self):
        for code in (-1, 1, 9):
            with self.subTest(code=code):
                row = harness.classify(gpu_output(), code, gpu=True)
                self.assertEqual(row['status'], 'fail')
                self.assertFalse(harness.all_features_passed([row]))

    def test_changed_binary_fails_even_on_skip(self):
        for code in (0, 77):
            with self.subTest(code=code):
                row = harness.classify(gpu_output(), code, stable=False, gpu=True)
                self.assertEqual(row['status'], 'fail')
                self.assertFalse(harness.all_features_passed([row]))

    def test_unexpected_skip_prevents_complete_coverage(self):
        row = harness.classify(gpu_output() + '\nSKIP NVN unknown feature: unavailable', 0, gpu=True)
        self.assertEqual(row['status'], 'pass')
        self.assertFalse(harness.all_features_passed([row]))

    def test_markers_must_start_a_line(self):
        for prefix in ('diagnostic: ', ' '):
            with self.subTest(prefix=prefix):
                text = '\n'.join(prefix + line for line in gpu_output().splitlines())
                self.assertEqual(harness.classify(text, 0, gpu=True)['status'], 'fail')

    def test_conflicting_feature_markers_fail(self):
        skipped = next(skipped for _, _, skipped in harness.GPU_FEATURES if skipped)
        row = harness.classify(gpu_output() + '\n' + skipped + ' unavailable', 0, gpu=True)
        self.assertEqual(row['status'], 'fail')
        self.assertFalse(harness.all_features_passed([row]))

    def test_native_check_needs_success_and_stable_binary(self):
        self.assertEqual(harness.classify('', 0)['status'], 'pass')
        self.assertEqual(harness.classify('', 1)['status'], 'fail')
        self.assertEqual(harness.classify('', 0, stable=False)['status'], 'fail')


class JobMatrix(unittest.TestCase):
    def setUp(self):
        self.native = [Path('/build/tests/snx-test-nvn-draw-banks')]
        self.vulkan = Path('/build/src/nx/snx-test-vulkan')
        self.drivers = [('default', None), ('icd-0', Path('/drivers/radeon.json'))]

    def test_default_keeps_old_names_and_native_jobs_once(self):
        jobs = harness.jobs(self.native, self.vulkan, self.drivers)
        self.assertEqual([job[0] for job in jobs], ['snx-test-nvn-draw-banks',
                         'default-descriptors-0', 'default-descriptors-1',
                         'icd-0-descriptors-0', 'icd-0-descriptors-1'])
        self.assertEqual(jobs[0], (self.native[0].name, [str(self.native[0])], None, None))
        self.assertTrue(all('--vk_record=0' in command for _, command, _, _ in jobs[1:]))
        self.assertTrue(all('--vk_validation=0' in command for _, command, _, _ in jobs[1:]))

    def test_both_covers_every_driver_descriptor_and_record_mode(self):
        jobs = harness.jobs(self.native, self.vulkan, self.drivers, validation=True, record='both')
        self.assertEqual(len(jobs), 1 + 2 * 2 * 2)
        self.assertEqual(len({name for name, _, _, _ in jobs}), len(jobs))
        actual = set()
        for name, command, manifest, marker in jobs[1:]:
            self.assertEqual(marker, harness.GPU_MARKER)
            self.assertEqual(command[:3], [str(self.vulkan), '-H', '--vk_memory=1'])
            self.assertIn('--vk_validation=1', command)
            mode = int(next(arg.split('=')[1] for arg in command if arg.startswith('--vk_record=')))
            descriptors = int(next(arg.split('=')[1] for arg in command if arg.startswith('--vk_descriptors=')))
            actual.add((manifest, descriptors, mode))
            self.assertEqual(name.endswith('-record-1'), mode == 1)
        self.assertEqual(actual, {(manifest, descriptors, mode) for _, manifest in self.drivers
                                  for descriptors in (0, 1) for mode in (0, 1)})

    def test_record_one_and_bad_mode(self):
        jobs = harness.jobs([], self.vulkan, self.drivers, record='1')
        self.assertEqual(len(jobs), 4)
        self.assertTrue(all(name.endswith('-record-1') for name, _, _, _ in jobs))
        self.assertTrue(all('--vk_record=1' in command for _, command, _, _ in jobs))
        with self.assertRaises(ValueError):
            harness.jobs([], self.vulkan, self.drivers, record='invalid')


if __name__ == '__main__':
    unittest.main()
