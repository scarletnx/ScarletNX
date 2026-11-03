# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/check-docs.py on checkouts of its own: the links, anchors, indexes and settings it refuses, each named."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/check-docs.py'
spec = importlib.util.spec_from_file_location('check_docs', SCRIPT)
cd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cd)

LONG = '\n'.join(f'Line {n}.' for n in range(cd.INDEX_LINES))


class Checkout:
    """a checkout in a temporary folder, its files given as {path: text}"""

    def __init__(self, files):
        self.folder = tempfile.TemporaryDirectory(prefix='snx-docs-test-')
        self.root = Path(self.folder.name)
        base = {'docs/README.md': '# Docs\n'}
        base.update(files)
        for name, text in base.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')

    def failures(self, check):
        failures = cd.Failures()
        docs = cd.documents(self.root)
        if check == 'links':
            cd.check_links(self.root, docs, failures)
        elif check == 'indexes':
            cd.check_indexes(self.root, docs, failures, False)
        elif check == 'docs index':
            cd.check_docs_index(self.root, failures)
        else:
            cd.check_settings(self.root, failures)
        return list(failures)

    def run(self, *args):
        return subprocess.run(
            [sys.executable, str(SCRIPT), '--root', str(self.root), *args], capture_output=True, text=True
        )

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.folder.cleanup()


class Links(unittest.TestCase):
    def test_a_file_that_is_not_there(self):
        with Checkout({'README.md': '# X\n\nSee [build](docs/BUILD.md) and [usage](docs/USAGE.md).\n'}) as c:
            (c.root / 'docs/BUILD.md').write_text('# Build\n')
            self.assertEqual(c.failures('links'), ['README.md:3: docs/USAGE.md: docs/USAGE.md is not there'])

    def test_an_anchor_no_heading_gives(self):
        doc = '# A\n\n## Two words\n\n[here](#two-words) [there](B.md#nope) [gone](#two_words)\n'
        with Checkout({'docs/A.md': doc, 'docs/B.md': '# B\n\n## Yes\n'}) as c:
            self.assertEqual(
                c.failures('links'),
                [
                    'docs/A.md:5: #two_words: this document has no heading or name with the anchor #two_words',
                    'docs/A.md:5: B.md#nope: docs/B.md has no heading or name with the anchor #nope',
                ],
            )

    def test_anchors_as_github_makes_them(self):
        doc = (
            '# A\n\n## Extract → AOT → run\n\n## Settings / input\n\n## Notes\n\n## Notes\n\n'
            '### `gamepad_map` and *more*\n\n## [Linked](x.md) title\n\n<a name="kept"></a>\n\n'
            '[a](#extract--aot--run) [b](#settings--input) [c](#notes-1) [d](#gamepad_map-and-more)\n'
            '[e](#linked-title) [f](#kept) [g][ref]\n\n[ref]: #notes\n'
        )
        with Checkout({'docs/A.md': doc, 'docs/x.md': '# X\n'}) as c:
            self.assertEqual(c.failures('links'), [])

    def test_code_is_not_a_link(self):
        doc = '# A\n\n`[x](missing.md)` and\n\n```md\n[y](missing.md)\n```\n'
        with Checkout({'docs/A.md': doc}) as c:
            self.assertEqual(c.failures('links'), [])

    def test_html_targets_and_line_anchors(self):
        doc = (
            '# A\n\n<img src="../dist/logo.svg">\n<img src="../dist/gone.svg">\n'
            '[ok](../src/a.c#L2) [past](../src/a.c#L2-L9) [word](../src/a.c#main) [web](https://example.org/x.md)\n'
        )
        with Checkout({'docs/A.md': doc, 'dist/logo.svg': '<svg/>', 'src/a.c': 'int a;\nint b;\n'}) as c:
            self.assertEqual(
                c.failures('links'),
                [
                    'docs/A.md:4: ../dist/gone.svg: ../dist/gone.svg is not there',
                    'docs/A.md:5: ../src/a.c#L2-L9: line 9 is past the end of ../src/a.c (2 lines)',
                    'docs/A.md:5: ../src/a.c#main: #main on a file that is not Markdown takes #L<n> alone',
                ],
            )

    def test_a_link_text_broken_over_lines(self):
        with Checkout({'docs/A.md': '# A\n\nThe [long\nname](B.md) here.\n'}) as c:
            self.assertEqual(c.failures('links'), ['docs/A.md:4: B.md: B.md is not there'])


