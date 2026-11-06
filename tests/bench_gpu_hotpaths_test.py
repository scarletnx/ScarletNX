#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Only exact, matching, immutable synthetic workloads can enter a timing comparison."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('hotpaths',
                                            Path(__file__).resolve().parents[1] / 'tools/bench_gpu_hotpaths.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
INLINE = 'NVN_INLINE_BENCH parameter=vertex draws=1000000 ns=125000000 exact=1'
GENERIC = 'GPU_HOTPATH_BENCH workload=storage_lookup_recent operations=20000 ns=40000 exact=1 results=65 bytes=131072'


class Records(unittest.TestCase):
    def test_inline_and_generic_units(self):
        row = bench.parse_samples(INLINE, 'inline')['inline-vertex']
        self.assertEqual(row['ns_per_operation'], 125)
        row = bench.parse_samples(GENERIC, 'hotpath')['storage_lookup_recent']
        self.assertEqual(row['shape'], {'operations': 20000, 'results': 65, 'bytes': 131072})
        self.assertEqual(row['counters']['results'], 65)

    def test_all_records_need_exact_positive_values(self):
        for old, new in [('exact=1', 'exact=0'), ('exact=1', 'exact=2'), ('ns=40000', 'ns=0'),
                         ('ns=40000', 'ns=nan'), ('operations=20000', 'operations=-1'),
                         ('results=65', 'results=-65'), ('ns=40000', 'ns=18446744073709551616'),
                         ('workload=storage_lookup_recent', 'workload=bad|name')]:
            with self.subTest(new=new), self.assertRaises(ValueError):
                bench.parse_samples(GENERIC.replace(old, new), 'hotpath')
        for text in ['', GENERIC + '\n' + GENERIC, GENERIC + ' exact=1', GENERIC + ' malformed']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench.parse_samples(text, 'hotpath')

    def test_indexed_strategies_keep_equal_workload_sizes(self):
        text = '\n'.join('INDEXED_HANDLES_BENCH unique=16 linear_limit=%d tables=100 lookups=256 ns=900 exact=1' % n
                         for n in [-1, 0, 8, 16])
        before = bench.parse_samples(text, 'indexed', -1)
        after = bench.parse_samples(text, 'indexed', 8)
        bench.validate_shapes(before, after)
        self.assertEqual(after['indexed-16-keys']['operations'], 25600)
        with self.assertRaises(ValueError):
            bench.parse_samples(text, 'indexed', 4)
        with self.assertRaises(ValueError):
            bench.validate_shapes(before, bench.parse_samples(text.replace('tables=100', 'tables=99'), 'indexed', 8))

    def test_dimensions_match_but_cost_counters_can_change(self):
        before = bench.parse_samples(GENERIC + ' copied_bytes=900', 'hotpath')
        bench.validate_shapes(before, bench.parse_samples(GENERIC + ' copied_bytes=100', 'hotpath'))
        for old, new in [('operations=20000', 'operations=20001'), ('results=65', 'results=66'),
                         ('bytes=131072', 'bytes=65536')]:
            with self.subTest(new=new), self.assertRaises(ValueError):
                bench.validate_shapes(before, bench.parse_samples(GENERIC.replace(old, new), 'hotpath'))
        extended = bench.parse_samples(GENERIC + ' bindings=10', 'hotpath', dimensions=['bindings'])
        with self.assertRaises(ValueError):
            bench.validate_shapes(extended, bench.parse_samples(GENERIC + ' bindings=11', 'hotpath',
                                                               dimensions=['bindings']))
        with self.assertRaises(ValueError):
            bench.parse_samples(GENERIC, 'hotpath', dimensions=['bindings'])

    def test_paired_rounds_and_no_cross_workload_winner(self):
        rows = []
        for round_number in range(3):
            for label, ns in [('baseline', 40000 + round_number * 1000), ('candidate', 20000 + round_number * 1000)]:
                sample = bench.parse_samples(GENERIC.replace('ns=40000', f'ns={ns}'), 'hotpath')
                rows.append({**next(iter(sample.values())), 'round': round_number, 'label': label})
        summaries = bench.summarize(rows, ['baseline', 'candidate'], 3)
        self.assertEqual(len(summaries), 1)
        winner = summaries[0]['ranking'][0]
        self.assertEqual(winner['label'], 'candidate')
        self.assertEqual(winner['median_ns_per_operation'], 1.05)
        self.assertAlmostEqual(winner['median_paired_speedup'], 41000 / 21000)
        self.assertEqual(winner['faster_paired_rounds'], 3)
        with self.assertRaises(ValueError):
            bench.summarize(rows[:-1], ['baseline', 'candidate'], 3)


