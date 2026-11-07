#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Draw the on-screen controls' own skins (dist/controls/skins) as SVG and render them for the runtime.

Each skin is a RetroArch overlay, as runtime/platform/vpad_skin.c reads one: its .cfg, its images (a two-frame APNG
each, the second frame its pressed look) and the SVGs they are rendered from, in dist/controls/skins/<name>/. The
images are as many pixels as the built-in sprites (dist/controls/controls.png), and the controls sit where the
built-in ones do. The yorha skin is the launcher's yorha theme on the controls: its page and ink, thin lines, square
markers, the inverted bar of a focused row for a pressed control, and the letters of the menus' font
(src/common/utilities/font.c), drawn pixel by pixel. The line skin is white lines alone, after the look of libretro's
flat retropad overlay, drawn here from nothing: outlines and letters of one width on the screen, a faint dark edge
under them for a bright game, and a pressed control filled white with its mark cut in dark. Needs rsvg-convert and
Pillow; --check needs neither: it compares the hashes each .cfg holds with this script, the font and the files.
"""

import argparse
import hashlib
import io
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / 'src/common/utilities/font.c'
ASPECT = 16 / 9  # the page's: the controls are placed on a landscape screen, sized by its height

# the yorha theme's colours (src/gui/themes/yorha/theme.ini): page, ink, soft, dim
PAGE = '#DAD6BE'
INK = '#45413A'
SOFT = '#8A8676'
DIM = '#B4B09C'
FILL = 0.86  # the page over the game: its picture shows through a little


def glyphs():
    """the menus' 8x16 font, 256 glyphs of 16 rows, a byte a row (its leftmost pixel the top bit)"""
    text = FONT.read_text()
    body = text[text.index('gui_vga_font[256 * 16] = {') :]
    values = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body[: body.index('};')])]
    assert len(values) == 256 * 16, len(values)
    return [values[c * 16 : c * 16 + 16] for c in range(256)]


GLYPHS = glyphs()


def svg(w, h, body):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!-- SPDX-License-Identifier: GPL-3.0-or-later -->\n'
        '<!-- Copyright 2026 the ScarletNX contributors. Made by scripts/build-skins.py: edit it, not this file -->\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
        f'shape-rendering="crispEdges">\n{body}</svg>\n'
    )


def text(chars, cx, cy, px, colour):
    """chars (bytes of the font) in its pixels, px a pixel, their lit rows' middle at cx, cy: a rect a run"""
    rows = [r for c in chars for r in range(16) if GLYPHS[c][r]]
    top, bottom = min(rows), max(rows)
    x0 = cx - len(chars) * 8 * px / 2
    y0 = cy - (top + bottom + 1) * px / 2
    out = []
    for i, c in enumerate(chars):
        for r in range(16):
            bits, b = GLYPHS[c][r], 0
            while b < 8:
                if bits & 0x80 >> b:
                    run = b
                    while run < 8 and bits & 0x80 >> run:
                        run += 1
                    out.append(
                        f'<rect x="{x0 + (i * 8 + b) * px:.2f}" y="{y0 + r * px:.2f}" width="{(run - b) * px:.2f}" '
                        f'height="{px:.2f}" fill="{colour}"/>'
                    )
                    b = run
                else:
                    b += 1
    return '\n'.join(out) + '\n'


def bitmap(rows, cx, cy, px, colour):
    """a drawing of our own, rows of '#' and '.', in pixels px wide, its middle at cx, cy"""
    x0 = cx - len(rows[0]) * px / 2
    y0 = cy - len(rows) * px / 2
    return ''.join(
        f'<rect x="{x0 + x * px:.2f}" y="{y0 + y * px:.2f}" width="{px:.2f}" height="{px:.2f}" fill="{colour}"/>\n'
        for y, row in enumerate(rows)
        for x, c in enumerate(row)
        if c == '#'
    )


def square(cx, cy, side, colour):
    """the theme's marker: a small filled square"""
    return f'<rect x="{cx - side / 2:.2f}" y="{cy - side / 2:.2f}" width="{side:.2f}" height="{side:.2f}" fill="{colour}"/>\n'


def colours(pressed):
    """a control's fill, its line and its letters: pressed, inverted as the theme's focused row"""
    return (INK, INK, PAGE) if pressed else (PAGE, INK, INK)


