#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""DenSinH's FuzzARM on the AGB board: one ROM per run, recompiled cold, run for a fixed number of frames.

Each ROM runs 10000 random ARM or Thumb instructions (data processing, multiplies, loads and stores) and compares the
registers and flags they leave with the values its generator computed. One that matched them all returns from its
test loop into a `b .` whose return address is that loop, and draws "End of testing" alone on its top line; one that
met a difference waits for a key in a loop of its own, the instruction, its inputs, what it got and what it expected
on the screen.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROMS = ('ARM_Any', 'ARM_DataProcessing', 'THUMB_Any', 'THUMB_DataProcessing', 'FuzzARM')
WIDTH, HEIGHT = 240, 160


def read_ppm(path):
    data = path.read_bytes()
    header = data.split(b'\n', 3)
    if header[0] != b'P6' or header[1] != b'%d %d' % (WIDTH, HEIGHT) or header[2] != b'255':
        raise ValueError(f'not a 240x160 frame: {path}')
    return header[3]


def lit_rows(pixels):
    """the rows that hold a pixel other than black"""
    return [y for y in range(HEIGHT) if any(pixels[y * WIDTH * 3 : (y + 1) * WIDTH * 3])]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True, help='scarletnx-agb')
    parser.add_argument('--roms', required=True, help='the FuzzARM checkout')
    parser.add_argument('--frames', type=int, default=1800)
    parser.add_argument('name', choices=ROMS)
    args = parser.parse_args()
    # Windows can still hold the runtime's copy open for a moment after the run: see tests/agb_gba_tests.py
    with tempfile.TemporaryDirectory(prefix='snx-agb-', ignore_cleanup_errors=True) as work:
        work = pathlib.Path(work)
        rom = work / (args.name + '.gba')
        shutil.copyfile(pathlib.Path(args.roms) / rom.name, rom)
        shot = work / 'frame.ppm'
        run = subprocess.run(
            [
                args.runner,
                f'--frames={args.frames}',
                f'--shot={shot}',
                '--registers',
                '--outdir',
                str(work),
                str(rom),
            ],
            capture_output=True,
            text=True,
            timeout=600,
            check=False,
        )
        if run.returncode:
            sys.exit(f'{args.name}: scarletnx-agb exited with {run.returncode}\n{run.stdout}{run.stderr}')
        found = re.search(r'\br14=([0-9A-F]{8}) r15=([0-9A-F]{8})', run.stdout)
        if not found:
            sys.exit(f'{args.name}: no registers in the output\n{run.stdout}')
        lr, pc = int(found.group(1), 16), int(found.group(2), 16)
        if lr != pc or pc >> 24 != 0x08:
            sys.exit(
                f'{args.name}: at frame {args.frames} it stood at 0x{pc:08X} (lr 0x{lr:08X}), not in the loop after its '
                f'tests: one gave other values than the generator computed (its screen names it), or they had not '
                f'ended\n{run.stdout}'
            )
        rows = lit_rows(read_ppm(shot))
        if not rows or rows[-1] > 7:
            sys.exit(f'{args.name}: the screen holds more than "End of testing" on its top line: lit rows {rows}')
    print(f'{args.name}: passed')


if __name__ == '__main__':
    main()
