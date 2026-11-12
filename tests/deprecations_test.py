# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/check-deprecations.py on histories of its own: the keys settings.txt dropped that INI_DEPRECATED or
docs/user/deprecations.md does not name, the two lists apart, a deprecated key settings.txt still has, each named; how
settingsgen's keys are read; --since; and what it does without git or a checkout"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/check-deprecations.py'
spec = importlib.util.spec_from_file_location('check_deprecations', SCRIPT)
cd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cd)

SETTINGS = 'src/nx/runtime/settings.txt'
INI_C = 'src/nx/runtime/ini.c'
DOC = 'docs/user/deprecations.md'


def ini_c(*keys):
    rows = ''.join(f'    {{"{key}", "what became of it"}},\n' for key in keys)
    return f'/* the runtime */\nstatic const struct {{\n    const char *key, *now;\n}} INI_DEPRECATED[] = {{\n{rows}    {{NULL, NULL}}}};\n'


def doc(*rows):
    table = ''.join(f'| {row} | a change | nothing | the same |\n' for row in rows)
    return f'# Deprecations\n\n## Now\n\n| Old | Replaced by | Now | How it is told |\n|---|---|---|---|\n{table}\n## Later\n'


def settings(*keys):
    return 'table g_game_ini\n' + ''.join(f'setting {key}\n    key {key}\n    int 0 1\n    default 0\n' for key in keys)


class History:
    """a checkout in a temporary folder with a commit for each state given, the oldest first; git's own config only,
    so no hook, signing or identity of the machine's comes into it"""

    def __init__(self):
        self.folder = tempfile.TemporaryDirectory(prefix='snx-deprecations-test-')
        self.root = Path(self.folder.name) / 'checkout'
        self.root.mkdir()
        empty = Path(self.folder.name) / 'gitconfig'
        empty.write_text('')
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=str(empty), GIT_CONFIG_NOSYSTEM='1')
        for name in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE'):
            self.env.pop(name, None)
        self.env.update(
            GIT_AUTHOR_NAME='t',
            GIT_AUTHOR_EMAIL='t@t',
            GIT_COMMITTER_NAME='t',
            GIT_COMMITTER_EMAIL='t@t',
            GIT_AUTHOR_DATE='2026-01-02T03:04:05+00:00',
            GIT_COMMITTER_DATE='2026-01-02T03:04:05+00:00',
        )
        self.git('init', '-q')
        self.head = None

    def git(self, *args):
        run = subprocess.run(
            ['git', '-C', str(self.root), *args], check=False, capture_output=True, text=True, env=self.env
        )
        assert run.returncode == 0, run.stderr
        return run.stdout.strip()

    def write(self, files):
        for name, text in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8', newline='\n')

    def commit(self, subject, files):
        self.write(files)
        self.git('add', '-A')
        tree = self.git('write-tree')
        self.head = self.git('commit-tree', tree, '-m', subject, *(['-p', self.head] if self.head else []))
        self.git('update-ref', 'HEAD', self.head)
        return self.head

    def short(self, rev):
        """rev's hash as the script names a commit: git's %h"""
        return self.git('log', '-1', '--format=%h', rev)

    def run(self, *args):
        return subprocess.run(
            [sys.executable, str(SCRIPT), '--root', str(self.root), *args],
            check=False,
            capture_output=True,
            text=True,
            env=self.env,
        )

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.folder.cleanup()


