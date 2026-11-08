#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
r"""Paired real-game runtime A/B benchmark, with isolated saves and unchanged graphics settings.

Example:
  python3 tools/bench_nvn_game.py --game /games/0100.../0100..._code.so \
      --baseline /build-before/scarletnx --candidate /build-after/scarletnx \
      --frames 600 --script scene.snxtas --rounds 3 --timeout 300 --out /tmp/game-ab

--script is a frame-based .snxtas file: frame:hex_buttons:lx,ly,rx,ry, including explicit button releases.
The final neutral event is inserted at frames-1, keeping replay active through the measured workload. The native
--script option releases buttons in milliseconds, so it is deliberately not used. Paths to update modules work:
the game directory's original basename and its base/update data are retained in every isolated copy.

The metric includes process startup, shader/pipeline preparation, game execution and final screenshot/exit. Each
run starts with an independent copy of the same saves/data/settings and empty runtime/driver caches. Files use
copy-on-write clones when supported; otherwise copying a large extracted game requires substantial disk space.
OS filesystem caches, CPU/GPU clocks and game nondeterminism are uncontrolled. Fixed-frame final screenshots are
retained for inspection; equal dimensions or even equal final pixels do not establish complete rendering correctness.
An overshot frame limit, uniform screenshot, timeout or crash rejects the run. No quality setting is overridden.
--uncapped applies --fps_limit=0 --vsync=0 equally to both builds; otherwise existing pacing settings are retained.
External source= dump paths stay read-only shared assets: do not modify them while the benchmark is running.
Relative source= paths and configured external shader/RenderDoc output paths or Tracy capture tools are rejected.
Key files are copied privately when found; result reports do not include their contents or hashes.
When every run emits unattended_ns, a separate game-interval summary excludes pre-game preparation. It still
includes guest initialization/loading and final screenshot; it is not GPU frame time or NVN-only time.
Alternatively, --seconds N measures presented-frame throughput over a whole-number timed interval with neutral
input. It requires unattended_ns, accepts variable frame counts, and compares candidate FPS to baseline FPS.
These timed runs can reach different scenes; process wall times are retained but are not compared as equal work.
Timed mode currently rejects --script and --native. Fixed --frames mode continues to reject an overshot frame limit.

Optional --native JSON must contain workload, scene, game_version, hardware, frames, script_sha256, metric
(end_to_end_fixed_workload_wall_time) and at least three elapsed_ns_samples. --workload and --scene must match it;
the runtime's game version must also match. This
is an explicitly user-supplied end-to-end comparison, never a measurement of native NVN or NVN translation alone.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

MARKER = re.compile(r'^frames=(\d+) hash=([0-9a-fA-F]{16})(?: (\d+)x(\d+))?$', re.MULTILINE)
INTERVAL = re.compile(r'^unattended_ns=(\d+)$', re.MULTILINE)
VERSION = re.compile(r'^(?:\[\s*\d+(?:\.\d+)?\]\s*)?game: [0-9a-fA-F]{16}, version ([^\r\n]+)$', re.MULTILINE)
MODULE_SUFFIXES = ('_code.so', '_code.dll', '_code.snxobj')
TAS_LINE = re.compile(r'(\d+):([0-9a-fA-F]{1,16}):(-?\d+),(-?\d+),(-?\d+),(-?\d+)')
EXTERNAL_OUTPUT_SETTINGS = frozenset(('shader_corpus', 'renderdoc', 'renderdoc_frames', 'vita_shader_corpus',
                                      'tracy_capture_tool'))


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def module_name(path):
    for suffix in MODULE_SUFFIXES:
        if path.name.endswith(suffix):
            return path.name[:-len(suffix)]
    raise ValueError('--game must name a compiled game directory or *_code.so/.dll/.snxobj module')


def find_game(path):
    path = path.resolve(strict=True)
    if path.is_dir():
        matches = [path / (path.name + suffix) for suffix in MODULE_SUFFIXES]
        matches = [p for p in matches if p.is_file()]
        if len(matches) != 1:
            raise ValueError('game directory must contain exactly one module named <directory>_code.so/.dll/.snxobj; '
                             'select an update or another program by its module path')
        path = matches[0]
    module_name(path)
    if not path.is_file():
        raise ValueError('game module is not a file')
    return path


def validate_script(path, frames):
    events = []
    if path:
        for line_number, text in enumerate(path.read_text().splitlines(), 1):
            text = text.strip()
            if not text or text.startswith('#'):
                continue
            match = TAS_LINE.fullmatch(text)
            if not match:
                raise ValueError(f'script line {line_number} is not frame:buttons:lx,ly,rx,ry')
            frame, buttons, *axes = match.groups()
            frame, axes = int(frame), [int(a) for a in axes]
            if frame >= frames or (events and frame <= events[-1][0]) or any(abs(a) > 32767 for a in axes):
                raise ValueError(f'script line {line_number} has an invalid frame order, frame limit or stick value')
            events.append((frame, int(buttons, 16), *axes))
    # Explicit final neutral state: the runtime releases replay after its last event's frame.
    if events and events[-1][0] == frames - 1:
        if any(events[-1][1:]):
            raise ValueError('the last workload frame must be neutral to keep replay active through exit')
    else:
        events.append((frames - 1, 0, 0, 0, 0, 0))
    return ''.join(f'{f}:{buttons:x}:{lx},{ly},{rx},{ry}\n' for f, buttons, lx, ly, rx, ry in events)


def clone_file(source, destination):
    """Independent bytes; never hard-link a mutable save, setting, module or cache."""
    with source.open('rb') as before, destination.open('xb') as after:
        cloned = False
        if sys.platform.startswith('linux'):
            import fcntl
            try:
                fcntl.ioctl(after.fileno(), 0x40049409, before.fileno())  # Linux FICLONE, not a hard link
                cloned = True
            except OSError:
                pass
        if not cloned:
            shutil.copyfileobj(before, after, 1024 * 1024)
    shutil.copystat(source, destination)


def clone_tree(source, destination, ancestors=()):
    """Dereference links into private copies; reject cycles and special files."""
    real = source.resolve(strict=True)
    if real in ancestors:
        raise ValueError(f'cyclic directory link in game data: {source}')
    if real.is_dir():
        if destination.resolve().is_relative_to(real):
            raise ValueError(f'snapshot destination is inside its source directory: {source}')
        destination.mkdir(parents=True, exist_ok=False)
        for item in sorted(source.iterdir()):
            clone_tree(item, destination / item.name, (*ancestors, real))
    elif real.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        clone_file(real, destination)
    else:
        raise ValueError(f'special file cannot be snapshotted: {source}')


def user_data_default():
    if os.name == 'nt':
        return Path(os.environ.get('APPDATA', Path.home())) / 'scarletnx'
    if sys.platform == 'darwin':
        return Path.home() / 'Library/Application Support/scarletnx'
    xdg = Path(os.environ.get('XDG_DATA_HOME', ''))
    return (xdg if xdg.is_absolute() else Path.home() / '.local/share') / 'scarletnx'


def app_root(folder, user_data):
    for parent in (folder, *folder.parents):
        if parent.name == '.scarletnx' or parent == user_data:
            return parent
    return None


def settings_folder(module, user_data):
    record = module.parent / (module_name(module) + '.folders')
    if record.is_file():
        for line in reversed(record.read_text().splitlines()):
            key, separator, value = line.partition('=')
            if separator and key.strip() == 'config' and Path(value.strip()).is_dir():
                return Path(value.strip()).resolve()
    # --user-data selects saves/keys; only the native default data root and .scarletnx ancestors redirect settings.
    default_data = user_data_default().resolve()
    root = app_root(module.parent, default_data)
    if root:
        if root == default_data and sys.platform not in ('darwin', 'win32'):
            xdg = Path(os.environ.get('XDG_CONFIG_HOME', ''))
            config = (xdg if xdg.is_absolute() else Path.home() / '.config') / 'scarletnx'
        else:
            config = root / 'config'
        return config / module.parent.relative_to(root)
    return module.parent


def hashes_under(path):
    if not path.is_dir():
        return {}
    return {p.relative_to(path).as_posix(): sha256(p) for p in sorted(path.rglob('*')) if p.is_file()}


def validate_settings(folder, game_name):
    """Match native INI line trimming: no shell quoting or inline-comment expansion is supported by the runtime."""
    if not folder.is_dir():
        return
    for ini in sorted(folder.glob(game_name + '*.ini')):
        for number, line in enumerate(ini.read_text(encoding='utf-8-sig').splitlines(), 1):
            line = line.strip(' \t\r')
            if not line or line[0] in ';#[':
                continue
            key, separator, value = line.partition('=')
            key, value = key.strip(' \t\r'), value.strip(' \t\r')
            if not separator or not value:
                continue
            if key in EXTERNAL_OUTPUT_SETTINGS:
                raise ValueError(f'{ini}:{number}: nonempty {key} is unsupported by isolated benchmarking; '
                                 'use settings without external debug outputs/tools')
            if key == 'source' and not Path(value).is_absolute():
                raise ValueError(f'{ini}:{number}: source= must be absolute because benchmark runs use private working folders')


def validate_destination(module, output, user_data):
    config = settings_folder(module, user_data)
    data = app_root(module.parent, user_data) or user_data
    for source in (module.parent, config, user_data, data):
        if output.is_relative_to(source.resolve()):
            raise ValueError('--out must be outside source game, configuration and user-data folders')
    validate_settings(module.parent, module.parent.name)
    if config != module.parent:
        validate_settings(config, module.parent.name)


def snapshot(module, output, user_data, keys=None):
    seed = output / 'seed' / '.scarletnx'
    game = seed / 'recompiled' / module.parent.name
    clone_tree(module.parent, game)
    config = seed / 'config' / 'recompiled' / module.parent.name
    config.mkdir(parents=True)
    source_config = settings_folder(module, user_data)
    if source_config.is_dir():
        for ini in sorted(source_config.glob(module.parent.name + '*.ini')):
            clone_file(ini, config / ini.name)
    validate_settings(config, module.parent.name)
    source_data = app_root(module.parent, user_data) or user_data
    for name in ('save', 'users'):
        if (source_data / name).exists():
            clone_tree(source_data / name, seed / name)
    copied_keys = []
    for name in ('prod.keys', 'title.keys'):
        explicit = (keys or {}).get(name)
        candidates = [explicit] if explicit else [source_data / name, user_data / name, Path.home() / '.switch' / name]
        found = next((p for p in candidates if p.is_file()), None)
        if explicit and not found:
            raise ValueError(f'explicit {name} file does not exist')
        if found:
            clone_file(found, seed / name)
            (seed / name).chmod(0o600)
            copied_keys.append(name)
    # Source .folders must never redirect a run to the player's files, including secondary program switches.
    for record in game.glob('*.folders'):
        record.unlink()
    return seed, module.relative_to(module.parent), {
        'source_game': str(module), 'source_config': str(source_config), 'source_user_data': str(source_data),
        'keys_copied': copied_keys,
        'module_sha256': sha256(module), 'settings_sha256': hashes_under(config),
        'global_saves_sha256': hashes_under(seed / 'save'), 'users_sha256': hashes_under(seed / 'users'),
        'game_saves_sha256': {str(p.relative_to(game)): hashes_under(p) for p in game.glob('*_data/save')},
    }


def prepare_run(seed, relative_module, run, script):
    root = run / '.scarletnx'
    clone_tree(seed, root)
    game_parent = root / 'recompiled' / relative_module.parent.name
    # relative_module here includes the original game directory, retaining update naming semantics.
    module = root / 'recompiled' / relative_module
    config = root / 'config' / 'recompiled' / game_parent.name
    state = root / 'state' / 'recompiled' / game_parent.name
    cache = root / 'cache' / 'recompiled' / game_parent.name
    for directory in (config, state, cache, run / 'driver-cache', run / 'tmp'):
        directory.mkdir(parents=True, exist_ok=True)
    for candidate in game_parent.iterdir():
        if any(candidate.name.endswith(suffix) for suffix in MODULE_SUFFIXES):
            (game_parent / (module_name(candidate) + '.folders')).write_text(
                f'config={config}\nstate={state}\ncache={cache}\n')
    tas = game_parent / 'tas'
    tas.mkdir(exist_ok=True)
    (tas / (module_name(module) + '.9.snxtas')).write_text(script)
    return module, config


def ppm(path):
    data = path.read_bytes()
    # ScarletNX emits precisely this binary PPM header, with no comments or arbitrary trailing payload.
    match = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\n', data)
    if not match:
        raise ValueError('final screenshot is not a ScarletNX P6 PPM image')
    width, height = map(int, match.groups())
    pixels = data[match.end():]
    if width <= 0 or height <= 0 or len(pixels) != width * height * 3:
        raise ValueError('final screenshot has invalid dimensions or truncated pixels')
    if pixels == pixels[:3] * (width * height):
        raise ValueError('final screenshot is uniform; scene rendering has not been established')
    return width, height, pixels


def validate_result(code, text, image, frames, dimensions=None, seconds=None):
    if code:
        raise ValueError(f'process exited with status {code}')
    matches = list(MARKER.finditer(text))
    if len(matches) != 1:
        raise ValueError('expected exactly one final frame marker')
    actual = int(matches[0][1])
    if actual <= 0:
        raise ValueError('runtime reported no presented frames')
    if frames is not None and actual != frames:
        raise ValueError(f'fixed workload requires exactly {frames} frames; runtime reported {actual}')
    if not image.is_file():
        raise ValueError('final screenshot is missing')
    width, height, _ = ppm(image)
    if dimensions and (width, height) != tuple(dimensions):
        raise ValueError('screenshot dimensions changed across benchmark runs')
    if matches[0][3] and (int(matches[0][3]), int(matches[0][4])) != (width, height):
        raise ValueError('frame marker dimensions do not match screenshot')
    versions = VERSION.findall(text)
    if len(set(versions)) != 1:
        raise ValueError('runtime game/version metadata is missing or ambiguous')
    result = {'frames': actual, 'dimensions': [width, height], 'screenshot_sha256': sha256(image),
              'game_version': versions[0], 'runtime_frame_hash': matches[0][2]}
    intervals = INTERVAL.findall(text)
    if frames is None and not intervals:
        raise ValueError('timed throughput requires one unattended_ns game-interval marker')
    if frames is None and (seconds is None or not 0 < seconds < float('inf')):
        raise ValueError('timed throughput requires its requested positive duration')
    if intervals:
        if len(intervals) != 1 or int(intervals[0]) <= 0:
            raise ValueError('game-interval timing marker is invalid or duplicated')
        result['game_interval_ns'] = int(intervals[0])
        result['game_interval_fps'] = actual * 1e9 / int(intervals[0])
        if frames is None and result['game_interval_ns'] < seconds * 1e9:
            raise ValueError(f'timed game interval ended before the requested {seconds} seconds')
        if frames is None:
            result['seconds_limit'] = seconds
    return result


def paired_orders(rounds, seed):
    first = random.Random(seed).choice(('baseline', 'candidate'))
    other = 'candidate' if first == 'baseline' else 'baseline'
    return [(first, other) if i % 2 == 0 else (other, first) for i in range(rounds)]


def run_process(command, run, timeout):
    env = dict(os.environ, TMPDIR=str(run / 'tmp'), TEMP=str(run / 'tmp'), TMP=str(run / 'tmp'),
               XDG_CACHE_HOME=str(run / 'driver-cache'), MESA_SHADER_CACHE_DIR=str(run / 'driver-cache' / 'mesa'),
               __GL_SHADER_DISK_CACHE_PATH=str(run / 'driver-cache' / 'nvidia'))
    with (run / 'stdout.log').open('wb') as stdout, (run / 'stderr.log').open('wb') as stderr:
        begin = time.perf_counter_ns()
        process = subprocess.Popen(command, cwd=run, env=env, stdout=stdout, stderr=stderr,
                                   start_new_session=os.name != 'nt')
        expired = threading.Event()

        def kill():
            if process.poll() is None:
                expired.set()
                try:
                    if os.name != 'nt':
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                except ProcessLookupError:
                    pass

        timer = threading.Timer(timeout, kill)
        timer.daemon = True
        timer.start()
        try:
            code = process.wait()  # Blocking wait avoids timeout polling's up-to-50ms measurement quantization.
            elapsed = time.perf_counter_ns() - begin
        finally:
            timer.cancel()
            timer.join()
        if expired.is_set():
            raise ValueError(f'run timed out after {timeout} seconds')
    return code, elapsed


def summarize(runs):
    pairs = []
    for round_number in sorted({r['round'] for r in runs}):
        pair = {r['variant']: r for r in runs if r['round'] == round_number}
        if set(pair) != {'baseline', 'candidate'}:
            raise ValueError('cannot summarize incomplete pairs')
        before, after = pair['baseline'], pair['candidate']
        if before['game_version'] != after['game_version']:
            raise ValueError('game versions changed between paired runs')
        pairs.append({'round': round_number, 'baseline_over_candidate': before['elapsed_ns'] / after['elapsed_ns'],
                      'final_screenshot_exact': before['screenshot_sha256'] == after['screenshot_sha256']})
    ratio = statistics.median(p['baseline_over_candidate'] for p in pairs)
    result = {'metric': 'end_to_end_fixed_workload_wall_time', 'pairs': pairs,
            'baseline_median_elapsed_ns': statistics.median(r['elapsed_ns'] for r in runs if r['variant'] == 'baseline'),
            'candidate_median_elapsed_ns': statistics.median(r['elapsed_ns'] for r in runs if r['variant'] == 'candidate'),
            'median_baseline_over_candidate': ratio, 'median_candidate_throughput_change_percent': (ratio - 1) * 100,
            'candidate_faster_every_pair': all(p['baseline_over_candidate'] > 1 for p in pairs),
            'final_screenshots_exact_every_pair': all(p['final_screenshot_exact'] for p in pairs),
            'rendering_equivalence': 'not_established_by_final_screenshots'}
    if all('game_interval_ns' in r for r in runs):
        interval_pairs = []
        for round_number in sorted({r['round'] for r in runs}):
            pair = {r['variant']: r for r in runs if r['round'] == round_number}
            interval_pairs.append(pair['baseline']['game_interval_ns'] / pair['candidate']['game_interval_ns'])
        result['game_interval'] = {
            'scope': 'After unattended_start through final screenshot; includes guest initialization/loading. '
                     'Excludes pre-game preparation. Not GPU frame time or NVN-only time.',
            'median_baseline_over_candidate': statistics.median(interval_pairs),
            'paired_baseline_over_candidate': interval_pairs,
            'candidate_faster_every_pair': all(ratio > 1 for ratio in interval_pairs),
        }
    else:
        result['game_interval'] = {'status': 'unavailable', 'reason': 'not every runtime emitted unattended_ns'}
    return result


def summarize_timed(runs):
    """Different frame counts are compared as rates over the runtime's measured game interval, never wall time."""
    pairs, rates = [], {'baseline': [], 'candidate': []}
    for round_number in sorted({r['round'] for r in runs}):
        pair = {r['variant']: r for r in runs if r['round'] == round_number}
        if set(pair) != {'baseline', 'candidate'}:
            raise ValueError('cannot summarize incomplete pairs')
        if pair['baseline']['game_version'] != pair['candidate']['game_version']:
            raise ValueError('game versions changed between paired runs')
        for variant, row in pair.items():
            if row['frames'] <= 0 or row.get('game_interval_ns', 0) <= 0:
                raise ValueError('timed throughput requires positive frames and game intervals')
            rates[variant].append(row['frames'] * 1e9 / row['game_interval_ns'])
        pairs.append({'round': round_number, 'baseline_frames': pair['baseline']['frames'],
                      'candidate_frames': pair['candidate']['frames'], 'baseline_fps': rates['baseline'][-1],
                      'candidate_fps': rates['candidate'][-1],
                      'candidate_over_baseline_fps': rates['candidate'][-1] / rates['baseline'][-1],
                      'final_screenshot_exact': pair['baseline']['screenshot_sha256'] == pair['candidate']['screenshot_sha256']})
    ratio = statistics.median(p['candidate_over_baseline_fps'] for p in pairs)
    return {'metric': 'game_interval_frame_throughput', 'pairs': pairs,
            'scope': 'Presented frames per second since unattended_start through final screenshot, including guest '
                     'initialization/loading. Variable frame counts; scene progress can differ. Not GPU/NVN-only time.',
            'baseline_median_fps': statistics.median(rates['baseline']),
            'candidate_median_fps': statistics.median(rates['candidate']),
            'median_candidate_over_baseline_fps': ratio,
            'median_candidate_throughput_change_percent': (ratio - 1) * 100,
            'candidate_higher_throughput_every_pair': all(p['candidate_over_baseline_fps'] > 1 for p in pairs),
            'process_walltime': 'Retained as elapsed_ns per run; not comparable directly when frame counts differ.',
            'rendering_equivalence': 'not_established_by_final_screenshots'}


