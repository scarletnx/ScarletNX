#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Budget arithmetic and evidence gates; no emulator, GPU, CUDA or benchmark process runs."""
import contextlib
import copy
from fractions import Fraction
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('gpu_budget', Path(__file__).resolve().parents[1] / 'tools/analyze_gpu_budget.py')
budget = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(budget)


def passed():
    return {'status': 'PASS', 'evidence': 'fixture byte comparison', 'validation_errors': 0}


def profile():
    return {'schema_version': 1, 'workload': {'id': 'unit-test-fixture'},
            'measurement': {'kind': 'measured', 'source': 'fixture timestamp trace', 'scope': 'frame',
                            'timing_domain': 'wall', 'serial_components': True, 'interval_ns': 1000,
                            'hardware_identity': 'fixture host/driver/settings', 'correctness': passed()},
            'components': [{'kind': 'measured', 'source': 'fixture component timestamps', 'name': 'prepare',
                            'workload_identity': 'same-input-shape', 'cost_ns': 100, 'calls_per_interval': 6,
                            'frequency_source': 'fixture call counter',
                            'candidate': {'kind': 'measured', 'source': 'fixture changed timestamps', 'cost_ns': 10,
                                          'workload_identity': 'same-input-shape', 'correctness': passed()}}]}


def identity_fixture(root, value):
    manifest = root / 'workload-manifest.json'
    manifest.write_text('unit-test build, scene, inputs and settings\n')
    digest = hashlib.sha256(manifest.read_bytes()).hexdigest()
    value['workload'].update(identity_sha256=digest, identity_artifact=manifest.name)
    return digest


def frame_record(digest, elapsed=100):
    return {'kind': 'measured', 'source': 'fixture complete frame timestamps', 'interval_ns': elapsed,
            'scope': 'frame', 'timing_domain': 'wall', 'hardware_identity': 'fixture host/driver/settings',
            'workload_identity_sha256': digest, 'correctness': passed()}


