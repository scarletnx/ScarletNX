#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""How long the real recompiler takes to compile a synthetic game, headless.

The recompiler itself is the workload: a generated NRO with 512 functions and about 38,000 A64 instructions is compiled
by `scarletnx -r` with one job, a fresh cache and a fresh output folder on every run. The median CPU time is compared
with the reference for this host, the same warn/confirm/fail policy as the CPU and GPU tests, and a canary stops a
massive regression before the full block.

The NRO is generated here, so no game, keys or extracted files are needed. The test measures compilation and deploy,
not extraction, a real game's code mix, or the compiler's own build.
"""

import argparse
import math
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import resource
except ImportError:  # Windows has no RUSAGE_CHILDREN; wall time is the fallback there
    resource = None

# The body is 12 instructions of integer arithmetic and flags, repeated six times in each function; 512 functions make
# about 38,000 instructions, which the real LLVM path takes roughly a second to compile on this project's machines.
_BODY = [
    0x8B010000,  # add x0, x0, x1
    0xCA000C21,  # eor x1, x1, x0, lsl #3
    0x92089C02,  # and x2, x0, #0xff00ff00ff00ff00
    0xB2401843,  # orr x3, x2, #0x7f
    0xAB010064,  # adds x4, x3, x1
    0x9A804085,  # csel x5, x4, x0, mi
    0xD34554A6,  # ubfx x6, x5, #5, #17
    0x9B0100C0,  # madd x0, x6, x1, x0
    0x8B411C27,  # add x7, x1, x1, lsr #7
    0x12001CE8,  # and w8, w7, #0xff
    0xCA080021,  # eor x1, x1, x8
    0xD1000C21,  # sub x1, x1, #3
]
_RET = 0xD65F03C0
_COUNT = 6
_FUNCTIONS = 512
_FUNCTIONS_AT = 0x1000
_FUNCTION_SIZE = 0x200


def _put32(image, offset, value):
    struct.pack_into('<I', image, offset, value)


def _branch(pc, target):
    return 0x94000000 | (((target - pc) >> 2) & 0x03FFFFFF)


def make_nro(path):
    size = _FUNCTIONS_AT + _FUNCTIONS * _FUNCTION_SIZE
    image = bytearray(size)
    _put32(image, 0, 0x14000020)  # b 0x80, over the NRO header
    at = 0x80
    for function in range(_FUNCTIONS):
        _put32(image, at, _branch(at, _FUNCTIONS_AT + function * _FUNCTION_SIZE))
        at += 4
    for instruction in (0xD2800000, 0xD40000E1, 0xD4200000):  # mov x0,#0; svc #7 ExitProcess; brk
        _put32(image, at, instruction)
        at += 4
    for function in range(_FUNCTIONS):
        at = _FUNCTIONS_AT + function * _FUNCTION_SIZE
        for _ in range(_COUNT):
            for instruction in _BODY:
                _put32(image, at, instruction)
                at += 4
        _put32(image, at, _RET)
    image[0x10:0x14] = b'NRO0'
    _put32(image, 0x18, len(image))
    _put32(image, 0x20, 0)  # .text at 0
    _put32(image, 0x24, len(image))
    _put32(image, 0x28, len(image))  # no .rodata
    _put32(image, 0x2C, 0)
    _put32(image, 0x30, len(image))  # no .data
    _put32(image, 0x34, 0)
    Path(path).write_bytes(image)


def env_on(name):
    value = os.environ.get(name, '')
    return bool(value) and value not in ('0', 'false', 'no')


def skip(test, reason):
    if env_on('SNX_PERF_REQUIRE') or env_on('CI'):
        print(f'perf: {test}: FAIL: this machine cannot be timed: {reason}')
        return 1
    print(f'perf: {test}: skipped: {reason}')
    return 77


def annotation(level, message):
    if os.environ.get('GITHUB_ACTIONS'):
        print(f'::{level} title=perf_compile::{message}')
    else:
        print(f'perf_compile: {level}: {message}')
    sys.stdout.flush()


def baseline(path, key):
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        parts = line.split()
        if not parts or parts[0].startswith('#'):
            continue
        if parts[0] != key:
            continue
        if len(parts) not in (5, 6):
            raise ValueError(f'{path}: {key} has {len(parts)} fields, not 5 or 6')
        reference, warn_percent, fail_percent, ceiling = map(float, parts[1:5])
        return reference, warn_percent, fail_percent, ceiling
    raise KeyError(f'{path}: no {key} line')


def child_cpu():
    if resource is None:
        return time.monotonic()
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def run_recompile(runtime, repo, nro, scratch, number):
    root = Path(scratch) / f'run-{number}'
    data, home, out = root / 'data', root / 'home', root / 'out'
    for folder in (data, home):
        folder.mkdir(parents=True)
    env = os.environ.copy()
    env['HOME'] = str(home)
    env['APPDATA'] = str(home)
    for name in ('XDG_CACHE_HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'SNX_GAME'):
        env.pop(name, None)
    command = [
        str(runtime), '--repo', str(repo), '-r', str(nro), 'CompileBench', '--runtime', str(runtime),
        '--jobs', '1', '--outdir', str(out), '--data', str(data), '--quiet'
    ]
    before = child_cpu()
    result = subprocess.run(command, cwd=str(root), env=env, capture_output=True, text=True, timeout=180)
    after = child_cpu()
    shutil.rmtree(root, ignore_errors=True)
    if result.returncode:
        raise RuntimeError(f'scarletnx -r returned {result.returncode}:\n{result.stdout[-2000:]}{result.stderr[-2000:]}')
    return after - before


def measure_block(runtime, repo, nro, scratch, runs, first_number):
    values = [run_recompile(runtime, repo, nro, scratch, first_number + i) for i in range(runs)]
    values.sort()
    median = values[(len(values) - 1) // 2]
    q1 = values[len(values) // 4]
    q3 = values[(3 * len(values)) // 4]
    spread = (q3 - q1) / median if median > 0 else 0.0
    se = 0.929 * spread / math.sqrt(len(values))
    return median, spread, median - 1.96 * se, median - 3.29 * se, median + 1.96 * se


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--result', type=Path, help='write key median lower upper count here')
    args = parser.parse_args(argv)
    if args.result is None and os.environ.get('SNX_PERF_RESULT'):
        args.result = Path(os.environ['SNX_PERF_RESULT'])
    args.runtime = args.runtime.resolve()
    args.repo = args.repo.resolve()
    if args.result:
        args.result = args.result.resolve()

    machine = platform.machine().lower()
    if sys.platform == 'darwin' and machine in ('arm64', 'aarch64'):
        key = 'compile_aarch64_darwin'
    elif sys.platform == 'win32' and machine in ('x86_64', 'amd64'):
        key = 'compile_x86_64_windows'
    elif sys.platform == 'win32' and machine in ('arm64', 'aarch64'):
        key = 'compile_aarch64_windows'
    elif machine in ('x86_64', 'amd64'):
        key = 'compile_x86_64'
    elif machine in ('arm64', 'aarch64'):
        key = 'compile_aarch64'
    else:
        key = 'compile_other'
    strict = env_on('SNX_PERF_REQUIRE') or env_on('CI')
    if not strict and env_on('SNX_PERF_SKIP'):
        return skip('perf_compile', 'SNX_PERF_SKIP')
    processors = os.cpu_count() or 0
    if processors < 2 and not strict:
        return skip('perf_compile', f'{processors} online processor: a compile timing needs a second one')
    if not strict and hasattr(os, 'getloadavg'):
        load = os.getloadavg()[0]
        if processors and load > processors - 1:
            return skip('perf_compile', f'load average {load:.2f} on {processors} processors leaves no processor idle')
    try:
        reference, warn_percent, fail_percent, ceiling = baseline(args.baseline, key)
    except (OSError, KeyError, ValueError) as error:
        print(f'perf_compile: {error}', file=sys.stderr)
        return 1
    warn = reference * (1.0 + warn_percent / 100.0)
    fail = reference * (1.0 + fail_percent / 100.0)
    if not (reference > 0 and 0 <= warn_percent < fail_percent and warn < fail < ceiling):
        print(f'perf_compile: {args.baseline} has an inconsistent {key} line', file=sys.stderr)
        return 1
    if not args.runtime.is_file() or not args.repo.is_dir():
        print('perf_compile: --runtime and --repo must name the built runtime and the checkout', file=sys.stderr)
        return 1
    if not strict and env_on('SNX_PERF_SKIP'):
        return skip('perf_compile', 'SNX_PERF_SKIP')

    with tempfile.TemporaryDirectory(prefix='snx-perf-compile-') as scratch:
        nro = Path(scratch) / 'CompileBench.nro'
        make_nro(nro)
        # one warm-up, then a two-compile canary; only a massive regression runs the second canary
        run_recompile(args.runtime, args.repo, nro, scratch, 0)
        canary = measure_block(args.runtime, args.repo, nro, scratch, 2, 1)
        massive = ceiling * 1.5
        print(f'perf_compile: canary {canary[0]:.3f} s (massive line {massive:.2f} s)')
        if canary[3] > massive:
            second = measure_block(args.runtime, args.repo, nro, scratch, 2, 3)
            print(f'perf_compile: canary confirmation {second[0]:.3f} s (99.9% lower {second[3]:.3f})')
            if second[3] > massive:
                annotation('error', f'two canary blocks ({canary[0]:.3f} and {second[0]:.3f} s) are over the '
                                     f'{massive:.2f} s massive line; the full block was not run')
                return 1
        first = measure_block(args.runtime, args.repo, nro, scratch, args.runs, 5)
        median, spread, lower, lower_strict, upper = first
        if args.result:
            args.result.parent.mkdir(parents=True, exist_ok=True)
            args.result.write_text(f'{key} {median:.6f} {lower:.6f} {upper:.6f} {args.runs}\n', encoding='utf-8')
        limit = 0.70 if strict else 0.50
        if spread > limit:
            return skip('perf_compile', f'the samples differ by {spread * 100:.0f}% (the limit is {limit * 100:.0f}%),'
                                         ' so the machine is too busy')
        print(f'perf_compile: {median:.3f} s (95% lower {lower:.3f}, 99.9% lower {lower_strict:.3f}, '
              f'reference {reference:.2f}, warn {warn:.2f}, fail {fail:.2f}, ceiling {ceiling:.2f}, '
              f'spread {spread * 100:.0f}%)')
        if lower_strict > fail or median > ceiling:
            second = measure_block(args.runtime, args.repo, nro, scratch, args.runs, 20)
            median2, _, _, strict2, _ = second
            print(f'perf_compile: confirmation {median2:.3f} s (99.9% lower {strict2:.3f})')
            if strict2 > fail:
                annotation('error', f'two independent blocks ({median:.3f} and {median2:.3f} s) are over the '
                                     f'{fail:.2f} s fail line at 99.9 percent confidence')
                return 1
            annotation('warning', f'first block {median:.3f} s crossed the {fail:.2f} s fail line and the '
                                  f'confirmation at {median2:.3f} s did not; likely variance')
            return 0
        if lower > warn:
            second = measure_block(args.runtime, args.repo, nro, scratch, args.runs, 20)
            median2, _, lower2, _, _ = second
            print(f'perf_compile: warning confirmation {median2:.3f} s (95% lower {lower2:.3f})')
            if lower2 > warn:
                annotation('warning', f'{median:.3f} s is over the {warn:.2f} s warning line and a second block '
                                      'confirms it; this is advisory and may still be variance')
            else:
                annotation('warning', f'{median:.3f} s is over the {warn:.2f} s warning line but a second block does '
                                      'not confirm it; likely variance')
        return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'perf_compile: {error}', file=sys.stderr)
        raise SystemExit(1)