def ring(s, pressed, accent=SOFT):
    """a round control: its disc, its outer line, a thin ring inside, and four square markers on the diagonals"""
    fill, line, ink = colours(pressed)
    w = max(2.0, s * 0.02)
    r = s / 2 - 2 * w
    body = (
        f'<circle cx="{s / 2}" cy="{s / 2}" r="{r:.2f}" fill="{fill}" fill-opacity="{FILL if not pressed else 0.95}" '
    )
    body += f'stroke="{line}" stroke-width="{w:.2f}" shape-rendering="geometricPrecision"/>\n'
    body += f'<circle cx="{s / 2}" cy="{s / 2}" r="{r - 3 * w:.2f}" fill="none" stroke="{ink if pressed else accent}" '
    body += f'stroke-width="{w * 0.6:.2f}" shape-rendering="geometricPrecision"/>\n'
    for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        k = (r + w) * 0.7071
        body += square(s / 2 + dx * k, s / 2 + dy * k, 2.2 * w, line)
    return body, ink


def face(letter, s, pressed):
    """A, B, X and Y: a round control, its letter in the menus' font"""
    body, ink = ring(s, pressed)
    return svg(s, s, body + text(letter.encode('latin-1'), s / 2, s / 2, round(s / 36), ink))


# drawings of our own, for the marks the font has none for
CAMERA = [
    '..####....',
    '##########',
    '#........#',
    '#..####..#',
    '#.#....#.#',
    '#.#....#.#',
    '#..####..#',
    '#........#',
    '##########',
]


def small(mark, s, pressed):
    """+, -, Home and Capture: a square with a thin frame inside and its mark"""
    fill, line, ink = colours(pressed)
    w = max(2.0, s * 0.025)
    body = f'<rect x="{2 * w:.2f}" y="{2 * w:.2f}" width="{s - 4 * w:.2f}" height="{s - 4 * w:.2f}" fill="{fill}" '
    body += f'fill-opacity="{FILL if not pressed else 0.95}" stroke="{line}" stroke-width="{w:.2f}"/>\n'
    body += f'<rect x="{4.5 * w:.2f}" y="{4.5 * w:.2f}" width="{s - 9 * w:.2f}" height="{s - 9 * w:.2f}" fill="none" '
    body += f'stroke="{ink if pressed else DIM}" stroke-width="{w * 0.6:.2f}"/>\n'
    px = round(s / 21)
    if mark == 'capture':
        body += bitmap(CAMERA, s / 2, s / 2, px, ink)
    else:
        body += text({'+': b'+', '-': b'-', 'home': b'\x7f'}[mark], s / 2, s / 2, px, ink)
    return svg(s, s, body)


def stick_click(letter, s, pressed):
    """L3 and R3: a round control, its letter and 3, and the font's down triangle under them for the stick's press"""
    body, ink = ring(s, pressed, DIM)
    px = round(s / 48)
    body += text((letter + '3').encode('latin-1'), s / 2, s * 0.44, px, ink)
    body += text(b'\x1f', s / 2, s * 0.7, px, ink)
    return svg(s, s, body)


def shoulder(letters, w, h, right, trigger, pressed):
    """L, R, ZL and ZR: a panel, the theme's square marker before the letters; the triggers' outer top corner cut"""
    fill, line, ink = colours(pressed)
    lw = max(2.0, h * 0.025)
    x0, y0, x1, y1 = lw, lw, w - lw, h - lw
    cut = (y1 - y0) * 0.45 if trigger else 0
    points = [(x0 + cut, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0 + cut)]
    if right:
        points = [(w - x, y) for x, y in points]
    d = 'M' + 'L'.join(f'{x:.2f} {y:.2f}' for x, y in points) + 'Z'
    body = f'<path d="{d}" fill="{fill}" fill-opacity="{FILL if not pressed else 0.95}" stroke="{line}" '
    body += f'stroke-width="{lw:.2f}" stroke-linejoin="miter"/>\n'
    # a rule under the panel's top, as the theme's head has one, clear of the cut corner
    r0, r1 = (x0 + 3 * lw, x1 - cut - 3 * lw) if right else (x0 + cut + 3 * lw, x1 - 3 * lw)
    body += f'<line x1="{r0:.2f}" y1="{y0 + 4 * lw:.2f}" x2="{r1:.2f}" y2="{y0 + 4 * lw:.2f}" '
    body += f'stroke="{ink if pressed else DIM}" stroke-width="{lw * 0.6:.2f}"/>\n'
    px = round(h / 30)
    letters_w = len(letters) * 8 * px
    body += square(w / 2 - letters_w / 2 - 6 * px, h / 2 + px, 3 * px, ink)
    body += text(letters.encode('latin-1'), w / 2 + 2 * px, h / 2 + px, px, ink)
    return svg(w, h, body)


