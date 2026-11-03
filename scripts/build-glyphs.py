#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The letters the menu draws past its VGA font's (CP437): GNU Unifont's glyphs.

--from UNIFONT.hex keeps of a Unifont .hex (unifont-<version>.hex or unifont_all-<version>.hex, gunzipped or not) its
8x16 glyphs of U+00A0 to U+024F that CP437 has not, in src/gui/unifont-latin.hex; every glyph of TITLE_RANGES, the
CJK and Hangul blocks game titles are written in, whatever the languages show, in src/gui/unifont-titles.hex; and its
16x16 glyphs of the other characters the menu's languages show (the names and translations of po/*.po and
tests/po/*.po), two columns wide, in src/gui/unifont-wide.hex. --check fails when those files are not what --from
keeps: their code points, widths and order, and with --from their glyphs too, compared with that .hex. --out FILE
writes the C source of the Latin and the wide glyphs, for src/gui/latin.h (each fitted to the VGA font, fit) and
src/gui/wide.h; --titles-out FILE that of the titles' glyphs, for src/gui/titles.h.
"""

import argparse
import gzip
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KEPT = 'src/gui/unifont-latin.hex'
WIDE_KEPT = 'src/gui/unifont-wide.hex'
TITLES_KEPT = 'src/gui/unifont-titles.hex'
LANGUAGES = ('po', 'tests/po')  # the menu's, and the tests' own
FONT = 'src/common/utilities/font.c'  # gui_codepage: what CP437 draws, which stays its own
RANGES = [(0x00A0, 0x024F)]  # Latin-1 Supplement, Latin Extended-A and B
LEFT_OUT = {0x00AD}  # Unifont draws the soft hyphen 16 wide, as a box naming it: left out, as an invisible character
# the blocks a game's title is written in past the Latin letters, kept whole whatever the languages show (no CJK
# Unified Ideographs Extension A: rare ideographs, 6592 glyphs more). Unifont draws every code point of them, most
# 16x16 and some 8x16 (the halfwidth forms)
TITLE_RANGES = [
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0x3130, 0x318F),  # Hangul Compatibility Jamo
    (0x31F0, 0x31FF),  # Katakana Phonetic Extensions
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs
    (0xAC00, 0xD7A3),  # Hangul Syllables
    (0xFF00, 0xFFEF),  # Halfwidth and Fullwidth Forms
]


def codepage(root):
    text = (root / FONT).read_text()
    table = text[text.index('gui_codepage[256] = {') :]
    points = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{4})', table[: table.index('};')])]
    if len(points) != 256:
        raise SystemExit(f'{FONT}: gui_codepage has {len(points)} code points, not 256')
    return set(points)


def wanted(root):
    cp437 = codepage(root)
    return [cp for first, last in RANGES for cp in range(first, last + 1) if cp not in cp437 and cp not in LEFT_OUT]


def read_hex(lines, where):
    """a .hex's glyphs, {code point: bitmap as hex}"""
    glyphs = {}
    for number, line in enumerate(lines, 1):
        line = line.strip()
        if not line:
            continue
        m = re.fullmatch(r'([0-9A-F]{4,6}):([0-9A-F]+)', line)
        if not m or len(m.group(2)) not in (32, 64):
            raise SystemExit(f'{where}:{number}: not a line of a Unifont .hex')
        glyphs[int(m.group(1), 16)] = m.group(2)
    return glyphs


def kept_lines(glyphs, points, where):
    lines = []
    for cp in points:
        if cp not in glyphs:
            raise SystemExit(f'{where}: no glyph for U+{cp:04X}')
        if len(glyphs[cp]) != 32:
            raise SystemExit(f'{where}: U+{cp:04X} is not 8x16 in it: add it to LEFT_OUT, or draw it otherwise')
        lines.append(f'{cp:04X}:{glyphs[cp]}\n')
    return ''.join(lines)


def shown(root):
    """the code points past ASCII the menu's languages show: each .po's X-Language-Name and its translations, those
    build-languages.py compiles (not the empty nor the fuzzy ones)"""
    spec = importlib.util.spec_from_file_location('build_languages', Path(__file__).with_name('build-languages.py'))
    bl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bl)
    text = []
    for folder in LANGUAGES:
        for po in sorted((root / folder).glob('*.po')):
            try:
                entries = bl.read_po(po)
            except bl.Refused as refused:
                raise SystemExit(f'build-glyphs.py: {refused}')
            if entries and not entries[0][0]:
                text.append(bl.header_of(entries[0][1]).get('X-Language-Name', ''))
            text += [t for m, t, flags, _ in entries[1:] if m and t and 'fuzzy' not in flags]
    return {ord(c) for c in ''.join(text) if ord(c) > 127}


def in_titles(cp):
    return any(first <= cp <= last for first, last in TITLE_RANGES)


def titles_wanted():
    return [cp for first, last in TITLE_RANGES for cp in range(first, last + 1)]


def wide_wanted(root):
    """the code points the languages show that CP437, the Latin letters and the titles' glyphs do not draw, ascending:
    those of RANGES left out stay so, and build-languages.py refuses them"""
    cp437 = codepage(root)
    latin = [range(first, last + 1) for first, last in RANGES]
    return sorted(cp for cp in shown(root) if cp not in cp437 and not any(cp in r for r in latin) and not in_titles(cp))


def wide_lines(glyphs, points, where):
    lines = []
    for cp in points:
        if cp not in glyphs:
            raise SystemExit(f'{where}: no glyph for U+{cp:04X}, which a language shows')
        if len(glyphs[cp]) != 64:
            raise SystemExit(f'{where}: U+{cp:04X} is 8x16 in it, not 16x16: its block goes in RANGES, as Latin does')
        lines.append(f'{cp:04X}:{glyphs[cp]}\n')
    return ''.join(lines)


def titles_lines(glyphs, where):
    lines = []
    for cp in titles_wanted():
        if cp not in glyphs:
            raise SystemExit(f'{where}: no glyph for U+{cp:04X}, which TITLE_RANGES keep')
        lines.append(f'{cp:04X}:{glyphs[cp]}\n')
    return ''.join(lines)


RAISE = 2  # Unifont's baseline is row 13, the VGA font's row 11


def fit(rows):
    """an 8x16 Unifont glyph as the VGA font draws its letters: stems two columns wide (each column of ink and the one
    left of it), on the VGA's baseline. The rows it rises come from the blank ones above it, then from those between a
    capital's mark and its body (one kept), then from rows of its body that repeat the one under them, as the VGA
    font's own accented capitals are shorter than its others"""
    rows = [(r | r << 1) & 0xFF for r in rows]
    need = RAISE
    while need and not rows[0]:
        rows.pop(0)
        need -= 1
    ink = [i for i, r in enumerate(rows) if r]
    if need and ink:
        gap = next((i for i in range(ink[0], ink[-1]) if not rows[i]), None)  # the first blank row inside the glyph
        while need and gap is not None and not rows[gap + 1]:
            rows.pop(gap)
            need -= 1
        for i in range(len(rows) - 2, (gap or ink[0]), -1):
            if need and rows[i] and rows[i] == rows[i + 1]:
                rows.pop(i)
                need -= 1
    return bytes(rows + [0] * (16 - len(rows)))


def c_text(glyphs, wide):
    points, wide_points = sorted(glyphs), sorted(wide)
    out = [
        f'/* Generated by scripts/build-glyphs.py from {KEPT} and {WIDE_KEPT}: GNU',
        '   Unifont, GPL-2.0-or-later with the font embedding exception, or OFL-1.1 (src/gui/FONT-NOTICE.txt), the',
        '   Latin letters fitted to the VGA font. Do not edit. */',
        '#include "gui/latin.h"',
        '#include "gui/wide.h"',
        f'const int gui_latin_count = {len(points)};',
        'const uint16_t gui_latin_points[] = {',
    ]
    for i in range(0, len(points), 12):
        out.append('    ' + ' '.join(f'0x{cp:04x},' for cp in points[i : i + 12]))
    out += ['};', 'const uint8_t gui_latin_font[] = {']
    for cp in points:
        rows = fit(bytes.fromhex(glyphs[cp]))
        out.append('    ' + ' '.join(f'0x{b:02x},' for b in rows) + f' /* U+{cp:04X} */')
    out += ['};', f'const int gui_wide_count = {len(wide_points)};']
    if not wide_points:  # C has no empty array
        out += [
            'const uint32_t gui_wide_points[1] = {0};',
            'const uint8_t gui_wide_font[16] = {0};',
        ]
        return '\n'.join(out) + '\n'
    out.append('const uint32_t gui_wide_points[] = {')
    for i in range(0, len(wide_points), 8):
        out.append('    ' + ' '.join(f'0x{cp:06x},' for cp in wide_points[i : i + 8]))
    out += ['};', 'const uint8_t gui_wide_font[] = {']
    for cp in wide_points:
        rows = bytes.fromhex(wide[cp])  # 16 rows of two bytes, the left half's first
        for half, side in ((rows[0::2], 'left'), (rows[1::2], 'right')):
            out.append('    ' + ' '.join(f'0x{b:02x},' for b in half) + f' /* U+{cp:04X}, {side} */')
    out.append('};')
    return '\n'.join(out) + '\n'


