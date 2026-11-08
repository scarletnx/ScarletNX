#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Draw the on-screen controls (dist/controls, rEden's vector drawables) into one atlas.

Writes dist/controls/controls.png and src/nx/runtime/platform/controls_atlas.h, the sprites' places in it. Needs
rsvg-convert and Pillow; --check needs neither: it only compares the sources' hash with the one the header holds.
"""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ANDROID = '{http://schemas.android.com/apk/res/android}'
PX_PER_DP = 1.5  # a face button 155 pixels wide: rEden draws it 11% of a phone's short side, 127 on 1156
# the stick buttons' drawables are a fifth of the others' size for a control rEden draws 15.5% of the short side
DENSITY = {'button_l3': 4.5, 'button_l3_depressed': 4.5, 'button_r3': 4.5, 'button_r3_depressed': 4.5}
PAD = 2  # transparent pixels around each sprite, so linear filtering never reads a neighbour
# each sprite's alpha this many times the drawable's: rEden's drawables are 0.6 opaque at most (their android:alpha, or
# the stick buttons' own), under its own opacity setting, so their most opaque texel is opaque here. vpad.c draws the
# built-in look at (controls_alpha / 100) ^ SNX_VPAD_BUILTIN_CURVE, 0.6 at 50: the drawables' own look
ALPHA = 1 / 0.6
ATLAS_W = 1024
# id, drawable: the order of the header's enum
SPRITES = [
    ('A', 'facebutton_a'),
    ('A_PRESSED', 'facebutton_a_depressed'),
    ('B', 'facebutton_b'),
    ('B_PRESSED', 'facebutton_b_depressed'),
    ('X', 'facebutton_x'),
    ('X_PRESSED', 'facebutton_x_depressed'),
    ('Y', 'facebutton_y'),
    ('Y_PRESSED', 'facebutton_y_depressed'),
    ('PLUS', 'facebutton_plus'),
    ('PLUS_PRESSED', 'facebutton_plus_depressed'),
    ('MINUS', 'facebutton_minus'),
    ('MINUS_PRESSED', 'facebutton_minus_depressed'),
    ('HOME', 'facebutton_home'),
    ('HOME_PRESSED', 'facebutton_home_depressed'),
    ('CAPTURE', 'facebutton_screenshot'),
    ('CAPTURE_PRESSED', 'facebutton_screenshot_depressed'),
    ('L', 'l_shoulder'),
    ('L_PRESSED', 'l_shoulder_depressed'),
    ('R', 'r_shoulder'),
    ('R_PRESSED', 'r_shoulder_depressed'),
    ('ZL', 'zl_trigger'),
    ('ZL_PRESSED', 'zl_trigger_depressed'),
    ('ZR', 'zr_trigger'),
    ('ZR_PRESSED', 'zr_trigger_depressed'),
    ('L3', 'button_l3'),
    ('L3_PRESSED', 'button_l3_depressed'),
    ('R3', 'button_r3'),
    ('R3_PRESSED', 'button_r3_depressed'),
    ('DPAD', 'dpad_standard'),
    ('DPAD_UP', 'dpad_standard_cardinal_depressed'),
    ('DPAD_UP_LEFT', 'dpad_standard_diagonal_depressed'),
    ('STICK', 'joystick'),
    ('STICK_PRESSED', 'joystick_depressed'),
    ('STICK_RANGE', 'joystick_range'),
]


def number(value):
    return float(re.sub(r'(dp|px)$', '', value))


def colour(value):
    """#RGB, #ARGB, #RRGGBB or #AARRGGBB as (#rrggbb, alpha)"""
    hex_ = value.lstrip('#')
    if len(hex_) in (3, 4):
        hex_ = ''.join(c * 2 for c in hex_)
    alpha = 1.0
    if len(hex_) == 8:
        alpha = int(hex_[:2], 16) / 255
        hex_ = hex_[2:]
    return '#' + hex_.lower(), alpha


