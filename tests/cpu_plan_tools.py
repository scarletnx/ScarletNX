# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Candidate selection must not rank invalid measurements or lose the plan's scope."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
        spec.loader.exec_module(module)
    return module


plan = tool('cpu_plan')
tuner = tool('tune_cpu_sequences')


class PlanTests(unittest.TestCase):
    def test_all_items_have_explicit_status(self):
        items = plan.read_plan(ROOT / 'docs/CPU_TRANSLATOR_PLAN.md')
        status = json.loads((ROOT / 'docs/cpu_plan_status.json').read_text())
        self.assertEqual(len(items), 41)
        self.assertEqual(set(items), set(status['items']))
        self.assertEqual(status['items']['L1']['status'], 'retired')
        self.assertEqual(status['items']['J1']['status'], 'deferred')
        self.assertIn('T5', items['I3']['dependencies'])

    def test_item_body_stops_at_unnumbered_peer(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'plan.md'
            path.write_text('#### I7 First\nproof one\n#### Other\nunrelated\n#### I8 Second\nproof two\n')
            items = plan.read_plan(path)
        self.assertEqual(items['I7']['body'], 'proof one')
        self.assertNotIn('unrelated', items['I8']['body'])

    def test_duplicate_item_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'plan.md'
            path.write_text('#### I7 One\n#### I7 Two\n')
            with self.assertRaises(ValueError):
                plan.read_plan(path)


class WorkflowTests(unittest.TestCase):
    def rows(self):
        return {
            'T5': {'id': 'T5', 'title': 'Fuzzer', 'body': 'P0', 'dependencies': [], 'status': 'in_progress'},
            'I2': {'id': 'I2', 'title': 'ABI', 'body': 'P1', 'dependencies': ['T5'], 'status': 'unverified'},
            'J1': {'id': 'J1', 'title': 'Tiering', 'body': 'P2', 'dependencies': [], 'status': 'deferred'},
            'R2': {'id': 'R2', 'title': 'Research', 'body': 'P3', 'dependencies': [], 'status': 'research'},
        }

    def matrix(self):
        return {'item': 'I8', 'axes': {'variant': ['base', 'candidate'], 'mode': ['normal', 'mixed']},
                'candidate': {'name': '${variant}-${mode}', 'cwd': '/isolated/${variant}',
                              'build': ['build', '${variant}'], 'check': ['check', '${variant}'],
                              'bench': {'ordinary': ['bench', '${mode}', 'literal;$(untouched)']}}}

    def test_queue_keeps_partial_dependency_blocked(self):
        rows = plan.work_queue(self.rows())
        self.assertEqual(rows[0]['id'], 'T5')
        self.assertEqual(next(row for row in rows if row['id'] == 'I2')['blocked_by'], ['T5'])
        self.assertNotIn('J1', [row['id'] for row in rows])
        self.assertEqual(next(row for row in rows if row['id'] == 'R2')['mode'], 'research')

    def test_queue_rejects_cycles_and_unknown_prerequisites(self):
        rows = self.rows()
        rows['T5']['dependencies'] = ['I2']
        with self.assertRaisesRegex(ValueError, 'cyclic'):
            plan.work_queue(rows)
        rows['T5']['dependencies'] = ['Missing']
        with self.assertRaisesRegex(ValueError, 'unknown prerequisite'):
            plan.work_queue(rows)

    def test_source_matches_are_filename_hints(self):
        row = {'body': 'Use `ir.c`, `tests/check.c`, and `do_not_run()`.', 'evidence': ['src/extra.h']}
        matches = plan.source_matches(row, [Path(p) for p in ('src/a/ir.c', 'tests/check.c', 'tests/other.c', 'src/extra.h')])
        self.assertEqual(matches, ['src/a/ir.c', 'src/extra.h', 'tests/check.c'])

    def test_matrix_preserves_argv_and_expands_all_combinations(self):
        manifest = plan.expand_matrix(self.matrix(), 'I8')
        self.assertEqual(len(manifest['candidates']), 4)
        self.assertEqual(manifest['candidates'][3]['name'], 'candidate-mixed')
        self.assertEqual(manifest['candidates'][3]['bench']['ordinary'], ['bench', 'mixed', 'literal;$(untouched)'])

    def test_matrix_rejects_unknown_axes_duplicates_and_large_grids(self):
        spec = self.matrix()
        spec['candidate']['name'] = '${unknown}'
        with self.assertRaisesRegex(ValueError, 'unknown matrix axis'):
            plan.expand_matrix(spec, 'I8')
        spec = self.matrix()
        spec['candidate']['name'] = 'same'
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            plan.expand_matrix(spec, 'I8')
        spec = self.matrix()
        spec['axes']['variant'] = list(range(257))
        with self.assertRaisesRegex(ValueError, '256-candidate'):
            plan.expand_matrix(spec, 'I8')

    def test_matrix_requires_matching_item_and_correctness_check(self):
        with self.assertRaisesRegex(ValueError, 'item must match'):
            plan.expand_matrix(self.matrix(), 'I15')
        spec = self.matrix()
        del spec['candidate']['check']
        with self.assertRaisesRegex(ValueError, 'commands'):
            plan.expand_matrix(spec, 'I8')

    def test_packets_preserve_status_and_refuse_overwrites(self):
        rows = self.rows()
        before = json.dumps(rows, sort_keys=True)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'src').mkdir()
            with mock.patch.object(plan, 'source_catalog', return_value=[]), \
                    mock.patch.object(plan.subprocess, 'check_output', return_value='revision\n'):
                snapshot = plan.write_packets(rows, root / 'packets', root)
                self.assertEqual(len(snapshot['queue']), 3)
                self.assertTrue((root / 'packets/I2/TASK.md').is_file())
                self.assertFalse((root / 'packets/J1').exists())
                with self.assertRaises(FileExistsError):
                    plan.write_packets(rows, root / 'packets', root)
        self.assertEqual(before, json.dumps(rows, sort_keys=True))

    def test_unknown_verification_mapping_does_not_run_commands(self):
        with mock.patch.object(plan, 'execute') as execute:
            with self.assertRaisesRegex(ValueError, 'no audited automatic check'):
                plan.verify_item('A1', Path('build'), Path('out'), 1, 1, ROOT)
            execute.assert_not_called()

    def test_verify_rejects_empty_tests_and_records_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'checks'
            with mock.patch.object(plan, 'execute', side_effect=['built', '{"tests": []}']) as execute:
                report = plan.verify_item('T3', Path('build'), out, 1, 1, ROOT)
                self.assertEqual(execute.call_count, 2)
            self.assertEqual(report['verdict'], 'incomplete')
            self.assertIn('no matching tests', report['error'])
            self.assertTrue((out / 'checks.json').is_file())

    def test_skipped_tests_do_not_pass_the_verification_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'checks'

            def execute(command, cwd, log, timeout):
                if log.stem == 'discover':
                    return json.dumps({'tests': [{'name': name} for name in plan.CHECKS['T3'][1]]})
                if log.stem == 'tests':
                    (out / 'ctest.xml').write_text('<testsuite><testcase name="missing-host"><skipped/></testcase></testsuite>')
                return ''

            with mock.patch.object(plan, 'execute', side_effect=execute):
                report = plan.verify_item('T3', Path('build'), out, 1, 1, ROOT)
            self.assertEqual(report['verdict'], 'incomplete')
            self.assertEqual(report['skipped'], ['missing-host'])


