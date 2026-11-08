#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare two settings arms of a recompiled game on one scene, in paired runs.

The two arms are the same game with different runtime settings, as a test build
that selects an implementation path by an ini key does:

    tools/ab_game.py GAME --a vk_memory=0 --b vk_memory=2 \\
        --script '15000:1,25000:1' --out .cache/ab-memory
    tools/ab_game.py GAME --a vk_record=1 --b vk_record=0 \\
        --state SLOT --seconds 60 --warmup 25 --rounds 4 --out .cache/ab-record

Each arm runs once to warm the caches. Then each round runs one arm, then the
other, in the opposite order from the round before. The measurement is the
steady window after --warmup, from the runtime's own five-second rate lines.
A run qualifies only when it finishes, presents frames in the window, leaves a
nonblank screenshot and shows no runtime failure. The first measured screenshot
is the scene reference; another run whose image differs past --image-limit is
rejected, because a different view is a different workload.

The result is the median of the paired speedups (mean milliseconds a frame).
The tool names a winner only when every round is valid, every round agrees on
the direction, every round clears --min-improvement, and the sign test over
four or more rounds is at or below 0.0625. Otherwise it keeps the baseline.
Nothing is applied to the game's settings. Logs and screenshots stay in --out.
This tool reads the Switch runtime's log, so it does not cover the 3DS, Vita or
GBA drivers.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import statistics
import subprocess
import sys

from run_game import data_folder, find_game

STAMP = re.compile(r'^\s*\[\s*([0-9.]+)\]\s*(.*)')
PERF = re.compile(r'perf: [0-9.]+ frames a second over ([0-9.]+) s \((\d+) shown in all\)')
SLOW = re.compile(r'vulkan: a (\d+) ms frame:')
FAIL = re.compile(r'\*\*\* Crash|svcBreak|FATAL|No new frame for|unknown option:|not implemented', re.I)
MIN_WINDOW_S = 10.0
MIN_WINDOW_FRAMES = 30
# These keep the comparison fair. They cannot be part of an arm.
FIXED = {'fps_limit': '0', 'vsync': '0', 'prepare_shaders': '0', 'state_auto_backup': '0',
         'stats': '0', 'showcase': '0', 'vk_gpu_profile': '0', 'vk_buffer_stats': '0', 'log': '1'}
KEY = re.compile(r'[a-z][a-z0-9_]*\Z')


def settings_args(values):
    """Parse repeated KEY=VALUE into a dict, refusing anything the runtime would not take."""
    result = {}
    for value in values:
        key, sep, val = value.partition('=')
        if not sep or not KEY.fullmatch(key) or not val:
            raise ValueError('settings must be KEY=VALUE, with a lower-case key: ' + value)
        result[key] = val
    return result


def check_fixed(settings, fixed=FIXED):
    """Refuse an arm that would change the measurement protocol."""
    for key, value in settings.items():
        if key in fixed and value != fixed[key]:
            raise ValueError(f'{key} is fixed to {fixed[key]} during a comparison')


def measure(text, warmup, pace=1000 / 30, from_state=False):
    """The steady window of one log: frames, mean milliseconds, hitches."""
    lines = [(float(m[1]), m[2]) for line in text.splitlines() if (m := STAMP.match(line))]
    loads = [t for t, line in lines if 'state: loaded ' in line]
    if from_state and not loads:
        raise ValueError('the requested state did not load')
    start = (loads[-1] if from_state else 0.0) + warmup
    previous = None
    elapsed, frames = 0.0, 0
    hitches, measured_start, measured_end = [], None, None
    for t, line in lines:
        if m := PERF.search(line):
            count = int(m[2])
            # The interval that starts in the window, never the boot-to-exit average.
            if previous and previous[0] >= start:
                dt, dn = t - previous[0], count - previous[1]
                if dn < 0:
                    raise ValueError('the frame counter reset during measurement')
                if dt > 0:
                    elapsed += dt
                    frames += dn
                    if measured_start is None:
                        measured_start = previous[0]
                    measured_end = t
            previous = (t, count)
        elif t >= start and (m := SLOW.search(line)):
            hitches.append(int(m[1]))
    if elapsed < MIN_WINDOW_S or frames < MIN_WINDOW_FRAMES:
        raise ValueError(f'insufficient steady gameplay: need {MIN_WINDOW_S:.0f} measured seconds and '
                         f'{MIN_WINDOW_FRAMES} presented frames')
    excess = sum(max(0, ms - pace) for ms in hitches)
    mean_ms = 1000 * elapsed / frames
    return {'measured_s': elapsed, 'frames': frames, 'fps': frames / elapsed, 'mean_ms': mean_ms,
            'slow_frames': len(hitches), 'hitch_ms_per_s': excess / elapsed,
            'window': [measured_start, measured_end]}


