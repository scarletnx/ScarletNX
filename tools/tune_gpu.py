#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Search GPU runtime settings, then retest the fastest candidates against baseline.

Examples:
  tools/tune_gpu.py GAME --script '15000:1,25000:1' --out /local/scratch/tune
  tools/tune_gpu.py GAME --state SLOT --warmup 20 --seconds 60 --out /local/scratch/tune

Quality settings stay fixed. Each run must finish, present frames in the measured
window, and produce a nonblank screenshot. Image comparison screens for large
regressions, but final screenshots still need visual review. Nothing is applied
to the game's settings. Logs, screenshots and any save backup stay local.
"""
import argparse
import configparser
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import time

from run_game import find_game

STAMP = re.compile(r'^\[\s*([0-9.]+)\]\s*(.*)')
PERF = re.compile(r'perf: [0-9.]+ frames a second over ([0-9.]+) s \((\d+) shown in all\)')
SLOW = re.compile(r'vulkan: a (\d+) ms frame:')
FAIL = re.compile(r'\*\*\* Crash|svcBreak|FATAL|No new frame for|unknown option:|not implemented', re.I)
# These change execution strategy without removing graphics work or reducing quality.
AXES = {'vk_descriptors': ['0', '1'], 'vk_record': ['1', '0'], 'emulated_cores': ['1', '0'],
        'vk_memory': ['0', '1', '2'], 'pipeline_threads': ['0', '1', '2'],
        'jit_threads': ['0', '1', '4']}
FIXED = {'async_pipelines': '0', 'compute': '1', 'vk_gpu_profile': '0',
         'vk_buffer_stats': '0', 'nvn_verify': '0'}


def settings_args(values):
    result = {}
    for value in values:
        key, sep, val = value.partition('=')
        if not sep or not re.fullmatch(r'[a-z][a-z0-9_]*', key) or not val:
            raise ValueError('settings must be key=value')
        result[key] = val
    for key, val in FIXED.items():
        if key in result and result[key] != val:
            raise ValueError(f'{key} must be {val} for a quality-preserving timing run')
    return result


def game_paths(exe):
    sidecar = exe.with_suffix('.folders')
    paths = dict(line.split('=', 1) for line in sidecar.read_text().splitlines() if '=' in line)
    return Path(paths['config']) / (exe.stem + '.ini'), Path(paths['state']) / (exe.stem + '.log')


def read_settings(path):
    ini = configparser.ConfigParser(interpolation=None, strict=False)
    ini.read(path)
    return {k: v for section in ini.sections() for k, v in ini[section].items()}


def measure(text, warmup, pace, state=False):
    lines = [(float(m[1]), m[2]) for line in text.splitlines() if (m := STAMP.match(line))]
    loads = [t for t, line in lines if 'state: loaded ' in line]
    if state and not loads:
        raise ValueError('the requested state did not load')
    start = (loads[-1] if state else 0) + warmup
    previous = None
    elapsed, frames = 0.0, 0
    hitches, measured_start, measured_end = [], None, None
    for t, line in lines:
        if m := PERF.search(line):
            count = int(m[2])
            # Use cumulative presented frames, never the boot-to-exit average.
            if previous and previous[0] >= start:
                dt, dn = t - previous[0], count - previous[1]
                if dn < 0:
                    raise ValueError('frame counter reset during measurement')
                if dt > 0:
                    elapsed += dt
                    frames += dn
                    if measured_start is None:
                        measured_start = previous[0]
                    measured_end = t
            previous = (t, count)
        if t >= start and (m := SLOW.search(line)):
            hitches.append((t, int(m[1])))
    if elapsed < 10 or frames < 30:
        raise ValueError('insufficient steady gameplay: need 10 measured seconds and 30 presented frames')
    included = [ms for t, ms in hitches if measured_start <= t <= measured_end]
    slow = len(included)
    excess = sum(max(0, ms - pace) for ms in included)
    mean = 1000 * elapsed / frames
    return {'measured_s': elapsed, 'frames': frames, 'fps': frames / elapsed, 'mean_ms': mean,
            'slow_frames': slow, 'hitch_ms_per_s': excess / elapsed,
            'score': mean + 0.05 * excess / elapsed}


def image_check(path, reference=None, limit=0.10):
    from PIL import Image, ImageChops, ImageStat
    with Image.open(path) as opened:
        image = opened.convert('RGB')
    small = image.resize((96, 54))
    stat = ImageStat.Stat(small)
    if max(stat.stddev) < 3 or sum(stat.mean) / 3 < 3:
        raise ValueError('blank or nearly uniform screenshot')
    error = 0.0
    if reference:
        with Image.open(reference) as other:
            if other.size != image.size:
                raise ValueError('screenshot resolution changed')
            delta = ImageChops.difference(small, other.convert('RGB').resize(small.size))
        error = sum(ImageStat.Stat(delta).mean) / (3 * 255)
        if error > limit:
            raise ValueError(f'scene/camera differs from reference by {error:.3f}, limit {limit:.3f}')
    return error


def aggregate(rows):
    if not rows or any(not row['valid'] for row in rows):
        return math.inf
    return statistics.median(row['score'] for row in rows)


def candidate_key(settings):
    return tuple(sorted(settings.items()))


def candidates(seed, active_axes=AXES):
    """Single-axis changes first, then combinations of the current winner's axes."""
    for key, values in active_axes.items():
        for value in values:
            if seed.get(key) != value:
                yield dict(seed, **{key: value})