class Arithmetic(unittest.TestCase):
    def test_sevenfold_means_six_hundred_percent_and_fraction_six_sevenths(self):
        report = budget.analyze(profile())
        self.assertEqual(report['target_speed_increase_percent'], 600)
        self.assertAlmostEqual(report['minimum_selected_fraction_at_infinite_speedup'], 6 / 7)
        self.assertEqual(report['selected_amdahl_bound']['attainability'], 'impossible')
        self.assertEqual(report['selected_amdahl_bound']['infinite_component_speedup_ceiling'], 2.5)

    def test_exact_amdahl_boundary_and_required_component_speed(self):
        boundary = budget.amdahl(Fraction(6, 7))
        self.assertEqual(boundary['attainability'], 'asymptotic')
        self.assertEqual(boundary['infinite_component_speedup_ceiling'], 7)
        self.assertIsNone(boundary['required_component_speedup'])
        self.assertAlmostEqual(budget.amdahl(Fraction(9, 10))['required_component_speedup'], 21)
        self.assertEqual(budget.amdahl(1)['required_component_speedup'], 7)
        self.assertTrue(budget.amdahl(1)['ceiling_is_unbounded'])
        self.assertEqual(budget.amdahl(0)['attainability'], 'impossible')

    def test_component_measurement_has_only_a_conditional_serial_estimate(self):
        report = budget.analyze(profile())
        self.assertEqual(report['components'][0]['candidate']['time_reduction_percent'], 90)
        self.assertEqual(report['serial_model']['interval_ns'], 460)
        self.assertEqual(report['serial_model']['time_reduction_percent'], 54)
        self.assertEqual(report['target_verification']['status'], 'unverified')
        self.assertEqual(report['reference_baseline']['status'], 'unavailable')

    def test_unknown_overlap_never_produces_a_combined_prediction(self):
        value = profile()
        value['measurement']['serial_components'] = False
        value['components'][0]['calls_per_interval'] = 60
        report = budget.analyze(value)
        self.assertEqual(report['serial_model']['status'], 'unavailable')
        self.assertEqual(report['selected_amdahl_bound']['status'], 'unavailable')
        self.assertNotIn('fraction_of_interval', report['components'][0])
        self.assertEqual(report['components'][0]['candidate']['saved_ns_per_interval'], 5400)

    def test_serial_overbooking_and_mixed_domains_are_rejected(self):
        value = profile()
        value['components'][0]['calls_per_interval'] = 11
        with self.assertRaisesRegex(ValueError, 'exceed'):
            budget.analyze(value)
        value = profile()
        value['components'][0]['timing_domain'] = 'gpu'
        with self.assertRaisesRegex(ValueError, 'timing domains'):
            budget.analyze(value)

    def test_changed_frequency_needs_a_measurement_source(self):
        value = profile()
        value['components'][0]['candidate'].update(calls_per_interval=2, frequency_source='fixture changed call counter')
        self.assertEqual(budget.analyze(value)['serial_model']['interval_ns'], 420)
        del value['components'][0]['candidate']['frequency_source']
        with self.assertRaisesRegex(ValueError, 'frequency_source'):
            budget.analyze(value)

    def test_roofline_uses_maximum_with_explicit_units_and_kind(self):
        value = profile()
        value['hardware'] = {'bandwidth_bytes_per_second': 1024, 'operations_per_second': 8192, 'operation_kind': 'fp32'}
        value['components'][0].update(bytes_per_call=1024, operations_per_call=4096, operation_kind='fp32')
        roof = budget.analyze(value)['components'][0]['roofline']
        self.assertEqual(roof['memory_ns_per_call'], 1e9)
        self.assertEqual(roof['compute_ns_per_call'], .5e9)
        self.assertEqual(roof['ns_per_call'], 1e9)
        self.assertEqual(roof['status'], 'optimistic_lower_bound')
        value['components'][0]['operation_kind'] = 'integer'
        with self.assertRaisesRegex(ValueError, 'kinds must match'):
            budget.analyze(value)

    def test_selected_components_change_only_the_bound(self):
        value = profile()
        second = copy.deepcopy(value['components'][0])
        second.update(name='submit', cost_ns=10, calls_per_interval=5)
        del second['candidate']
        value['components'].append(second)
        report = budget.analyze(value, selected=['submit'])
        self.assertEqual(report['selected_amdahl_bound']['selected_fraction'], .05)
        self.assertEqual(report['serial_model']['interval_ns'], 460)
        with self.assertRaisesRegex(ValueError, 'not in the profile'):
            budget.analyze(value, selected=['unknown'])