def svg(vector):
    """an Android VectorDrawable's paths and gradients as SVG: the subset rEden's controls use"""
    a = lambda element, name, default=None: element.get(ANDROID + name, default)
    width, height = number(a(vector, 'width')), number(a(vector, 'height'))
    view_w, view_h = number(a(vector, 'viewportWidth')), number(a(vector, 'viewportHeight'))
    defs, body = [], []
    for index, path in enumerate(vector.iter('path')):
        style = {'fill': 'none'}
        fill = a(path, 'fillColor')
        if fill:
            style['fill'], alpha = colour(fill)
            style['fill-opacity'] = alpha * float(a(path, 'fillAlpha', '1'))
        for attr in path.findall('{http://schemas.android.com/aapt}attr'):
            gradient = attr.find('gradient')
            if attr.get('name') != 'android:fillColor' or gradient is None:
                raise SystemExit(f'unsupported aapt:attr {attr.get("name")}')
            ident = f'g{index}'
            stops = ''.join(
                '<stop offset="{}" stop-color="{}" stop-opacity="{:.4f}"/>'.format(
                    a(item, 'offset'), *colour(a(item, 'color'))
                )
                for item in gradient.findall('item')
            )
            kind = a(gradient, 'type', 'linear')
            if kind == 'radial':
                defs.append(
                    f'<radialGradient id="{ident}" gradientUnits="userSpaceOnUse" '
                    f'cx="{a(gradient, "centerX")}" cy="{a(gradient, "centerY")}" '
                    f'r="{a(gradient, "gradientRadius")}">{stops}</radialGradient>'
                )
            elif kind == 'linear':
                defs.append(
                    f'<linearGradient id="{ident}" gradientUnits="userSpaceOnUse" x1="{a(gradient, "startX")}" '
                    f'y1="{a(gradient, "startY")}" x2="{a(gradient, "endX")}" y2="{a(gradient, "endY")}">'
                    f'{stops}</linearGradient>'
                )
            else:
                raise SystemExit(f'unsupported gradient type {kind}')
            style['fill'] = f'url(#{ident})'
            style['fill-opacity'] = float(a(path, 'fillAlpha', '1'))
        stroke = a(path, 'strokeColor')
        if stroke:
            style['stroke'], alpha = colour(stroke)
            style['stroke-opacity'] = alpha * float(a(path, 'strokeAlpha', '1'))
            style['stroke-width'] = a(path, 'strokeWidth', '0')
        if a(path, 'fillType') == 'evenOdd':
            style['fill-rule'] = 'evenodd'
        attrs = ' '.join(f'{k}="{v}"' for k, v in style.items())
        body.append(f'<path d="{a(path, "pathData")}" {attrs}/>')
    return (
        width,
        height,
        (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {view_w} {view_h}">'
            f'<defs>{"".join(defs)}</defs><g opacity="{a(vector, "alpha", "1")}">{"".join(body)}</g></svg>'
        ),
    )


def lf(path):
    """a text file's bytes as the repository holds them: a Windows checkout turns its line ends into CR LF"""
    return path.read_bytes().replace(b'\r\n', b'\n')


