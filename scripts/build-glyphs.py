#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The letters the menu draws past its VGA font's (CP437): GNU Unifont's glyphs.

--from UNIFONT.hex keeps of a Unifont .hex (unifont-<version>.hex or unifont_all-<version>.hex, gunzipped or not) its
8x16 glyphs of U+00A0 to U+024F that CP437 has not, in src/gui/unifont-latin.hex, and its 16x16 glyphs of the other
characters the menu's languages show (the names and translations of po/*.po and tests/po/*.po), two columns wide, in
src/gui/unifont-wide.hex; --check fails when those files are not what --from keeps; --out FILE writes the C source of
their glyphs, for src/gui/latin.h (each fitted to the VGA font, fit) and src/gui/wide.h.
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
LANGUAGES = ('po', 'tests/po')  # the menu's, and the tests' own
FONT = 'src/common/utilities/font.c'  # gui_codepage: what CP437 draws, which stays its own
RANGES = [(0x00A0, 0x024F)]  # Latin-1 Supplement, Latin Extended-A and B
LEFT_OUT = {0x00AD}  # Unifont draws the soft hyphen 16 wide, as a box naming it: left out, as an invisible character


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


def wide_wanted(root):
    """the code points the languages show that CP437 and the Latin letters do not draw, ascending: those of RANGES
    left out stay so, and build-languages.py refuses them"""
    cp437 = codepage(root)
    latin = [range(first, last + 1) for first, last in RANGES]
    return sorted(cp for cp in shown(root) if cp not in cp437 and not any(cp in r for r in latin))


def wide_lines(glyphs, points, where):
    lines = []
    for cp in points:
        if cp not in glyphs:
            raise SystemExit(f'{where}: no glyph for U+{cp:04X}, which a language shows')
        if len(glyphs[cp]) != 64:
            raise SystemExit(f'{where}: U+{cp:04X} is 8x16 in it, not 16x16: its block goes in RANGES, as Latin does')
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
        out += ['const uint32_t gui_wide_points[1] = {0};', 'const uint8_t gui_wide_font[16] = {0};']
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT, help="the checkout (default: this script's)")
    parser.add_argument(
        '--from', dest='unifont', type=Path, help=f'a Unifont .hex, whose glyphs go in {KEPT} and {WIDE_KEPT}'
    )
    parser.add_argument(
        '--check', action='store_true', help=f'fail when {KEPT} or {WIDE_KEPT} is not what --from keeps'
    )
    parser.add_argument('--out', type=Path, help='the C source of the glyphs')
    args = parser.parse_args()
    kept, wide_kept = args.root / KEPT, args.root / WIDE_KEPT
    if args.unifont:
        opener = gzip.open if args.unifont.suffix == '.gz' else open
        with opener(args.unifont, 'rt', encoding='ascii') as f:
            glyphs = read_hex(f, args.unifont)
        latin = kept_lines(glyphs, wanted(args.root), args.unifont)
        wide = wide_lines(glyphs, wide_wanted(args.root), args.unifont)
        kept.write_text(latin)
        wide_kept.write_text(wide)
        return 0
    glyphs = read_hex(kept.read_text().splitlines(), KEPT)
    wide = read_hex(wide_kept.read_text().splitlines(), WIDE_KEPT)
    if args.check:  # a glyph missing or of the other width stops kept_lines; one more, or out of order, makes it differ
        if kept.read_text().replace('\r\n', '\n') != kept_lines(glyphs, wanted(args.root), KEPT):
            raise SystemExit(f'{KEPT} is not the glyphs of {RANGES} CP437 lacks, in order: run build-glyphs.py --from')
        points = wide_wanted(args.root)
        extra = sorted(set(wide) - set(points))
        if extra:
            raise SystemExit(
                f'{WIDE_KEPT}: U+{extra[0]:04X} is in it, which no language shows: run build-glyphs.py --from'
            )
        have = wide_kept.read_text().replace('\r\n', '\n').splitlines()
        want = wide_lines(wide, points, WIDE_KEPT).splitlines()
        for number, (line, cp) in enumerate(zip(have + [''] * len(want), points), 1):
            if line != want[number - 1]:
                raise SystemExit(
                    f'{WIDE_KEPT}:{number}: {line[:7]!r} where the line of U+{cp:04X} goes, the glyphs in order and a'
                    ' line each: run build-glyphs.py --from'
                )
        if len(have) != len(want):
            raise SystemExit(
                f'{WIDE_KEPT}: {len(have)} lines, not the {len(want)} of its glyphs: run build-glyphs.py --from'
            )
        return 0
    if not args.out:
        parser.error('--from, --check or --out')
    text = c_text(glyphs, wide)
    if not args.out.exists() or args.out.read_text() != text:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
