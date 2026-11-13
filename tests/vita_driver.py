#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Vita driver's command line, without a game: the recompile's options every driver shares
(common/utilities/cli.h) in its help, each value they do not take stopping the run with it, and none of them with -x,
which compiles no code."""

import pathlib
import subprocess
import sys


def main():
    # absolute, not resolved: scarletnx-vita may be a link to scarletnx, which runs the driver its name says
    driver = pathlib.Path(sys.argv[1]).absolute()
    failures = []

    def run(args, status, contains):
        p = subprocess.run([str(driver), *args], capture_output=True, text=True, timeout=30)
        output = p.stdout + p.stderr
        if p.returncode != status or contains not in output:
            failures.append(f'{args}: exit {p.returncode}, not {status}, or no "{contains}"\n{output}')
        return output

    help_text = run(['--help'], 0, 'Common options')
    for line in (
        '\n  -O, --opt LEVEL        LLVM',
        '\n  --jobs N',
        '\n  --chunk N',
        '\n  --jit-cache PATH',
        '\n  --jit-only',
        '\n  --keep-ir',
        '\n  -q, --quiet',
        'On the Vita, -O is the level',
    ):
        if line not in help_text:
            failures.append(f'the help lacks "{line.strip()}"')
    run(['-O9', 'game.vpk'], 2, 'scarletnx-vita: -O 9: not 0, 1, 2, 3, s or z')
    run(['--jobs=1025', 'game.vpk'], 2, 'scarletnx-vita: --jobs 1025: not a whole number from 0 (Auto) to 1024')
    run(['--chunk', 'x', 'game.vpk'], 2, 'scarletnx-vita: --chunk x: not a whole number of instructions from 1')
    run(['-x', '--keep-ir', 'game.vpk'], 2, "scarletnx-vita: --keep-ir is the recompile's, and -x compiles no code")
    run(['-x', '--opt=1', 'game.vpk'], 2, "scarletnx-vita: -O is the recompile's, and -x compiles no code")
    if failures:
        sys.exit('vita_driver:\n' + '\n'.join(failures))
    print('vita_driver: ok')


if __name__ == '__main__':
    main()
