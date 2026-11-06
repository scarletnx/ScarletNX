# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/build-glyphs.py: the Latin letters, the titles' blocks and the wide letters it keeps from a Unifont .hex,
the check of what it kept, and the C."""

import gzip
import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/build-glyphs.py'
spec = importlib.util.spec_from_file_location('build_glyphs', SCRIPT)
bg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bg)


def glyph(cp, wide=False):
    """a .hex line, its rows numbered after the code point"""
    return f'{cp:04X}:' + f'{cp & 0xFF:02X}' * (32 if wide else 16) + '\n'


def halves(cp):
    """a 16x16 .hex line whose halves differ: row r is r on the left, r + 0x80 on the right"""
    return f'{cp:04X}:' + ''.join(f'{r:02X}{r | 0x80:02X}' for r in range(16)) + '\n'


TITLES = [glyph(cp, True) for cp in bg.titles_wanted()]  # a font's titles' blocks, every one 16x16


class Glyphs(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix='snx-glyphs-')
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        (self.root / 'src/gui').mkdir(parents=True)
        (self.root / 'src/common/utilities').mkdir(parents=True)
        shutil.copy(ROOT / bg.FONT, self.root / bg.FONT)
        (self.root / bg.WIDE_KEPT).write_text('')  # no language shows a wide letter
        (self.root / bg.TITLES_KEPT).write_text(''.join(TITLES))
        self.unifont = self.root / 'unifont.hex.gz'

    def run_script(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), '--root', str(self.root), *args], capture_output=True)

    def write_unifont(self, lines, titles=True):
        """a Unifont .hex of lines, then (titles) the titles' blocks' unless lines has them already"""
        points = {int(line.split(':')[0], 16) for line in lines if ':' in line}
        with gzip.open(self.unifont, 'wt', encoding='ascii') as f:
            f.writelines(lines + [g for g in TITLES if titles and int(g[: g.index(':')], 16) not in points])

    def refused(self, says, *args):
        """the script run with args fails, saying says"""
        run = self.run_script(*args)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(says, run.stderr.decode())

    def write_po(self, folder, code, entries, name='Test'):
        """a .po of entries [(msgid, msgstr, fuzzy)] in folder (po or tests/po), named name"""
        (self.root / folder).mkdir(parents=True, exist_ok=True)
        text = f'msgid ""\nmsgstr ""\n"Language: {code}\\n"\n"X-Language-Name: {name}\\n"\n\n'
        for msgid, msgstr, fuzzy in entries:
            text += ('#, fuzzy\n' if fuzzy else '') + f'msgid "{msgid}"\nmsgstr "{msgstr}"\n\n'
        (self.root / folder / f'{code}.po').write_text(text, encoding='utf-8')

    def test_the_letters_kept(self):
        points = bg.wanted(ROOT)
        self.assertEqual(points, sorted(points))
        self.assertTrue({0xE3, 0xF5, 0xD5, 0x152, 0x219, 0x24F} <= set(points))  # the tilde's a and o, OE, s comma
        self.assertFalse(set(points) & bg.codepage(ROOT))  # CP437's stay its own
        self.assertNotIn(0xAD, points)  # the soft hyphen, 16 wide in Unifont
        self.assertEqual((ROOT / bg.KEPT).read_text().count('\n'), len(points))

    def test_from_check_and_the_c(self):
        points = bg.wanted(self.root)
        self.write_unifont([glyph(0x41)] + [glyph(cp) for cp in points] + [glyph(0xAD, True), glyph(0x4E00, True)])
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        kept = (self.root / bg.KEPT).read_text()
        self.assertEqual(kept, ''.join(glyph(cp) for cp in points))
        self.assertEqual(self.run_script('--check').returncode, 0)
        out = self.root / 'gui_latin.c'
        self.assertEqual(self.run_script('--out', str(out)).returncode, 0)
        text = out.read_text()
        self.assertIn(f'const int gui_latin_count = {len(points)};', text)
        self.assertIn('0x00e3,', text)
        self.assertIn(' '.join(f'0x{b:02x},' for b in bg.fit(bytes([0xE3] * 16))) + ' /* U+00E3 */', text)
        self.assertTrue(text.isascii())

    def test_fit(self):
        def rows(*pattern):  # (row, bits) pairs, the rest blank
            out = [0] * 16
            for r, bits in pattern:
                out[r] = bits
            return bytes(out)

        # a small letter: up the two blank rows to the VGA's baseline (row 11), its stems doubled leftwards
        o = [(6, 0x3C), (7, 0x42), (8, 0x42), (9, 0x42), (10, 0x42), (11, 0x42), (12, 0x42), (13, 0x3C)]
        self.assertEqual(bg.fit(rows(*o)), rows(*[(r - 2, b | b << 1) for r, b in o]))
        # a capital with its mark in the top rows: one of the two blank rows under the mark, then a repeated row of
        # its body (the lowest), as the VGA font's accented capitals
        mark = [(0, 0x32), (1, 0x4C)]
        body = [(4, 0x18), (5, 0x24), (6, 0x24), (7, 0x42), (8, 0x42), (9, 0x7E), (10, 0x42), (11, 0x42), (12, 0x42)]
        body.append((13, 0x42))
        fitted = bg.fit(rows(*mark, *body))
        doubled = [(r, (b | b << 1) & 0xFF) for r, b in mark + body]
        self.assertEqual(fitted[:3], bytes([doubled[0][1], doubled[1][1], 0]))  # the mark, one blank row
        self.assertEqual(fitted[3:12], bytes(b for _, b in doubled[2:10] + doubled[11:]))  # a 0x42 row fewer
        self.assertEqual(fitted[12:], bytes(4))
        # no blank row and no repeated one: it stays where it is
        full = bytes(range(1, 17))
        self.assertEqual(bg.fit(full), bytes((b | b << 1) & 0xFF for b in full))
        self.assertEqual(len(bg.fit(rows())), 16)

    def test_the_wide_letters_kept(self):
        points = bg.wide_wanted(ROOT)
        kept = bg.read_hex((ROOT / bg.WIDE_KEPT).read_text().splitlines(), bg.WIDE_KEPT)
        self.assertEqual(sorted(kept), points)
        self.assertTrue(all(len(rows) == 64 for rows in kept.values()))
        self.assertFalse(set(points) & (bg.codepage(ROOT) | set(bg.wanted(ROOT)) | set(bg.titles_wanted())))
        self.assertTrue({0x83DC, 0x5355} <= set(bg.titles_wanted()))  # tests/po/xx.po's Menu: the titles' glyphs

    def test_the_titles_kept(self):
        kept = bg.read_hex((ROOT / bg.TITLES_KEPT).read_text().splitlines(), bg.TITLES_KEPT)
        self.assertEqual(sorted(kept), bg.titles_wanted())
        self.assertEqual(len(kept), 32772)  # Unifont 18.0.01's: 32649 16x16, 123 8x16
        narrow = {cp for cp, rows in kept.items() if len(rows) == 32}
        self.assertEqual(len(narrow), 123)
        self.assertTrue({0x303F, 0xFF61, 0xFF9F, 0xFFE8} <= narrow)  # the halfwidth forms, and U+303F
        self.assertFalse({0x3000, 0x3042, 0x30AB, 0x4E00, 0x9FFF, 0xAC00, 0xD7A3, 0xFF21} & narrow)
        self.assertFalse(set(kept) & (bg.codepage(ROOT) | set(bg.wanted(ROOT))))
        self.assertNotIn(0x3400, kept)  # no Extension A

    def test_wide_from_check_and_the_c(self):
        latin = bg.wanted(self.root)
        # what the languages show: a name and translations, the menu's and the tests', not a fuzzy one nor ASCII nor
        # a letter CP437, the Latin ones or the titles' blocks draw (U+83DC); Yi syllables, as Unifont draws them wide
        self.write_po('po', 'ii', [('Menu', '\ua000\ua001\u83dc', False), ('Exit', '\ua010', True)], '\ua002\ua003')
        self.write_po('tests/po', 'xx', [('Back', 'Voltar\\n\u00e3 \u00e7 \ua004', False)])
        points = [0xA004, 0xA002, 0xA003, 0xA000, 0xA001]
        self.assertEqual(bg.wide_wanted(self.root), sorted(points))
        unifont = [glyph(cp) for cp in latin] + [glyph(cp, True) for cp in points + [0xA010] if cp != 0xA002]
        unifont.append(halves(0xA002))
        self.write_unifont(unifont)
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        kept = [halves(cp) if cp == 0xA002 else glyph(cp, True) for cp in sorted(points)]
        self.assertEqual((self.root / bg.WIDE_KEPT).read_text(), ''.join(kept))
        self.assertEqual(self.run_script('--check').returncode, 0)
        out = self.root / 'gui_glyphs.c'
        self.assertEqual(self.run_script('--out', str(out)).returncode, 0)
        text = out.read_text()
        self.assertIn('const int gui_wide_count = 5;', text)
        self.assertIn('0x00a000, 0x00a001, 0x00a002, 0x00a003, 0x00a004,', text)
        self.assertIn(' '.join(f'0x{r:02x},' for r in range(16)) + ' /* U+A002, left */', text)
        self.assertIn(' '.join(f'0x{r | 0x80:02x},' for r in range(16)) + ' /* U+A002, right */', text)
        self.assertTrue(text.isascii())
        # none shown: arrays of one, C having no empty ones
        (self.root / 'po/ii.po').unlink()
        (self.root / 'tests/po/xx.po').unlink()
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        self.assertEqual((self.root / bg.WIDE_KEPT).read_text(), '')
        self.assertEqual(self.run_script('--out', str(out)).returncode, 0)
        self.assertIn('const int gui_wide_count = 0;\nconst uint32_t gui_wide_points[1] = {0};', out.read_text())

    def test_wide_check_refuses(self):
        latin = bg.wanted(self.root)
        self.write_unifont([glyph(cp) for cp in latin])
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        self.write_po('po', 'ii', [('Menu', '\ua000\ua001', False)])
        wide = self.root / bg.WIDE_KEPT
        lines = [glyph(0xA000, True), glyph(0xA001, True)]
        wide.write_text(''.join(lines))
        self.assertEqual(self.run_script('--check').returncode, 0)
        for broken, says in (
            (lines[1:], 'no glyph for U+A000, which a language shows'),  # a letter the .po took up
            (lines + [glyph(0xA010, True)], 'U+A010 is in it, which no language shows'),
            (lines + [glyph(0xAC00, True)], f'U+AC00 is in it, which {bg.TITLES_KEPT} holds'),
            (lines[::-1], ":1: 'A001:01' where the line of U+A000 goes"),
            (lines + lines[1:], '3 lines, not the 2 of its glyphs'),  # one twice
            (lines[:1] + [glyph(0xA001)], 'U+A001 is 8x16 in it, not 16x16'),
        ):
            wide.write_text(''.join(broken))
            self.refused(says, '--check')

    def test_wide_from_refuses(self):
        latin = [glyph(cp) for cp in bg.wanted(self.root)]
        self.write_po('po', 'ii', [('Menu', '\ua000\ua001', False)])
        self.write_unifont(latin + [glyph(0xA000, True)])  # one the font lacks
        self.refused('no glyph for U+A001, which a language shows', '--from', str(self.unifont))
        self.write_po('po', 'ii', [('Menu', '\ua000 \u2026', False)])  # one it draws 8 wide: a block for RANGES
        self.write_unifont(latin + [glyph(0xA000, True), glyph(0x2026)])
        self.refused('U+2026 is 8x16 in it, not 16x16: its block goes in RANGES', '--from', str(self.unifont))
        self.write_po('po', 'ii', [('Menu', '\ua000\u00ad', False)])  # Latin's left out: not a wide one
        self.write_unifont(latin + [glyph(0xA000, True), glyph(0xAD, True)])
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        self.assertEqual((self.root / bg.WIDE_KEPT).read_text(), glyph(0xA000, True))
        (self.root / 'po/ii.po').write_text('msgid "x"\nmsgctxt "y"\n')  # a .po the languages' script refuses
        self.refused('build-glyphs.py: ', '--from', str(self.unifont))

    def test_titles_from_check_and_the_c(self):
        latin = [glyph(cp) for cp in bg.wanted(self.root)]
        # the titles' blocks whatever the languages show: a halfwidth form 8x16, an ideograph whose halves differ
        self.write_unifont(latin + [halves(0x4E00), glyph(0xFF61)])
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        kept = [halves(cp) if cp == 0x4E00 else glyph(cp, cp != 0xFF61) for cp in bg.titles_wanted()]
        self.assertEqual((self.root / bg.TITLES_KEPT).read_text(), ''.join(kept))
        self.assertEqual((self.root / bg.WIDE_KEPT).read_text(), '')
        self.assertEqual(self.run_script('--check').returncode, 0)
        self.assertEqual(self.run_script('--check', '--from', str(self.unifont)).returncode, 0)
        out = self.root / 'gui_titles.c'
        self.assertEqual(self.run_script('--titles-out', str(out)).returncode, 0)
        text = out.read_text()
        self.assertIn('const int gui_title_run_count = 8;', text)  # the blocks apart, the halfwidth one in three
        self.assertIn('    {0x003000, 0x0030ff, 1, 0},\n    {0x003130, 0x00318f, 1, 256},\n', text)
        self.assertIn('{0x00ff00, 0x00ff60, 1, 32532},\n    {0x00ff61, 0x00ff61, 0, 0},\n', text)
        self.assertIn('    {0x00ff62, 0x00ffef, 1, 32629},\n};', text)
        self.assertIn('const int gui_title_narrow_count = 1;\nconst int gui_title_wide_count = 32771;', text)
        self.assertIn(' '.join(['0x61,'] * 16) + ' /* U+FF61 */', text)
        left, right = (' '.join(f'0x{r | top:02x},' for r in range(16)) for top in (0, 0x80))
        self.assertIn(f'{left} {right} /* U+4E00 */', text)
        self.assertTrue(text.isascii())

    def test_titles_check_refuses(self):
        self.write_unifont([glyph(cp) for cp in bg.wanted(self.root)])
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        titles = self.root / bg.TITLES_KEPT
        self.assertEqual(titles.read_text(), ''.join(TITLES))
        self.assertEqual(self.run_script('--check').returncode, 0)
        at = bg.titles_wanted().index(0x4E01)
        for broken, says in (
            (TITLES[:at] + TITLES[at + 1 :], f'{bg.TITLES_KEPT}: no glyph for U+4E01, which TITLE_RANGES keep'),
            (TITLES[:-1], 'no glyph for U+FFEF, which TITLE_RANGES keep'),
            (TITLES + [glyph(0xA000, True)], f'{bg.TITLES_KEPT}: 32773 lines, not the 32772 of its glyphs'),
            (
                TITLES[:at] + TITLES[at + 1 : at + 2] + TITLES[at : at + 1] + TITLES[at + 2 :],
                f"{bg.TITLES_KEPT}:{at + 1}: '4E02:02' where the line of U+4E01 goes",
            ),
        ):
            titles.write_text(''.join(broken))
            self.refused(says, '--check')

    def test_check_from_compares_the_glyphs(self):
        latin = [glyph(cp) for cp in bg.wanted(self.root)]
        self.write_unifont(latin)
        self.assertEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        titles = self.root / bg.TITLES_KEPT
        titles.write_text(titles.read_text().replace(glyph(0xAC00, True), halves(0xAC00)))  # its rows, not Unifont's
        self.assertEqual(self.run_script('--check').returncode, 0)  # the code points, widths and order are right
        self.refused(f'{bg.TITLES_KEPT} is not what unifont.hex.gz gives', '--check', '--from', str(self.unifont))
        self.assertEqual(titles.read_text().count(halves(0xAC00)), 1)  # --check writes nothing
        self.write_unifont(latin + [g for g in TITLES if not g.startswith('AC00:')], titles=False)
        self.refused('no glyph for U+AC00, which TITLE_RANGES keep', '--from', str(self.unifont))

    def test_check_refuses(self):
        points = bg.wanted(self.root)
        kept = self.root / bg.KEPT
        lines = [glyph(cp) for cp in points]
        for broken in (
            lines[1:],  # one missing
            lines + [glyph(0x4E00)],  # one more
            lines[1:2] + lines[:1] + lines[2:],  # out of order
            [glyph(points[0], True)] + lines[1:],  # one 16 wide
        ):
            kept.write_text(''.join(broken))
            run = self.run_script('--check')
            self.assertNotEqual(run.returncode, 0)
            # the file by its path in the repository, the same on every system (a folder's own has \\ on Windows)
            self.assertTrue(run.stderr.decode().startswith(bg.KEPT), run.stderr.decode())

    def test_from_refuses(self):
        points = bg.wanted(self.root)
        self.write_unifont([glyph(cp) for cp in points[1:]])  # one the font lacks
        self.assertNotEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        self.write_unifont([glyph(cp, cp == points[0]) for cp in points])  # one it draws 16 wide
        self.assertNotEqual(self.run_script('--from', str(self.unifont)).returncode, 0)
        self.write_unifont([glyph(cp) for cp in points] + ['not a glyph\n'])
        self.assertNotEqual(self.run_script('--from', str(self.unifont)).returncode, 0)


if __name__ == '__main__':
    unittest.main()