def arm(direction, w, h, pressed):
    """one of the D-pad's four: a panel from its outer end to the middle's square, the font's triangle near its end and
    a square marker at its inner end"""
    fill, line, ink = colours(pressed)
    across, length = (w, h) if direction in ('up', 'down') else (h, w)
    lw = max(2.0, across * 0.025)
    inner = length - across * 0.42  # the middle square's edge: the arms leave it to the game
    a0, a1 = across * 0.12, across * 0.88
    # the arm pointing up, then turned
    body = f'<rect x="{a0:.2f}" y="{lw:.2f}" width="{a1 - a0:.2f}" height="{inner - lw:.2f}" fill="{fill}" '
    body += f'fill-opacity="{FILL if not pressed else 0.95}" stroke="{line}" stroke-width="{lw:.2f}"/>\n'
    body += text(b'\x1e', across / 2, inner * 0.36, round(across / 34), ink)
    body += square(across / 2, inner - across * 0.14, across * 0.07, ink if pressed else SOFT)
    turn = {
        'up': '',
        'down': f' transform="rotate(180 {w / 2} {h / 2})"',
        'left': f' transform="translate(0 {h}) rotate(-90)"',
        'right': f' transform="translate({w} 0) rotate(90)"',
    }
    return svg(w, h, f'<g{turn[direction]}>\n{body}</g>\n')


def stick_ring(s):
    """a stick's ring: a clear disc, its edge, a thinner ring inside, a tick at each way it leans and four squares"""
    w = max(2.0, s * 0.008)
    body = (
        f'<circle cx="{s / 2}" cy="{s / 2}" r="{s / 2 - 2 * w:.2f}" fill="{PAGE}" fill-opacity="0.32" stroke="{INK}" '
    )
    body += f'stroke-opacity="0.9" stroke-width="{w:.2f}" shape-rendering="geometricPrecision"/>\n'
    body += f'<circle cx="{s / 2}" cy="{s / 2}" r="{s * 0.36:.2f}" fill="none" stroke="{SOFT}" stroke-width="{w * 0.7:.2f}" '
    body += f'stroke-dasharray="{s * 0.02:.2f} {s * 0.025:.2f}" shape-rendering="geometricPrecision"/>\n'
    for angle in (0, 90, 180, 270):
        body += (
            f'<line x1="{s / 2}" y1="{2 * w + s * 0.015:.2f}" x2="{s / 2}" y2="{2 * w + s * 0.06:.2f}" stroke="{INK}" '
        )
        body += f'stroke-width="{2 * w:.2f}" transform="rotate({angle} {s / 2} {s / 2})"/>\n'
    for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        k = (s / 2 - 2 * w) * 0.7071
        body += square(s / 2 + dx * k, s / 2 + dy * k, 3 * w, INK)
    return svg(s, s, body)


def knob(s, pressed):
    """a stick's knob, as wide as the ring over 1.66 (as the built-in one): a disc, its line, a thin ring and the
    theme's square marker in the middle; drawn on a canvas as large as the ring, as RetroArch draws a stick's image
    over the whole stick"""
    fill, line, ink = colours(pressed)
    r = s / 1.66 / 2
    w = max(2.0, s * 0.01)
    body = (
        f'<circle cx="{s / 2}" cy="{s / 2}" r="{r:.2f}" fill="{fill}" fill-opacity="{0.92 if not pressed else 0.96}" '
    )
    body += f'stroke="{line}" stroke-width="{w:.2f}" shape-rendering="geometricPrecision"/>\n'
    body += f'<circle cx="{s / 2}" cy="{s / 2}" r="{r * 0.72:.2f}" fill="none" stroke="{ink if pressed else DIM}" '
    body += f'stroke-width="{w * 0.7:.2f}" shape-rendering="geometricPrecision"/>\n'
    body += square(s / 2, s / 2, r * 0.22, ink)
    return svg(s, s, body)


