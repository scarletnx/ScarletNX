#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""tracy_compare.py joins canned tracy-csvexport output without a capture or the Tracy tools."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


compare = tool('tracy_compare')

ZONE_A = ('name,total_ns,counts,mean_ns\n'
          'Game::draw,1000000000,100,10000000\n'
          'Game::pipeline make,500000000,50,10000000\n'
          'Other zone,200000000,20,10000000\n')
ZONE_B = ('name,total_ns,counts,mean_ns\n'
          'Game::draw,1800000000,120,15000000\n'
          'Game::pipeline make,480000000,40,12000000\n'
          'Other zone,240000000,20,12000000\n')
FRAME_A = 'frame_set,counts,frames,median_ns,p90_ns,p99_ns\ndefault,100,100,16000000,20000000,25000000\n'
FRAME_B = 'frame_set,counts,frames,median_ns,p90_ns,p99_ns\ndefault,120,120,15000000,19000000,24000000\n'
GPU_A = 'name,GPU execution time\nGame::draw,1000000\nGame::draw,2000000\n'
GPU_B = 'name,GPU execution time\nGame::draw,3000000\n'


class Export:
    """The canned tracy-csvexport: it records the command and answers from the tables above."""

    def __init__(self):
        self.calls = []

    def __call__(self, command, **kwargs):
        self.calls.append(list(command))
        run = Path(command[-1]).name
        suffix = '_B' if run == 'b.tracy' else '_A'
        if '-F' in command:
            text = globals()['FRAME' + suffix]
        elif '-g' in command:
            text = globals()['GPU' + suffix]
        else:
            text = globals()['ZONE' + suffix]
        return subprocess.CompletedProcess(command, 0, stdout=text, stderr='')


