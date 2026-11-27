#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Android driver's command line, without an app: the Android group's settings of the launcher's Settings by their
keys with a - for each _ (docs/user/settings.md), read before any JVM starts. Each value a setting does not take stops
the run with it, and what is taken ends in the usage, as no APK is given (status 2, and no option is unknown)."""

import pathlib
import subprocess
import sys


def main():
    # absolute, not resolved: scarletnx-android may be a link to scarletnx, which runs the driver its name says
    driver = pathlib.Path(sys.argv[1]).absolute()
    failures = []

    def run(args, contains, absent=()):
        p = subprocess.run([str(driver), *args], capture_output=True, text=True, timeout=60)
        output = p.stdout + p.stderr
        if p.returncode != 2 or contains not in output or any(a in output for a in absent):
            failures.append(f'{args}: exit {p.returncode}, not 2, no "{contains}" or one of {absent}\n{output}')

    usage = 'Usage: scarletnx-android'
    run(['--help-me'], 'unknown option --help-me')
    for given in (
        ['--android-jit-opt=s'],
        ['--android-jit-opt=0', '--android-jit-opt=2'],
        ['--android-java-options=-Xmx2g -Dname=value'],
        ['--android-java-options='],
        ['--android-log-level=W'],
        ['--android-trace-frames=1', '--android-trace-frames=0'],
        ['--android-log-signals=1', '--android-sample=1'],
        ['--android-jit-verbose=1', '--android-linker-verbose=1'],
        ['--android-jit-keep-ir=/tmp/no-such-folder', '--android-jit-keep-ir='],
        ['--android_log_level=V'],
    ):
        run(given, usage, ('unknown option',))
    # LLVM's level and kept IR, which went with it: said with the value given, and nothing done
    run(['--android-jit-opt=9'], 'settings: android_jit_opt=9, deprecated, went with LLVM')
    run(['--android-jit-keep-ir=/a'], 'settings: android_jit_keep_ir=/a, deprecated, went with LLVM')
    run(['--android-trace-frames=2'], 'android_trace_frames=2 is not a whole number from 0 to 1')
    run(['--android-sample=x'], 'android_sample=x is not a whole number from 0 to 1')
    run(['--android-log-level=VV'], '--android-log-level takes V, D, I, W, E or A, not VV')
    run(['--android-log-level=Q'], '--android-log-level takes V, D, I, W, E or A, not Q')
    run(['--android-nosuch=1'], 'unknown option --android-nosuch=1')
    if failures:
        sys.exit('android_driver:\n' + '\n'.join(failures))
    print('android_driver: ok')


if __name__ == '__main__':
    main()
