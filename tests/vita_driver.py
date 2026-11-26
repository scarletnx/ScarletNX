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
        'KEY is the key with a - for each _; the key as written, the older spelling, is still\n'
        '                         read, and said once',
    ):
        if line not in help_text:
            failures.append(f'the help lacks "{line.strip()}"')
    run(['-O9', 'game.vpk'], 2, 'scarletnx-vita: -O 9: not 0, 1, 2, 3, s or z')
    run(['--jobs=1025', 'game.vpk'], 2, 'scarletnx-vita: --jobs 1025: not a whole number from 0 (Auto) to 1024')
    run(['--chunk', 'x', 'game.vpk'], 2, 'scarletnx-vita: --chunk x: not a whole number of instructions from 1')
    run(['-x', '--keep-ir', 'game.vpk'], 2, "scarletnx-vita: --keep-ir is the recompile's, and -x compiles no code")
    run(['-x', '--opt=1', 'game.vpk'], 2, "scarletnx-vita: -O is the recompile's, and -x compiles no code")
    # a setting's key with a - for each _ is the same setting, said by its key, --press-len= the Vita's press length
    # as --press-len= is; some _ left is no setting
    run(['--aot-threads=many', 'game.vpk'], 2, 'scarletnx-vita: aot_threads=many is not a whole number from 0 to 1024')
    run(
        ['--press-len=-5', '--help'],
        0,
        'settings: vita_press_len=-5 is not a whole number from 0 to 2147483647: it keeps 0',
    )
    run(['--vita-hle_trace=1', 'game.vpk'], 2, 'scarletnx-vita: unknown setting: --vita-hle_trace=1')
    # the on-screen controls' settings and the front panel's, as a Switch game's: taken, a value they do not take said
    run(['--controls=1', '--controls-stick-l-shown=0', '--touch=1', '--launcher-picks=1', '--help'], 0, 'Vita options:')
    run(['--controls=3', 'game.vpk'], 2, 'scarletnx-vita: controls=3 ')
    run(['--touch=x', 'game.vpk'], 2, 'scarletnx-vita: touch=x ')
    # the Switch's settings a Vita game takes, as the controls' (common/utilities/console_settings.c); one it does not
    # take said with why
    run(
        [
            '--fullscreen=1',
            '--upscaler=fsr1',
            '--vulkan-ban-ignore=x',
            '--vsync=0',
            '--volume=40',
            '--gpu-pstate=auto',
            '--vk-pipeline-cache=1',
            '--slp-vectorizer=0',
            '--perf-map=0',
            '--gdb-jit=0',
            '--stats=0',
            '--frame-pacing=2',
            '--async-pipelines=1',
            '--pipeline-threads=2',
            '--renderdoc=',
            '--prepare-shaders=1',
            '--compress-prepared-shaders=0',
            '--vk-gpu-profile=0',
            '--vk-checkpoints=0',
            '--help',
        ],
        0,
        'Vita options:',
    )
    run(['--gpu-driver=', '--vk-validation=1', '--gamepad-swap-ab=1', '--help'], 0, 'Vita options:')
    run(['--docked=1', 'game.vpk'], 2, 'scarletnx-vita: --docked=1 does nothing in a Vita game: it has no dock')
    run(['--log=2', 'game.vpk'], 2, 'scarletnx-vita: --log=2 does nothing in a Vita game: it writes no game log yet')
    # the key as the .ini writes it, the older spelling: still read, and said once a run with the name now
    out = run(
        ['--vita_hle_trace=1', '--vita_hle_trace=2', '--help'],
        0,
        'settings: --vita_hle_trace is the old spelling of --vita-hle-trace: it is read as that; write --vita-hle-trace',
    )
    if out.count('old spelling') != 1:
        failures.append(f'the old spelling not said once:\n{out}')
    if failures:
        sys.exit('vita_driver:\n' + '\n'.join(failures))
    print('vita_driver: ok')


if __name__ == '__main__':
    main()
