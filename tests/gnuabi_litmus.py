#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Arm memory model on the translated code: the litmus tests of M1 in docs/layers/cpu/plan.md.

tests/gnuabi/litmus.c is built once for each test, freestanding for aarch64-linux-gnu with Clang and a linker, mold or
LLD, and run on the GNU ABI runtime. Each thread repeats its accesses behind barriers of its own, and the fixture prints the values
its reads saw. Every outcome printed must be one the model allows: an outcome in a test's forbidden set is a fence,
an acquire or a release the translator did not keep, or a plain access the back end moved across one.

The forbidden sets are the guest's, so they are the same on every host. What differs is which host can show one: the
same lowered code runs on Linux x86-64, Linux AArch64, Windows x86-64, Windows AArch64 and macOS, and the split
barrier of mp_half in particular is a different instruction or fence on each.

The expectations are the AArch64 application level memory model of the Arm ARM, whose authoritative form is
aarch64.cat in the herd7 tool suite (Jade Alglave and Luc Maranget). tests/litmus/ has the same tests in the herd7
format, so the expectations can be re-derived with herd7 or with Arm's copy at https://developer.arm.com/herd7
instead of taken on trust.

    gnuabi_litmus.py RUNNER [--iterations N] [--runs N] [--cc CC] [--linker L] [--timeout SECONDS]
    gnuabi_litmus.py --build-only      # build the images, and say which linker did it, without running them
    gnuabi_litmus.py --replay FILE     # check the fixture's printed lines, with no compiler and no runner
    gnuabi_litmus.py --self-test       # check this script's own decisions on histograms known by hand

The compiler and the linker are both found by building something, not by looking a name up on PATH. The compiler is
the one the build named (CMake passes its C compiler) and then `clang` from PATH, and it counts only if the object it
emits is an AArch64 one: clang takes the triple with `--target=`, a cross GCC has it in its own name, and a native
compiler that quietly builds for the host is rejected rather than tested. The linker is mold or LLD. mold is tried
first because it is the faster of the two, and LLD, which CI installs, is the fallback. `--linker lld` or
`--linker PATH` says which.

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
            if irregular.group(1) != name:
                problems.append(f'{name}: the run printed a bad count of {irregular.group(1)}')
                continue
            bad += int(irregular.group(2))
            continue
        done = DONE.match(line)
        if done:
            if done.group(1) != name:
                problems.append(f'{name}: the run printed the done line of {done.group(1)}')
                continue
            reported_done = True
            attempts = int(done.group(2))
            continue
        if line.startswith('litmus '):
            problems.append(f'{name}: unrecognized line {line!r}')
    if not reported_done:
        problems.append(f'{name}: the run never printed its "done" line, so the spin loop or a barrier did not end')
    return attempts, bad, problems


def counted(histogram, bad):
    """The attempts the fixture accounted for: one outcome or one bad read each."""
    return sum(histogram.values()) + bad


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


TARGET = 'aarch64-linux-gnu'
AARCH64_EM = 183  # e_machine in an ELF header, at offset 18, little endian


