#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Real-game measurements cannot silently time different work or touch the player's saves."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location('gamebench', Path(__file__).resolve().parents[1] / 'tools/bench_nvn_game.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
IMAGE = b'P6\n2 1\n255\n\x01\x02\x03\x04\x05\x06'
VALID = 'game: 0100000000000000, version 1.2.0\nframes=12 hash=0123456789abcdef\n'


class Measurements(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'image.ppm'
        self.image.write_bytes(IMAGE)

    def test_final_frame_and_screenshot_required(self):
        row = bench.validate_result(0, VALID, self.image, 12)
        self.assertEqual(row['dimensions'], [2, 1])
        self.assertEqual(bench.validate_result(0, VALID.replace('game:', '[   0.25] game:'), self.image, 12)['game_version'],
                         '1.2.0')
        for text in ('', VALID + VALID, VALID.replace('frames=12', 'frames=0'),
                     VALID.replace('frames=12', 'frames=13'), VALID.replace('1.2.0', '1.2.0\ngame: ffffffffffffffff, version 2')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench.validate_result(0, text, self.image, 12)
        for code in (-11, 1, 77):
            with self.subTest(code=code), self.assertRaises(ValueError):
                bench.validate_result(code, VALID, self.image, 12)
        with self.assertRaises(ValueError):
            bench.validate_result(0, VALID, self.root / 'missing.ppm', 12)
        with self.assertRaises(ValueError):
            bench.validate_result(0, VALID, self.image, 12, (1, 2))
        with self.assertRaises(ValueError):
            bench.validate_result(0, VALID.replace('abcdef', 'abcdef 3x2'), self.image, 12)

    def test_uniform_or_truncated_screenshot_rejected(self):
        for content in (IMAGE[:-1], b'P6\n2 1\n255\n' + b'\0' * 6, b'not an image'):
            with self.subTest(content=content), self.assertRaises(ValueError):
                self.image.write_bytes(content)
                bench.ppm(self.image)

    def test_optional_interval_is_separate_from_end_to_end_metric(self):
        row = bench.validate_result(0, VALID + 'unattended_ns=10000000000\n', self.image, 12)
        self.assertEqual(row['game_interval_ns'], 10000000000)
        self.assertEqual(row['game_interval_fps'], 1.2)
        for suffix in ('unattended_ns=0\n', 'unattended_ns=1\nunattended_ns=2\n'):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                bench.validate_result(0, VALID + suffix, self.image, 12)
        rows = [dict(row, variant='baseline', round=1, elapsed_ns=30),
                dict(row, variant='candidate', round=1, elapsed_ns=20, game_interval_ns=5000000000)]
        summary = bench.summarize(rows)
        self.assertEqual(summary['median_baseline_over_candidate'], 1.5)
        self.assertEqual(summary['game_interval']['median_baseline_over_candidate'], 2)
        del rows[0]['game_interval_ns']
        self.assertEqual(bench.summarize(rows)['game_interval']['status'], 'unavailable')

    def test_frame_based_script_validation(self):
        script = self.root / 'input.snxtas'
        script.write_text('# comment\n1:1:0,0,0,0\n3:0:0,0,0,0\n')
        result = bench.validate_script(script, 12)
        self.assertEqual(result, '1:1:0,0,0,0\n3:0:0,0,0,0\n11:0:0,0,0,0\n')
        for content in ('300f:1', '2:0:0,0,0,0\n1:0:0,0,0,0', '12:0:0,0,0,0',
                        '2:1:32768,0,0,0', '11:1:0,0,0,0', '1:10000000000000000:0,0,0,0'):
            script.write_text(content)
            with self.subTest(content=content), self.assertRaises(ValueError):
                bench.validate_script(script, 12)

    def test_alternating_pairs_are_reproducible_and_balanced(self):
        order = bench.paired_orders(4, 781)
        self.assertEqual(order, bench.paired_orders(4, 781))
        self.assertEqual(order[0], order[2])
        self.assertEqual(order[1], tuple(reversed(order[0])))
        self.assertTrue(all(set(p) == {'baseline', 'candidate'} for p in order))

    def test_copies_dereference_links_without_hard_linking(self):
        source = self.root / 'source'
        source.mkdir()
        (source / 'save').write_text('original')
        (source / 'link').symlink_to(source / 'save')
        copy = self.root / 'copy'
        bench.clone_tree(source, copy)
        (copy / 'save').write_text('changed')
        (copy / 'link').write_text('independent')
        self.assertEqual((source / 'save').read_text(), 'original')
        self.assertFalse((copy / 'link').is_symlink())
        (source / 'loop').symlink_to(source, target_is_directory=True)
        with self.assertRaises(ValueError):
            bench.clone_tree(source, self.root / 'bad-copy')

    def test_native_requires_matching_workload_metadata(self):
        native = self.root / 'native.json'
        report = {'metric': 'end_to_end_fixed_workload_wall_time', 'script_sha256': 'abc',
                  'runs': [{'variant': 'candidate', 'game_version': '1.2.0', 'elapsed_ns': 50}]}
        args = SimpleNamespace(workload='route', scene='village', frames=12)
        self.assertEqual(bench.native_comparison(None, report, args)['status'], 'unavailable')
        value = {'hardware': 'Switch model/clock profile', 'game_version': '1.2.0', 'workload': 'route',
                 'scene': 'village', 'frames': 12, 'script_sha256': 'abc', 'elapsed_ns_samples': [100, 110, 120],
                 'metric': 'end_to_end_fixed_workload_wall_time'}
        native.write_text(json.dumps(value))
        result = bench.native_comparison(native, report, args)
        self.assertEqual(result['status'], 'user_supplied_end_to_end_unverified')
        self.assertEqual(result['native_over_candidate'], 2.2)
        for key in value:
            changed = dict(value)
            del changed[key]
            native.write_text(json.dumps(changed))
            with self.subTest(key=key), self.assertRaises(ValueError):
                bench.native_comparison(native, report, args)

    def fixture(self, failure=''):
        source = self.root / 'source' / '.scarletnx'
        game = source / 'recompiled' / '0100000000000000'
        game.mkdir(parents=True)
        selected = game / '0100000000000000_0100000000000800v65536_code.so'
        selected.write_bytes(b'compiled update module')
        data = game / '0100000000000000_data' / 'save'
        data.mkdir(parents=True)
        (data / 'local').write_text('initial local save')
        global_save = source / 'save'
        global_save.mkdir()
        (global_save / 'global').write_text('initial global save')
        (source / 'prod.keys').write_text('private test key sentinel')
        config = self.root / 'external-config'
        config.mkdir()
        (config / '0100000000000000.ini').write_text('[Graphics]\nresolution_scale=2\n')
        (game / (bench.module_name(selected) + '.folders')).write_text(f'config={config}\nstate={config}\ncache={config}\n')
        runtime = self.root / 'arbitrary-runtime-name'
        runtime.write_text(f'''#!{sys.executable}
import pathlib, sys, time
args = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
module = pathlib.Path(args['game'])
assert module.is_absolute() and module.name.endswith('v65536_code.so')
app = next(p for p in module.parents if p.name == '.scarletnx')
local = module.parent / '0100000000000000_data/save/local'
global_save = app / 'save/global'
assert local.read_text() == 'initial local save'
assert global_save.read_text() == 'initial global save'
local.write_text('changed local save')
global_save.write_text('changed global save')
assert (module.parent / ('tas/' + module.name[:-8] + '.9.snxtas')).exists()
assert args['input_replay'] == '9' and args['script'] == ''
assert pathlib.Path(args['keys']).parent == app
assert pathlib.Path(args['keys']).read_text() == 'private test key sentinel'
{'time.sleep(5)' if failure == 'timeout' else ''}
{'sys.exit(1)' if failure == 'crash' else ''}
if {failure != 'noimage'!r}:
    pathlib.Path(args['shot'] + '.ppm').write_bytes({IMAGE!r})
print('game: 0100000000000000, version 1.2.0', file=sys.stderr)
frames = int(args['frames'])
if frames < 0:
    frames = 48 if pathlib.Path(__file__).name == 'candidate-runtime' else 24
    assert (module.parent / ('tas/' + module.name[:-8] + '.9.snxtas')).read_text() == '2147483646:0:0,0,0,0\\n'
    if {failure != 'nointerval'!r}:
        print('unattended_ns=2000000000', file=sys.stderr)
print('frames=' + str(frames) + ' hash=0123456789abcdef', file=sys.stderr)
{'module.write_bytes(module.read_bytes() + b"mutated")' if failure == 'modulechange' else ''}
{'with pathlib.Path(__file__).open("a") as stream: stream.write("# mutated\\n")' if failure == 'binarychange' else ''}
''')
        runtime.chmod(0o755)
        return selected, runtime, source, config

    def invoke(self, selected, runtime, output, timeout=10, uncapped=False, user_data=None):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return bench.main(['--game', str(selected), '--baseline', str(runtime), '--candidate', str(runtime),
                               '--frames', '12', '--rounds', '3', '--timeout', str(timeout), '--out', str(output),
                               *(['--uncapped'] if uncapped else []),
                               *(['--user-data', str(user_data)] if user_data else [])])

    def test_full_harness_preserves_source_saves_and_update_name(self):
        selected, runtime, source, config = self.fixture()
        output = self.root / 'results'
        self.assertEqual(self.invoke(selected, runtime, output, uncapped=True), 0)
        result = json.loads((output / 'results.json').read_text())
        self.assertEqual(result['status'], 'complete')
        self.assertEqual(len(result['runs']), 6)
        self.assertEqual(result['native_comparison']['status'], 'unavailable')
        self.assertEqual(result['snapshot']['keys_copied'], ['prod.keys'])
        self.assertNotIn('private test key sentinel', (output / 'results.json').read_text())
        self.assertEqual(result['pacing_policy'], 'fps_limit=0, vsync=0 for both builds')
        self.assertTrue(all('--fps_limit=0' in row['command'] and '--vsync=0' in row['command'] for row in result['runs']))
        self.assertEqual(result['snapshot']['settings_sha256'], {
            '0100000000000000.ini': hashlib.sha256(b'[Graphics]\nresolution_scale=2\n').hexdigest()})
        self.assertTrue(all(r['module_sha256'] == result['snapshot']['module_sha256'] for r in result['runs']))
        self.assertTrue(all(r['module_sha256_after'] == result['snapshot']['module_sha256'] for r in result['runs']))
        self.assertTrue(all(r['binary_sha256_after'] == result['binary_sha256'] for r in result['runs']))
        self.assertEqual((source / 'save/global').read_text(), 'initial global save')
        self.assertEqual((selected.parent / '0100000000000000_data/save/local').read_text(), 'initial local save')
        self.assertEqual(list(config.iterdir()), [config / '0100000000000000.ini'])
        self.assertTrue(result['summary']['final_screenshots_exact_every_pair'])
        # Existing reports must never be overwritten on an accidental rerun.
        before = (output / 'results.json').read_bytes()
        self.assertEqual(self.invoke(selected, runtime, output), 2)
        self.assertEqual((output / 'results.json').read_bytes(), before)

    def test_failed_runs_retain_evidence_and_cannot_win(self):
        for failure in ('crash', 'noimage', 'timeout', 'modulechange', 'binarychange'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                old = self.root
                self.root = Path(temporary)
                try:
                    selected, runtime, _, _ = self.fixture(failure)
                    output = self.root / 'results'
                    self.assertEqual(self.invoke(selected, runtime, output, .1 if failure == 'timeout' else 10), 2)
                    result = json.loads((output / 'results.json').read_text())
                    self.assertEqual(result['status'], 'rejected')
                    self.assertFalse(result['runs'])
                    self.assertNotIn('summary', result)
                    self.assertTrue(result['rejected'])
                    active = Path(result['active_run']['directory'])
                    self.assertTrue((active / 'stderr.log').is_file())
                finally:
                    self.root = old

    def test_recursive_output_destinations_rejected_before_creation(self):
        selected, runtime, source, config = self.fixture()
        for parent in (source, source / 'save', source / 'users', config, selected.parent):
            output = parent / 'benchmark-output'
            with self.subTest(parent=parent):
                self.assertEqual(self.invoke(selected, runtime, output), 2)
                self.assertFalse(output.exists())
        user_data = self.root / 'custom-user-data'
        output = user_data / 'save' / 'benchmark-output'
        self.assertEqual(self.invoke(selected, runtime, output, user_data=user_data), 2)
        self.assertFalse(user_data.exists())
        # clone_tree also guards against a directory link leading to the destination after initial validation.
        with self.assertRaises(ValueError):
            bench.clone_tree(source, source / 'new-snapshot')
        self.assertFalse((source / 'new-snapshot').exists())

    def test_external_debug_outputs_and_relative_source_rejected(self):
        selected, runtime, _, config = self.fixture()
        ini = config / '0100000000000000.ini'
        for key in sorted(bench.EXTERNAL_OUTPUT_SETTINGS):
            ini.write_text(f'\ufeff[Debug]\r\n {key} = /external/output \r\n')
            output = self.root / ('results-' + key)
            with self.subTest(key=key):
                self.assertEqual(self.invoke(selected, runtime, output), 2)
                self.assertFalse(output.exists())
        ini.write_text('[Game]\nsource=relative.xci\n')
        self.assertEqual(self.invoke(selected, runtime, self.root / 'relative-output'), 2)
        self.assertFalse((self.root / 'relative-output').exists())
        ini.write_text('[Game]\nsource=/absolute/immutable.xci\n[Debug]\nshader_corpus=\n; renderdoc=/ignored\n')
        bench.validate_settings(config, selected.parent.name)

    def test_timed_samples_compare_frame_rates_instead_of_wall_times(self):
        before = bench.validate_result(0, VALID + 'unattended_ns=1000000000\n', self.image, None, seconds=1)
        after = bench.validate_result(0, VALID.replace('frames=12', 'frames=24') + 'unattended_ns=1000000000\n',
                                      self.image, None, seconds=1)
        rows = [dict(before, variant='baseline', round=1, elapsed_ns=100),
                dict(after, variant='candidate', round=1, elapsed_ns=1000)]
        summary = bench.summarize_timed(rows)
        self.assertEqual(summary['metric'], 'game_interval_frame_throughput')
        self.assertEqual(summary['baseline_median_fps'], 12)
        self.assertEqual(summary['candidate_median_fps'], 24)
        self.assertEqual(summary['median_candidate_over_baseline_fps'], 2)
        self.assertNotIn('median_baseline_over_candidate', summary)
        self.assertEqual(bench.native_comparison(None, summary, SimpleNamespace())['status'], 'unavailable')
        with self.assertRaises(ValueError):
            bench.native_comparison(self.root / 'native.json', summary, SimpleNamespace())
        for text in (VALID, VALID + 'unattended_ns=0\n', VALID.replace('frames=12', 'frames=0') + 'unattended_ns=1\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench.validate_result(0, text, self.image, None)
        for seconds, interval in ((2, 1000000000), (1, 999999999), (None, 1000000000)):
            with self.subTest(seconds=seconds, interval=interval), self.assertRaises(ValueError):
                bench.validate_result(0, VALID + f'unattended_ns={interval}\n', self.image, None, seconds=seconds)

    def test_timed_harness_uses_neutral_input_and_retains_wall_time(self):
        selected, runtime, _, _ = self.fixture()
        candidate = self.root / 'candidate-runtime'
        bench.clone_file(runtime, candidate)
        output = self.root / 'timed-results'
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = bench.main(['--game', str(selected), '--baseline', str(runtime), '--candidate', str(candidate),
                               '--seconds', '2', '--rounds', '3', '--out', str(output)])
        self.assertEqual(code, 0)
        result = json.loads((output / 'results.json').read_text())
        self.assertEqual(result['metric'], 'game_interval_frame_throughput')
        self.assertEqual(result['runtime_seconds_limit'], 2)
        self.assertEqual(result['summary']['median_candidate_over_baseline_fps'], 2)
        self.assertEqual(result['native_comparison']['status'], 'unavailable')
        self.assertEqual(len(result['runs']), 6)
        self.assertTrue(all(r['elapsed_ns'] > 0 for r in result['runs']))
        self.assertTrue(all('--frames=-1' in r['command'] and '--seconds=2' in r['command'] for r in result['runs']))

    def test_timed_mode_rejects_invalid_limits_and_incompatible_options(self):
        basic = ['--game', 'unused', '--baseline', 'unused', '--candidate', 'unused', '--out', 'unused']
        for flags in (['--seconds', '0'], ['--seconds', '-1'], ['--seconds', 'nan'], ['--seconds', 'inf'],
                      ['--seconds', '1.5'], ['--seconds', '2147483648'], ['--seconds', '2', '--frames', '12'],
                      ['--seconds', '2', '--script', 'input.snxtas'], ['--seconds', '2', '--native', 'native.json']):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                bench.main([*basic, *flags])


if __name__ == '__main__':
    unittest.main()