def compare(rounds, min_improvement=3.0):
    """The paired result of the complete rounds. B faster means A's frame took longer."""
    pairs = [r for r in rounds if r.get('a') and r.get('b')]
    ratios = [r['a']['mean_ms'] / r['b']['mean_ms'] for r in pairs]
    result = {'rounds': len(rounds), 'valid_rounds': len(pairs), 'ratios': ratios,
              'median_ratio': statistics.median(ratios) if ratios else None,
              'faster': None, 'speedup_percent': None, 'confirmed_rounds': 0,
              'sign_p': None, 'verdict': False, 'reason': ''}
    if not pairs:
        result['reason'] = 'no round has both arms'
        return result
    direction = 'b' if result['median_ratio'] > 1 else 'a'
    speedups = [abs(ratio - 1) * 100 for ratio in ratios]
    agree = sum(1 for ratio in ratios if (ratio > 1) == (result['median_ratio'] > 1))
    result.update({'faster': direction if result['median_ratio'] != 1 else None,
                   'speedup_percent': statistics.median(speedups),
                   'confirmed_rounds': agree,
                   # one-sided sign test: every round points the same way
                   'sign_p': 0.5 ** len(pairs)})
    if len(pairs) < len(rounds):
        result['reason'] = f'{len(rounds) - len(pairs)} round(s) have a failed arm'
    elif agree < len(pairs):
        result['reason'] = f'only {agree} of {len(pairs)} rounds agree on the direction'
    elif min(speedups) < min_improvement:
        result['reason'] = (f'one round moves by {min(speedups):.2f}%, under the '
                            f'{min_improvement:g}% gate')
    elif len(pairs) < 4:
        result['reason'] = f'{len(pairs)} rounds cannot clear the sign test; use --rounds 4 or more'
    else:
        result['verdict'] = True
    return result


def game_paths(program):
    """The game's .ini and .log, from its .folders sidecar when the driver wrote one."""
    sidecar = program.with_suffix('.folders')
    if sidecar.is_file():
        try:
            paths = dict(line.split('=', 1) for line in sidecar.read_text().splitlines() if '=' in line)
            return Path(paths['config']) / (program.stem + '.ini'), Path(paths['state']) / (program.stem + '.log')
        except (KeyError, OSError, ValueError) as error:
            raise ValueError(f'cannot read the game folders in {sidecar}: {error}')
    data = Path(data_folder())
    return (data / 'config' / 'recompiled' / program.stem / (program.stem + '.ini'),
            data / 'state' / 'recompiled' / program.stem / (program.stem + '.log'))


def module_path(program):
    suffix = '_code.dll' if sys.platform == 'win32' else '_code.so'
    return program.with_name(program.stem + suffix)


def stamp(path):
    """What a rebuild or a replacement changes, without reading the file."""
    stat = path.stat()
    return {'device': stat.st_dev, 'inode': stat.st_ino, 'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns}


def digest(path):
    """The content hash, which also catches a rewrite that keeps the size and the time."""
    hasher = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1 << 20), b''):
            hasher.update(chunk)
    return hasher.hexdigest()


def image_error(path, reference):
    """Blank check, and normalized mean RGB difference from the reference, on a small resize."""
    from PIL import Image, ImageChops, ImageStat
    with Image.open(path) as opened:
        image = opened.convert('RGB')
    small = image.resize((96, 54))
    stat = ImageStat.Stat(small)
    if max(stat.stddev) < 3 or sum(stat.mean) / 3 < 3:
        raise ValueError('blank or nearly uniform screenshot')
    if reference is None:
        return 0.0
    with Image.open(reference) as other:
        if other.size != image.size:
            raise ValueError('screenshot resolution changed')
        delta = ImageChops.difference(small, other.convert('RGB').resize(small.size))
    return sum(ImageStat.Stat(delta).mean) / (3 * 255)