# ---- the line skin: white lines of one width on the screen, as libretro's flat retropad draws its controls
WHITE = '#FFFFFF'
DARK = '#000000'
LINE = 0.0048  # a line's width, of the screen's height (the retropad's buttons' is about 0.005)
EDGE = 0.28  # the dark edge under every line: its opacity; it is 2.6 lines wide
PRESSED = 0.88  # a pressed control's white fill
# the letters, drawn as lines: polylines in a box one high (y down), and each letter's width. An arc is (cx, cy, rx, ry,
# from, to) in degrees (y down: a growing angle turns clockwise on the screen), drawn every 5 degrees
GLYPHS_LINE = {
    'A': (0.76, [[(0, 1), (0.38, 0), (0.76, 1)], [(0.15, 0.66), (0.61, 0.66)]]),
    'B': (
        0.64,
        [
            [(0.36, 0.48), (0, 0.48), (0, 0), (0.36, 0), ('arc', 0.36, 0.24, 0.24, 0.24, -90, 90)],
            [(0, 0.48), (0, 1), (0.38, 1), ('arc', 0.38, 0.74, 0.26, 0.26, 90, -90), (0, 0.48)],
        ],
    ),
    'X': (0.7, [[(0, 0), (0.7, 1)], [(0.7, 0), (0, 1)]]),
    'Y': (0.72, [[(0, 0), (0.36, 0.52), (0.72, 0)], [(0.36, 0.52), (0.36, 1)]]),
    'L': (0.52, [[(0, 0), (0, 1), (0.52, 1)]]),
    'R': (
        0.64,
        [[(0, 1), (0, 0), (0.36, 0), ('arc', 0.36, 0.25, 0.25, 0.25, -90, 90), (0, 0.5)], [(0.3, 0.5), (0.64, 1)]],
    ),
    'Z': (0.62, [[(0, 0), (0.62, 0), (0, 1), (0.62, 1)]]),
    '3': (0.56, [[('arc', 0.27, 0.25, 0.27, 0.25, 200, 450)], [('arc', 0.28, 0.75, 0.28, 0.25, 270, 520)]]),
}


def glyph_points(item):
    """an arc's points; any other item is a point already"""
    if item[0] != 'arc':
        return [item]
    _, cx, cy, rx, ry, a0, a1 = item
    steps = max(2, round(abs(a1 - a0) / 5))
    angles = (math.radians(a0 + (a1 - a0) * k / steps) for k in range(steps + 1))
    return [(cx + rx * math.cos(a), cy + ry * math.sin(a)) for a in angles]


def line_text(chars, cx, cy, cap):
    """chars as line glyphs, cap pixels high, their middle at cx, cy: a path's d"""
    gap = 0.24
    width = sum(GLYPHS_LINE[c][0] for c in chars) + gap * (len(chars) - 1)
    x0, y0, d = cx - width * cap / 2, cy - cap / 2, ''
    for c in chars:
        w, strokes = GLYPHS_LINE[c]
        for stroke in strokes:
            points = [q for item in stroke for q in glyph_points(item)]
            d += 'M' + 'L'.join(f'{x0 + x * cap:.2f} {y0 + y * cap:.2f}' for x, y in points)
        x0 += (w + gap) * cap
    return d


class Lines:
    """a control drawn in lines: its outline (white, and filled white when pressed), the marks on it (white lines and
    shapes, dark on a pressed one), written over a dark edge as wide as 2.6 lines"""

    def __init__(self, w, h, scale):
        self.w, self.h = w, h
        self.lw = LINE * scale  # scale: the image's pixels of the screen's height
        self.margin = self.lw * 1.3 + 1  # the dark edge of a line along the image's side stays on it
        self.shape, self.marks, self.solid = None, [], []

    def outline(self, d, width=1.0, pressed=None):
        """the control's outline; pressed, the shape filled (the outline's own unless an open one gives another)"""
        self.shape = (d, width, pressed or d)

    def mark(self, d, width=1.0):
        self.marks.append((d, width))

    def fill(self, d, edge=True):
        self.solid.append((d, edge))

    def svg(self, pressed, transform=''):
        lw, body = self.lw, ''
        edge = f'stroke="{DARK}" stroke-opacity="{EDGE}" stroke-linecap="round" stroke-linejoin="round"'
        for d, k in [self.shape[:2]] + self.marks:
            body += f'<path d="{d}" fill="none" {edge} stroke-width="{lw * k * 2.6:.2f}"/>\n'
        for d, has_edge in self.solid:
            if has_edge:
                body += f'<path d="{d}" fill="{DARK}" fill-opacity="{EDGE}" {edge} stroke-width="{lw * 1.6:.2f}"/>\n'
        if pressed:
            body += f'<path d="{self.shape[2]}" fill="{WHITE}" fill-opacity="{PRESSED}"/>\n'
        ink = DARK if pressed else WHITE
        for (d, k), colour in [(self.shape[:2], WHITE)] + [(m, ink) for m in self.marks]:
            body += f'<path d="{d}" fill="none" stroke="{colour}" stroke-width="{lw * k:.2f}" stroke-linecap="round" '
            body += 'stroke-linejoin="round"/>\n'
        for d, _ in self.solid:
            body += f'<path d="{d}" fill="{ink}"/>\n'
        transform = f' transform="{transform}"' if transform else ''
        return svg(self.w, self.h, f'<g shape-rendering="geometricPrecision"{transform}>\n{body}</g>\n')