class Evidence(unittest.TestCase):
    def test_failed_skipped_or_missing_pass_never_enters_candidate_savings(self):
        for gate in [None, {'status': 'PASS'}, {'status': 'FAIL', 'evidence': 'mismatch'},
                     {**passed(), 'skipped': True}, {**passed(), 'validation_errors': 1}]:
            with self.subTest(gate=gate):
                value = profile()
                value['components'][0]['candidate']['correctness'] = gate
                report = budget.analyze(value)
                self.assertEqual(report['components'][0]['candidate']['status'], 'rejected')
                self.assertEqual(report['serial_model']['interval_ns'], 1000)

    def test_mismatching_candidate_shape_cannot_claim_a_speedup(self):
        value = profile()
        value['components'][0]['candidate']['workload_identity'] = 'fewer-output-pixels'
        result = budget.analyze(value)['components'][0]['candidate']
        self.assertEqual(result['status'], 'rejected')
        self.assertNotIn('time_reduction_percent', result)

    def test_target_uses_measured_matching_full_frames_and_strict_threshold(self):
        with tempfile.TemporaryDirectory() as name:
            root, value = Path(name), profile()
            value['measurement']['interval_ns'] = 700
            digest = identity_fixture(root, value)
            value['candidate_measurement'] = frame_record(digest)
            report = budget.analyze(value, directory=root)
            self.assertEqual(report['target_verification']['status'], 'met')
            value['candidate_measurement']['interval_ns'] = 100.0000001
            self.assertEqual(budget.analyze(value, directory=root)['target_verification']['status'], 'not_met')
            value['measurement']['scope'] = value['candidate_measurement']['scope'] = 'component'
            self.assertEqual(budget.analyze(value, directory=root)['target_verification']['status'], 'unverified')

    def test_frame_identity_hardware_and_correctness_gates(self):
        with tempfile.TemporaryDirectory() as name:
            root, value = Path(name), profile()
            digest = identity_fixture(root, value)
            for field, changed in [('workload_identity_sha256', '0' * 64), ('hardware_identity', 'different GPU'),
                                   ('correctness', {'status': 'SKIP'}), ('scope', 'component')]:
                value['candidate_measurement'] = {**frame_record(digest), field: changed}
                report = budget.analyze(value, directory=root)
                self.assertEqual(report['measured_candidate']['status'], 'rejected')
                self.assertEqual(report['target_verification']['status'], 'unverified')
            value['candidate_measurement'] = frame_record(digest)
            (root / 'workload-manifest.json').write_text('changed settings')
            self.assertFalse(budget.analyze(value, directory=root)['workload_identity']['verified'])

    def test_reference_requires_explicit_measured_matching_identity(self):
        with tempfile.TemporaryDirectory() as name:
            root, value = Path(name), profile()
            digest = identity_fixture(root, value)
            value['reference_baseline'] = {**frame_record(digest, 2000), 'engine': 'external', 'hardware_identity': 'reference device'}
            reference = budget.analyze(value, directory=root)['reference_baseline']
            self.assertEqual(reference['status'], 'available')
            self.assertEqual(reference['relative_time_ratio_to_current'], 2)
            self.assertFalse(reference['same_hardware'])
            value['reference_baseline']['kind'] = 'estimated'
            self.assertEqual(budget.analyze(value, directory=root)['reference_baseline']['status'], 'unavailable')


class InputOutput(unittest.TestCase):
    def test_invalid_numbers_and_ambiguous_json_are_rejected(self):
        for bad in [True, -1, float('nan'), float('inf'), 10 ** 1000]:
            with self.subTest(value=str(bad)[:20]), self.assertRaises(ValueError):
                budget.number(bad, 'cost')
        for content in ['{"a":1,"a":2}', '{"cost":NaN}', '{"cost":Infinity}']:
            with self.assertRaises(ValueError):
                budget.load_json(content)
        for target in [0, 1, float('inf')]:
            with self.assertRaises(ValueError):
                budget.analyze(profile(), target)

    def test_malformed_objects_and_unused_invalid_hardware_rates_fail(self):
        for field, invalid in [('workload', None), ('hardware', []), ('components', {}),
                               ('measurement', {'kind': 'estimated'})]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                budget.analyze({**profile(), field: invalid})
        value = profile()
        value['hardware'] = {'bandwidth_bytes_per_second': -1}
        with self.assertRaises(ValueError):
            budget.analyze(value)

    def test_benchmark_reports_remain_metadata_without_frame_claims(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / 'results.json'
            data = {'status': 'complete', 'kind': 'hotpath', 'rounds': 7,
                    'measurements': [{'exact': True, 'ns_per_operation': 1}],
                    'summaries': [{'workload': 'fixture'}]}
            path.write_text(json.dumps(data))
            metadata = budget.benchmark_metadata(path)
            self.assertTrue(metadata['exact_records'])
            report = budget.analyze(profile(), metadata=[metadata])
            self.assertEqual(report['target_verification']['status'], 'unverified')
            self.assertEqual(report['reference_baseline']['status'], 'unavailable')
            data['measurements'][0]['exact'] = False
            path.write_text(json.dumps(data))
            self.assertFalse(budget.benchmark_metadata(path)['exact_records'])

    def test_cli_writes_reviewable_json_without_overwriting(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source, out = root / 'profile.json', root / 'analysis.json'
            source.write_text(json.dumps(profile()))
            report = budget.main(['--profile', str(source), '--out', str(out)])
            self.assertEqual(json.loads(out.read_text()), report)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                budget.main(['--profile', str(source), '--out', str(out)])
            self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
