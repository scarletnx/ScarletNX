# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The frozen references of arm_x64.c (test-references' arm/): LLVM -O0's results of the blocks of the profiles and
memories the Unicorn tests have no random blocks of, a hash a block over its states, kept for when LLVM is gone.

  python3 tests/arm_frozen.py record PROGRAM --out DIR [--jobs N] [--only NAME...]
  python3 tests/arm_frozen.py verify PROGRAM --out DIR [--jobs N] [--only NAME...]
  python3 tests/arm_frozen.py selftest PROGRAM --out DIR

DIR is the arm folder of a test-references clone (scarletnx/test-references): the files recorded there go up by a
commit of that repository and its new pin in cpmfile.json.

PROGRAM is snx-test-arm-x64, built with LLVM (SNX_TEST_LLVM) to record. record writes each file with --record, the
back end's results held to LLVM's as it goes; verify runs each with --frozen, where LLVM's results are held to the
file's as well as the back end's (the back end's alone without LLVM); selftest runs --frozen on copies of two files
made wrong, each refused as it must be (ctest's arm_x64_frozen_refusals). Each says what it covered: the file, its
profile, memory and trace, seed, A32 and Thumb blocks, states, the runs, the blocks whose results differ, the file's
bytes. A change to how arm_x64.c draws its blocks bumps its FROZEN_GENERATOR, and the files are recorded again where
a reference of those results is built."""

import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

STATES = 16
# the blocks of each mode's file, the first of them on the machines under Unicorn (arm_native_modes.cmake)
A32, THUMB = 2000, 1500


def mode_args(mode):
    """a mode of arm_native_modes.cmake, as arm_x64.c's options: its profile, then its memory or the trace"""
    profile, _, rest = mode.partition('_')
    args = ['--profile', profile]
    if rest in ('hosted', 'checked'):
        args += ['--memory', rest]
    elif rest == 'trace':
        args += ['--trace']
    elif rest:
        raise ValueError(f'{mode}: no mode of arm_native_modes.cmake')
    return args + ['--blocks', str(A32), '--thumb', str(THUMB), '--states', str(STATES)]


MODES = (
    'a9 arm11 arm946e arm7tdmi arm7tdmi_trace a57_hosted a9_hosted arm11_hosted a57_checked a9_checked arm11_checked'
).split()
CONFIGS = {mode: mode_args(mode) for mode in MODES}
# what random words hardly are (arm_native_modes.cmake says why each is there)
for halves in ('f841,1d04', 'f801,1d04', 'f821,1d04', 'f841,1b04'):
    CONFIGS['a9_own_base_' + halves.replace(',', '_')] = f'--profile a9 --states 64 --halves {halves}'.split()
CONFIGS['arm946e_ldm_base'] = (
    '--profile arm946e --states 64 --word e8b10002 --word e9b10002 --word e8310002 --word e9310002 --word e8b10006 '
    '--word e8b10003'
).split()
CONFIGS['arm7tdmi_list_save'] = (
    '--profile arm7tdmi --states 256 --word e3a0140e,e2811002,e891000c --word e3a0140e,e2811002,e881000c'
).split()
CONFIGS['arm7tdmi_trace_long'] = (
    '--profile arm7tdmi --trace --states 64 --word ' + '00000291,10000291,' * 23 + '00000291'
).split()
for halfword in ('b400', 'bc00'):
    CONFIGS['a57_checked_empty_' + halfword] = f'--memory checked --states 64 --halves {halfword}'.split()
# the Windows hosts' blocks unwound at every instruction (tests/CMakeLists.txt, --step), few of them
CONFIGS['step_bus'] = '--profile arm7tdmi --trace --blocks 300 --thumb 200 --states 2 --seed 5'.split()
CONFIGS['step_checked'] = '--memory checked --blocks 300 --thumb 200 --states 2 --seed 6'.split()
CONFIGS['step_hosted'] = '--memory hosted --blocks 300 --thumb 200 --states 2 --seed 4'.split()
# the random blocks and words of the tests that compared with LLVM as they ran (tests/CMakeLists.txt, arm_call_abi.cmake):
# the generator's options of each, which those of the same share
CONFIGS['seed2_2000'] = '--blocks 2000 --seed 2'.split()
CONFIGS['seed3_2000'] = '--blocks 2000 --seed 3'.split()
CONFIGS['win64_step'] = '--blocks 600 --thumb 600 --states 2 --seed 4'.split()
CONFIGS['arm64win'] = '--blocks 1500 --thumb 1000 --states 8'.split()
CONFIGS['arm64win_spill'] = '--blocks 1500 --thumb 1000 --states 8 --seed 2'.split()
CONFIGS['arm64win_step'] = '--blocks 300 --thumb 200 --states 2 --seed 3'.split()
CONFIGS['half_words'] = (
    '--word eef21ae3 --word eeb21a44 --word eeb25be4 --word eeb26b6f --word eef35ac2 --word eeb36a62 --word eef36bc3 --word eeb37bc3 --word eef37b42'
).split()
CONFIGS['odd_words'] = '--word eef82bef --word eef82b6f'.split()
CONFIGS['call_abi'] = '--blocks 600 --states 8'.split()
for profile in ('arm946e', 'arm7tdmi'):
    CONFIGS['call_abi_' + profile] = f'--profile {profile} --blocks 600 --states 8'.split()

RECORDED = re.compile(
    r'^recorded (\S+): (\d+) A32 and (\d+) Thumb blocks, (.*), LLVM\'s results; the .* back end\'s '
    r'are not LLVM\'s on (\d+) of them$',
    re.M,
)
FROZEN = re.compile(
    r'^frozen (\S+): (\d+) of its (\d+) A32 blocks and (\d+) of its (\d+) Thumb ones checked, (\d+) '
    r'states each; blocks whose results are not the file\'s:(?: LLVM\'s (\d+),)? the .* back end\'s '
    r'(\d+)$',
    re.M,
)
RUNS = re.compile(r'compared from \d+ states each: (\d+) runs')
ENV = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY')}  # no window, ever


def field(args, name, default):
    return args[args.index(name) + 1] if name in args else default


def run(program, out, name, verb):
    path = out / f'{name}.txt'
    command = (
        [str(program), '--record', str(path)] + CONFIGS[name]
        if verb == 'record'
        else [str(program), '--frozen', str(path)]
    )
    start = time.monotonic()
    done = subprocess.run(command, capture_output=True, text=True, timeout=3600, check=False, env=ENV)
    seconds = time.monotonic() - start
    text = done.stdout + done.stderr
    found = (RECORDED if verb == 'record' else FROZEN).search(done.stdout)
    runs = RUNS.search(done.stdout)
    return name, done.returncode, seconds, text, found, int(runs.group(1)) if runs else 0, path


def selftest(program, out):
    """--frozen on copies of two files, each made wrong in one way: what each run must exit with and say"""
    one, step = out / 'a57_checked_empty_b400.txt', out / 'step_checked.txt'
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:

        def case(what, source, edit, extra, status, *says):
            nonlocal failures
            lines = source.read_text().splitlines()
            edit(lines)
            path = pathlib.Path(tmp) / source.name
            path.write_text('\n'.join(lines) + '\n')
            done = subprocess.run(
                [str(program), '--frozen', str(path)] + extra,
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
                env=ENV,
            )
            text = done.stdout + done.stderr
            missing = [
                s.format(path=path, line=len(lines)) for s in says if s.format(path=path, line=len(lines)) not in text
            ]
            if done.returncode != status or missing:
                failures += 1
                print(f'FAIL: {what}: exit {done.returncode} (wanted {status}), missing {missing}\n{text[-2000:]}')
            else:
                print(f'{what}: exit {status}, as it says')

        def flip(lines):
            lines[-1] = f'{int(lines[-1], 16) ^ 1:016x}'

        def header(key, value):
            def edit(lines):
                at = next(k for k, line in enumerate(lines) if line.startswith(f'# {key} '))
                lines[at] = f'# {key} {value}'

            return edit

        def keep(lines):
            pass

        case('the file as it is', one, keep, [], 0, 'the blocks and states it was recorded from')
        case('a hash with a bit flipped', one, flip, [], 1, '{path}:{line}, Thumb block 0 [b400]: ', "back end's 1\n")
        case('another generator', one, header('generator', '999'), [], 1, "generator 999, the test's is 1")
        case(
            "another inputs' digest", one, header('inputs', '0123456789abcdef'), [], 1, "the file's to 0123456789abcdef"
        )
        case(
            'an option of no drawing in its args',
            one,
            header('args', '--memory checked --pool 3'),
            [],
            1,
            "--pool is not an option of the blocks' drawing",
        )
        case('items its args do not draw', one, header('items', '1 0'), [], 1, 'items 1 0, where its args draw 0 A32')
        case('a seed beside it', one, keep, ['--seed', '2'], 2, '--seed beside --frozen')
        case('more blocks than it holds', step, keep, ['--blocks', '301'], 2, 'which holds 300 A32 blocks')
        case(
            'a run of its first blocks',
            step,
            keep,
            ['--blocks', '5', '--thumb', '4'],
            0,
            '5 of its 300 A32 blocks and 4 of its 200 Thumb ones checked',
            "its inputs' digest is checked by a run of all its blocks",
        )
        case('a hash past the blocks run', step, flip, ['--blocks', '5', '--thumb', '4'], 0, " back end's 0\n")
    print(f'{failures} failures')
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('verb', choices=('record', 'verify', 'selftest'))
    parser.add_argument('program', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path, required=True, help='the arm folder of a test-references clone')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    parser.add_argument('--only', nargs='+', default=[], metavar='NAME')
    args = parser.parse_args()
    unknown = [n for n in args.only if n not in CONFIGS]
    if unknown:
        sys.exit(f'--only {" ".join(unknown)}: no such file; the files are {", ".join(CONFIGS)}')
    if not args.program.is_file():
        sys.exit(f'{args.program}: no such program')
    if args.verb == 'selftest':
        return selftest(args.program, args.out)
    if args.verb == 'record' and not (args.out / os.pardir / '.git').exists():
        sys.exit(f"--out {args.out}: no .git beside it, so not a test-references clone (CPM's copy is every tree's)")
    names = args.only or list(CONFIGS)
    bad = 0
    total = {'blocks': 0, 'runs': 0, 'bytes': 0, 'seconds': 0.0}
    print(f'{args.verb} {len(names)} files of {args.program} in {args.out}, {args.jobs} at a time')
    print(
        f'{"file":26} {"profile":9} {"memory":8} {"trace":5} {"seed":6} {"A32":>5} {"Thumb":>5} {"states":>6} '
        f'{"runs":>7} {"differ":>6} {"bytes":>7} {"s":>6}'
    )
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for name, rc, seconds, text, found, runs, path in pool.map(
            lambda n: run(args.program, args.out, n, args.verb), names
        ):
            config = CONFIGS[name]
            if args.verb == 'record':
                a32, thumb, differ = (
                    (int(found.group(2)), int(found.group(3)), found.group(5)) if found else (0, 0, '?')
                )
            else:
                a32, thumb = (int(found.group(2)), int(found.group(4))) if found else (0, 0)
                differ = f'{found.group(7) or "-"}/{found.group(8)}' if found else '?'
            size = path.stat().st_size if path.exists() else 0
            print(
                f'{name:26} {field(config, "--profile", "a57"):9} {field(config, "--memory", "flat"):8} '
                f'{"yes" if "--trace" in config else "no":5} {field(config, "--seed", "0xa32"):6} {a32:5} {thumb:5} '
                f'{field(config, "--states", "16"):>6} {runs:7} {differ:>6} {size:7} {seconds:6.1f}',
                flush=True,
            )
            total['blocks'] += a32 + thumb
            total['runs'] += runs
            total['bytes'] += size
            total['seconds'] += seconds
            if rc or not found:
                bad += 1
                tail = '\n'.join(line for line in text.splitlines() if line.startswith('FAIL'))[-3000:] or text[-1500:]
                print(f'  {name}: exit {rc}{", no summary line" if not found else ""}\n{tail}', flush=True)
    print(
        f'{len(names)} files, {total["blocks"]} blocks, {total["runs"]} runs, {total["bytes"]} bytes, '
        f'{total["seconds"]:.0f} s of runs; {bad} failed'
        + ('' if args.verb == 'record' else " (differ: LLVM's blocks/the back end's whose results are not the file's)")
    )
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