def circle_d(cx, cy, r):
    return f'M{cx - r:.2f} {cy:.2f}A{r:.2f} {r:.2f} 0 1 0 {cx + r:.2f} {cy:.2f}A{r:.2f} {r:.2f} 0 1 0 {cx - r:.2f} {cy:.2f}Z'


def rounded_d(x0, y0, x1, y1, radii):
    """a rectangle, its corners' radii top left, top right, bottom right, bottom left"""
    a, b, c, e = radii
    return (
        f'M{x0 + a:.2f} {y0:.2f}H{x1 - b:.2f}A{b:.2f} {b:.2f} 0 0 1 {x1:.2f} {y0 + b:.2f}V{y1 - c:.2f}'
        f'A{c:.2f} {c:.2f} 0 0 1 {x1 - c:.2f} {y1:.2f}H{x0 + e:.2f}A{e:.2f} {e:.2f} 0 0 1 {x0:.2f} {y1 - e:.2f}'
        f'V{y0 + a:.2f}A{a:.2f} {a:.2f} 0 0 1 {x0 + a:.2f} {y0:.2f}Z'
    )


def arrowhead_d(cx, cy, size, direction):
    """a filled arrowhead, size wide, pointing up or down"""
    h = size * 0.62 * (1 if direction == 'down' else -1)
    return f'M{cx - size / 2:.2f} {cy - h / 2:.2f}L{cx + size / 2:.2f} {cy - h / 2:.2f}L{cx:.2f} {cy + h / 2:.2f}Z'


def line_face(letter, s, size, pressed):
    """A, B, X and Y: a circle and its letter"""
    g = Lines(s, s, s / size)
    g.outline(circle_d(s / 2, s / 2, s / 2 - g.margin))
    g.mark(line_text(letter, s / 2, s / 2, s * 0.38))
    return g.svg(pressed)


def line_small(mark, s, size, pressed):
    """+, - and Home in a circle; Capture a rounded square with a circle in it, as the console's"""
    g = Lines(s, s, s / size)
    m, c, k = g.margin, s / 2, s * 0.2
    g.outline(rounded_d(m, m, s - m, s - m, [s * 0.16] * 4) if mark == 'capture' else circle_d(c, c, c - m))
    if mark == '-':
        g.mark(f'M{c - k:.2f} {c:.2f}H{c + k:.2f}')
    elif mark == '+':
        g.mark(f'M{c - k:.2f} {c:.2f}H{c + k:.2f}M{c:.2f} {c - k:.2f}V{c + k:.2f}')
    elif mark == 'home':  # a roof over a house
        g.mark(
            f'M{c - s * 0.25:.2f} {c + s * 0.02:.2f}L{c:.2f} {c - s * 0.22:.2f}L{c + s * 0.25:.2f} {c + s * 0.02:.2f}'
        )
        g.mark(f'M{c - s * 0.16:.2f} {c - s * 0.05:.2f}V{c + s * 0.21:.2f}H{c + s * 0.16:.2f}V{c - s * 0.05:.2f}')
    else:
        g.mark(circle_d(c, c, s * 0.15))
    return g.svg(pressed)


def line_click(letter, s, size, pressed):
    """L3 and R3: a circle, its letter and 3, and an arrowhead under them for the stick's press"""
    g = Lines(s, s, s / size)
    g.outline(circle_d(s / 2, s / 2, s / 2 - g.margin))
    g.mark(line_text(letter + '3', s / 2, s * 0.43, s * 0.24))
    g.fill(arrowhead_d(s / 2, s * 0.7, s * 0.16, 'down'))
    return g.svg(pressed)


def line_shoulder(letters, w, h, size, right, trigger, pressed):
    """L, R, ZL and ZR: a rounded bar and its letters; a trigger's outer top corner rounder"""
    g = Lines(w, h, max(w, h) / size)
    m = g.margin
    r = (h - 2 * m) * 0.28
    big = (h - 2 * m) * 0.62 if trigger else r
    g.outline(rounded_d(m, m, w - m, h - m, [r, big, r, r] if right else [big, r, r, r]))
    g.mark(line_text(letters, w / 2, h / 2, h * 0.38))
    return g.svg(pressed)


