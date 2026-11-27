#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The recompile's options every driver shares (common/utilities/cli.h), on the GBA runner: -O and --keep-ir are said
and change nothing (the native back end has no levels and no IR), -q says no progress, --jit-only plays with no module, --jit-cache names
the map of the blocks runs reached that the recompile starts from, and --jobs, --chunk and the launcher's Gentle
--aot-threads-most=N, which one module compiled whole has nothing to give, are said. A ROM made here puts 42 in r5 and
spins (mov r5, #42; b .)."""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile


def rom():
    """A cartridge the board takes: b 0x080000C0, its fixed byte and complement, then the code"""
    image = bytearray(0xC0)
    image[0:4] = (0xEA00002E).to_bytes(4, 'little')
    image[0xA0:0xAC] = b'OPTIONS'.ljust(12, b'\0')
    image[0xAC:0xB2] = b'AOPT01'
    image[0xB2] = 0x96
    image[0xBD] = -(sum(image[0xA0:0xBD]) + 0x19) & 0xFF
    for word in (0xE3A0502A, 0xEAFFFFFE):
        image += word.to_bytes(4, 'little')
    return bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='snx-agb-options-', ignore_cleanup_errors=True) as temporary:
        work = pathlib.Path(temporary)
        (work / 'game.gba').write_bytes(rom())

        def run(name, *options, status=0):
            """the runner on game.gba with options, its games in work/name: its output"""
            p = subprocess.run(
                [args.runner, *options, '--outdir', str(work / name), str(work / 'game.gba')],
                capture_output=True,
                text=True,
                timeout=300,
            )
            if p.returncode != status:
                failures.append(f'{options}: exit {p.returncode}, not {status}\n{p.stdout}{p.stderr}')
            return p.stdout + p.stderr

        def check(good, what, text):
            if not good:
                failures.append(f'{what}\n{text}')

        def module(name):
            path = work / name / 'game' / 'game_code.so'
            return path.read_bytes() if path.is_file() else None

        text = run('kept', '-r', '-O0', '--keep-ir', '--jobs', '4', '--chunk', '100')
        check(
            'Recompiling ' in text and re.search(r'^Built \d+ blocks$', text, re.M), 'the recompile said nothing', text
        )
        check("--jobs 4: a ROM's code is one module, compiled on 1 thread" in text, '--jobs not said', text)
        check("--chunk 100: a ROM's code is one module, compiled as one unit" in text, '--chunk not said', text)
        check("--keep-ir went with LLVM's IR: the native back ends keep none" in text, '--keep-ir not said', text)
        check("-O 0, LLVM's level, went with LLVM" in text, '-O0 not said', text)
        check(not (work / 'kept' / 'game' / 'game_build').exists(), '--keep-ir made a folder', text)
        # -q: no progress; -O, said and choosing nothing, makes the same module
        text = run('quiet', '-r', '-q')
        check(
            'Recompiling' not in text and 'Built' not in text and 'back end: ' not in text, '-q said its progress', text
        )
        run('two', '-r', '-q', '--opt=2')
        check(module('quiet') is not None and module('quiet') == module('two'), '--opt=2 changed the module', text)
        # the native back end's module, whatever the level and --keep-ir
        run('kept2', '-r', '-q', '--keep-ir')
        check(module('kept2') is not None and module('kept2') == module('kept'), '--keep-ir changed the module', text)
        text = run('zero', '-r', '-O0')
        native = re.search(r'^(\S+) back end: (\d+) of (\d+) blocks', text, re.M)
        check(bool(native), 'who made the blocks is not said', text)
        check(not native or native[2] == native[3], 'the native back end left blocks', text)
        check(module('zero') == module('quiet'), '-O0 changed the module', text)
        # --jit-only: no module, every block compiled as the run reaches it
        text = run('jit', '--frames=2', '--registers', '--jit-only')
        check('No code ahead of time (--jit-only)' in text and re.search(r'\br5=0000002A\b', text), '--jit-only', text)
        check(module('jit') is None, '--jit-only made a module', text)
        # --jit-cache: another folder's map, which the recompile starts from; one that is not there stops it
        text = run('cached', '-r', '--jit-cache', str(work / 'kept' / 'game' / 'game_code.map'))
        check(re.search(r'^Built \d+ blocks$', text, re.M) and module('cached'), '--jit-cache made no module', text)
        missing = work / 'none.map'
        text = run('none', '-r', '--jit-cache', str(missing), status=1)
        check(f'scarletnx-agb: --jit-cache {missing}: cannot read {missing}' in text, 'a missing map not said', text)
        text = run('bad', '-r', '-O9')
        check("-O 9, LLVM's level, went with LLVM" in text and module('bad') == module('quiet'), '-O9 not said', text)
        # the launcher's Gentle queue gives every driver --aot-threads=0 and --aot-threads-most=N: both said
        text = run('gentle', '-r', '--aot-threads=0', '--aot-threads-most=4')
        check("aot_threads_most=4: a ROM's code is one module, compiled on 1 thread" in text, 'Gentle refused', text)
        # the same settings by their keys as written, the older spelling, each said once with the name now
        text = run('old', '-r', '--aot_threads=2', '--aot_threads_most=4')
        check(
            "aot_threads=2: a ROM's code is one module, compiled on 1 thread" in text
            and "aot_threads_most=4: a ROM's code is one module, compiled on 1 thread" in text
            and text.count('settings: --aot_threads is the old spelling of --aot-threads') == 1
            and text.count('settings: --aot_threads_most is the old spelling of --aot-threads-most') == 1,
            '--aot_threads refused, or not said once as the old spelling',
            text,
        )
    if failures:
        sys.exit('agb_recompile_options:\n' + '\n'.join(failures))
    print('agb_recompile_options: ok')


if __name__ == '__main__':
    main()