class Execution(unittest.TestCase):
    def test_content_change_is_detected_with_same_size_and_timestamp(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / 'binary'
            path.write_bytes(b'first')
            binary = {'path': path, 'identity': bench.identity(path)}
            before = path.stat()
            path.write_bytes(b'other')
            os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
            with self.assertRaisesRegex(RuntimeError, 'binary changed'):
                bench.unchanged(binary)

    def test_arguments_are_never_shell_text_and_failures_stop(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            path = root / 'binary with spaces'
            path.write_bytes(b'fixture')
            binary = {'path': path, 'identity': bench.identity(path)}

            def execute(command, **kwargs):
                self.assertEqual(command, [str(path), '--bench', '$(do-not-run)'])
                self.assertNotIn('shell', kwargs)
                kwargs['stdout'].write(GENERIC + '\n')
                return types.SimpleNamespace(returncode=0)

            with mock.patch.object(bench.subprocess, 'run', side_effect=execute):
                text, _, _ = bench.execute(binary, ['--bench', '$(do-not-run)'], root / 'work', 10)
            self.assertEqual(text.strip(), GENERIC)
            for code in [1, 77]:
                with mock.patch.object(bench.subprocess, 'run', return_value=types.SimpleNamespace(returncode=code)), \
                        self.assertRaises(RuntimeError):
                    bench.execute(binary, [], root / f'failure-{code}', 10)
            with mock.patch.object(bench.subprocess, 'run', side_effect=subprocess.TimeoutExpired(['fixture'], 1)), \
                    self.assertRaisesRegex(RuntimeError, 'timed out'):
                bench.execute(binary, [], root / 'timeout', 1)

    def test_complete_run_is_serial_reproducible_and_keeps_provenance(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            exe = root / 'fixture'
            exe.write_bytes(b'executable stub; never run')
            exe.chmod(0o755)
            index = root / 'index.json'
            index.write_text(json.dumps({'api_coverage': {'hooks': 539},
                'classification': 'static_contract_and_api_evidence', 'counts': {'contracts': 18},
                'api_inventory': {'counts': {'canonical_api_names': 562}, 'apis': ['local-reference-only']}}))
            calls = []

            def execute(binary, arguments, directory, timeout):
                calls.append((directory.name, arguments))
                directory.mkdir()
                return GENERIC, [str(binary['path']), *arguments], str(directory / 'run.log')

            arguments = ['--baseline', str(exe), '--candidate', f'cached={exe}', '--kind', 'hotpath',
                         '--rounds', '3', '--variant-arg', 'baseline=--baseline', '--index', str(index)]
            with mock.patch.object(bench, 'execute', side_effect=execute), contextlib.redirect_stdout(io.StringIO()):
                one = bench.main([*arguments, '--out', str(root / 'one')])
                two = bench.main([*arguments, '--out', str(root / 'two')])
            self.assertEqual(one['orders'], two['orders'])
            self.assertEqual(len(one['measurements']), 6)
            self.assertEqual(one['status'], 'complete')
            self.assertEqual(one['reference_index']['api_coverage']['hooks'], 539)
            self.assertEqual(one['reference_index']['api_inventory_counts']['canonical_api_names'], 562)
            self.assertEqual(one['reference_index']['contract_counts']['contracts'], 18)
            self.assertNotIn('apis', one['reference_index'])
            self.assertTrue(any(args == ['--bench', '--baseline'] for _, args in calls))
            self.assertIn('not game FPS', (root / 'one/report.md').read_text())
            with mock.patch.object(bench, 'execute', side_effect=RuntimeError('exact output failed')), \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                bench.main([*arguments, '--out', str(root / 'failed')])
            report = json.loads((root / 'failed/results.json').read_text())
            self.assertEqual(report['status'], 'failed')
            self.assertNotIn('summaries', report)


if __name__ == '__main__':
    unittest.main()
