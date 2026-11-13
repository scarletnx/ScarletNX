#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The GBA runner's scripted presses (--script=ms:hex, --press_len=ms) reach the game as KEYINPUT reads them.

A ROM made here reads KEYINPUT into r0 in a loop: the registers the runner prints at the end hold what the game read
in its last frame, a key held reading as its bit clear. The script goes by the console's frames (about 16.74 ms each),
so the same run reads the same keys with or without a window.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile


def rom():
    """A cartridge the board takes (its branch, fixed byte and complement), whose code reads KEYINPUT forever."""
    image = bytearray(0x100)
    image[0:4] = (0xEA00002E).to_bytes(4, 'little')  # b 0x080000C0
    image[0xA0:0xAC] = b'KEYS'.ljust(12, b'\0')
    image[0xAC:0xB2] = b'AKYS01'
    image[0xB2] = 0x96
    image[0xBD] = -(sum(image[0xA0:0xBD]) + 0x19) & 0xFF
    code = [
        0xE59F1008,  # ldr r1, [pc, #8]: KEYINPUT's address
        0xE1D100B0,  # ldrh r0, [r1]
        0xEAFFFFFD,  # b back to the ldrh
        0xE1A00000,  # nop
        0x04000130,  # KEYINPUT
    ]
    for i, word in enumerate(code):
        image[0xC0 + i * 4 : 0xC4 + i * 4] = word.to_bytes(4, 'little')
    return bytes(image)


def keys(runner, work, *options):
    """What the game read in KEYINPUT in the last of 5 frames (about 67 ms), and the run's output."""
    run = subprocess.run(
        [runner, '--frames=5', '--registers', '--outdir', str(work), str(work / 'keys.gba'), *options],
        capture_output=True,
        text=True,
        timeout=600,
    )
    found = re.search(r'\br0=([0-9A-F]{8})', run.stdout)
    if run.returncode or not found:
        sys.exit('agb_keys: %s exited with %d\n%s%s' % (' '.join(options), run.returncode, run.stdout, run.stderr))
    return int(found.group(1), 16), run.stdout + run.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    args = parser.parse_args()
    # Windows can still hold the runner open after a passed run (WinError 32 on its removal)
    with tempfile.TemporaryDirectory(prefix='snx-agb-keys-', ignore_cleanup_errors=True) as temporary:
        work = pathlib.Path(temporary)
        (work / 'keys.gba').write_bytes(rom())
        cases = [
            ((), 0x3FF, None),  # nothing held
            (('--script=0:9',), 0x3F6, None),  # A and Start from the start, held 300 ms
            (('--script=0:9', '--press_len=50'), 0x3FF, None),  # let go after 50 ms, before frame 5
            (('--script=40:200',), 0x1FF, None),  # L from 40 ms
            (('--script=0:1,soon:8',), 0x3FE, 'the script stops after 1 presses, at "soon:8"'),
            (('--script=0:400',), 0x3FF, 'the script stops after 0 presses, at "0:400"'),  # past KEYINPUT's ten keys
        ]
        for options, expected, said in cases:
            read, text = keys(args.runner, work, *options)
            if read != expected or (said and said not in text):
                sys.exit(
                    'agb_keys: %s read 0x%03X, not 0x%03X%s\n%s'
                    % (
                        ' '.join(options) or 'no script',
                        read,
                        expected,
                        ', or said nothing of it' if said else '',
                        text,
                    )
                )
        bad = subprocess.run(
            [args.runner, '--press_len=soon', str(work / 'keys.gba')], capture_output=True, text=True, timeout=60
        )
        if bad.returncode != 2 or '--press_len=soon is not a whole number of ms' not in bad.stderr:
            sys.exit('agb_keys: --press_len=soon was taken (%d)\n%s' % (bad.returncode, bad.stderr))
        # --aot_threads=, which the launcher's queue gives every driver: taken, and the recompile says what it does
        # with it; a value it does not take stops the run with the value
        made = subprocess.run(
            [args.runner, '-r', '--aot_threads=4', '--outdir', str(work), str(work / 'keys.gba')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        if made.returncode or "aot_threads=4: a ROM's code is one module, compiled on 1 thread\n" not in made.stdout:
            sys.exit('agb_keys: -r --aot_threads=4 (%d)\n%s%s' % (made.returncode, made.stdout, made.stderr))
        bad = subprocess.run(
            [args.runner, '-r', '--aot_threads=many', str(work / 'keys.gba')],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if bad.returncode != 2 or 'aot_threads=many is not a whole number from 0 to 1024' not in bad.stderr:
            sys.exit('agb_keys: --aot_threads=many was taken (%d)\n%s' % (bad.returncode, bad.stderr))
        # --gba_color=, the launcher's GBA colors: the screen in forced blank is white, raw 255 255 255 and gba as the
        # original GBA's screen shows white, 252 238 242; a value it does not take stops the run with the value
        for value, white in (('raw', b'\xff\xff\xff'), ('gba', b'\xfc\xee\xf2')):
            shot = work / ('%s.ppm' % value)
            run = subprocess.run(
                [
                    args.runner,
                    '--frames=2',
                    '--gba_color=%s' % value,
                    '--shot=%s' % shot,
                    '--outdir',
                    str(work),
                    str(work / 'keys.gba'),
                ],
                capture_output=True,
                text=True,
                timeout=600,
            )
            pixel = shot.read_bytes().split(b'\n', 3)[3][:3] if shot.is_file() else b''
            if run.returncode or pixel != white:
                sys.exit(
                    'agb_keys: --gba_color=%s (%d) showed white as %s, not %s\n%s%s'
                    % (value, run.returncode, pixel.hex(), white.hex(), run.stdout, run.stderr)
                )
        bad = subprocess.run(
            [args.runner, '--gba_color=sepia', str(work / 'keys.gba')], capture_output=True, text=True, timeout=60
        )
        if bad.returncode != 2 or '--gba_color=sepia is not raw or gba' not in bad.stderr:
            sys.exit('agb_keys: --gba_color=sepia was taken (%d)\n%s' % (bad.returncode, bad.stderr))
    print('agb_keys: passed')


if __name__ == '__main__':
    main()