def screenshot(ppm, png):
    """Keep a PNG when Pillow is there; otherwise leave the runtime's PPM."""
    try:
        from PIL import Image
    except ImportError:
        return ppm
    with Image.open(ppm) as image:
        image.save(png)
    return png


def run_arm(program, settings, scene, seconds, prefix, timeout, headless):
    """One unattended run, with its console, log and screenshot beside prefix."""
    command = [str(program)] + (['-H'] if headless else []) + [f'--seconds={seconds}', '--shot=' + str(prefix)] + scene
    command += [f'--{key}={value}' for key, value in settings.items() if not key.startswith('_')]
    console = prefix.with_suffix('.out')
    with console.open('w') as output:
        try:
            code = subprocess.run(command, cwd=str(program.parent), stdout=output, stderr=subprocess.STDOUT,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    return code, console, command


def run_snapshot(program, settings, scene, seconds, prefix, timeout, log_path, headless):
    """A run, then what the log and the screenshot say about it."""
    if log_path.exists():
        before = log_path.stat().st_mtime_ns
    else:
        before = None
    code, console, command = run_arm(program, settings, scene, seconds, prefix, timeout, headless)
    row = {'exit': code, 'command': shlex.join(command), 'valid': False, 'reason': '',
           'settings': settings, 'log': str(prefix.with_suffix('.log')),
           'screenshot': str(prefix.with_suffix('.png'))}
    try:
        if code != 0:
            raise ValueError(f'exit {code}')
        text = console.read_text(errors='replace')
        if m := FAIL.search(text):
            raise ValueError('runtime failure: ' + m[0])
        frames = re.search(r'^frames=(\d+) hash=', text, re.M)
        if not frames or int(frames[1]) <= 0:
            raise ValueError('no completed frame summary')
        if not log_path.is_file() or log_path.stat().st_mtime_ns == before:
            raise ValueError('game log is missing or stale')
        shutil.copyfile(log_path, prefix.with_suffix('.log'))
        game_log = Path(row['log']).read_text(errors='replace')
        if m := FAIL.search(game_log):
            raise ValueError('runtime failure: ' + m[0])
        row['metric'] = measure(game_log, settings['_warmup'], from_state=settings['_from_state'])
        ppm = prefix.with_suffix('.ppm')
        if not ppm.is_file():
            raise ValueError('the run wrote no screenshot')
        kept = screenshot(ppm, prefix.with_suffix('.png'))
        row['screenshot'] = str(kept)
        if kept != ppm:
            ppm.unlink()
        row['valid'] = True
    except (ValueError, OSError) as error:
        row['reason'] = str(error)
    # The internal keys are the protocol's, not the runtime's.
    row['settings'] = {k: v for k, v in settings.items() if not k.startswith('_')}
    return row


def plan(args):
    arms = {'a': dict(args.a, **args.common), 'b': dict(args.b, **args.common)}
    scene = [f'--script={args.script}'] if args.script is not None else [f'--state_load=0:{args.state}']
    return {'a': arms['a'], 'b': arms['b'], 'scene': scene,
            'program': {'a': str(args.a_exe), 'b': str(args.b_exe)},
            'seconds': args.seconds, 'warmup': args.warmup, 'rounds': args.rounds,
            'min_improvement': args.min_improvement, 'image_limit': args.image_limit,
            'headless': args.headless, 'order': ['a', 'b']}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], allow_abbrev=False,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', help='its folder in recompiled/, or a path to the folder or program')
    ap.add_argument('--a', action='append', default=[], metavar='KEY=VALUE', help='first arm')
    ap.add_argument('--b', action='append', default=[], metavar='KEY=VALUE', help='second arm')
    scene = ap.add_mutually_exclusive_group(required=True)
    scene.add_argument('--script', help='input script that reaches the scene')
    scene.add_argument('--state', help='saved state slot or absolute .snxstate path')
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--warmup', type=float, default=25)
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--min-improvement', type=float, default=3.0, help='percent every round must clear')
    ap.add_argument('--pace', type=float, default=1000 / 30, help='frame time a hitch is measured over')
    ap.add_argument('--image-limit', type=float, default=0.05,
                    help='normalized RGB difference from the first measured scene; 1 accepts any scene')
    ap.add_argument('--a-exe', type=Path, help='first build of the game program (default: GAME)')
    ap.add_argument('--b-exe', type=Path, help='second build of the game program (default: GAME)')
    ap.add_argument('--headless', action='store_true', help='add -H, for an unattended run with no window')
    ap.add_argument('--out', type=Path, help='new local output directory')
    ap.add_argument('--seed', type=int, default=142)
    ap.add_argument('--dry-run', action='store_true', help='print the plan and the first commands, run nothing')
    a = ap.parse_args(argv)
    if a.seconds < a.warmup + 20 or a.warmup < 0 or a.rounds < 2:
        ap.error('require seconds >= warmup + 20, warmup >= 0 and rounds >= 2')
    if not 0 <= a.min_improvement < 100 or not 0 <= a.image_limit <= 1 or a.pace <= 0:
        ap.error('require min-improvement in [0,100), image-limit in [0,1] and a positive pace')
    try:
        program = Path(find_game(a.game)).resolve()
    except SystemExit as error:
        ap.error(str(error))
    try:
        a.a_exe = (a.a_exe or program).resolve()
        a.b_exe = (a.b_exe or program).resolve()
        a.a, a.b = settings_args(a.a), settings_args(a.b)
        check_fixed(a.a)
        check_fixed(a.b)
        for name, exe in (('a', a.a_exe), ('b', a.b_exe)):
            if not exe.is_file():
                raise ValueError(f'arm {name}: {exe} is not a file')
        ini_a, log_a = game_paths(a.a_exe)
        ini_b, log_b = game_paths(a.b_exe)
    except (ValueError, OSError) as error:
        ap.error(str(error))
    if a.a == a.b and a.a_exe == a.b_exe:
        ap.error('the two arms are identical')
    if a.script is None and not a.state:
        ap.error('state must name a slot or file')
    if a.state and a.state.endswith('.snxstate'):
        a.state = str(Path(a.state).expanduser().resolve())
        if not Path(a.state).is_file():
            ap.error('state file does not exist')
    a.common = dict(FIXED)
    protocol = {'_warmup': a.warmup, '_from_state': a.script is None}
    # The arms carry the protocol to each run without making it a runtime option.
    current = plan(a)
    if a.dry_run:
        print(json.dumps(current | {'program': {'a': str(a.a_exe), 'b': str(a.b_exe)},
                                    'ini': {'a': str(ini_a), 'b': str(ini_b)},
                                    'log': {'a': str(log_a), 'b': str(log_b)}}, indent=2))
        return 0
    if a.out is None:
        ap.error('--out names a new local output directory')
    a.out = a.out.resolve()
    if a.out.exists() and (not a.out.is_dir() or any(a.out.iterdir())):
        ap.error(f'{a.out} is not an empty directory: use a new output directory')
    try:
        a.out.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        ap.error(f'cannot make {a.out}: {error}')
    lock = ini_a.parent / '.ab_game.lock'
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        ap.error(f'another comparison holds {lock}. Remove a stale lock only after its process exits')
    except OSError as error:
        ap.error(f'cannot make {lock}: {error}')
    os.write(fd, str(os.getpid()).encode())
    os.close(fd)
    logs = {'a': log_a, 'b': log_b}
    programs = {'a': a.a_exe, 'b': a.b_exe}
    modules = {'a': module_path(a.a_exe), 'b': module_path(a.b_exe)}
    fingerprint = {}
    try:
        for name in ('a', 'b'):
            fingerprint[name] = {'program': stamp(programs[name]), 'program_sha256': digest(programs[name])}
            if modules[name].is_file():
                fingerprint[name]['module'] = stamp(modules[name])
                fingerprint[name]['module_sha256'] = digest(modules[name])
        reference = None
        runs, rounds = [], []
        rng = random.Random(a.seed)
        timeout = 2 * a.seconds + 180
        print(f"arm A: {' '.join(f'--{k}={v}' for k, v in a.a.items()) or '(the game .ini)'}")
        print(f"arm B: {' '.join(f'--{k}={v}' for k, v in a.b.items()) or '(the game .ini)'}")
        print(f'{str(a.a_exe)}\n{str(a.b_exe)}' if a.a_exe != a.b_exe else str(a.a_exe))

        def do(name, phase, number):
            nonlocal reference
            for other in ('a', 'b'):
                if stamp(programs[other]) != fingerprint[other]['program']:
                    raise RuntimeError(f'the build of arm {other} changed during the comparison')
                has_module = modules[other].is_file()
                if has_module != ('module' in fingerprint[other]) or \
                        (has_module and stamp(modules[other]) != fingerprint[other]['module']):
                    raise RuntimeError(f'the game module of arm {other} changed during the comparison')
            prefix = a.out / (f'{phase}{number:02d}-{name}' if phase == 'round' else f'warm-{name}')
            settings = dict(current[name], **protocol)
            print(f'{phase} {number} {name.upper()}: {shlex.join([programs[name].name] + settings_to_options(settings))}',
                  flush=True)
            row = run_snapshot(programs[name], settings, current['scene'], a.seconds, prefix, timeout,
                               logs[name], a.headless)
            row.update({'arm': name, 'phase': phase, 'round': number})
            if row['valid'] and row['screenshot'].endswith('.png'):
                try:
                    # The warmup only gets the blank check; the reference is a measured scene.
                    row['image_error'] = image_error(Path(row['screenshot']),
                                                     reference if phase == 'round' else None)
                    if phase == 'round' and reference is not None and row['image_error'] > a.image_limit:
                        row['valid'] = False
                        row['reason'] = f"scene differs from the reference by {row['image_error']:.3f}"
                    elif phase == 'round' and reference is None:
                        reference = row['screenshot']
                except (ValueError, OSError) as error:
                    row['valid'] = False
                    row['reason'] = str(error)
            if row['valid']:
                print(f"  {row['metric']['fps']:.2f} FPS, {row['metric']['mean_ms']:.2f} ms, "
                      f"{row['metric']['slow_frames']} slow frames", flush=True)
            else:
                print('  REJECTED: ' + row['reason'], flush=True)
            runs.append(row)
            return row

        # One run of each arm primes the shader, pipeline and JIT caches.
        for name in ('a', 'b'):
            row = do(name, 'warm', 0)
            if not row['valid']:
                raise RuntimeError(f'arm {name} failed its warmup: {row["reason"]}')
        for number in range(1, a.rounds + 1):
            order = ['a', 'b'] if number % 2 else ['b', 'a']
            if number == 1 and rng.random() < 0.5:
                order.reverse()
            results = {name: do(name, 'round', number) for name in order}
            rounds.append({'round': number, 'a': results['a'].get('metric') if results['a']['valid'] else None,
                           'b': results['b'].get('metric') if results['b']['valid'] else None})
            pair = compare([rounds[-1]], a.min_improvement)
            if pair['faster']:
                print(f"  round {number}: {pair['faster'].upper()} faster by "
                      f"{pair['speedup_percent']:.2f}%", flush=True)
            else:
                print(f"  round {number}: no result ({pair['reason']})", flush=True)
        result = compare(rounds, a.min_improvement)
        report = {'game': str(program), 'programs': {k: str(v) for k, v in programs.items()},
                  'fingerprint': fingerprint, 'plan': current, 'common': a.common,
                  'rounds': rounds, 'runs': runs, 'result': result,
                  'quality': 'The image check screens a changed scene. It does not prove correct rendering.'}
        text = json.dumps(report, indent=2)
        temporary = a.out / 'results.tmp'
        temporary.write_text(text)
        temporary.replace(a.out / 'results.json')
        if result['verdict']:
            print(f"\n{result['faster'].upper()} faster by {result['speedup_percent']:.2f}% "
                  f"(median of {result['valid_rounds']} rounds, sign-test p={result['sign_p']:.4f}, "
                  f"gate {a.min_improvement:g}%)")
        else:
            print(f"\nNo winner: {result['reason']}. Keep the baseline.")
        print('Results: ' + str(a.out))
        return 0
    finally:
        lock.unlink(missing_ok=True)


def settings_to_options(settings):
    return [f'--{key}={value}' for key, value in settings.items() if not key.startswith('_')]


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, KeyboardInterrupt, OSError) as error:
        print('Comparison stopped: ' + str(error), file=sys.stderr)
        sys.exit(1)