class Indexes(unittest.TestCase):
    def doc(self, index=''):
        return f'# Long\n\n{index}Intro.\n\n## One\n\n### One a\n\n## Two\n\n## Three\n\n{LONG}\n'

    def test_a_long_document_without_one(self):
        with Checkout({'docs/LONG.md': self.doc(), 'docs/SHORT.md': '# Short\n\n## A\n\n## B\n\n## C\n'}) as c:
            self.assertEqual(
                c.failures('indexes'),
                [
                    'docs/LONG.md: over 100 lines with 3 or more sections and no ## Index: '
                    'run scripts/check-docs.py --index'
                ],
            )

    def test_index_writes_it_once_and_then_holds(self):
        with Checkout({'docs/LONG.md': self.doc(), 'docs/README.md': '# Docs\n\n- [Long](LONG.md)\n'}) as c:
            self.assertEqual(c.run('--index').returncode, 0)
            text = (c.root / 'docs/LONG.md').read_text()
            self.assertIn(
                '# Long\n\n## Index\n\n* [One](#one)\n  * [One a](#one-a)\n* [Two](#two)\n* [Three](#three)\n\nIntro.',
                text,
            )
            self.assertEqual(c.failures('indexes'), [])
            c.run('--index')
            self.assertEqual((c.root / 'docs/LONG.md').read_text(), text)

    def test_a_stale_index_is_refused_and_made_again_where_it_stands(self):
        stale = 'Before it.\n\n## Index\n\n* [One](#one)\n\n'
        with Checkout({'docs/LONG.md': self.doc(stale)}) as c:
            self.assertEqual(
                c.failures('indexes'),
                ['docs/LONG.md: ## Index does not list its sections as they are: run scripts/check-docs.py --index'],
            )
            c.run('--index')
            self.assertIn(
                'Before it.\n\n## Index\n\n* [One](#one)\n  * [One a](#one-a)\n', (c.root / 'docs/LONG.md').read_text()
            )


class WindowsOrder(type(Path())):
    """a path of this system that sorts as a Windows path does, case folded"""

    def __lt__(self, other):
        return PureWindowsPath(self) < PureWindowsPath(other)


class DocsIndex(unittest.TestCase):
    FILES = {
        'docs/README.md': '# Docs\n\n- [Build](./BUILD.md)\n',
        'docs/BUILD.md': '# Build\n',
        'docs/USAGE.md': '# Usage\n',
        'docs/audit/one.md': '# One\n',
    }
    UNLINKED = ['docs/README.md: does not link USAGE.md', 'docs/README.md: does not link audit/one.md']

    def test_every_document_of_docs_is_linked(self):
        with Checkout(self.FILES) as c:
            self.assertEqual(c.failures('docs index'), self.UNLINKED)

    def test_the_same_order_on_windows(self):
        with Checkout(self.FILES) as c:
            root = WindowsOrder(c.root)
            self.assertEqual(
                [doc.relative_to(root).as_posix() for doc in cd.documents(root)],
                ['docs/BUILD.md', 'docs/README.md', 'docs/USAGE.md', 'docs/audit/one.md'],
            )
            failures = cd.Failures()
            cd.check_docs_index(root, failures)
            self.assertEqual(failures, self.UNLINKED)