def titles_c_text(titles):
    """the C of the titles' glyphs: their runs, each the code points of one width in a row, then the 8x16 glyphs and
    the 16x16 ones, each in order"""
    narrow = [cp for cp in sorted(titles) if len(titles[cp]) == 32]
    wide = [cp for cp in sorted(titles) if len(titles[cp]) == 64]
    runs, at = [], {False: 0, True: 0}
    for cp in sorted(titles):
        is_wide = len(titles[cp]) == 64
        if runs and runs[-1][2] == is_wide and runs[-1][1] == cp - 1:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp, is_wide, at[is_wide]])
        at[is_wide] += 1
    out = [
        f'/* Generated by scripts/build-glyphs.py from {TITLES_KEPT}: GNU Unifont, GPL-2.0-or-later with the font',
        '   embedding exception, or OFL-1.1 (src/gui/FONT-NOTICE.txt), unaltered. Do not edit. */',
        '#include "gui/titles.h"',
        f'const int gui_title_run_count = {len(runs)};',
        'const GuiTitleRun gui_title_runs[] = {',
    ]
    out += [f'    {{0x{a:06x}, 0x{b:06x}, {int(w)}, {n}}},' for a, b, w, n in runs]
    out += [
        '};',
        f'const int gui_title_narrow_count = {len(narrow)};',
        f'const int gui_title_wide_count = {len(wide)};',
    ]
    out.append(
        'const uint8_t gui_title_narrow_font[] = {' if narrow else 'const uint8_t gui_title_narrow_font[16] = {0'
    )
    for cp in narrow:
        out.append('    ' + ' '.join(f'0x{b:02x},' for b in bytes.fromhex(titles[cp])) + f' /* U+{cp:04X} */')
    out += [
        '};',
        'const uint8_t gui_title_wide_font[] = {' if wide else 'const uint8_t gui_title_wide_font[32] = {0',
    ]
    for cp in wide:
        rows = bytes.fromhex(titles[cp])  # 16 rows of two bytes, the left half's first
        out.append('    ' + ' '.join(f'0x{b:02x},' for b in rows[0::2] + rows[1::2]) + f' /* U+{cp:04X} */')
    out.append('};')
    return '\n'.join(out) + '\n'