class TunerTests(unittest.TestCase):
    def candidate(self, name='exact'):
        return {'name': name, 'build': ['build'], 'check': ['check'], 'bench': {'mixed': ['bench']}}

    def test_workloads_must_match(self):
        a, b = self.candidate(), self.candidate('second')
        b['bench'] = {'ordinary': ['bench']}
        with self.assertRaises(ValueError):
            tuner.validate({'candidates': [a, b]})

    def test_names_cannot_escape_output_folder(self):
        with self.assertRaises(ValueError):
            tuner.validate({'candidates': [self.candidate('../outside')]})

    def test_every_candidate_needs_correctness_check(self):
        candidate = self.candidate()
        del candidate['check']
        with self.assertRaises(ValueError):
            tuner.validate({'candidates': [candidate]})

    def test_invalid_timings_are_rejected(self):
        for value in (0, -1, True, 'fast', float('nan'), float('inf')):
            with self.subTest(value=value), self.assertRaises(ValueError):
                tuner.measurement(json.dumps({'elapsed_ns': value, 'checksum': 'exact'}))

    def test_pareto_keeps_size_speed_tradeoff(self):
        summary = {name: {'mixed': {'median_ns': time}} for name, time in [('small', 20), ('fast', 10), ('worse', 30)]}
        self.assertEqual(set(tuner.pareto(summary, {'small': 5, 'fast': 10, 'worse': 20})), {'small', 'fast'})

    def test_paired_bootstrap_and_regression_gate(self):
        base = {'ordinary': [{'elapsed_ns': 20} for _ in range(7)],
                'mixed': [{'elapsed_ns': 20} for _ in range(7)]}
        good = {'ordinary': [{'elapsed_ns': 10} for _ in range(7)],
                'mixed': [{'elapsed_ns': 10} for _ in range(7)]}
        bad = {'ordinary': [{'elapsed_ns': 2} for _ in range(7)],
               'mixed': [{'elapsed_ns': 30} for _ in range(7)]}
        analysis = tuner.analyze_candidates({'base': base, 'good': good, 'bad': bad}, 'base', None, 1000, 1, 902)
        score = analysis['comparisons']['good']['weighted_geometric_speedup']
        self.assertAlmostEqual(score['ratio'], 2)
        self.assertAlmostEqual(score['bootstrap_95_interval'][0], 2)
        self.assertEqual(tuner.recommend(analysis, {}, .01, .02, None)['candidate'], 'good')
        self.assertEqual(tuner.recommend(analysis, {'base': 5, 'good': 20, 'bad': 30}, .01, .02, 10)['candidate'], 'base')
        self.assertIsNone(tuner.recommend(analysis, {'base': 15, 'good': 20, 'bad': 30}, .01, .02, 10)['candidate'])

    def test_bootstrap_rejects_mismatched_pairs_and_weights(self):
        base = {'ordinary': [{'elapsed_ns': 20} for _ in range(7)]}
        short = {'ordinary': [{'elapsed_ns': 10} for _ in range(5)]}
        with self.assertRaisesRegex(ValueError, 'equal counts'):
            tuner.analyze_candidates({'base': base, 'short': short}, 'base', None, 1000, 1, 902)
        for weights in ({}, {'ordinary': 0}, {'ordinary': float('nan')}, {'ordinary': True}):
            with self.assertRaisesRegex(ValueError, 'positive finite weight'):
                tuner.analyze_candidates({'base': base}, 'base', weights, 1000, 1, 902)

    def test_variation_uses_raw_observations(self):
        result = tuner.summarize({'exact': {'mixed': [{'elapsed_ns': x} for x in (10, 11, 12, 13, 100)]}})
        self.assertEqual(result['exact']['mixed']['median_ns'], 12)
        self.assertEqual(result['exact']['mixed']['max_ns'], 100)
        self.assertEqual(len(result['exact']['mixed']['samples']), 5)

    def test_failed_correctness_check_excludes_candidate(self):
        with tempfile.TemporaryDirectory() as folder:
            candidate = self.candidate()
            candidate['build'] = [sys.executable, '-c', 'pass']
            candidate['check'] = [sys.executable, '-c', 'raise SystemExit(3)']
            name, row = tuner.prepare(candidate, Path(folder), 10)
            self.assertEqual(name, 'exact')
            self.assertFalse(row['valid'])
            self.assertIn('returned 3', row['error'])

    def test_prepare_timeout_aborts_the_search(self):
        with tempfile.TemporaryDirectory() as folder:
            with mock.patch.object(tuner, 'execute', side_effect=subprocess.TimeoutExpired(['compiler'], 1)):
                with self.assertRaises(subprocess.TimeoutExpired):
                    tuner.prepare(self.candidate(), Path(folder), 1)

    def test_timeout_keeps_partial_output(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / 'timeout.log'
            command = [sys.executable, '-c', 'import time; print("started", flush=True); time.sleep(10)']
            with self.assertRaises(subprocess.TimeoutExpired):
                tuner.execute(command, None, log, 1)
            self.assertIn('started', log.read_text())

    def test_checksum_mismatch_prevents_results(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            candidates = []
            for name in ('one', 'two'):
                candidate = self.candidate(name)
                candidate['build'] = candidate['check'] = [sys.executable, '-c', 'pass']
                row = json.dumps({'elapsed_ns': 10, 'checksum': name})
                candidate['bench'] = {'mixed': [sys.executable, '-c', f'print({row!r})']}
                candidates.append(candidate)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'candidates': candidates}))
            output = root / 'results'
            result = subprocess.run([sys.executable, str(ROOT / 'tools/tune_cpu_sequences.py'), str(manifest),
                                     '--out', str(output), '--rounds', '5'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('checksum mismatch', result.stderr)
            self.assertFalse((output / 'results.json').exists())


if __name__ == '__main__':
    unittest.main()