def line_arm(direction, w, h, size, pressed):
    """one of the D-pad's four: three sides of its arm, open to the middle, where the next arm's side meets them, and
    an arrowhead pointing out. Drawn pointing up, then turned"""
    across, length = (w, h) if direction in ('up', 'down') else (h, w)
    g = Lines(w, h, DPAD / size)
    m = g.margin
    inner = length - across / 2 + m  # the middle square's corner, where two arms' sides meet
    sides = f'M{m:.2f} {inner:.2f}V{m:.2f}H{across - m:.2f}V{inner:.2f}'
    g.outline(sides, pressed=sides + 'Z')
    g.fill(arrowhead_d(across / 2, (inner + m) * 0.42, across * 0.3, 'up'))
    turn = {
        'up': '',
        'down': f'rotate(180 {w / 2} {h / 2})',
        'left': f'translate(0 {h}) rotate(-90)',
        'right': f'translate({w} 0) rotate(90)',
    }
    return g.svg(pressed, turn[direction])


def line_ring(s, size):
    """a stick's ring: one thin circle"""
    g = Lines(s, s, s / size)
    g.outline(circle_d(s / 2, s / 2, s / 2 - g.margin), 0.8)
    return g.svg(False)


def line_knob(s, size, pressed):
    """a stick's knob, as wide as the ring over 1.66 (as the built-in one): a circle, a smaller one in it filled with
    dots, as the retropad's knob is; drawn on a canvas as large as the ring, as RetroArch draws a stick's image over
    the whole stick"""
    g = Lines(s, s, s / size)
    r, c = s / 1.66 / 2, s / 2
    g.outline(circle_d(c, c, r))
    g.mark(circle_d(c, c, r * 0.58), 0.7)
    dot, step, dots = g.lw * 0.42, g.lw * 2.6, ''
    rows = int(r / (step * 0.866)) + 1
    for j in range(-rows, rows + 1):
        y = c + j * step * 0.866
        for i in range(-rows - 1, rows + 2):
            x = c + (i + (j % 2) * 0.5) * step
            if math.hypot(x - c, y - c) < r * 0.58 - g.lw * 1.2:
                dots += f'M{x - dot:.2f} {y:.2f}a{dot:.2f} {dot:.2f} 0 1 0 {2 * dot:.2f} 0a{dot:.2f} {dot:.2f} 0 1 0 '
                dots += f'{-2 * dot:.2f} 0Z'
    g.fill(dots, edge=False)
    return g.svg(pressed)


