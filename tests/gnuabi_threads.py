#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Guest threads and signals in the GNU ABI runtime: the freestanding fixtures in tests/gnuabi (threads.c, signals.c,
restart.c, sigsuspend.c, wake.c, with thread_start.S), built with Clang for aarch64-linux-gnu and LLD, must each exit 0
and print their lines. Without a compiler that targets AArch64 the test is skipped (exit 77).

    gnuabi_threads.py RUNNER [--clang CLANG]
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

FIXTURES = {
    'threads': ['threads: lock ok', 'threads: futex results ok', 'threads: requeue ok'],
    'signals': ['signals: kill ok', 'signals: frame ok', 'signals: mask ok', 'signals: ucontext ok',
                'signals: altstack ok', 'signals: sigqueue ok', 'signals: thread ok'],
    'restart': ['restart: SA_RESTART ok', 'restart: EINTR ok', 'restart: nanosleep ok'],
    'sigsuspend': ['sigsuspend: ok', 'sigtimedwait: ok'],
    'wake': ['wake: ppoll waits after a signal'],
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--clang', default=shutil.which('clang'))
    args = parser.parse_args()
    if not args.clang or not shutil.which('ld.lld'):
        print('no clang or ld.lld: skipped')
        return 77
    source = pathlib.Path(__file__).resolve().parent / 'gnuabi'
    failures = 0
    with tempfile.TemporaryDirectory(prefix='snx-gnuabi-threads-') as temporary:
        scratch = pathlib.Path(temporary)
        for name, lines in FIXTURES.items():
            elf = scratch / name
            # -mno-outline-atomics: on an AArch64 host Clang calls libgcc's __aarch64_* helpers for atomics, and
            # -nostdlib leaves libgcc out
            build = [args.clang, '--target=aarch64-linux-gnu', '-fuse-ld=lld', '-static', '-O2', '-ffreestanding',
                     '-fno-builtin', '-fno-stack-protector', '-fno-pie', '-nostdlib', '-march=armv8-a',
                     '-mno-outline-atomics',
                     '-Wl,-e,_start', '-I', str(source), str(source / 'start.S'), str(source / 'thread_start.S'),
                     str(source / f'{name}.c'), '-o', str(elf)]
            built = subprocess.run(build, capture_output=True, text=True)
            if built.returncode:
                if 'unable to create target' in built.stderr or 'unknown target' in built.stderr:
                    print('clang has no AArch64 target: skipped')
                    return 77
                print(f'{name}: build failed\n{built.stderr}')
                failures += 1
                continue
            run = subprocess.run([args.runner, '--isolated', f'--jit-cache={scratch / "cache"}', str(elf)],
                                 capture_output=True, text=True, timeout=120)
            missing = [line for line in lines if line not in run.stdout.splitlines()]
            if run.returncode or missing:
                print(f'{name}: exit {run.returncode}, missing {missing}\n{run.stdout}{run.stderr}')
                failures += 1
            else:
                print(f'{name}: ok')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