def active_axes(text, baseline):
    axes = dict(AXES)
    if 'graphics descriptor buffers unavailable' in text:
        axes.pop('vk_descriptors')
    if not re.search(r'vulkan: [1-9]\d* shaders translated|[1-9]\d* pipelines made', text):
        axes.pop('pipeline_threads')
    if re.search(r'jit: 0 batches compiled 0 pcs', text):
        axes.pop('jit_threads')
    memory = list(axes['vk_memory'])
    if baseline['vk_memory'] == '0':
        if 'guest buffers copied (vk_memory=auto)' in text:
            memory.remove('1')
        elif 'guest buffers read in place where they can be (vk_memory=auto)' in text:
            memory.remove('2')
    if 'VK_EXT_external_memory_host no' in text and '2' in memory:
        memory.remove('2')
    axes['vk_memory'] = memory
    return axes


def stop(process):
    if process.poll() is not None:
        return
    try:
        if os.name == 'posix':
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
    except ProcessLookupError:
        process.wait()
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if os.name == 'posix':
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        process.wait()


def run_command(command, cwd, console, timeout):
    with console.open('w') as output:
        process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=os.name == 'posix')
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return 'timeout'
        finally:
            stop(process)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game')
    scene = ap.add_mutually_exclusive_group(required=True)
    scene.add_argument('--script', help='input script that reaches gameplay')
    scene.add_argument('--state', help='saved state slot or absolute .snxstate path')
    ap.add_argument('--out', type=Path, required=True, help='new local output directory')
    ap.add_argument('--seconds', type=int, default=90)
    ap.add_argument('--warmup', type=float, default=55)
    ap.add_argument('--trials', type=int, default=12, help='screening budget, including baseline')
    ap.add_argument('--confirm', type=int, default=2, help='fresh interleaved runs of baseline and each finalist')
    ap.add_argument('--finalists', type=int, default=2)
    ap.add_argument('--pace', type=float, default=1000 / 30)
    ap.add_argument('--base', action='append', default=[], metavar='KEY=VALUE')
    ap.add_argument('--reference', type=Path, help='optional known-good screenshot, otherwise first baseline')
    ap.add_argument('--image-error', type=float, default=0.10, help='maximum normalized RGB image error')
    ap.add_argument('--reset-save', action='store_true', help='restore a local save snapshot before every run and at exit')
    ap.add_argument('--seed', type=int, default=142)
    ap.add_argument('--plan', action='store_true', help='show initial candidates without running the game')
    a = ap.parse_args()
    if not math.isfinite(a.warmup) or a.seconds < a.warmup + 20 or a.warmup < 0 or a.trials < 2 or a.confirm < 1 or a.finalists < 1:
        ap.error('require seconds >= warmup + 20, trials >= 2, confirm/finalists >= 1')
    if a.script is None and not a.state:
        ap.error('state must name a slot or file')
    if not math.isfinite(a.pace) or a.pace <= 0 or not 0 <= a.image_error <= 1:
        ap.error('pace must be positive and image-error must be in [0,1]')
    try:
        from PIL import Image  # noqa: F401
        base = settings_args(a.base)
        exe = Path(find_game(a.game)).resolve()
        config, game_log = game_paths(exe)
    except (ValueError, OSError, KeyError, ImportError) as error:
        ap.error(str(error))
    ini = read_settings(config)
    baseline = {key: ini.get(key, values[0]) for key, values in AXES.items()}
    baseline.update(base)
    common = dict(FIXED, fps_limit='0', vsync='0', prepare_shaders='0', state_auto_backup='0', stats='0', showcase='0')
    # Base values cannot override the measurement protocol.
    for key, value in common.items():
        if key in baseline and baseline[key] != value:
            ap.error(f'{key} is fixed to {value} during measurement')
    if a.plan:
        print(json.dumps({'common': common, 'baseline': baseline,
                          'initial_candidates': list(candidates(baseline)),
                          'quality': {k: ini.get(k) for k in ('resolution', 'shadows', 'docked', 'anisotropy')}}, indent=2))
        return 0
    a.out = a.out.resolve()
    save_path = (exe.parent / (exe.stem + '_data') / 'save').resolve()
    if a.out == save_path or save_path in a.out.parents:
        ap.error('output must be outside the save folder')
    if a.state and a.state.endswith('.snxstate'):
        a.state = str(Path(a.state).expanduser().resolve())
        if not Path(a.state).is_file():
            ap.error('state file does not exist')
    a.out.mkdir(parents=True, exist_ok=True)
    if (a.out / 'results.json').exists():
        ap.error('output already has results: choose a new directory')
    lock = config.parent / '.tune_gpu.lock'
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        ap.error(f'another tuner holds {lock}. Remove a stale lock only after its process exits')
    os.write(fd, str(os.getpid()).encode())
    os.close(fd)
    rows, tested = [], set()
    reference = a.reference.resolve() if a.reference else None
    rng = random.Random(a.seed)
    save = exe.parent / (exe.stem + '_data') / 'save'
    backup = a.out / 'original-save'
    snapshot = False
    signature = (exe.stat().st_ino, exe.stat().st_mtime_ns, exe.stat().st_size)

    def restore():
        if snapshot:
            shutil.rmtree(save, ignore_errors=True)
            shutil.copytree(backup, save)

    def persist():
        tmp = a.out / 'results.tmp'
        tmp.write_text(json.dumps({'baseline': baseline, 'common': common, 'runs': rows}, indent=2))
        tmp.replace(a.out / 'results.json')

    def trial(settings, phase):
        nonlocal reference
        if (exe.stat().st_ino, exe.stat().st_mtime_ns, exe.stat().st_size) != signature:
            raise RuntimeError('runtime changed during search. Stop builds and start a new search')
        restore()
        number = len(rows) + 1
        prefix = a.out / f'run{number:03d}'
        console, log, shot = prefix.with_suffix('.out'), prefix.with_suffix('.log'), prefix.with_suffix('.ppm')
        command = [str(exe), f'--seconds={a.seconds}', '--shot=' + str(prefix)]
        command += ['--state_load=0:' + a.state] if a.state else ['--script=' + a.script]
        command += [f'--{k}={v}' for k, v in dict(settings, **common).items()]
        before_log = game_log.stat().st_mtime_ns if game_log.exists() else None
        print(f'Run {number} ({phase}): {settings}', flush=True)
        code = run_command(command, exe.parent, console, 2 * a.seconds + 90)
        row = {'run': number, 'phase': phase, 'settings': settings, 'exit': code, 'valid': False,
               'log': str(log), 'screenshot': str(prefix.with_suffix('.png')), 'command': command}
        try:
            if code != 0:
                raise ValueError(f'exit {code}')
            output = console.read_text(errors='replace')
            if m := FAIL.search(output):
                raise ValueError('runtime failure: ' + m[0])
            frames = re.search(r'^frames=(\d+) hash=', output, re.M)
            if not frames or int(frames[1]) <= 0:
                raise ValueError('no completed frame summary')
            if not game_log.exists() or game_log.stat().st_mtime_ns == before_log:
                raise ValueError('game log is missing or stale')
            shutil.copyfile(game_log, log)
            text = log.read_text(errors='replace')
            if m := FAIL.search(text):
                raise ValueError('runtime failure: ' + m[0])
            row.update(measure(text, a.warmup, a.pace, bool(a.state)))
            row['image_error'] = image_check(shot, reference, a.image_error)
            with Image.open(shot) as image:
                image.save(row['screenshot'])
            shot.unlink()
            row['valid'] = True
            if reference is None:
                reference = Path(row['screenshot'])
        except (ValueError, OSError) as error:
            row['reason'] = str(error)
        rows.append(row)
        persist()
        print(f"  {row['fps']:.2f} FPS, {row['mean_ms']:.2f} ms, score {row['score']:.2f}" if row['valid']
              else '  REJECTED: ' + row['reason'], flush=True)
        return row

    try:
        # Never overwrite a player's save while a separately launched game is active.
        if sys.platform.startswith('linux'):
            for proc in Path('/proc').glob('[0-9]*/exe'):
                try:
                    if proc.resolve().name.removesuffix(' (deleted)') == exe.name:
                        raise RuntimeError('the game is already running. Close it before tuning')
                except (FileNotFoundError, PermissionError):
                    pass
        if a.reset_save:
            if not save.is_dir():
                raise RuntimeError('--reset-save requires the standard game_data/save folder')
            shutil.copytree(save, backup)
            snapshot = True
        meta = {'executable': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                'arguments': vars(a) | {'out': str(a.out), 'reference': str(a.reference) if a.reference else None},
                'quality_review': 'Automated image checks are a screen. Review finalist screenshots before applying.'}
        (a.out / 'search.json').write_text(json.dumps(meta, indent=2, default=str))
        # Prime lazy shader/pipeline caches, then start baseline measurements again.
        warm = trial(baseline, 'cache warmup')
        if not warm['valid']:
            raise RuntimeError('baseline did not reach valid gameplay: ' + warm.get('reason', 'unknown failure'))
        first = trial(baseline, 'screen')
        if not first['valid']:
            raise RuntimeError('baseline measurement failed')
        tested.add(candidate_key(baseline))
        best = baseline
        best_score = first['score']
        # Worker counts and equivalent memory paths need no duplicate timing trials.
        active = active_axes(Path(first['log']).read_text(errors='replace'), baseline)
        queue = list(candidates(baseline, active))
        while len(tested) < a.trials:
            queue = [c for c in queue if candidate_key(c) not in tested]
            if not queue:
                # Exhaust the interacting execution/memory strategies before stopping.
                keys = list(active)
                queue = [dict(baseline, **dict(zip(keys, v))) for v in itertools.product(*(active[k] for k in keys))
                         if candidate_key(dict(baseline, **dict(zip(keys, v)))) not in tested]
                rng.shuffle(queue)
            if not queue:
                break
            settings = queue.pop(0)
            tested.add(candidate_key(settings))
            row = trial(settings, 'screen')
            if row['valid'] and row['score'] < best_score * 0.97:
                best, best_score = settings, row['score']
                # Try combinations around the new winner, rather than only isolated switches.
                queue = list(candidates(best, active)) + queue
        screen = [r for r in rows if r['phase'] == 'screen' and r['valid'] and r['settings'] != baseline]
        screen.sort(key=lambda r: r['score'])
        finalists = [baseline] + [r['settings'] for r in screen[:a.finalists]]
        for repeat in range(a.confirm):
            order = list(finalists)
            rng.shuffle(order)
            for settings in order:
                trial(settings, 'confirm')
        ranked = []
        for settings in finalists:
            confirm = [r for r in rows if r['phase'] == 'confirm' and r['settings'] == settings]
            value = aggregate(confirm)
            if math.isfinite(value):
                ranked.append((value, settings, confirm))
        ranked.sort(key=lambda x: x[0])
        base_rows = [r for r in rows if r['phase'] == 'confirm' and r['settings'] == baseline]
        base_score = aggregate(base_rows)
        if not math.isfinite(base_score):
            raise RuntimeError('baseline confirmation failed. No winner can be verified')
        # Small changes within run-to-run noise do not displace the baseline.
        noise = statistics.pstdev([r['score'] for r in base_rows]) if len(base_rows) > 1 else 0
        threshold = max(base_score * 0.03, 2 * noise)
        winning = next(x for x in ranked if x[1] == baseline)
        for value, settings, checks in ranked:
            if settings == baseline:
                continue
            differences = [b['score'] - c['score'] for b, c in zip(base_rows, checks)]
            margin = max(threshold, 2 * statistics.pstdev(differences))
            if value + margin < base_score and all(d > 0 for d in differences):
                winning, threshold = (value, settings, checks), margin
                break
        score, settings, checks = winning
        result = {'settings': settings, 'baseline_score': base_score, 'score': score,
                  'minimum_improvement': threshold, 'improved': settings != baseline,
                  'confirmation_runs': [r['run'] for r in checks], 'visual_review_required': True,
                  'screenshots': [r['screenshot'] for r in checks],
                  'command': [str(exe)] + [f'--{k}={v}' for k, v in settings.items()],
                  'quality': 'Resolution, shadows, filtering and effects were not searched. No draw skipping.'}
        (a.out / 'winner.json').write_text(json.dumps(result, indent=2))
        print('\nFastest confirmed candidate passing automated checks:' if result['improved'] else
              '\nNo improvement cleared validation and the noise margin. Keep baseline.', flush=True)
        print(shlex.join(result['command']))
        print('Review screenshots in winner.json. Settings were not applied. Results: ' + str(a.out), flush=True)
        return 0
    finally:
        restore()
        lock.unlink(missing_ok=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, KeyboardInterrupt) as error:
        print('Search stopped: ' + str(error), file=sys.stderr)
        sys.exit(1)
