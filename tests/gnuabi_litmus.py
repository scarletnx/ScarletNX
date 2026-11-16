#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Arm memory model on the translated code: the litmus tests of M1 in docs/layers/cpu/plan.md.

tests/gnuabi/litmus.c is built once for each test, freestanding for aarch64-linux-gnu with Clang and LLD, and run on
the GNU ABI runtime. Each thread repeats its accesses behind barriers of its own, and the fixture prints the values
its reads saw. Every outcome printed must be one the model allows: an outcome in a test's forbidden set is a fence,
an acquire or a release the translator did not keep, or a plain access LLVM moved across one.

The forbidden sets are the guest's, so they are the same on every host. What differs is which host can show one: the
same lowered code runs on Linux x86-64, Linux AArch64, Windows x86-64, Windows AArch64 and macOS, and the split
barrier of mp_half in particular is a different instruction or fence on each.

The expectations are the AArch64 application level memory model of the Arm ARM, whose authoritative form is
aarch64.cat in the herd7 tool suite (Jade Alglave and Luc Maranget). tests/litmus/ has the same tests in the herd7
format, so the expectations can be re-derived with herd7 or with Arm's copy at https://developer.arm.com/herd7
instead of taken on trust.

    gnuabi_litmus.py RUNNER [--iterations N] [--runs N] [--clang CLANG] [--linker L] [--timeout SECONDS]
    gnuabi_litmus.py --build-only      # build the images, and say which linker did it, without running them
    gnuabi_litmus.py --replay FILE     # check the fixture's printed lines, with no compiler and no runner
    gnuabi_litmus.py --self-test       # check this script's own decisions on histograms known by hand

The linker is mold or LLD, whichever the build machine has: each is tried by linking a probe program, so a name on
PATH that cannot build the image is not taken for a linker that can. mold is tried first because it is the faster of
the two, and LLD, which CI installs, is the fallback. `--linker lld` or `--linker PATH` says which.

Without a Clang that targets AArch64 and a linker that builds the image, the test is skipped (exit 77), as
tests/gnuabi_threads.py does.
"""

import argparse
import collections
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SOURCE = pathlib.Path(__file__).resolve().parent / 'gnuabi'

# Each test: its number in the fixture, how many reads it takes, and the outcomes the model forbids, as a mask of the
# reads that saw a nonzero value (read i is bit i). The reason names the ordering the outcome would need.
LITMUS = {
    'sb': {
        'test': 1,
        'reads': 2,
        'forbidden': {
            0b00: 'both threads loaded before their own store became visible, so the DMB ISH between a store and the '
                  "other thread's load was not kept",
        },
    },
    'mp': {
        'test': 2,
        'reads': 2,
        'forbidden': {
            0b01: 'the flag was read set while the message was still clear, so the DMB ISH between the loads was not '
                  'kept',
        },
    },
    'mp_rel_acq': {
        'test': 3,
        'reads': 2,
        'forbidden': {
            0b01: 'the flag was read set while the message was still clear, so the STLR to LDAR order was not kept',
        },
    },
    'wrc': {
        'test': 4,
        'reads': 3,
        'forbidden': {
            0b011: 'the first thread read the message, the second saw the publication, and still read the message '
                   'clear: the model is multi-copy atomic, so the DMB ISH after each load was not kept',
        },
    },
    'mp_half': {
        'test': 6,
        'reads': 2,
        'forbidden': {
            0b01: 'the flag was read set while the message was still clear, so the writer\'s DMB ISHST or the '
                  'reader\'s DMB ISHLD was not kept',
        },
    },
    'spin': {
        'test': 5,
        'reads': 1,
        'forbidden': {},  # liveness only: the loop must end, which the harness reports as a missing "done" line
    },
}

# Tried in this order: mold first, because it is the faster linker of the two, and LLD second, which is what CI
# installs (`.github/workflows/build.yml`), so a host with only LLD still runs the test. Neither changes what the
# images assert: a linked layout moves the guest's addresses, not its orderings. --linker says which of the two to use.
# GNU gold is not here: binutils 2.44 moved it out of the release tarball and deprecated it, and a distribution's
# ld.gold links only the host's target, so it cannot link an AArch64 image there.
LINKERS = ('mold', 'lld')

OUTCOME = re.compile(r'^litmus (\S+) ([0-9a-f]{8}) (\d+)$')
BAD = re.compile(r'^litmus (\S+) bad (\d+)$')
DONE = re.compile(r'^litmus (\S+) done (\d+)$')


def check(name, histogram):
    """The problems a test's histogram shows: a forbidden outcome, or a read that was not 0 or 1."""
    spec = LITMUS[name]
    problems = []
    for mask in sorted(histogram):
        if mask >= 1 << spec['reads']:
            problems.append(f'{name}: outcome {mask:08x} sets a bit no read of this test has')
        elif mask in spec['forbidden']:
            problems.append(f'{name}: {histogram[mask]} attempts of forbidden outcome {mask:08x}, '
                            f'which needs that {spec["forbidden"][mask]}')
    return problems