def native_comparison(path, report, args):
    if report['metric'] == 'game_interval_frame_throughput':
        if path:
            raise ValueError('external native comparisons are unsupported for timed throughput mode')
        return {'status': 'unavailable', 'reason': 'native schema is unsupported for timed throughput mode'}
    if not path:
        return {'status': 'unavailable', 'reason': 'no external native measurement supplied'}
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError('external native measurement must be a JSON object')
    if value.get('metric') != report['metric']:
        raise ValueError('external native metric must be end_to_end_fixed_workload_wall_time')
    for key in ('hardware', 'game_version', 'workload', 'scene'):
        if not isinstance(value.get(key), str) or not value[key].strip():
            raise ValueError(f'external native measurement requires {key} metadata')
    if not args.workload or not args.scene or value['workload'] != args.workload or value['scene'] != args.scene:
        raise ValueError('external native workload/scene must match explicit --workload and --scene')
    if value.get('frames') != args.frames or value.get('script_sha256') != report['script_sha256']:
        raise ValueError('external native frames/script do not match the measured workload')
    if {r['game_version'] for r in report['runs']} != {value['game_version']}:
        raise ValueError('external native game version does not match the runtime')
    samples = value.get('elapsed_ns_samples')
    if not isinstance(samples, list) or len(samples) < 3 or any(type(n) is not int or n <= 0 for n in samples):
        raise ValueError('external native measurement requires at least three positive integer elapsed_ns_samples')
    candidate = statistics.median(r['elapsed_ns'] for r in report['runs'] if r['variant'] == 'candidate')
    return {'status': 'user_supplied_end_to_end_unverified', 'metadata': value,
            'native_over_candidate': statistics.median(samples) / candidate,
            'scope': 'Different hardware; external timing and scene equivalence are not independently verified. '
                     'This does not isolate NVN performance.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('game', 'baseline', 'candidate', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    limit = parser.add_mutually_exclusive_group(required=True)
    limit.add_argument('--frames', type=int, help='require this exact number of frames in every run')
    limit.add_argument('--seconds', type=float, help='timed neutral-input throughput; positive whole seconds only')
    parser.add_argument('--script', type=Path, help='frame-based .snxtas input file; omitted means neutral input')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--seed', type=int, default=781)
    parser.add_argument('--user-data', type=Path, help='source global saves/users root; default: detected ScarletNX data')
    parser.add_argument('--keys', type=Path, help='prod.keys to copy privately; default: data folder, then ~/.switch')
    parser.add_argument('--title-keys', type=Path, help='title.keys to copy privately; default: data folder, then ~/.switch')
    parser.add_argument('--uncapped', action='store_true', help='apply fps_limit=0 and vsync=0 equally to both builds')
    parser.add_argument('--native', type=Path, help='external native end-to-end samples and required workload metadata')
    parser.add_argument('--workload', help='description identifying the measured workload')
    parser.add_argument('--scene', help='description identifying the final scene/save route')
    args = parser.parse_args(argv)
    timed = args.seconds is not None
    if args.rounds < 3 or not 0 < args.timeout < float('inf'):
        parser.error('require rounds >= 3 and a finite positive timeout')
    if (timed and not 0 < args.seconds <= 2147483647) or (not timed and not 2 <= args.frames <= 2147483647):
        parser.error('require finite seconds in (0, INT_MAX] or frames from 2 to INT_MAX')
    if timed and not args.seconds.is_integer():
        parser.error('the runtime seconds limit requires positive whole seconds')
    if timed and (args.script or args.native):
        parser.error('timed mode supports neutral input only and cannot use --script or --native')
    seconds_limit = int(args.seconds) if timed else 0
    output = args.out.resolve()
    output_created = False
    report = {'scope': 'Whole runtime, fixed game workload; not NVN-only time or a native NVN benchmark.',
              'metric': 'game_interval_frame_throughput' if timed else 'end_to_end_fixed_workload_wall_time',
              'runs': [], 'rejected': [],
              'native_comparison': {'status': 'unavailable'}, 'host': platform.platform(),
              'cache_policy': 'Fresh runtime and driver caches per run; OS filesystem cache is uncontrolled.',
              'isolation_policy': 'Independent data/save/settings copies; source folder redirects replaced. '
                                  'External debug-output paths/tools and relative source= paths rejected.',
              'limitations': ['Timing includes startup/preparation, game execution, final screenshot and exit.',
                              'Clock, filesystem-cache and game nondeterminism remain uncontrolled.',
                              'Final screenshots require visual review and do not prove complete rendering correctness.']}
    try:
        module = find_game(args.game)
        binaries = {name: getattr(args, name).resolve(strict=True) for name in ('baseline', 'candidate')}
        if any(not binary.is_file() or not os.access(binary, os.X_OK) for binary in binaries.values()):
            raise ValueError('baseline and candidate must be executable runtime files')
        script = validate_script(None, 2147483647) if timed else validate_script(args.script, args.frames)
        user_data = (args.user_data or user_data_default()).resolve()
        validate_destination(module, output, user_data)
        output.mkdir(parents=True, exist_ok=False)
        output_created = True
        output.chmod(0o700)
        report.update({'binary_sha256': {name: sha256(binary) for name, binary in binaries.items()},
                       'binaries': {name: str(binary) for name, binary in binaries.items()},
                       'frames_requested': args.frames, 'rounds': args.rounds, 'seed': args.seed,
                       'seconds_requested': args.seconds, 'runtime_seconds_limit': seconds_limit,
                       'script_sha256': hashlib.sha256(script.encode()).hexdigest(),
                       'workload': args.workload, 'scene': args.scene,
                       'pacing_policy': 'fps_limit=0, vsync=0 for both builds' if args.uncapped else 'existing settings'})
        if timed:
            report['scope'] = 'Whole-game interval throughput with neutral input for a fixed duration; '
            report['scope'] += 'includes initialization/loading, variable frame counts. Not NVN-only or GPU time.'
            report['limitations'].append('Timed runs can reach different frames/scenes; startup/loading is not gameplay.')
        (output / 'input.snxtas').write_text(script)
        seed, relative, evidence = snapshot(module, output, user_data, {'prod.keys': args.keys, 'title.keys': args.title_keys})
        relative = Path(module.parent.name) / relative
        report['snapshot'] = evidence
        save_json(output / 'results.json', report)
        dimensions = None
        for round_number, order in enumerate(paired_orders(args.rounds, args.seed), 1):
            for variant in order:
                run = output / f'round{round_number}-{variant}'
                run.mkdir()
                row = {'round': round_number, 'variant': variant, 'directory': str(run)}
                report['active_run'] = row
                save_json(output / 'results.json', report)
                private_module, config = prepare_run(seed, relative, run, script)
                command = [str(binaries[variant]), f'--game={private_module}', '-H',
                           f'--frames={-1 if timed else args.frames}', f'--seconds={seconds_limit}',
                           f'--shot={run / "final"}', '--shot_every=0', '--script=',
                           '--input_record=0', '--input_replay=9']
                if args.uncapped:
                    command.extend(('--fps_limit=0', '--vsync=0'))
                for name, flag in (('prod.keys', '--keys'), ('title.keys', '--title-keys')):
                    if (run / '.scarletnx' / name).is_file():
                        command.append(f'{flag}={run / ".scarletnx" / name}')
                row.update({'command': command, 'module_sha256': sha256(private_module),
                            'settings_sha256': hashes_under(config)})
                if row['module_sha256'] != evidence['module_sha256']:
                    raise ValueError('compiled module changed while the benchmark was being prepared')
                if sha256(binaries[variant]) != report['binary_sha256'][variant]:
                    raise ValueError('runtime binary changed during the benchmark')
                if row['settings_sha256'] != evidence['settings_sha256']:
                    raise ValueError('initial settings differ from the shared snapshot')
                save_json(run / 'run.json', row)
                try:
                    code, elapsed = run_process(command, run, args.timeout)
                finally:
                    # Verify after even the last run: a fast result from self-modified code cannot compete.
                    row['binary_sha256_after'] = {name: sha256(binary) for name, binary in binaries.items()}
                    row['module_sha256_after'] = sha256(private_module)
                    save_json(run / 'run.json', row)
                if row['binary_sha256_after'] != report['binary_sha256']:
                    raise ValueError('runtime binary changed during the measured run')
                if row['module_sha256_after'] != evidence['module_sha256']:
                    raise ValueError('compiled module changed during the measured run')
                row.update({'returncode': code, 'elapsed_ns': elapsed})
                text = '\n'.join((run / name).read_text(errors='replace') for name in ('stdout.log', 'stderr.log'))
                row.update(validate_result(code, text, run / 'final.ppm', args.frames, dimensions,
                                           seconds=seconds_limit if timed else None))
                row['runtime_hardware'] = [line for line in text.splitlines() if line.startswith('system: ') or
                                           (line.startswith('vulkan: ') and (', Vulkan ' in line or
                                                                            'the driver that runs:' in line))]
                dimensions = row['dimensions']
                if report['runs'] and row['game_version'] != report['runs'][0]['game_version']:
                    raise ValueError('game version changed across benchmark runs')
                report['runs'].append(row)
                report.pop('active_run', None)
                save_json(run / 'run.json', row)
                save_json(output / 'results.json', report)
                print(f'round {round_number} {variant}: {elapsed / 1e9:.3f}s, '
                      f'{row["frames"]} frames, {dimensions[0]}x{dimensions[1]}', flush=True)
        report['summary'] = summarize_timed(report['runs']) if timed else summarize(report['runs'])
        report['native_comparison'] = native_comparison(args.native, report, args)
        report['status'] = 'complete'
        save_json(output / 'results.json', report)
        print(json.dumps(report['summary'], indent=2))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report['status'] = 'rejected'
        report['rejected'].append({'reason': str(error), 'run': report.get('active_run')})
        if output_created:
            save_json(output / 'results.json', report)
        print(f'benchmark rejected: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