@unittest.skipUnless(shutil.which('git'), 'git is not on PATH')
class Dropped(unittest.TestCase):
    def test_a_dropped_key_both_name_passes(self):
        with History() as h:
            h.commit('Add a and b', {SETTINGS: settings('a', 'b'), INI_C: ini_c(), DOC: doc()})
            h.commit('Drop b', {SETTINGS: settings('a'), INI_C: ini_c('b'), DOC: doc('`.ini` key `b`')})
            run = h.run()
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn(f'3 states of {SETTINGS} read', run.stdout)
            self.assertIn('1 keys it dropped, 1 deprecated', run.stdout)

    def test_a_dropped_key_neither_names(self):
        with History() as h:
            h.commit('Add a and b', {SETTINGS: settings('a', 'b'), INI_C: ini_c(), DOC: doc()})
            h.commit('Drop b', {SETTINGS: settings('a')})
            run = h.run()
            self.assertEqual(run.returncode, 1)
            dropped = f'b: {SETTINGS} dropped it in {h.short("HEAD")}'
            self.assertIn(f'{INI_C}:4: {dropped}', run.stderr)
            self.assertIn('(2026-01-02 Drop b), and INI_DEPRECATED does not name it', run.stderr)
            self.assertIn(f'{DOC}:3: {dropped}', run.stderr)
            self.assertIn('no `.ini` key row of ## Now names it', run.stderr)
            self.assertIn(': 2 failures', run.stderr)

    def test_one_list_without_it(self):
        with History() as h:
            h.commit('Add a and b', {SETTINGS: settings('a', 'b'), INI_C: ini_c(), DOC: doc()})
            h.commit('Drop b', {SETTINGS: settings('a'), INI_C: ini_c('b'), DOC: doc('`.ini` key `c`')})
            failures, _ = cd.check(h.root)
            self.assertEqual(
                failures,
                [
                    (
                        f'{DOC}:3: b: {SETTINGS} dropped it in {h.short("HEAD")} (2026-01-02 Drop b), and no '
                        '`.ini` key row of ## Now names it'
                    ),
                    f'{DOC}:3: b: INI_DEPRECATED ({INI_C}:5) names it, no row of ## Now',
                    f'{INI_C}:4: c: {DOC}:7 names it, INI_DEPRECATED does not',
                ],
            )

    def test_a_deprecated_key_settings_has(self):
        with History() as h:
            h.commit('Add a', {SETTINGS: settings('a'), INI_C: ini_c('a'), DOC: doc('`.ini` key `a`')})
            failures, _ = cd.check(h.root)
            self.assertEqual(failures, [f'{INI_C}:5: a: deprecated, and {SETTINGS} has it: the runtime never reads it'])

    def test_a_deprecated_key_a_document_sets(self):
        with History() as h:
            h.commit('Add a', {SETTINGS: settings('a'), INI_C: ini_c('b'), DOC: doc('`.ini` key `b` (`b=1` was on)')})
            h.write(
                {
                    'docs/guide.md': '# G\n\nSet `b=1` to trace.\n\nThe `b` setting is gone; `a=1` is fine.\n',
                    'README.md': 'Run with `--b=2`.\n',
                }
            )
            failures, _ = cd.check(h.root)
            said = 'a deprecated key written as a setting, which the runtime does not read (docs/user/deprecations.md)'
            self.assertEqual(failures, [f'README.md:1: b=: {said}', f'docs/guide.md:3: b=: {said}'])

    def test_a_key_back_again_is_not_dropped(self):
        with History() as h:
            h.commit('Add a and b', {SETTINGS: settings('a', 'b'), INI_C: ini_c(), DOC: doc()})
            h.commit('Drop b', {SETTINGS: settings('a')})
            h.commit('b again', {SETTINGS: settings('a', 'b')})
            self.assertEqual(h.run().returncode, 0)

    def test_the_working_tree_drops_one(self):
        with History() as h:
            h.commit('Add a and b', {SETTINGS: settings('a', 'b'), INI_C: ini_c(), DOC: doc()})
            h.write({SETTINGS: settings('a')})
            failures, _ = cd.check(h.root)
            self.assertIn(
                f'{INI_C}:4: b: {SETTINGS} dropped it in the working tree, and INI_DEPRECATED does not name it',
                failures,
            )

    def test_since_reads_its_state_and_after(self):
        with History() as h:
            h.commit('Add a, b and c', {SETTINGS: settings('a', 'b', 'c'), INI_C: ini_c(), DOC: doc()})
            h.commit('Drop c', {SETTINGS: settings('a', 'b')})
            base = h.commit('Docs', {'README.md': 'x\n'})
            h.commit('Drop b', {SETTINGS: settings('a')})
            failures, said = cd.check(h.root, base)
            self.assertEqual([f for f in failures if ' c: ' in f], [])  # dropped before the base: not this span's
            self.assertEqual(len([f for f in failures if ' b: ' in f]), 2)
            self.assertIn(f'3 states of {SETTINGS} read, from {h.short(base)} (', said)
            self.assertEqual(len(cd.check(h.root)[0]), 4)  # the whole history: both