# a control's place and size as the built-in look has them (vpad.c's VPAD_CONTROLS): its middle, and its longer side
# of the screen's height
FACE, SMALL, CLICK, BAR = 156, 104, 156, (368, 148)
DPAD, STICK = 336, 400
ARM = (120, 168)
IMAGES = {  # image: its drawing, normal and pressed
    'a': lambda p: face('A', FACE, p),
    'b': lambda p: face('B', FACE, p),
    'x': lambda p: face('X', FACE, p),
    'y': lambda p: face('Y', FACE, p),
    'plus': lambda p: small('+', SMALL, p),
    'minus': lambda p: small('-', SMALL, p),
    'home': lambda p: small('home', SMALL, p),
    'capture': lambda p: small('capture', SMALL, p),
    'l3': lambda p: stick_click('L', CLICK, p),
    'r3': lambda p: stick_click('R', CLICK, p),
    'l': lambda p: shoulder('L', *BAR, False, False, p),
    'r': lambda p: shoulder('R', *BAR, True, False, p),
    'zl': lambda p: shoulder('ZL', *BAR, False, True, p),
    'zr': lambda p: shoulder('ZR', *BAR, True, True, p),
    'up': lambda p: arm('up', *ARM, p),
    'down': lambda p: arm('down', *ARM, p),
    'left': lambda p: arm('left', ARM[1], ARM[0], p),
    'right': lambda p: arm('right', ARM[1], ARM[0], p),
    'knob': lambda p: knob(STICK, p),
}
STILL = {'ring': lambda: stick_ring(STICK)}  # images with no pressed look
# descriptors: RetroArch's name, image, middle, hit area, longer side of the screen's height, and the image's pixels
CONTROLS = [
    ('l', 'l', 0.09, 0.21, 'rect', 0.26, BAR),
    ('r', 'r', 0.91, 0.21, 'rect', 0.26, BAR),
    ('l2', 'zl', 0.09, 0.08, 'rect', 0.26, BAR),
    ('r2', 'zr', 0.91, 0.08, 'rect', 0.26, BAR),
    ('select', 'minus', 0.25, 0.07, 'rect', 0.07, (SMALL, SMALL)),
    ('start', 'plus', 0.75, 0.07, 'rect', 0.07, (SMALL, SMALL)),
    ('screenshot', 'capture', 0.36, 0.90, 'rect', 0.07, (SMALL, SMALL)),
    ('menu_toggle', 'home', 0.64, 0.90, 'rect', 0.07, (SMALL, SMALL)),
    ('l3', 'l3', 0.25, 0.36, 'radial', 0.155, (CLICK, CLICK)),
    ('r3', 'r3', 0.64, 0.62, 'radial', 0.155, (CLICK, CLICK)),
    ('a', 'a', 0.93, 0.47, 'radial', 0.11, (FACE, FACE)),
    ('b', 'b', 0.87, 0.60, 'radial', 0.11, (FACE, FACE)),
    ('x', 'x', 0.87, 0.34, 'radial', 0.11, (FACE, FACE)),
    ('y', 'y', 0.81, 0.47, 'radial', 0.11, (FACE, FACE)),
]
DPAD_AT, DPAD_SIZE = (0.23, 0.78), 0.25
STICKS = [('analog_left', 0.12, 0.47), ('analog_right', 0.77, 0.78)]
STICK_SIZE = 0.30
SIZE = {image: size for _, image, _, _, _, size, _ in CONTROLS}  # an image's control's longer side, of the screen
LINE_IMAGES = {  # the line skin's images: their drawings, normal and pressed
    'a': lambda p: line_face('A', FACE, SIZE['a'], p),
    'b': lambda p: line_face('B', FACE, SIZE['b'], p),
    'x': lambda p: line_face('X', FACE, SIZE['x'], p),
    'y': lambda p: line_face('Y', FACE, SIZE['y'], p),
    'plus': lambda p: line_small('+', SMALL, SIZE['plus'], p),
    'minus': lambda p: line_small('-', SMALL, SIZE['minus'], p),
    'home': lambda p: line_small('home', SMALL, SIZE['home'], p),
    'capture': lambda p: line_small('capture', SMALL, SIZE['capture'], p),
    'l3': lambda p: line_click('L', CLICK, SIZE['l3'], p),
    'r3': lambda p: line_click('R', CLICK, SIZE['r3'], p),
    'l': lambda p: line_shoulder('L', *BAR, SIZE['l'], False, False, p),
    'r': lambda p: line_shoulder('R', *BAR, SIZE['r'], True, False, p),
    'zl': lambda p: line_shoulder('ZL', *BAR, SIZE['zl'], False, True, p),
    'zr': lambda p: line_shoulder('ZR', *BAR, SIZE['zr'], True, True, p),
    'up': lambda p: line_arm('up', *ARM, DPAD_SIZE, p),
    'down': lambda p: line_arm('down', *ARM, DPAD_SIZE, p),
    'left': lambda p: line_arm('left', ARM[1], ARM[0], DPAD_SIZE, p),
    'right': lambda p: line_arm('right', ARM[1], ARM[0], DPAD_SIZE, p),
    'knob': lambda p: line_knob(STICK, STICK_SIZE, p),
}
LINE_STILL = {'ring': lambda: line_ring(STICK, STICK_SIZE)}
# each skin's images, and those with no pressed look
SKINS = {'line': (LINE_IMAGES, LINE_STILL), 'yorha': (IMAGES, STILL)}


def ranges(size, pixels):
    """the half sizes on the page, x of its width and y of its height, of a box size tall or wide as pixels are"""
    w, h = pixels
    long = max(w, h)
    return size * w / long / 2 / ASPECT, size * h / long / 2


def descriptors():
    """the page's descriptors, a .cfg line each"""
    lines, count = [], 0

    def desc(name, image, x, y, shape, rx, ry, more=()):
        nonlocal count
        lines.append(f'overlay0_desc{count} = "{name},{x:.5f},{y:.5f},{shape},{rx:.5f},{ry:.5f}"')
        lines.append(f'overlay0_desc{count}_overlay = {image}.png')
        lines.extend(f'overlay0_desc{count}_{k} = {v}' for k, v in more)
        count += 1

    for name, image, x, y, shape, size, pixels in CONTROLS:
        desc(name, image, x, y, shape, *ranges(size, pixels))
    # the D-pad: four arms from its middle, each half its size long, the box around them its size
    half_w, half_h = ranges(DPAD_SIZE, (DPAD, DPAD))
    across_w, across_h = half_w * ARM[0] / (DPAD / 2), half_h * ARM[0] / (DPAD / 2)
    for name, dx, dy in (('up', 0, -1), ('down', 0, 1), ('left', -1, 0), ('right', 1, 0)):
        rx, ry = (across_w / 2, half_h / 2) if dx == 0 else (half_w / 2, across_h / 2)
        desc(name, name, DPAD_AT[0] + dx * half_w / 2, DPAD_AT[1] + dy * half_h / 2, 'rect', rx, ry)
    for name, x, y in STICKS:  # each stick's ring, then its knob, which moves
        rx, ry = ranges(STICK_SIZE, (STICK, STICK))
        desc('nul', 'ring', x, y, 'radial', rx, ry)
        desc(name, 'knob', x, y, 'radial', rx, ry, (('movable', 'true'),))
    return count, lines


