#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The DS driver's command line, without a card image: the recompile's threads as every driver takes them, aot_threads
and aot_threads_most, which the launcher's Gentle queue gives every driver, each by its name with - and by its key,
the older spelling, said once; a value they do not take stops the run with it. The WFC service, nds_wfc and
nds_wfc_dns, from the command line or the launcher's settings.ini: a service the list has not, or a custom server
that is not an address, stops the run before anything is made, naming the value."""

import os
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

        # a home of the test's own: the launcher's settings.ini the driver reads is the one written below
        env = dict(os.environ)
        for name in ('HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME'):
            env[name] = str(work / 'home' / name.lower())
        config = pathlib.Path(env['XDG_CONFIG_HOME']) / 'scarletnx'
        config.mkdir(parents=True)

        def run(args, status, contains):
            """the driver with args; its output, which must hold contains"""
            p = subprocess.run([str(driver), *args], capture_output=True, text=True, timeout=30, cwd=work, env=env)
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
        # the WFC service: a word the list has not, refused with it and the list
        run(['-r', '--nds-wfc=nintendo', *missing], 2, '"nintendo" is none of off, custom, kaeru, wiilink, wiimmfi')
        run(['-r', '--nds-wfc=custom', *missing], 2, 'nds_wfc_dns=: the custom DNS server "" is not an IPv4 address')
        run(['-r', '--nds-wfc=custom', '--nds-wfc-dns=10.0.0.256', *missing], 2, '"10.0.0.256" is not an IPv4')
        run(['-r', '--nds-wfc-dns=1234567890123456', *missing], 2, "nds_wfc_dns's value is longer than 15 bytes")
        # taken: the run goes on to the image
        for given in (['--nds-wfc=off'], ['--nds-wfc=altwfc'], ['--nds-wfc=custom', '--nds-wfc-dns=10.0.0.53']):
            out = run(['-r', *given, *missing], 1, 'scarletnx-nds: ')
            if 'nds_wfc' in out:
                failures.append(f'{given} refused:\n{out}')
        # the launcher's settings.ini: read where the command line gives none, the command line's over it
        (config / 'settings.ini').write_text('[Online]\nnds_wfc=retired\n', encoding='utf-8')
        run(['-r', *missing], 2, 'nds_wfc=retired nds_wfc_dns=: "retired" is none of off, custom, kaeru')
        out = run(['-r', '--nds-wfc=wiimmfi', *missing], 1, 'scarletnx-nds: ')
        if 'retired' in out:
            failures.append(f'the command line did not win over settings.ini:\n{out}')
        (config / 'settings.ini').write_text('[Online]\nnds_wfc=custom\nnds_wfc_dns=10.0.0.53\n', encoding='utf-8')
        out = run(['-r', *missing], 1, 'scarletnx-nds: ')
        if 'nds_wfc' in out:
            failures.append(f'settings.ini\'s custom server refused:\n{out}')
    if failures:
        sys.exit('nds_driver:\n' + '\n'.join(failures))
    print('nds_driver: ok')


if __name__ == '__main__':
    main()
