#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Make the icons in dist/icons/ from dist/wimsy.svg, or check them.

    scripts/make-icons.py           render them again (needs rsvg-convert, from librsvg, and optipng)
    scripts/make-icons.py --check   check them against the SVG and the manifest (no renderer needed)

dist/icons/hicolor/ holds the PNG sizes desktops look for, laid out as installed under share/icons/; wimsy.ico packs
the Windows sizes for the executables' resources (src/common/platform/windows/runtime.rc); the 128 one is the window
icon (cmake/window_icon.cmake). Each PNG goes through optipng, keeping it RGBA (the ICO's images must be, and a
palette came out larger). MANIFEST records the SVG's and every output's SHA-256 and the tools that made them, so a
changed SVG, or an output edited by hand, fails the check until this runs again. When the tools at hand are the
recorded ones, --check also makes each size again and compares the bytes.
"""

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist'
SOURCE = DIST / 'wimsy.svg'
ICONS = DIST / 'icons'
MANIFEST = ICONS / 'MANIFEST'
SIZES = (16, 22, 24, 32, 48, 64, 128, 256)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'


def png_path(size):
    return ICONS / f'hicolor/{size}x{size}/apps/wimsy.png'


def outputs():
    return [png_path(size) for size in SIZES] + [ICONS / 'wimsy.ico']


def sha256(path):
    data = path.read_bytes()
    if path.suffix == '.svg':  # text: a Windows checkout may give it CRLF line ends
        data = data.replace(b'\r\n', b'\n')
    return hashlib.sha256(data).hexdigest()


OPTIPNG = ['optipng', '-quiet', '-o7', '-strip', 'all', '-nc', '-nb']


def version(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.splitlines()[0].strip()


def renderer():
    """The tools that make the PNGs, as the manifest names them; None when one is missing."""
    draw, squeeze = version(['rsvg-convert', '--version']), version(['optipng', '-v'])
    return f'{draw} and {squeeze} ({" ".join(OPTIPNG[2:])})' if draw and squeeze else None


def render(size, destination):
    subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), '-o', str(destination), str(SOURCE)], check=True)
    subprocess.run(OPTIPNG + [str(destination)], check=True)


def png_size(data):
    """Width, height and colour type from the IHDR chunk, which a PNG starts with."""
    if data[:8] != PNG_SIGNATURE or data[12:16] != b'IHDR':
        raise ValueError('not a PNG')
    width, height, _depth, colour = struct.unpack('>IIBB', data[16:26])
    return width, height, colour


def make_ico(images):
    """An ICO holding each PNG as is (Windows reads PNG images in icons since Vista)."""
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries, payload = b'', b''
    for size, data in images:
        # 0 in a width or height byte means 256
        entries += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset + len(payload))
        payload += data
    return header + entries + payload


def read_ico(data):
    """The (size, image bytes) pairs of an ICO."""
    reserved, kind, count = struct.unpack('<HHH', data[:6])
    if reserved != 0 or kind != 1:
        raise ValueError('not an icon file')
    images = []
    for i in range(count):
        width, height, _colours, _reserved, _planes, _bits, length, offset = struct.unpack(
            '<BBBBHHII', data[6 + 16 * i : 22 + 16 * i]
        )
        if width != height:
            raise ValueError(f'entry {i} is not square')
        images.append((width or 256, data[offset : offset + length]))
    return images


def make():
    drawn_by = renderer()
    if not drawn_by:
        sys.exit('make-icons.py: rsvg-convert (librsvg) and optipng are needed to make the icons')
    for size in SIZES:
        png_path(size).parent.mkdir(parents=True, exist_ok=True)
        render(size, png_path(size))
    ico = make_ico([(size, png_path(size).read_bytes()) for size in ICO_SIZES])
    (ICONS / 'wimsy.ico').write_bytes(ico)
    lines = [f'# scripts/make-icons.py drew these from wimsy.svg with {drawn_by}; run it again after changing the SVG']
    for path in [SOURCE] + outputs():
        lines.append(f'{sha256(path)}  {path.relative_to(DIST).as_posix()}')
    MANIFEST.write_text('\n'.join(lines) + '\n')
    print(f'made {len(outputs())} files in {ICONS.relative_to(ROOT)} with {drawn_by}')


def check():
    problems = []
    if not MANIFEST.is_file():
        sys.exit(f'{MANIFEST.relative_to(ROOT)} is missing: run scripts/make-icons.py')
    lines = MANIFEST.read_text().splitlines()
    drawn_by = lines[0].split(' with ', 1)[1].split(';', 1)[0] if lines and ' with ' in lines[0] else None
    listed = {}
    for line in lines[1:]:
        digest, name = line.split('  ', 1)
        listed[name] = digest
    wanted = [SOURCE] + outputs()
    if sorted(listed) != sorted(path.relative_to(DIST).as_posix() for path in wanted):
        problems.append('the manifest does not list wimsy.svg and exactly the outputs this script makes')
    for path in wanted:
        name = path.relative_to(DIST).as_posix()
        if not path.is_file():
            problems.append(f'{name} is missing')
        elif listed.get(name) != sha256(path):
            what = 'the SVG changed since the icons were drawn' if path == SOURCE else 'it was changed by hand'
            problems.append(f'{name} does not match the manifest: {what}')
    for size in SIZES:
        if png_path(size).is_file():
            width, height, colour = png_size(png_path(size).read_bytes())
            if (width, height) != (size, size) or colour != 6:
                problems.append(f'{png_path(size).relative_to(DIST)} is {width}x{height}, colour type {colour}')
    if (ICONS / 'wimsy.ico').is_file():
        images = read_ico((ICONS / 'wimsy.ico').read_bytes())
        if [size for size, _ in images] != list(ICO_SIZES):
            problems.append(f'wimsy.ico holds the sizes {[size for size, _ in images]}, not {list(ICO_SIZES)}')
        for size, data in images:
            if size in SIZES and png_path(size).is_file() and data != png_path(size).read_bytes():
                problems.append(f"wimsy.ico's {size} image is not hicolor/{size}x{size}/apps/wimsy.png")
    if not problems:
        at_hand = renderer()
        if at_hand and at_hand == drawn_by:
            with tempfile.TemporaryDirectory() as temporary:
                for size in SIZES:
                    drawn = Path(temporary) / f'{size}.png'
                    render(size, drawn)
                    if drawn.read_bytes() != png_path(size).read_bytes():
                        problems.append(f'{size}x{size} drawn again differs from the checked-in one')
            if not problems:
                print(f'made every size again with {at_hand}: the same bytes')
        else:
            print(f'not made again: made with {drawn_by}, and here there is {at_hand or "no rsvg-convert or optipng"}')
    if problems:
        sys.exit('make-icons.py --check:\n  ' + '\n  '.join(problems))
    print(f'icons checked: {len(outputs())} files match wimsy.svg and the manifest')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true', help='check the icons instead of drawing them')
    args = parser.parse_args()
    check() if args.check else make()


if __name__ == '__main__':
    main()
