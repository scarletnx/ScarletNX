#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The DS driver's command line, without a card image: the recompile's threads as every driver takes them, aot_threads
and aot_threads_most, which the launcher's Gentle queue gives every driver, each by its name with - and by its key,
the older spelling, said once; a value they do not take stops the run with it."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    # absolute, not resolved: scarletnx-nds may be a link to scarletnx, which runs the driver its name says
    driver = pathlib.Path(sys.argv[1]).absolute()
    failures = []

    with tempfile.TemporaryDirectory(prefix='snx-nds-driver-', ignore_cleanup_errors=True) as temporary:
        work = pathlib.Path(temporary)

        def run(args, status, contains):
            """the driver with args; its output, which must hold contains"""
            p = subprocess.run([str(driver), *args], capture_output=True, text=True, timeout=30, cwd=work)
            output = p.stdout + p.stderr
            if p.returncode != status or contains not in output:
                failures.append(f'{args}: exit {p.returncode}, not {status}, or no "{contains}"\n{output}')
            return output

        help_text = run(['--help'], 0, 'NDS options:')
        if '\n  --aot-threads-most=N   The most threads --aot-threads=0 takes' not in help_text:
            failures.append(f'the help lacks --aot-threads-most=N:\n{help_text}')
        # taken, as the Gentle queue gives them: the run goes on to the image, which is not there (status 1)
        missing = ['--output', str(work / 'game'), str(work / 'missing.nds')]
        for given in (['--aot-threads=0', '--aot-threads-most=3'], ['--aot-threads-most=3']):
            out = run(['-r', *given, *missing], 1, 'scarletnx-nds: ')
            if 'unexpected argument' in out:
                failures.append(f'{given} refused:\n{out}')
        # the older spelling, which the launcher gave before: read, and said once
        out = run(
            ['-r', '--aot_threads_most=3', '--aot_threads_most=3', *missing],
            1,
            'settings: --aot_threads_most is the old spelling of --aot-threads-most: it is read as that; write '
            '--aot-threads-most',
        )
        if out.count('old spelling') != 1 or 'unexpected argument' in out:
            failures.append(f'the old spelling refused or not said once:\n{out}')
        run(['-r', '--aot-threads-most=abc', *missing], 2, 'scarletnx-nds: aot_threads_most=abc is not a whole number')
        run(['-r', '--aot-threads=2000', *missing], 2, 'scarletnx-nds: aot_threads=2000 is not a whole number from 0')
    if failures:
        sys.exit('nds_driver:\n' + '\n'.join(failures))
    print('nds_driver: ok')


if __name__ == '__main__':
    main()