def read_output(name, text, histogram):
    """Accumulate one run's lines into a histogram. Returns (attempts, bad reads, problems)."""
    attempts = bad = 0
    reported_done = False
    problems = []
    for line in text.splitlines():
        outcome = OUTCOME.match(line)
        if outcome:
            if outcome.group(1) != name:
                problems.append(f'{name}: the run printed an outcome of {outcome.group(1)}')
                continue
            histogram[int(outcome.group(2), 16)] += int(outcome.group(3))
            continue
        irregular = BAD.match(line)
        if irregular:
            bad += int(irregular.group(2))
            continue
        done = DONE.match(line)
        if done:
            reported_done = True
            attempts = int(done.group(2))
            continue
        if line.startswith('litmus '):
            problems.append(f'{name}: unrecognized line {line!r}')
    if not reported_done:
        problems.append(f'{name}: the run never printed its "done" line, so the spin loop or a barrier did not end')
    return attempts, bad, problems


def report(name, attempts, bad, histogram, problems):
    """One line per test, and the problems under it. Returns 1 when the test failed."""
    if bad:
        problems = problems + [f'{name}: {bad} reads were not 0 or 1']
    problems = problems + check(name, histogram)
    if problems:
        print(f'{name}: FAIL')
        for problem in problems:
            print(f'  {problem}')
        return 1
    outcomes = ', '.join(f'{mask:08x} x{count}' for mask, count in sorted(histogram.items())) or 'none'
    verdict = 'no forbidden outcome' if LITMUS[name]['forbidden'] else 'ended'
    print(f'{name}: {attempts} attempts, {verdict} [{outcomes}]')
    return 0


def probe_linker(linker, clang, scratch):
    """Whether this linker builds the kind of image the fixtures need: a static freestanding AArch64 executable.

    A name on PATH is not enough. A distribution's GNU gold has the host's target alone, and a wrapper or a
    half-installed mold fails on a flag this link uses, so the question is asked by linking something."""
    program = scratch / 'probe.c'
    program.write_text('void _start(void) { for (;;) { } }\n')
    flags = [clang, '--target=aarch64-linux-gnu', f'-fuse-ld={linker}', '-static', '-ffreestanding', '-nostdlib',
             '-fno-pie', '-march=armv8-a', '-Wl,-e,_start', str(program), '-o', str(scratch / 'probe')]
    try:
        return subprocess.run(flags, check=False, capture_output=True).returncode == 0
    except OSError:
        return False


def choose_linker(candidates, clang, scratch):
    """The first of the candidates that links a probe image, or None."""
    for linker in candidates:
        if probe_linker(linker, clang, scratch):
            return linker
    return None


def build(name, linker, clang, scratch, iterations):
    """One freestanding AArch64 image of the fixture, with this test selected at compile time."""
    image = scratch / name
    flags = [clang, '--target=aarch64-linux-gnu', f'-fuse-ld={linker}', '-static', '-O2', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-fno-pie', '-nostdlib', '-march=armv8-a', '-mno-outline-atomics',
             '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', f'-DLITMUS_TEST={LITMUS[name]["test"]}',
             f'-DLITMUS_ITERATIONS={iterations[name]}', '-Wl,-e,_start', '-I', str(SOURCE), str(SOURCE / 'start.S'),
             str(SOURCE / 'thread_start.S'), str(SOURCE / 'litmus.c'), '-o', str(image)]
    result = subprocess.run(flags, check=False, capture_output=True, text=True)
    if result.returncode:
        if 'unable to create target' in result.stderr or 'unknown target' in result.stderr:
            return None, 'clang has no AArch64 target'
        return None, f'{name}: build failed\n{result.stderr}'
    return image, None


def replay(path):
    """Check the lines a fixture printed, grouped by test, without building or running anything."""
    groups = collections.defaultdict(list)
    for number, line in enumerate(path.read_text().splitlines(), 1):
        matched = OUTCOME.match(line) or BAD.match(line) or DONE.match(line)
        if matched:
            groups[matched.group(1)].append(line)
        elif line.strip():
            print(f'{path}:{number}: not a line of the fixture: {line!r}')
            return 1
    if not groups:
        print(f'{path}: no lines of the fixture')
        return 1
    failures = 0
    for name in sorted(groups):
        if name not in LITMUS:
            print(f'{path}: no test named {name}')
            failures += 1
            continue
        histogram = collections.Counter()
        attempts, bad, problems = read_output(name, '\n'.join(groups[name]), histogram)
        failures += report(name, attempts, bad, histogram, problems)
    return 1 if failures else 0