class TracyCompareTests(unittest.TestCase):
    def run_tool(self, *args, export=None):
        export = export or Export()
        with TemporaryDirectory() as folder:
            paths = []
            for name in ('a', 'b'):
                path = Path(folder) / (name + '.tracy')
                path.write_text('')
                paths.append(str(path))
            output, errors = io.StringIO(), io.StringIO()
            with mock.patch.object(compare.shutil, 'which', return_value='/fake/tracy-csvexport'), \
                    mock.patch.object(compare.subprocess, 'run', side_effect=export), \
                    redirect_stdout(output), redirect_stderr(errors):
                code = compare.main([*paths, *args])
            return code, output.getvalue(), errors.getvalue(), export

    def test_json_report(self):
        code, text, _, export = self.run_tool('--json')
        self.assertEqual(code, 0)
        report = json.loads(text)
        self.assertEqual(report['runs']['a']['timed_frames'], 100)
        self.assertEqual(report['runs']['b']['timed_frames'], 120)
        self.assertEqual(report['runs']['a']['frame_set'], 'default')
        self.assertEqual(report['filters'], {'prefix': None, 'gpu': False})
        rows = {row['name']: row for row in report['zones']}
        self.assertEqual(set(rows), {'Game::draw', 'Game::pipeline make', 'Other zone'})
        draw = rows['Game::draw']
        self.assertAlmostEqual(draw['ms_per_frame_a'], 10)
        self.assertAlmostEqual(draw['ms_per_frame_b'], 15)
        self.assertAlmostEqual(draw['delta_ms_per_frame'], 5)
        self.assertAlmostEqual(draw['us_per_call_a'], 10000)
        self.assertEqual((draw['counts_a'], draw['counts_b']), (100, 120))
        pipe = rows['Game::pipeline make']
        self.assertAlmostEqual(pipe['delta_ms_per_frame'], -1)
        self.assertAlmostEqual(pipe['delta_percent'], -20)
        self.assertEqual(len(export.calls), 4)

    def test_prefix_filters_names_without_a_second_export_filter(self):
        code, text, _, export = self.run_tool('--json', '--prefix', 'Game::')
        self.assertEqual(code, 0)
        report = json.loads(text)
        self.assertEqual({row['name'] for row in report['zones']}, {'Game::draw', 'Game::pipeline make'})
        self.assertEqual(report['filters']['prefix'], 'Game::')
        # -f is not forwarded: the tool filters the joined names once, with startswith
        self.assertFalse(any('-f' in call for call in export.calls))
        self.assertFalse(any('-g' in call for call in export.calls))

    def test_gpu_flag_reaches_only_the_zone_export(self):
        code, text, _, export = self.run_tool('--json', '-g')
        self.assertEqual(code, 0)
        report = json.loads(text)
        self.assertEqual(report['filters']['gpu'], True)
        self.assertIn('Game::draw', {row['name'] for row in report['zones']})
        zone_calls = [call for call in export.calls if '-F' not in call]
        frame_calls = [call for call in export.calls if '-F' in call]
        self.assertEqual(len(zone_calls), 2)
        self.assertTrue(all('-g' in call for call in zone_calls))
        self.assertTrue(all('-g' not in call for call in frame_calls))

    def test_table_names_the_columns(self):
        code, text, _, _ = self.run_tool()
        self.assertEqual(code, 0)
        self.assertIn('A ms/frame', text)
        self.assertIn('B ms/frame', text)
        self.assertIn('counts', text)
        self.assertIn('Game::draw', text)
        self.assertIn('p50 16.000 ms', text)

    def test_no_frames_is_reported(self):
        class NoFrames(Export):
            def __call__(self, command, **kwargs):
                if '-F' in command:
                    self.calls.append(list(command))
                    return subprocess.CompletedProcess(command, 0, stdout='There are currently no frames\n',
                                                       stderr='')
                return super().__call__(command, **kwargs)

        code, _, errors, _ = self.run_tool(export=NoFrames())
        self.assertEqual(code, 1)
        self.assertIn('no frame statistics', errors)

    def test_annotations_threshold_range_and_schema(self):
        messages = [(200_000_000, 'a 40 ms frame: below the threshold'),
                    (500_000_000, 'a 120 ms frame: nvn sync wait 22')]
        labels = compare.annotations(messages)
        self.assertEqual(len(labels), 1)
        label = labels[0]
        self.assertEqual(label['text'], messages[1][1])
        self.assertEqual(label['min'], 380_000_000)
        self.assertEqual(label['max'], 500_000_000)
        self.assertEqual(label['color'], compare.SLOW_FRAME_COLOR)
        self.assertEqual(set(label), {'text', 'min', 'max', 'color'})

    def test_annotations_skips_bad_times(self):
        messages = [(-1, 'a 100 ms frame: negative time'),
                    (1_000_000_000, 'a ' + str(int(1e303)) + ' ms frame: overflow'),
                    (1_000_000_000, 'a 100 ms frame: fine')]
        labels = compare.annotations(messages)
        self.assertEqual(len(labels), 1)
        self.assertEqual(labels[0]['text'], messages[2][1])

    def test_csv_messages_bad_header_and_timestamp(self):
        with self.assertRaises(ValueError):
            compare.csv_messages('not,a,message,header\n')
        with self.assertRaises(ValueError):
            compare.csv_messages('message,total_ns\na 100 ms frame: no time\n')
        rows = compare.csv_messages('message,total_ns\na 100 ms frame: fine,1234\n')
        self.assertEqual(rows, [(1234, 'a 100 ms frame: fine')])

    def test_annotate_cli_writes_public_sidecar(self):
        class AnnotateExport:
            def __init__(self):
                self.calls = []

            def __call__(self, command, **kwargs):
                self.calls.append(list(command))
                if '-m' in command:
                    text = 'message,total_ns\na 120 ms frame: nvn sync wait 22,500000000\n'
                elif '-F' in command:
                    text = FRAME_A
                else:
                    text = ZONE_A
                return subprocess.CompletedProcess(command, 0, stdout=text, stderr='')

        export = AnnotateExport()
        with TemporaryDirectory() as folder:
            capture = Path(folder) / 'a.tracy'
            capture.write_text('')
            output, errors = io.StringIO(), io.StringIO()
            with mock.patch.object(compare, 'local_mcp_trace', return_value=None), \
                    mock.patch.object(compare.shutil, 'which', return_value='/fake/tracy-csvexport'), \
                    mock.patch.object(compare.subprocess, 'run', side_effect=export), \
                    redirect_stdout(output), redirect_stderr(errors):
                code = compare.main(['--annotate', str(capture)])
            self.assertEqual(code, 0, errors.getvalue())
            sidecar = Path(str(capture) + '.json')
            self.assertTrue(sidecar.is_file())
            labels = json.loads(sidecar.read_text())['annotations']
            self.assertEqual(len(labels), 1)
            self.assertEqual(labels[0]['min'], 380_000_000)
            self.assertEqual(labels[0]['max'], 500_000_000)
            self.assertFalse(Path(str(sidecar) + '.tmp').exists())
            self.assertIn('tracy-csvexport', output.getvalue())
            self.assertIn('p99 frame', output.getvalue())
        self.assertTrue(any('-m' in call for call in export.calls))
        self.assertTrue(any('-F' in call for call in export.calls))

    def test_annotate_cli_with_mcp_bindings(self):
        messages = [(500_000_000, 'a 120 ms frame: nvn sync wait 22')]
        with TemporaryDirectory() as folder:
            capture = Path(folder) / 'a.tracy'
            capture.write_text('')
            output, errors = io.StringIO(), io.StringIO()
            frame_times = [10_000_000] * 100 + [500_000_000]
            with mock.patch.object(compare, 'local_mcp_trace', return_value=(messages, frame_times)), \
                    mock.patch.object(compare.subprocess, 'run', side_effect=AssertionError('no csvexport')), \
                    redirect_stdout(output), redirect_stderr(errors):
                code = compare.main(['--annotate', str(capture)])
            self.assertEqual(code, 0, errors.getvalue())
            labels = json.loads(Path(str(capture) + '.json').read_text())['annotations']
            self.assertEqual(len(labels), 1)
            self.assertEqual(labels[0]['min'], 380_000_000)
            self.assertIn('MCP bindings', output.getvalue())
            self.assertIn('p99 frame 10.0 ms', output.getvalue())

    def test_annotations_keep_touching_spans(self):
        messages = [(500_000_000, 'a 120 ms frame: first'),
                    (500_000_100, 'a 120 ms frame: second')]
        labels = compare.annotations(messages)
        self.assertEqual(len(labels), 2)

    def test_annotations_collapse_exact_duplicates(self):
        message = 'a 120 ms frame: nvn sync wait 22'
        labels = compare.annotations([(500_000_000, message), (500_000_000, message)])
        self.assertEqual(len(labels), 1)
        self.assertEqual(labels[0]['text'], message)

    def test_annotate_rejects_two_captures(self):
        with TemporaryDirectory() as folder:
            a = Path(folder) / 'a.tracy'
            b = Path(folder) / 'b.tracy'
            a.write_text('')
            b.write_text('')
            errors = io.StringIO()
            with redirect_stderr(errors):
                code = compare.main(['--annotate', str(a), str(b)])
            self.assertEqual(code, 1)
            self.assertIn('takes one capture', errors.getvalue())

    def test_annotate_rejects_compare_flags(self):
        with TemporaryDirectory() as folder:
            capture = Path(folder) / 'a.tracy'
            capture.write_text('')
            for flag in ('--json', '--prefix', '-g'):
                output, errors = io.StringIO(), io.StringIO()
                args = ['--annotate', str(capture)] + ([flag, 'Game::'] if flag == '--prefix' else [flag])
                with redirect_stdout(output), redirect_stderr(errors):
                    code = compare.main(args)
                self.assertEqual(code, 1)
                self.assertIn('does not accept', errors.getvalue())


if __name__ == '__main__':
    unittest.main()