class Keys(unittest.TestCase):
    def test_as_settingsgen_makes_them(self):
        text = (
            '# a comment\ninclude <limits.h>\nlist pads a=A b=B_side\n'
            'table g_one\nsetting One\n    key one\n'
            'repeat 2\nsetting Pad\n    key pad<N>_map\nsetting Each\n    key pad<N>_<ID>\n    each pads\nend\n'
            'needs tracy\nsetting Zones\n    key tracy_zones\nend\n'
            'setting Button\n    key button_<ID>\n    each pads\n'
            'table g_launch launcher\nsetting Launch\n    key launch\n'
            'table g_llvm llvm\nsetting @instcombine\nsetting @LoopVectorization\n    key llvm_own\n'
            'table g_vk vulkan\nsetting @VK_EXT_line_rasterization\n'
            'section Game: g_one\n'
        )
        self.assertEqual(
            cd.settings_keys(text),
            {
                'one',
                'pad1_map',
                'pad2_map',
                'pad1_a',
                'pad1_b',
                'pad2_a',
                'pad2_b',
                'tracy_zones',
                'button_a',
                'button_b',
                'launch',
                'vk_ext_line_rasterization',
            },
        )

    def test_the_rows_of_now_and_their_code(self):
        with tempfile.TemporaryDirectory(prefix='snx-deprecations-test-') as folder:
            (Path(folder) / 'docs/user').mkdir(parents=True)
            (Path(folder) / DOC).write_text(
                '# D\n\n## Before\n\n| `.ini` key `early` | x | y | z |\n\n## Now\n\n| Old | a | b | c |\n'
                '|---|---|---|---|\n| `.ini` keys `a`, `b_2` (`0` Off, `line` or `--x`; (`nested`)) | x | y | z |\n'
                "| `--keep-c` | x | `--keep-ir` | z |\n| A theme's `[body] gap` | x | y | z |\n"
                '| `.ini` key `user` (the number) and `--user=N` | x | y | z |\n',
                encoding='utf-8',
            )
            keys, now = cd.doc_keys(Path(folder))
            self.assertEqual((keys, now), ({'a': 11, 'b_2': 11, 'user': 14}, 7))


class Refused(unittest.TestCase):
    def test_no_checkout(self):
        with tempfile.TemporaryDirectory(prefix='snx-deprecations-test-') as folder:
            env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(Path(folder).parent))
            run = subprocess.run(
                [sys.executable, str(SCRIPT), '--root', folder], check=False, capture_output=True, text=True, env=env
            )
            self.assertEqual(run.returncode, 77, run.stderr)
            self.assertIn(
                f'{folder} is no git checkout git can read (' if shutil.which('git') else 'git is not on PATH',
                run.stderr,
            )

    @unittest.skipUnless(shutil.which('git'), 'git is not on PATH')
    def test_no_table_in_ini_c(self):
        with History() as h:
            h.commit('Add a', {SETTINGS: settings('a'), INI_C: '/* nothing */\n', DOC: doc()})
            run = h.run()
            self.assertEqual(run.returncode, 2)
            self.assertEqual(run.stderr, f'{INI_C}: no INI_DEPRECATED[] = {{...{{NULL, NULL}}}}; in it\n')


if __name__ == '__main__':
    unittest.main()