SETTINGS = """# a comment
table g_game_ini
setting Docked
    key docked
    int 0 1
    default 1
setting Fit
    key memory_fit
    int 0 1
    default 0
    default-if SNX_IOS 1
setting Source
    key source
    text 64
repeat 8
setting Player rumble
    key gamepad_p<N>_rumble
    int -10 100
    default -10
end
setting Place
    key controls_<ID>
    each controls
    text 32
setting Words
    key mode
    text 8
    default a|b
table g_llvm_ini llvm
setting @loop-unroll
    name Loop unrolling pass
setting @InlinerThreshold
    name Inliner threshold
section Game: g_game_ini
"""

DOC = """# Settings

| Key | Default | Effect |
| --- | --- | --- |
| `docked`, `memory_fit` | `1`, `0` (iOS `1`) | Two keys, a default each |
| `source` | none | Empty text |
| `gamepad_p<N>_rumble` (N from 1 to 8) | `-10` | Repeated |
| `controls_<ID>` (ID one of `a`, `b`) | empty | Each |
| `mode` | `a\\|b` | A pipe |
| `llvm_pass_loop_unroll` | `1` | A pass |
| `llvm_inliner_threshold` | LLVM's own | An option |
"""


class Settings(unittest.TestCase):
    def checkout(self, doc=DOC, settings=SETTINGS):
        index = '# Docs\n\n- [Settings](SETTINGS.md)\n'
        return Checkout({'src/nx/runtime/settings.txt': settings, 'docs/SETTINGS.md': doc, 'docs/README.md': index})

    def test_a_doc_that_says_what_the_code_does(self):
        with self.checkout() as c:
            self.assertEqual(c.failures('settings'), [])

    def test_a_key_with_no_row(self):
        with self.checkout(DOC.replace('| `source` | none | Empty text |\n', '')) as c:
            self.assertEqual(
                c.failures('settings'),
                ['src/nx/runtime/settings.txt:12: `source` (default empty) has no row in docs/SETTINGS.md'],
            )

    def test_a_row_of_a_key_the_code_no_longer_has(self):
        with self.checkout(DOC + '| `docked_old` | `1` | Renamed |\n') as c:
            self.assertEqual(
                c.failures('settings'),
                ['docs/SETTINGS.md:12: `docked_old` is no key of src/nx/runtime/settings.txt'],
            )

    def test_a_default_the_code_does_not_have(self):
        doc = DOC.replace('`1`, `0` (iOS `1`)', '`0`, `0`')
        with self.checkout(doc) as c:
            self.assertEqual(
                c.failures('settings'),
                [
                    'docs/SETTINGS.md:5: `docked` says the default is `0`; src/nx/runtime/settings.txt:3 makes it `1`',
                    'docs/SETTINGS.md:5: `memory_fit` says the default is `0`; '
                    'src/nx/runtime/settings.txt:7 makes it `0` (iOS `1`)',
                ],
            )

    def test_a_default_cell_it_cannot_read(self):
        with self.checkout(DOC.replace('| `source` | none |', '| `source` | nothing |')) as c:
            self.assertEqual(len(c.failures('settings')), 2)
            self.assertTrue(c.failures('settings')[0].startswith("docs/SETTINGS.md:6: Default 'nothing' is not one"))

    def test_a_platform_it_does_not_know(self):
        with self.checkout(settings=SETTINGS.replace('default-if SNX_IOS 1', 'default-if SNX_PLAN9 1')) as c:
            self.assertEqual(
                c.failures('settings'),
                [
                    'src/nx/runtime/settings.txt:7: memory_fit: default-if SNX_PLAN9 names no platform '
                    'scripts/check-docs.py knows (SNX_IOS, __ANDROID__)'
                ],
            )

    def test_the_command_line_says_each_and_fails(self):
        with self.checkout(DOC.replace('`-10`', '`0`')) as c:
            run = c.run()
            self.assertEqual(run.returncode, 1)
            self.assertEqual(
                run.stderr,
                'docs/SETTINGS.md:7: `gamepad_p<N>_rumble` says the default is `0`; '
                'src/nx/runtime/settings.txt:16 makes it `-10`\ncheck-docs.py: 1 to fix\n',
            )


if __name__ == '__main__':
    unittest.main()