def render(svg_text, w, h, directory):
    source = Path(directory) / 'image.svg'
    out = Path(directory) / 'image.png'
    source.write_text(svg_text)
    subprocess.run(['rsvg-convert', '-w', str(w), '-h', str(h), '-o', str(out), str(source)], check=True)
    from PIL import Image

    return Image.open(out).convert('RGBA')


def pixels_of(svg_text):
    head = svg_text[svg_text.index('<svg') :]
    w = int(head.split('width="')[1].split('"')[0])
    h = int(head.split('height="')[1].split('"')[0])
    return w, h


def lf(path):
    return path.read_bytes().replace(b'\r\n', b'\n')


def digest(paths):
    h = hashlib.sha256()
    for p in paths:
        h.update(p.name.encode() + b'\0' + lf(p) if p.suffix != '.png' else p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


def skin_files(folder):
    return sorted(folder.glob('*.png')) + sorted((folder / 'svg').glob('*.svg'))


def build(skin, script):
    """skin's SVGs, images and .cfg into dist/controls/skins/<skin>"""
    from PIL import Image

    images, still = SKINS[skin]
    folder = ROOT / 'dist/controls/skins' / skin
    cfg = folder / f'{skin}.cfg'
    (folder / 'svg').mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        for name, draw in images.items():
            frames = []
            for pressed in (False, True):
                source = draw(pressed)
                (folder / 'svg' / f'{name}{"-pressed" if pressed else ""}.svg').write_text(source)
                frames.append(render(source, *pixels_of(source), temporary))
            # two frames: the image, then its pressed look, as RetroArch draws a two-frame APNG
            out = io.BytesIO()
            frames[0].save(
                out, format='PNG', save_all=True, append_images=frames[1:], duration=1000, loop=0, optimize=True
            )
            (folder / f'{name}.png').write_bytes(out.getvalue())
            assert Image.open(folder / f'{name}.png').n_frames == 2, name
        for name, draw in still.items():
            source = draw()
            (folder / 'svg' / f'{name}.svg').write_text(source)
            render(source, *pixels_of(source), temporary).save(folder / f'{name}.png', optimize=True)
    count, lines = descriptors()
    head = [
        '# SPDX-License-Identifier: GPL-3.0-or-later',
        '# Copyright 2026 the ScarletNX contributors',
        f"# The on-screen controls' {skin} skin, a RetroArch overlay (runtime/platform/vpad_skin.c reads it). Made by",
        '# scripts/build-skins.py from the drawings in svg/: edit the script, not these files.',
        f'# script {script}',
        f'# files {digest(skin_files(folder))}',
        'overlays = 1',
        'overlay0_name = "landscape"',
        'overlay0_full_screen = true',
        'overlay0_normalized = true',
        f'overlay0_aspect_ratio = {ASPECT:.6f}',
        f'overlay0_descs = {count}',
    ]
    cfg.write_text('\n'.join(head + lines) + '\n')
    size = sum(p.stat().st_size for p in folder.glob('*.png'))
    print(
        f'{cfg.relative_to(ROOT)}: {count} descriptors, {len(images) + len(still)} images, {size} bytes',
        file=sys.stderr,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='fail when the skins were not made by this script')
    args = parser.parse_args()
    script = hashlib.sha256(lf(Path(__file__)) + b'\0' + lf(FONT)).hexdigest()
    for skin in SKINS:
        if not args.check:
            build(skin, script)
            continue
        folder = ROOT / 'dist/controls/skins' / skin
        cfg = folder / f'{skin}.cfg'
        text_ = cfg.read_text() if cfg.is_file() else ''
        if f'script {script}' not in text_ or f'files {digest(skin_files(folder))}' not in text_:
            raise SystemExit(f'dist/controls/skins/{skin} is not what scripts/build-skins.py makes: run it')


if __name__ == '__main__':
    main()
