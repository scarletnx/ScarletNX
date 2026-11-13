#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""One section of mGBA's test suite (mgba-emu/suite, the ROM scarletnx/mgba-suite-rom keeps) on the AGB board.

The suite's menu lists its sections; the run moves down to one, presses A, and runs until the section has drawn its
results: "passes/total" on its second text row, read here through the suite's own font, which the pinned ROM holds at
FONT. Its expected values were measured on the console. A section passes when it reaches the count given: all of its
results, or the count it reaches today with the issue that keeps it there; any other count fails, so a regression and
a fix alike say so. The results that failed are in the save, which the suite writes as text.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

# the menu's order: (name, results)
SECTIONS = [
    ('memory', 1552),
    ('io_read', 130),
    ('timing', 2020),
    ('timer_count_up', 936),
    ('timer_irq', 90),
    ('shifter', 140),
    ('carry', 93),
    ('multiply_long', 72),
    ('bios_math', 615),
    ('dma', 1244),
    ('sio_read', 90),
    ('sio_timing', 8),
    ('misc_edge', 12),
]
FONT = 0x2C194  # the 96 glyphs from ' ', 8x8 at 4 bits a pixel, in the ROM of e694203
WIDTH, HEIGHT = 240, 160


def glyphs(rom):
    table = {}
    for index in range(96):
        tile = rom[FONT + index * 32 : FONT + (index + 1) * 32]
        mask = tuple((tile[y * 4 + x // 2] >> (x % 2 * 4) & 15) != 0 for y in range(8) for x in range(8))
        table.setdefault(mask, chr(0x20 + index))
    return table


def text_row(frame, row, table):
    """Row row of the text grid, which the suite scrolls down 4 lines, each cell matched against the font"""
    line = []
    for column in range(30):
        cell = [
            frame[
                ((row * 8 + 4 + y) * WIDTH + column * 8 + x) * 3 : ((row * 8 + 4 + y) * WIDTH + column * 8 + x) * 3 + 3
            ]
            for y in range(8)
            for x in range(8)
        ]
        background = max(set(cell), key=cell.count)
        mask = tuple(pixel != background for pixel in cell)
        line.append(table.get(mask, '?') if any(mask) else ' ')
    return ''.join(line)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True, help='scarletnx-agb')
    parser.add_argument('--rom', required=True, help="the suite's ROM")
    parser.add_argument('--frames', type=int, default=3600)
    parser.add_argument('--expect', type=int, help='the results it passes today, when not all (with --issue)')
    parser.add_argument('--issue', help='the issue that keeps it at --expect')
    parser.add_argument('name', choices=[name for name, _ in SECTIONS])
    args = parser.parse_args()
    index = [name for name, _ in SECTIONS].index(args.name)
    total = SECTIONS[index][1]
    expect = total if args.expect is None else args.expect
    with tempfile.TemporaryDirectory(prefix='snx-agb-mgba-', ignore_cleanup_errors=True) as work:
        work = pathlib.Path(work)
        rom = work / 'suite.gba'
        shutil.copyfile(args.rom, rom)
        shot = work / 'frame.ppm'
        presses = [f'{300 * (i + 1)}:80' for i in range(index)] + [f'{300 * (index + 2)}:1']
        run = subprocess.run(
            [
                args.runner,
                f'--frames={args.frames}',
                f'--shot={shot}',
                f'--script={",".join(presses)}',
                '--press_len=100',
                '--outdir',
                str(work),
                str(rom),
            ],
            capture_output=True,
            text=True,
            timeout=900,
            check=False,
        )
        if run.returncode:
            sys.exit(f'{args.name}: scarletnx-agb exited with {run.returncode}\n{run.stdout}{run.stderr}')
        frame = shot.read_bytes().split(b'\n', 3)[3]
        row = text_row(frame, 1, glyphs(rom.read_bytes()))
        field = row[21:].strip()
        passes, slash, counted = field.partition('/')
        if not slash or not passes.isdigit() or not counted.isdigit():
            sys.exit(f'{args.name}: no "passes/total" drawn by frame {args.frames}: its row reads "{row.rstrip()}"')
        passes, counted = int(passes), int(counted)
        save = work / 'suite' / 'suite.sav'
        log = save.read_bytes().split(b'\0', 1)[0].decode('latin-1') if save.is_file() else ''
        failed = '\n'.join(line for line in log.splitlines() if line.endswith('FAIL'))
        if counted != total or passes != expect:
            issue = f', {args.issue}' if args.issue else ''
            sys.exit(f'{args.name}: {passes}/{counted}, not {expect}/{total}{issue}; the failed results:\n{failed}')
    note = f' (all {total})' if expect == total else f' of {total}, as {args.issue} has it'
    print(f'{args.name}: {passes}{note}')


if __name__ == '__main__':
    main()