def check_kept(root, kept, wide_kept, titles_kept):
    """the kept files' code points, widths and order, as --from writes them; SystemExit says the first that is not"""
    glyphs = read_hex(kept.read_text().splitlines(), KEPT)
    wide = read_hex(wide_kept.read_text().splitlines(), WIDE_KEPT)
    titles = read_hex(titles_kept.read_text().splitlines(), TITLES_KEPT)
    if kept.read_text().replace('\r\n', '\n') != kept_lines(glyphs, wanted(root), KEPT):
        raise SystemExit(f'{KEPT} is not the glyphs of {RANGES} CP437 lacks, in order: run build-glyphs.py --from')
    points = wide_wanted(root)
    extra = sorted(set(wide) - set(points))
    if extra:
        held = f'{TITLES_KEPT} holds' if in_titles(extra[0]) else 'no language shows'
        raise SystemExit(f'{WIDE_KEPT}: U+{extra[0]:04X} is in it, which {held}: run build-glyphs.py --from')
    in_order(wide_kept, WIDE_KEPT, wide_lines(wide, points, WIDE_KEPT), points)
    points = titles_wanted()
    in_order(titles_kept, TITLES_KEPT, titles_lines(titles, TITLES_KEPT), points)


def in_order(path, name, text, points):
    """path's lines are text's, a line a glyph of points"""
    have = path.read_text().replace('\r\n', '\n').splitlines()
    want = text.splitlines()
    for number, (line, cp) in enumerate(zip(have + [''] * len(want), points), 1):
        if line != want[number - 1]:
            raise SystemExit(
                f'{name}:{number}: {line[:7]!r} where the line of U+{cp:04X} goes, the glyphs in order and a'
                ' line each: run build-glyphs.py --from'
            )
    if len(have) != len(want):
        raise SystemExit(f'{name}: {len(have)} lines, not the {len(want)} of its glyphs: run build-glyphs.py --from')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT, help="the checkout (default: this script's)")
    parser.add_argument(
        '--from',
        dest='unifont',
        type=Path,
        help=f'a Unifont .hex, whose glyphs go in {KEPT}, {TITLES_KEPT} and {WIDE_KEPT}',
    )
    parser.add_argument(
        '--check',
        action='store_true',
        help=f'fail when {KEPT}, {TITLES_KEPT} or {WIDE_KEPT} is not what --from keeps (with --from: its glyphs too)',
    )
    parser.add_argument('--out', type=Path, help='the C source of the Latin and the wide glyphs')
    parser.add_argument('--titles-out', type=Path, help="the C source of the titles' glyphs")
    args = parser.parse_args()
    kept, wide_kept, titles_kept = (
        args.root / KEPT,
        args.root / WIDE_KEPT,
        args.root / TITLES_KEPT,
    )
    if args.unifont:
        opener = gzip.open if args.unifont.suffix == '.gz' else open
        with opener(args.unifont, 'rt', encoding='ascii') as f:
            glyphs = read_hex(f, args.unifont)
        texts = {
            kept: kept_lines(glyphs, wanted(args.root), args.unifont),
            wide_kept: wide_lines(glyphs, wide_wanted(args.root), args.unifont),
            titles_kept: titles_lines(glyphs, args.unifont),
        }
        for path, text in texts.items():
            if not args.check:
                path.write_text(text)
            elif path.read_text().replace('\r\n', '\n') != text:
                name = path.relative_to(args.root).as_posix()
                raise SystemExit(f'{name} is not what {args.unifont.name} gives: run build-glyphs.py --from')
        return 0
    if args.check:
        check_kept(args.root, kept, wide_kept, titles_kept)
        return 0
    if not args.out and not args.titles_out:
        parser.error('--from, --check, --out or --titles-out')
    if args.out:
        glyphs = read_hex(kept.read_text().splitlines(), KEPT)
        wide = read_hex(wide_kept.read_text().splitlines(), WIDE_KEPT)
        write_if_changed(args.out, c_text(glyphs, wide))
    if args.titles_out:
        write_if_changed(
            args.titles_out,
            titles_c_text(read_hex(titles_kept.read_text().splitlines(), TITLES_KEPT)),
        )
    return 0


def write_if_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


if __name__ == '__main__':
    sys.exit(main())
