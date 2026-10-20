#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""jsmolka's gba-tests on the AGB board: one ROM per run, recompiled cold, run for a fixed number of frames.

The self-checking ROMs keep the number of their first failed test in a register (0 when every test passed) and
draw their verdict as text, so a ROM passes when that register is 0 and the frame holds the text: one that never
reached its verdict leaves the register at 0 and the screen empty. The display ROMs draw a fixed picture, checked
against what their source draws.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

# name: (path in gba-tests, the register holding the result, or the display check)
ROMS = {
    'arm': ('arm/arm.gba', 12),
    'thumb': ('thumb/thumb.gba', 7),
    'memory': ('memory/memory.gba', 12),
    'bios': ('bios/bios.gba', 12),
    'nes': ('nes/nes.gba', 12),
    'unsafe': ('unsafe/unsafe.gba', 12),
    'save_none': ('save/none.gba', 12),
    'save_sram': ('save/sram.gba', 12),
    'save_flash64': ('save/flash64.gba', 12),
    'save_flash128': ('save/flash128.gba', 12),
    'ppu_hello': ('ppu/hello.gba', 'hello'),
    'ppu_shades': ('ppu/shades.gba', 'shades'),
    'ppu_stripes': ('ppu/stripes.gba', 'stripes'),
}
WIDTH, HEIGHT = 240, 160


def read_ppm(path):
    data = path.read_bytes()
    header = data.split(b'\n', 3)
    if header[0] != b'P6' or header[1] != b'%d %d' % (WIDTH, HEIGHT) or header[2] != b'255':
        raise ValueError('not a 240x160 frame: %s' % path)
    pixels = header[3]
    return [[tuple(pixels[(y * WIDTH + x) * 3:(y * WIDTH + x) * 3 + 3]) for x in range(WIDTH)] for y in range(HEIGHT)]


def runs(row):
    out = []
    for pixel in row:
        if out and out[-1][0] == pixel:
            out[-1][1] += 1
        else:
            out.append([pixel, 1])
    return out


def same_rows(frame):
    return all(row == frame[0] for row in frame)


def check_hello(frame):
    # white text on the black backdrop
    colors = {pixel for row in frame for pixel in row}
    return (0, 0, 0) in colors and len(colors) > 1, 'expected text on a black backdrop'


def check_shades(frame):
    # 16 palette entries of rising blue, a tile each, two map columns per tile: 15 bands of 16 pixels on screen
    bands = runs(frame[0])
    ok = same_rows(frame) and len(bands) == 15 and all(n == 16 for _, n in bands)
    ok = ok and bands[0][0] == (0, 0, 0) and all(r == 0 and g == 0 for (r, g, _), _ in bands)
    ok = ok and all(bands[i][0][2] < bands[i + 1][0][2] for i in range(len(bands) - 1))
    return ok, 'expected 15 bands of 16 pixels in rising blue'


def check_stripes(frame):
    # tile 1 in every other map entry over the backdrop: 30 stripes of 8 pixels in two alternating colours
    stripes = runs(frame[0])
    ok = same_rows(frame) and len(stripes) == 30 and all(n == 8 for _, n in stripes)
    ok = ok and stripes[0][0] != stripes[1][0]
    ok = ok and all(stripes[i][0] == stripes[i % 2][0] for i in range(len(stripes)))
    return ok, 'expected 30 stripes of 8 pixels in two alternating colours'


CHECKS = {'hello': check_hello, 'shades': check_shades, 'stripes': check_stripes}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True, help='scarletnx-agb')
    parser.add_argument('--tests', required=True, help='the gba-tests checkout')
    parser.add_argument('--frames', type=int, default=120)
    parser.add_argument('name', choices=sorted(ROMS))
    args = parser.parse_args()
    path, check = ROMS[args.name]
    with tempfile.TemporaryDirectory(prefix='snx-agb-') as work:
        work = pathlib.Path(work)
        rom = work / pathlib.Path(path).name  # the recompiled module is cached beside the ROM
        shutil.copyfile(pathlib.Path(args.tests) / path, rom)
        shot = work / 'frame.ppm'
        run = subprocess.run([args.runner, '--frames=%d' % args.frames, '--shot=%s' % shot, '--registers', str(rom)],
                             capture_output=True, text=True, timeout=600)
        if run.returncode:
            sys.exit('%s: scarletnx-agb exited with %d\n%s%s' % (args.name, run.returncode, run.stdout, run.stderr))
        frame = read_ppm(shot)
        if isinstance(check, int):
            found = re.search(r'\br%d=([0-9A-F]{8})' % check, run.stdout)
            if not found:
                sys.exit('%s: no registers in the output\n%s' % (args.name, run.stdout))
            failed = int(found.group(1), 16)
            if failed:
                sys.exit('%s: failed test %d' % (args.name, failed))
            if same_rows(frame) and len(runs(frame[0])) == 1:
                sys.exit('%s: the ROM drew no verdict' % args.name)
        else:
            ok, expected = CHECKS[check](frame)
            if not ok:
                sys.exit('%s: %s' % (args.name, expected))
    print('%s: passed' % args.name)


if __name__ == '__main__':
    main()
