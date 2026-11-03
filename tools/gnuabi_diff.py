# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare standalone AArch64 Linux ELFs with ScarletNX and external QEMU.

Use --suite for deterministic freestanding CPU/syscall fixtures, --fuzz COUNT
for seeded unsigned-integer programs, or positional ELFs for existing programs.
Only stdout and exit status are compared by default; stderr is preserved because
the two runtimes can emit different diagnostics. No QEMU code enters ScarletNX.
"""

import argparse
import hashlib
import json
import os
import random
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def command(value):
    # An existing filename can contain spaces, particularly on Windows. Absolute, not resolved: scarletnx-gnuabi may
    # be a link to scarletnx, which runs the driver its name says
    if Path(value).is_file():
        return [str(Path(value).absolute())]
    result = shlex.split(value)
    if not result:
        raise ValueError('empty command')
    if Path(result[0]).is_file():
        result[0] = str(Path(result[0]).absolute())
    return result


def execute(argv, cwd, timeout, stdin=b''):
    start = time.monotonic()
    env = os.environ.copy()
    env.update(LC_ALL='C', TZ='UTC')
    child = subprocess.Popen(
        argv,
        cwd=cwd,
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=os.name != 'nt',
    )
    timed_out = False
    try:
        stdout, stderr = child.communicate(stdin, timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], capture_output=True, check=False)
            child.kill()
        else:
            os.killpg(child.pid, signal.SIGKILL)
        stdout, stderr = child.communicate()
    return {
        'argv': argv,
        'returncode': None if timed_out else child.returncode,
        'timeout': timed_out,
        'seconds': time.monotonic() - start,
        'stdout': stdout,
        'stderr': stderr,
    }


def mismatches(left, right, compare_stderr):
    failures = []
    if left['timeout'] or right['timeout']:
        failures.append('timeout')
    if any(result['returncode'] is not None and result['returncode'] < 0 for result in (left, right)):
        failures.append('host signal/crash')
    if left['returncode'] != right['returncode']:
        failures.append('exit status')
    if left['stdout'] != right['stdout']:
        failures.append('stdout')
    if compare_stderr and left['stderr'] != right['stderr']:
        failures.append('stderr')
    # QEMU may report a guest signal as 128+signal rather than a host signal.
    if b'uncaught target signal' in right['stderr']:
        failures.append('guest signal/crash')
    return failures


def fuzz_source(seed):
    rng = random.Random(seed)
    statements = []
    for index in range(160):
        constant = rng.getrandbits(64)
        shift = rng.randrange(1, 64)
        choice = rng.randrange(8)
        statement = (
            f'x += 0x{constant:016x}UL;',
            f'x ^= 0x{constant:016x}UL;',
            f'x *= 0x{constant | 1:016x}UL;',
            f'x = (x << {shift}) | (x >> {64 - shift});',
            f'x ^= (u64)((s64)x >> {shift});',
            f'if (x & (1UL << {shift})) x += 0x{constant:016x}UL; else x ^= x >> {shift};',
            f'memory[{index % 31}] = x; x ^= memory[{rng.randrange(31)}];',
            f'x ^= (u64)((__uint128_t)x * 0x{constant:016x}UL >> 64);',
        )[choice]
        statements.append('    ' + statement)
    return (
        '/* SPDX-License-Identifier: GPL-3.0-or-later */\n'
        '/* Copyright 2026 the ScarletNX contributors */\n'
        f'/* Reproduction seed: {seed} */\n#include "guest.h"\n'
        f'static volatile u64 initial = 0x{rng.getrandbits(64):016x}UL;\n'
        'int guest_main(u64 *stack) {\n    (void)stack;\n'
        '    u64 x = initial;\n'
        '    volatile u64 memory[31] = {0};\n' + '\n'.join(statements) + '\n    emit_hex(x);\n    return 0;\n}\n'
    )


def compile_fixture(cc, extra, fixture, sources, include, cwd, timeout, pie=False):
    mode = ['-fPIE', '-Wl,-pie,--no-dynamic-linker'] if pie else ['-fno-pie', '-no-pie']
    argv = [
        *cc,
        '-static',
        '-O2',
        '-ffreestanding',
        '-fno-builtin',
        '-fno-stack-protector',
        *mode,
        '-nostdlib',
        '-march=armv8-a',
        '-Wl,-e,_start',
        '-I',
        str(include),
        *extra,
        *map(str, sources),
        '-o',
        str(fixture),
    ]
    result = execute(argv, cwd, timeout)
    if result['timeout'] or result['returncode']:
        sys.stderr.buffer.write(result['stderr'])
        raise RuntimeError(f'fixture compiler failed: {shlex.join(argv)}')
    return argv


def save_case(artifact_dir, name, elf, source_files, seed, compile_command, results, reasons, stdin):
    artifact_dir.mkdir(parents=True, exist_ok=True)
    case = artifact_dir / f'{name}-{time.time_ns()}'
    case.mkdir()
    shutil.copy2(elf, case / 'program.elf')
    for source in source_files:
        shutil.copy2(source, case / source.name)
    (case / 'stdin.bin').write_bytes(stdin)
    metadata = {
        'name': name,
        'seed': seed,
        'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'compile_command': compile_command,
        'reasons': reasons,
        'results': {},
    }
    for runtime, result in results.items():
        for stream in ('stdout', 'stderr'):
            (case / f'{runtime}.{stream}.bin').write_bytes(result[stream])
        metadata['results'][runtime] = {key: value for key, value in result.items() if key not in ('stdout', 'stderr')}
        metadata['results'][runtime]['replay_argv'] = [
            str(case / 'program.elf') if item == str(elf) else item for item in result['argv']
        ]
    (case / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    return case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', nargs='*', type=Path)
    parser.add_argument('--scarletnx', required=True, help='GNU ABI executable or quoted command with arguments')
    parser.add_argument('--qemu', default='qemu-aarch64', help='External QEMU executable or command')
    parser.add_argument('--cc', default='aarch64-linux-gnu-gcc', help='Cross-compiler executable or command')
    parser.add_argument('--cc-arg', action='append', default=[], help='Extra compiler flag; use --cc-arg=--target=...')
    parser.add_argument(
        '--suite', choices=('cpu', 'syscalls', 'pie', 'threads', 'all'), help='Compile and run repository fixtures'
    )
    parser.add_argument('--fuzz', type=int, default=0, metavar='COUNT')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--compile-timeout', type=float, default=60)
    parser.add_argument('--artifact-dir', type=Path, default=Path('build/gnuabi-failures'))
    parser.add_argument('--compare-stderr', action='store_true')
    parser.add_argument('--keep-success', action='store_true')
    parser.add_argument('--stdin', type=Path)
    parser.add_argument('--guest-arg', action='append', default=[])
    args = parser.parse_args()
    if args.fuzz < 0 or args.timeout <= 0 or args.compile_timeout <= 0:
        parser.error('counts must be nonnegative and timeouts must be positive')
    if not args.elf and not args.suite and not args.fuzz:
        parser.error('provide an ELF, --suite, or --fuzz')
    repo = Path(__file__).resolve().parents[1]
    fixtures = repo / 'tests/gnuabi'
    artifact_dir = args.artifact_dir.resolve()
    stdin = args.stdin.read_bytes() if args.stdin else b''
    try:
        scarletnx, qemu, cc = command(args.scarletnx), command(args.qemu), command(args.cc)
        with tempfile.TemporaryDirectory(prefix='scarletnx-diff-') as temporary:
            scratch = Path(temporary)
            cases = [(path.stem, path.resolve(), [], None, None, None) for path in args.elf]
            suites = ('cpu', 'syscalls', 'pie', 'threads') if args.suite == 'all' else ((args.suite,) if args.suite else ())
            # threads: the thread and signal fixtures, each a case of its own (gnuabi_threads.py runs them in CTest)
            if 'threads' in suites:
                suites = tuple(x for x in suites if x != 'threads') + ('threads', 'signals', 'restart', 'sigsuspend')
            for suite in suites:
                sources = [fixtures / 'start.S', fixtures / f'{suite}.c']
                if suite in ('threads', 'signals', 'restart', 'sigsuspend'):
                    sources.insert(1, fixtures / 'thread_start.S')
                if suite == 'cpu':
                    sources.append(fixtures / 'boundary.S')
                elf = scratch / f'{suite}.elf'
                compile_command = compile_fixture(
                    cc, args.cc_arg, elf, sources, fixtures, scratch, args.compile_timeout, suite == 'pie'
                )
                cases.append((suite, elf, [*sources, fixtures / 'guest.h'], None, compile_command, 0))
            for offset in range(args.fuzz):
                seed = args.seed + offset
                source = scratch / f'fuzz-{seed}.c'
                source.write_text(fuzz_source(seed), encoding='ascii')
                elf = scratch / f'fuzz-{seed}.elf'
                sources = [fixtures / 'start.S', source]
                compile_command = compile_fixture(
                    cc, args.cc_arg, elf, sources, fixtures, scratch, args.compile_timeout
                )
                cases.append((f'fuzz-{seed}', elf, [*sources, fixtures / 'guest.h'], seed, compile_command, 0))
            failures = 0
            for name, elf, sources, seed, compile_command, expected_exit in cases:
                results = {}
                for runtime, argv in (('scarletnx', scarletnx), ('qemu', qemu)):
                    cwd = scratch / f'{name}-{runtime}'
                    cwd.mkdir(exist_ok=True)
                    results[runtime] = execute([*argv, str(elf), *args.guest_arg], cwd, args.timeout, stdin)
                reasons = mismatches(results['scarletnx'], results['qemu'], args.compare_stderr)
                if expected_exit is not None and any(
                    result['returncode'] != expected_exit for result in results.values()
                ):
                    reasons.append(f'fixture expected exit {expected_exit}')
                if reasons or args.keep_success:
                    saved = save_case(artifact_dir, name, elf, sources, seed, compile_command, results, reasons, stdin)
                    print(f'{"FAIL" if reasons else "PASS"} {name}: {", ".join(reasons) or "identical"}; saved {saved}')
                else:
                    print(f'PASS {name}')
                failures += bool(reasons)
            print(f'{len(cases) - failures}/{len(cases)} cases matched QEMU.')
            return bool(failures)
    except (OSError, RuntimeError, ValueError) as error:
        print(f'gnuabi differential test: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
