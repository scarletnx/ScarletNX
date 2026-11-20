# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/build-glyphs.py: the Latin letters, the wide letters the languages show and the titles' blocks it takes from
a Unifont .hex, what it refuses, and the C. SNX_UNIFONT names the Unifont the build downloaded, whose glyphs are
checked too (ctest sets it)."""

import gzip
import hashlib
import importlib.util
import os
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


def lines(glyphs):
    """glyphs, {code point: bitmap as hex}, as a .hex's lines in order"""
    return ''.join(f'{cp:04X}:{rows}\n' for cp, rows in sorted(glyphs.items()))


TITLES = [glyph(cp, True) for cp in bg.titles_wanted()]  # a font's titles' blocks, every one 16x16
# GNU Unifont 18.0.01's glyphs as .hex lines, in order: the Latin letters and the titles' blocks the menu draws
LATIN_SHA256 = '44e106d08046a28a95eaf1642bf91089517e0460a72ba98a892daf5c5cc9745d'
TITLES_SHA256 = '7df45cb8a9ab89ba9bb388ca01f6a151962232ddf880dea74b7bdddab9470a1a'


class Glyphs(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix='snx-glyphs-')
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        (self.root / 'src/common/utilities').mkdir(parents=True)
        shutil.copy(ROOT / bg.FONT, self.root / bg.FONT)  # no .po: no language shows a wide letter
        self.unifont = self.root / 'unifont.hex.gz'
        self.latin = [glyph(cp) for cp in bg.wanted(self.root)]

    def run_script(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), '--root', str(self.root), *args], capture_output=True)

    def write_unifont(self, lines, latin=True, titles=True):
        """a Unifont .hex of lines, then (latin) the Latin letters' and (titles) the titles' blocks' unless lines has
        them already"""
        points = {int(line.split(':')[0], 16) for line in lines if ':' in line}
        more = (self.latin if latin else []) + (TITLES if titles else [])
        with gzip.open(self.unifont, 'wt', encoding='ascii') as f:
            f.writelines(lines + [g for g in more if int(g[: g.index(':')], 16) not in points])

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

    def test_the_letters_wanted(self):
        points = bg.wanted(ROOT)
        self.assertEqual(points, sorted(points))
        self.assertTrue({0xE3, 0xF5, 0xD5, 0x152, 0x219, 0x24F} <= set(points))  # the tilde's a and o, OE, s comma
        self.assertFalse(set(points) & bg.codepage(ROOT))  # CP437's stay its own
        self.assertNotIn(0xAD, points)  # the soft hyphen, 16 wide in Unifont
        self.assertEqual(len(points), 375)
        wide = bg.wide_wanted(ROOT)
        self.assertEqual(wide, sorted(wide))
        self.assertFalse(set(wide) & (bg.codepage(ROOT) | set(points) | set(bg.titles_wanted())))
        self.assertTrue({0x83DC, 0x5355} <= set(bg.titles_wanted()))  # tests/po/xx.po's Menu: the titles' glyphs

    def test_the_glyphs_of_the_build(self):
        unifont = Path(os.environ.get('SNX_UNIFONT', ''))
        self.assertTrue(unifont.is_file(), f"SNX_UNIFONT={unifont} names no file: ctest gives it the build's Unifont")
        glyphs = bg.read_unifont(unifont)
        latin = bg.latin_of(glyphs, bg.wanted(ROOT), unifont)
        self.assertEqual(hashlib.sha256(lines(latin).encode()).hexdigest(), LATIN_SHA256)
        self.assertTrue(all(len(rows) == 32 for rows in latin.values()))
        wide = bg.wide_of(glyphs, bg.wide_wanted(ROOT), unifont)  # each letter the languages show, 16x16
        self.assertEqual(sorted(wide), bg.wide_wanted(ROOT))
        kept = bg.titles_of(glyphs, unifont)
        self.assertEqual(hashlib.sha256(lines(kept).encode()).hexdigest(), TITLES_SHA256)
        self.assertEqual(list(kept), bg.titles_wanted())
        self.assertEqual(len(kept), 32772)  # Unifont 18.0.01's: 32649 16x16, 123 8x16
        narrow = {cp for cp, rows in kept.items() if len(rows) == 32}
        self.assertEqual(len(narrow), 123)
        self.assertTrue({0x303F, 0xFF61, 0xFF9F, 0xFFE8} <= narrow)  # the halfwidth forms, and U+303F
        self.assertFalse({0x3000, 0x3042, 0x30AB, 0x4E00, 0x9FFF, 0xAC00, 0xD7A3, 0xFF21} & narrow)
        self.assertFalse(set(kept) & (bg.codepage(ROOT) | set(bg.wanted(ROOT))))
        self.assertNotIn(0x3400, kept)  # no Extension A

    def test_the_c(self):
        points = bg.wanted(self.root)
        self.write_unifont([glyph(0x41), glyph(0xAD, True)])  # ASCII and the soft hyphen are not taken
        self.assertEqual(self.run_script('--check', '--from', str(self.unifont)).returncode, 0)
        out = self.root / 'gui_latin.c'
        self.assertEqual(self.run_script('--from', str(self.unifont), '--out', str(out)).returncode, 0)
        text = out.read_text()
        self.assertTrue(text.startswith('/* Generated by scripts/build-glyphs.py from unifont.hex.gz: GNU Unifont,'))
        self.assertIn(f'const int gui_latin_count = {len(points)};', text)
        self.assertIn('0x00e3,', text)
        self.assertNotIn('0x00ad,', text)
        self.assertIn(' '.join(f'0x{b:02x},' for b in bg.fit(bytes([0xE3] * 16))) + ' /* U+00E3 */', text)
        # no language shows a wide letter: arrays of one, C having no empty ones
        self.assertIn('const int gui_wide_count = 0;\nconst uint32_t gui_wide_points[1] = {0};', text)
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

    def test_the_wide_letters_and_the_c(self):
        # what the languages show: a name and translations, the menu's and the tests', not a fuzzy one nor ASCII nor
        # a letter CP437, the Latin ones or the titles' blocks draw (U+83DC); Yi syllables, as Unifont draws them wide
        self.write_po('po', 'ii', [('Menu', 'ꀀꀁ菜', False), ('Exit', 'ꀐ', True)], 'ꀂꀃ')
        self.write_po('tests/po', 'xx', [('Back', 'Voltar\\nã ç ꀄ', False)])
        points = [0xA004, 0xA002, 0xA003, 0xA000, 0xA001]
        self.assertEqual(bg.wide_wanted(self.root), sorted(points))
        self.write_unifont([glyph(cp, True) for cp in points + [0xA010] if cp != 0xA002] + [halves(0xA002)])
        out = self.root / 'gui_glyphs.c'
        self.assertEqual(self.run_script('--from', str(self.unifont), '--out', str(out)).returncode, 0)
        text = out.read_text()
        self.assertIn('const int gui_wide_count = 5;', text)
        self.assertIn('0x00a000, 0x00a001, 0x00a002, 0x00a003, 0x00a004,', text)
        self.assertNotIn('0x00a010', text)  # the fuzzy one's
        self.assertIn(' '.join(f'0x{r:02x},' for r in range(16)) + ' /* U+A002, left */', text)
        self.assertIn(' '.join(f'0x{r | 0x80:02x},' for r in range(16)) + ' /* U+A002, right */', text)
        self.assertTrue(text.isascii())
        # Latin's left out is not a wide one
        self.write_po('po', 'ii', [('Menu', 'ꀀ­', False)])
        (self.root / 'tests/po/xx.po').unlink()
        self.assertEqual(bg.wide_wanted(self.root), [0xA000])

    def test_titles_and_the_c(self):
        # the titles' blocks whatever the languages show: a halfwidth form 8x16, an ideograph whose halves differ
        self.write_unifont([halves(0x4E00), glyph(0xFF61)])
        out = self.root / 'gui_titles.c'
        self.assertEqual(self.run_script('--from', str(self.unifont), '--titles-out', str(out)).returncode, 0)
        text = out.read_text()
        self.assertTrue(text.startswith('/* Generated by scripts/build-glyphs.py from unifont.hex.gz: GNU Unifont,'))
        self.assertIn('const int gui_title_run_count = 8;', text)  # the blocks apart, the halfwidth one in three
        self.assertIn('    {0x003000, 0x0030ff, 1, 0},\n    {0x003130, 0x00318f, 1, 256},\n', text)
        self.assertIn('{0x00ff00, 0x00ff60, 1, 32532},\n    {0x00ff61, 0x00ff61, 0, 0},\n', text)
        self.assertIn('    {0x00ff62, 0x00ffef, 1, 32629},\n};', text)
        self.assertIn('const int gui_title_narrow_count = 1;\nconst int gui_title_wide_count = 32771;', text)
        self.assertIn(' '.join(['0x61,'] * 16) + ' /* U+FF61 */', text)
        left, right = (' '.join(f'0x{r | top:02x},' for r in range(16)) for top in (0, 0x80))
        self.assertIn(f'{left} {right} /* U+4E00 */', text)
        self.assertTrue(text.isascii())

    def test_refuses(self):
        unifont = str(self.unifont)
        out = str(self.root / 'gui_glyphs.c')
        at = bg.titles_wanted().index(0x4E01)
        first = bg.wanted(self.root)[0]
        # each font for every output: a Latin letter it lacks or draws 16 wide, a line no .hex has, a title's glyph
        for font, titles, says in (
            (self.latin[1:], True, f'unifont.hex.gz: no glyph for U+{first:04X}, a Latin letter'),
            ([glyph(first, True)] + self.latin[1:], True, f'U+{first:04X} is not 8x16 in it: add it to LEFT_OUT'),
            (self.latin + ['not a glyph\n'], True, 'not a line of a Unifont .hex'),
            (self.latin + TITLES[:at] + TITLES[at + 1 :], False, 'no glyph for U+4E01, which TITLE_RANGES keep'),
            (self.latin + TITLES[:-1], False, 'no glyph for U+FFEF, which TITLE_RANGES keep'),
        ):
            self.write_unifont(font, latin=False, titles=titles)
            for args in (('--check',), ('--out', out), ('--titles-out', out)):
                self.refused(says, '--from', unifont, *args)
        self.assertFalse((self.root / 'gui_glyphs.c').exists())
        # the wide letters a language shows: one the font lacks, and one it draws 8 wide (a block for RANGES)
        self.write_po('po', 'ii', [('Menu', 'ꀀꀁ', False)])
        self.write_unifont([glyph(0xA000, True)])
        self.refused('no glyph for U+A001, which a language shows', '--from', unifont, '--check')
        self.write_po('po', 'ii', [('Menu', 'ꀀ …', False)])
        self.write_unifont([glyph(0xA000, True), glyph(0x2026)])
        self.refused('U+2026 is 8x16 in it, not 16x16: its block goes in RANGES', '--from', unifont, '--out', out)
        (self.root / 'po/ii.po').write_text('msgid "x"\nmsgctxt "y"\n')  # a .po the languages' script refuses
        self.refused('build-glyphs.py: ', '--from', unifont, '--check')
        # the font is always named, --check writes nothing, and the old option of the titles' font is gone
        (self.root / 'po/ii.po').unlink()
        self.refused('the following arguments are required: --from', '--out', out)
        self.refused('one or the other', '--from', unifont)
        self.refused('one or the other', '--from', unifont, '--check', '--out', out)
        self.refused('unrecognized arguments: --titles-from', '--from', unifont, '--titles-from', unifont, '--check')


if __name__ == '__main__':
    unittest.main()