def emits_aarch64(cc, scratch):
    """The flag that makes this compiler emit guest objects, or None when it cannot.

    clang takes the triple with --target=. A cross GCC has it in its own name and refuses the flag. The question is
    asked by compiling, and the object is read back: a native compiler given neither flag builds happily, for the
    host, and must not be mistaken for a cross compiler that works."""
    source = scratch / 'probe-empty.c'
    source.write_text('void snx_target_probe(void) { }\n')
    for flags in (['--target=' + TARGET], []):
        obj = scratch / 'probe-empty.o'
        try:
            built = subprocess.run([cc, *flags, '-c', str(source), '-o', str(obj)], check=False, capture_output=True,
                                   timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if built.returncode or not obj.exists():
            continue
        header = obj.read_bytes()[:20]
        if len(header) == 20 and header[:4] == b'\x7fELF' and header[18] == AARCH64_EM and header[19] == 0:
            return flags
    return None


def probe_linker(linker, cc, cc_flags, scratch):
    """Whether this linker builds the kind of image the fixtures need: a static freestanding AArch64 executable."""
    program = scratch / 'probe.c'
    program.write_text('void _start(void) { for (;;) { } }\n')
    flags = [cc, *cc_flags, f'-fuse-ld={linker}', '-static', '-ffreestanding', '-nostdlib', '-fno-pie',
             '-march=armv8-a', '-Wl,-e,_start', str(program), '-o', str(scratch / 'probe')]
    try:
        return subprocess.run(flags, check=False, capture_output=True, timeout=120).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def choose_toolchain(cc, linkers, scratch):
    """The first (compiler, compiler flags, linker) that builds a probe image, or None.

    The build's compiler is tried before clang on PATH, and the linkers in their order, so a host that has one of
    each still runs the tests, and the one it used is named in the output."""
    seen = []
    for compiler in (cc, shutil.which('clang')):
        if not compiler or compiler in seen:
            continue
        seen.append(compiler)
        flags = emits_aarch64(compiler, scratch)
        if flags is None:
            continue
        for linker in linkers:
            if probe_linker(linker, compiler, flags, scratch):
                return compiler, flags, linker
    return None


def build(name, linker, cc, cc_flags, scratch, iterations):
    """One freestanding AArch64 image of the fixture, with this test selected at compile time."""
    image = scratch / name
    flags = [cc, *cc_flags, f'-fuse-ld={linker}', '-static', '-O2', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-fno-pie', '-nostdlib', '-march=armv8-a', '-mno-outline-atomics',
             '-std=c23', '-pedantic', '-Wall', '-Wextra', '-Werror', f'-DLITMUS_TEST={LITMUS[name]["test"]}',
             f'-DLITMUS_ITERATIONS={iterations[name]}', '-Wl,-e,_start', '-I', str(SOURCE), str(SOURCE / 'start.S'),
             str(SOURCE / 'thread_start.S'), str(SOURCE / 'litmus.c'), '-o', str(image)]
    try:
        result = subprocess.run(flags, check=False, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return None, f'{name}: the build took longer than 300 s'
    if result.returncode:
        return None, f'{name}: build failed\n{result.stderr}'
    return image, None


def replay(path):
    """Check the lines a fixture printed, grouped by test, without building or running anything."""
    groups = collections.defaultdict(list)
    failures = 0
    for number, line in enumerate(path.read_text().splitlines(), 1):
        matched = OUTCOME.match(line) or BAD.match(line) or DONE.match(line)
        if matched:
            groups[matched.group(1)].append(line)
        elif line.strip():
            print(f'{path}:{number}: not a line of the fixture: {line!r}')
            failures += 1
    if not groups:
        print(f'{path}: no lines of the fixture')
        return 1
    for name in sorted(groups):
        if name not in LITMUS:
            print(f'{path}: no test named {name}')
            failures += 1
            continue
        histogram = collections.Counter()
        attempts, bad, problems = read_output(name, '\n'.join(groups[name]), histogram)
        if not problems and counted(histogram, bad) != attempts:
            problems.append(f'{name}: {counted(histogram, bad)} outcomes for {attempts} attempts, so an attempt is '
                            'missing from the record')
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
    for histogram, bad, attempts, expected in (({}, 0, 0, True), ({1: 7}, 0, 8, False), ({1: 7}, 1, 8, True),
                                               ({1: 7, 3: 1}, 0, 8, True)):
        if (counted(histogram, bad) == attempts) is not expected:
            print(f'self-test: counted({histogram}, {bad}) against {attempts} is wrong')
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
    parser.add_argument('--cc', '--clang', dest='cc', default=shutil.which('clang'),
                        help='the C compiler to build the guest images with; clang by default')
    parser.add_argument('--linker', default='auto', help='auto, lld, mold, or the path of a linker')
    parser.add_argument('--iterations', type=int, default=200000,
                        help='attempts per ordered test; the spin test always runs once')
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
    if not args.cc:
        print('no C compiler: skipped')
        return 77
    if not shutil.which(args.cc):
        print(f'{args.cc}: not a program')
        return 1
    if not args.build_only and not pathlib.Path(args.runner or '').exists():
        print(f'{args.runner}: no such runner')
        return 1
    candidates = LINKERS if args.linker == 'auto' else (args.linker,)
    iterations = {name: (1 if name == 'spin' else args.iterations) for name in LITMUS}
    failures = 0
    with tempfile.TemporaryDirectory(prefix='snx-gnuabi-litmus-') as temporary:
        scratch = pathlib.Path(temporary)
        chosen = choose_toolchain(args.cc, candidates, scratch)
        if chosen is None:
            print(f'no compiler and linker built a freestanding {TARGET} image (compilers {args.cc} and clang, '
                  f'linkers {", ".join(candidates)}): skipped')
            return 77
        cc, cc_flags, linker = chosen
        print(f'compiler: {cc} {" ".join(cc_flags)}'.rstrip() + f', linker: {linker}')
        cache = scratch / 'cache'
        for name in LITMUS:
            image, error = build(name, linker, cc, cc_flags, scratch, iterations)
            if error:  # the compiler and the linker were probed above, so a build that fails now is a defect
                print(error)
                return 1
            if args.build_only:
                print(f'{name}: {image.name} built, {image.stat().st_size} bytes')
                continue
            histogram = collections.Counter()
            attempts = bad = 0
            problems = []
            for _ in range(args.runs):
                try:
                    run = subprocess.run([args.runner, '--isolated', f'--jit-cache={cache}', str(image)],
                                         check=False, capture_output=True, text=True, timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    problems.append(f'{name}: no answer in {args.timeout:g} s, so the spin loop or a barrier did not '
                                    'end')
                    break
                if run.returncode:
                    problems.append(f'{name}: exit {run.returncode}\n{run.stdout}{run.stderr}')
                    break
                run_attempts, run_bad, run_problems = read_output(name, run.stdout, histogram)
                attempts += run_attempts
                bad += run_bad
                problems += run_problems
            expected = iterations[name] * args.runs
            if not problems and counted(histogram, bad) != expected:
                problems.append(f'{name}: {counted(histogram, bad)} outcomes for {expected} attempts, so an attempt is '
                                'missing from the record')
            failures += report(name, attempts, bad, histogram, problems)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