def sources(root):
    digest = hashlib.sha256(lf(Path(__file__)))
    for _, name in SPRITES:
        digest.update(lf(root / 'dist/controls' / f'{name}.xml'))
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='fail when the header was not made from these sources')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    atlas_path = root / 'dist/controls/controls.png'
    header_path = root / 'src/nx/runtime/platform/controls_atlas.h'
    digest = sources(root)
    if args.check:
        text = header_path.read_text()
        png = hashlib.sha256(atlas_path.read_bytes()).hexdigest()
        if f'sources {digest}' not in text or f'atlas {png}' not in text:
            raise SystemExit(
                'controls_atlas.h or controls.png is not what dist/controls makes: run scripts/build-controls.py'
            )
        return
    from PIL import Image

    images = []
    with tempfile.TemporaryDirectory() as temporary:
        for ident, name in SPRITES:
            width, height, text = svg(ET.parse(root / 'dist/controls' / f'{name}.xml').getroot())
            source = Path(temporary) / f'{name}.svg'
            source.write_text(text)
            out = Path(temporary) / f'{name}.png'
            density = DENSITY.get(name, PX_PER_DP)
            w, h = round(width * density), round(height * density)
            subprocess.run(['rsvg-convert', '-w', str(w), '-h', str(h), '-o', str(out), str(source)], check=True)
            image = Image.open(out).convert('RGBA')
            image.putalpha(image.getchannel('A').point(lambda a: min(255, round(a * ALPHA))))
            images.append((ident, image))
    # skyline packing, tallest first: each piece where it lands lowest, then leftmost. The menu's font (16 rows of 16
    # glyphs of 8x16) and a white block for plain fills go in too: the runtime draws them there
    pieces = [(image.width, image.height) for _, image in images] + [(16 * 8, 16 * 16), (4, 4)]
    sky, places = [0] * ATLAS_W, {}
    for i in sorted(range(len(pieces)), key=lambda i: (-pieces[i][1], i)):
        w, h = pieces[i][0] + 2 * PAD, pieces[i][1] + 2 * PAD
        best = None
        for x in sorted({0} | {places[j][0] + pieces[j][0] + PAD for j in places}):
            if x + w <= ATLAS_W:
                y = max(sky[x : x + w])
                if best is None or y < best[1]:
                    best = (x, y)
        x, y = best
        sky[x : x + w] = [y + h] * w
        places[i] = (x + PAD, y + PAD)
    height = (max(sky) + 3) & ~3
    font, white = places[len(images)], places[len(images) + 1]
    atlas = Image.new('RGBA', (ATLAS_W, height), (0, 0, 0, 0))
    for i, (_, image) in enumerate(images):
        atlas.paste(image, places[i])
    atlas.save(atlas_path, optimize=True)
    png = hashlib.sha256(atlas_path.read_bytes()).hexdigest()
    lines = [
        '/* SPDX-License-Identifier: GPL-3.0-or-later',
        '   Copyright 2026 the ScarletNX contributors',
        '   Generated by scripts/build-controls.py from dist/controls. Do not edit.',
        f'   sources {digest}',
        f'   atlas {png} */',
        '#ifndef SNX_CONTROLS_ATLAS_H',
        '#define SNX_CONTROLS_ATLAS_H',
        f'#define SNX_CONTROLS_ATLAS_WIDTH {ATLAS_W}',
        f'#define SNX_CONTROLS_ATLAS_HEIGHT {height}',
        f"#define SNX_CONTROLS_FONT_X {font[0]} /* the menu's font: glyph c at x + c % 16 * 8, y + c / 16 * 16 */",
        f'#define SNX_CONTROLS_FONT_Y {font[1]}',
        f'#define SNX_CONTROLS_WHITE_X {white[0]} /* 4x4 white texels */',
        f'#define SNX_CONTROLS_WHITE_Y {white[1]}',
        '/* the sprites of dist/controls/controls.png: each as rEden draws it, then pressed */',
        'enum {',
    ]
    lines += [f'    SNX_SPRITE_{ident},' for ident, _ in SPRITES]
    lines += [
        '    SNX_SPRITE_COUNT',
        '};',
        '/* x, y, width, height in the atlas */',
        'static const unsigned short snx_controls_sprites[SNX_SPRITE_COUNT][4] = {',
    ]
    lines += [
        f'    {{{places[i][0]}, {places[i][1]}, {image.width}, {image.height}}}, /* {ident} */'
        for i, (ident, image) in enumerate(images)
    ]
    lines += ['};', '#endif', '']
    header_path.write_text('\n'.join(lines))
    print(f'{atlas_path.relative_to(root)}: {ATLAS_W}x{height}, {atlas_path.stat().st_size} bytes', file=sys.stderr)


if __name__ == '__main__':
    main()