def self_test():
    """This script's decisions, on histograms whose answer is known by hand."""
    cases = [
        # (test, histogram, the number of problems expected)
        ('sb', {0b00: 1}, 1),          # the forbidden outcome
        ('sb', {0b01: 1}, 0),
        ('sb', {0b10: 1}, 0),
        ('sb', {0b11: 1}, 0),          # every read saw a store: allowed
        ('mp', {0b01: 1}, 1),
        ('mp', {0b00: 1}, 0),          # the flag was read clear: allowed
        ('mp', {0b11: 1}, 0),
        ('mp_rel_acq', {0b01: 1}, 1),
        ('mp_rel_acq', {0b10: 1}, 0),
        ('wrc', {0b011: 1}, 1),        # message, publication, then a clear message
        ('wrc', {0b010: 1}, 0),
        ('wrc', {0b111: 1}, 0),
        ('wrc', {0b100: 1}, 0),
        ('mp_half', {0b01: 1}, 1),     # the split barrier lost an edge on one side or the other
        ('mp_half', {0b00: 1}, 0),
        ('spin', {0b1: 1}, 0),
        ('sb', {0b100: 1}, 1),         # a bit no read of this test has
    ]
    failures = 0
    for name, histogram, expected in cases:
        problems = check(name, histogram)
        if len(problems) != expected:
            print(f'self-test: {name} {histogram} gave {len(problems)} problems, expected {expected}: {problems}')
            failures += 1
    histogram = collections.Counter()
    attempts, bad, problems = read_output('sb', 'litmus sb 00000001 7\nlitmus sb done 8\n', histogram)
    if histogram != {1: 7} or attempts != 8 or bad or problems:
        print(f'self-test: parsed {dict(histogram)}, {attempts}, {bad}, {problems}')
        failures += 1
    if not read_output('mp', 'litmus mp 00000001 1\n', collections.Counter())[2]:
        print('self-test: a run without its done line was accepted')
        failures += 1
    attempts, bad, problems = read_output('mp', 'litmus mp bad 3\nlitmus mp done 4\n', collections.Counter())
    if attempts != 4 or bad != 3 or problems:
        print(f'self-test: a bad count came back as {attempts}, {bad}, {problems}')
        failures += 1
    if LINKERS[0] != 'mold' or 'lld' not in LINKERS or 'gold' in LINKERS:
        print(f'self-test: the linker order is {LINKERS}')
        failures += 1
    if failures:
        print(f'self-test: {failures} failures')
        return 1
    print(f'self-test: {len(cases)} histograms and 3 runs, as expected')
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner', nargs='?')
    parser.add_argument('--clang', default=shutil.which('clang'))
    parser.add_argument('--linker', default='auto', help='auto, lld, mold, or the path of a linker')
    parser.add_argument('--iterations', type=int, default=200000)
    parser.add_argument('--runs', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=300.0)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--replay')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.replay:
        return replay(pathlib.Path(args.replay))
    if not args.runner and not args.build_only:
        parser.error('a runner is needed unless --build-only, --replay or --self-test is given')
    if not args.clang:
        print('no clang: skipped')
        return 77
    if not shutil.which(args.clang):
        print(f'{args.clang}: not a program')
        return 1
    if not args.build_only and not pathlib.Path(args.runner or '').exists():
        print(f'{args.runner}: no such runner')
        return 1
    candidates = LINKERS if args.linker == 'auto' else (args.linker,)
    iterations = {name: (1 if name == 'spin' else args.iterations) for name in LITMUS}
    failures = 0
    with tempfile.TemporaryDirectory(prefix='snx-gnuabi-litmus-') as temporary:
        scratch = pathlib.Path(temporary)
        linker = choose_linker(candidates, args.clang, scratch)
        if linker is None:
            print(f'no linker built a freestanding aarch64-linux-gnu image (tried {", ".join(candidates)}): skipped')
            return 77
        print(f'linker: {linker}')
        cache = scratch / 'cache'
        for name in LITMUS:
            image, error = build(name, linker, args.clang, scratch, iterations)
            if error:
                print(error)
                return 77 if 'no AArch64 target' in error else 1
            if args.build_only:
                print(f'{name}: {image.name} built, {image.stat().st_size} bytes')
                continue
            histogram = collections.Counter()
            attempts = bad = 0
            problems = []
            for _ in range(args.runs):
                run = subprocess.run([args.runner, '--isolated', f'--jit-cache={cache}', str(image)],
                                     check=False, capture_output=True, text=True, timeout=args.timeout)
                if run.returncode:
                    problems.append(f'{name}: exit {run.returncode}\n{run.stdout}{run.stderr}')
                    break
                run_attempts, run_bad, run_problems = read_output(name, run.stdout, histogram)
                attempts += run_attempts
                bad += run_bad
                problems += run_problems
            expected = iterations[name] * args.runs
            if not problems and attempts != expected:
                problems.append(f'{name}: {attempts} attempts, expected {expected}')
            failures += report(name, attempts, bad, histogram, problems)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
