#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Tune small Linux CPU-sampling rings, then profile a command without perf privileges.

  python3 tools/profile_cpu.py tune --out /local/scratch/ring-search
  python3 tools/profile_cpu.py run --config /local/scratch/ring-search/winner.json \
      --out /local/scratch/profile -- PROGRAM ARGUMENTS...

Each thread samples its own CPU clock. The signal handler writes a flat ring in
memory. One collector drains rings to samples.bin, whose little-endian records
are monotonic ns, host PC (two u64), TID and timer weight (two u32). Kernel timer
coalescing and ring drops are reported separately. Use sampling for cost
attribution, and unprofiled runs for game speed comparisons. A tuned buffer is
validated for the supplied workload, not for every application or system load.
"""
import argparse
import hashlib
import mmap
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import struct
import subprocess
import sys
import time

from tune_cpu_sequences import analyze_candidates, measurement

ROOT = Path(__file__).resolve().parents[1]


def compile_tools(out):
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'aarch64'):
        raise ValueError('the sampler supports Linux x86-64 and AArch64')
    clang = shutil.which('clang')
    if not clang:
        raise ValueError('Clang is required to build the diagnostic sampler')
    library = out / 'cpu_sample.so'
    workload = out / 'cpu_sample_workload'
    with (out / 'build.log').open('w') as log:
        subprocess.run([clang, '-std=c99', '-pedantic-errors', '-O2', '-Wall', '-Wextra', '-shared', '-fPIC',
                        str(ROOT / 'tools/cpu_sample.c'), '-pthread', '-ldl', '-lrt', '-o', str(library)],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run([clang, '-std=c99', '-pedantic-errors', '-O2', '-Wall', '-Wextra',
                        str(ROOT / 'tests/support/cpu_sample_workload.c'), '-pthread', '-o', str(workload)],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    return library, workload


HEADER = struct.Struct('<QIIIIIIQQIIQ')
RECORD = struct.Struct('<QQII')
MAGIC = 0x534e5853414d5031


def recover(path):
    """Recover published ring records after waitpid, even when the command uses _exit.

    A collector can exit between write() and its tail publication. Timestamp/TID
    identities make recovery idempotent. An interrupted signal handler can leave
    one unpublished attempt per thread, which is distinct from ring overflow.
    """
    data = path.read_bytes()
    complete = len(data) - len(data) % RECORD.size
    data = data[:complete]
    latest = {}
    ordered = True
    for ns, pc, tid, weight in RECORD.iter_unpack(data):
        if not pc or not tid or not weight or ns <= latest.get(tid, 0):
            ordered = False
        latest[tid] = ns
    appended = bytearray()
    attempted = dropped = overruns = produced = interrupted = peak = threads = 0
    with path.with_suffix(path.suffix + '.state').open('rb') as state_file:
        with mmap.mmap(state_file.fileno(), 0, access=mmap.ACCESS_READ) as state:
            if len(state) < HEADER.size:
                raise ValueError('truncated sampler state header')
            magic, version, capacity, stride, count, error, ready, poll, interval, collector, focus, offset = \
                HEADER.unpack_from(state)
            if magic != MAGIC or version != 1 or offset != HEADER.size or not capacity or capacity & (capacity - 1) or \
                    stride < 144 + capacity * RECORD.size or stride & 63 or capacity > 65536 or count > 4096 or offset + count * stride > len(state):
                raise ValueError('invalid sampler state layout')
            for i in range(count):
                at = offset + i * stride
                if not struct.unpack_from('<I', state, at + 140)[0]:
                    continue
                head, high, tries, lost, over = struct.unpack_from('<IIQQQ', state, at)
                tail = struct.unpack_from('<I', state, at + 64)[0]
                tid = struct.unpack_from('<I', state, at + 136)[0]
                outstanding = (head - tail) & 0xffffffff
                unfinished = tries - head - lost
                if outstanding > capacity or unfinished not in (0, 1):
                    raise ValueError('sampler ring accounting differs from its published records')
                for number in range(outstanding):
                    entry = at + 144 + ((tail + number) & (capacity - 1)) * RECORD.size
                    record = state[entry:entry + RECORD.size]
                    ns, pc, record_tid, weight = RECORD.unpack(record)
                    if not pc or record_tid != tid or not weight:
                        ordered = False
                    if ns > latest.get(tid, 0):
                        appended.extend(record)
                        latest[tid] = ns
                attempted += tries
                dropped += lost
                overruns += over
                produced += head
                interrupted += unfinished
                peak = max(peak, high)
                threads += 1
    samples = (len(data) + len(appended)) // RECORD.size
    # Replace a partial trailing write before appending the still-published ring records.
    with path.open('r+b') as output:
        output.truncate(complete)
        output.seek(complete)
        output.write(appended)
    stats = dict(capacity=capacity, poll_ns=poll, interval_ns=interval, threads=threads, samples=samples,
                 attempted=attempted, dropped=dropped, timer_overruns=overruns, exit_incomplete_samples=interrupted,
                 recovered_samples=len(appended) // RECORD.size, peak_slots=peak, reserved_bytes=threads * stride,
                 error=error, records_ordered=ordered, collector_tid=collector)
    stats['valid'] = bool(ready) and not error and samples > 0 and not dropped and ordered and \
        samples == produced and attempted == samples + dropped + interrupted
    path.with_suffix(path.suffix + '.json').write_text(json.dumps(stats, indent=2))
    return stats


def capture(command, folder, library=None, config=None, timeout=300, monitor=False):
    folder.mkdir()
    environment = dict(os.environ)
    if library:
        prior = environment.get('LD_PRELOAD', '')
        environment['LD_PRELOAD'] = str(library) + (':' + prior if prior else '')
        environment.update(SNX_SAMPLE_PARENT_PID=str(os.getpid()), SNX_SAMPLE_PATH=str(folder / 'samples.bin'),
                           SNX_SAMPLE_CAPACITY=str(config['capacity']), SNX_SAMPLE_POLL_NS=str(config['poll_ns']),
                           SNX_SAMPLE_INTERVAL_NS=str(config['interval_ns']))
    start = time.monotonic()
    with (folder / 'run.log').open('w') as log, (folder / 'sched.jsonl').open('w') as sched:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
        expired = False
        try:
            while process.poll() is None:
                if monitor:
                    tasks = []
                    for path in Path(f'/proc/{process.pid}/task').glob('*'):
                        try:
                            cpu, wait, slices = map(int, (path / 'schedstat').read_text().split())
                            state = (path / 'stat').read_text().rsplit(') ', 1)[1][0]
                            tasks.append([int(path.name), (path / 'comm').read_text().strip(), state, cpu, wait, slices])
                        except (OSError, ValueError):
                            pass
                    sched.write(json.dumps([time.monotonic_ns(), tasks]) + '\n')
                    try:
                        maps = Path(f'/proc/{process.pid}/maps').read_text()
                        if '_code.so' in maps or not (folder / 'maps.txt').exists():
                            (folder / 'maps.txt').write_text(maps)
                    except OSError:
                        pass
                if time.monotonic() - start > timeout:
                    expired = True
                    break
                time.sleep(.1 if monitor else .01)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    stats = None
    if library:
        stats = recover(folder / 'samples.bin')
        stats.update(wall_ns=int((time.monotonic() - start) * 1e9), exit_code=process.returncode)
        (folder / 'capture.json').write_text(json.dumps(stats, indent=2))
    if expired:
        raise TimeoutError('the profiled command exceeded its deadline')
    if process.returncode:
        raise RuntimeError(f'command exited {process.returncode}: {folder / "run.log"}')
    return stats


def tune(args, library, workload):
    command = [str(workload), str(args.iterations), str(args.threads)]
    configs = {}
    checks = []
    # Start at one slot. Each drain period grows independently until repeated runs lose no delivered sample.
    for poll_ns in args.poll_ns:
        capacity = 1
        while capacity <= args.max_capacity:
            config = dict(capacity=capacity, poll_ns=poll_ns, interval_ns=args.interval_ns)
            good = True
            for attempt in range(3):
                folder = args.out / f'probe-p{poll_ns}-c{capacity}-{attempt}'
                stats = capture(command, folder, library, config, args.timeout)
                checks.append(dict(config=config, stats=stats))
                if stats['error']:
                    raise ValueError(f'sampler initialization or I/O failed: {stats}')
                if not stats['valid']:
                    good = False
                    break
            if good:
                name = f'p{poll_ns}-c{capacity}'
                configs[name] = config
                print(name, 'loss-free calibration', flush=True)
                break
            capacity *= 2
    if not configs:
        raise ValueError('no buffer passed the supplied size bound')
    checksum = None
    generation = 0
    while True:
        observations = {'baseline': {'native': []}, **{name: {'native': []} for name in configs}}
        invalid = set()
        reserved = {}
        rng = random.Random(args.seed + generation)
        for round_index in range(-2, args.rounds):
            order = list(observations)
            rng.shuffle(order)
            for name in order:
                if name in invalid:
                    continue
                folder = args.out / f'round-g{generation}-{round_index + 2}-{name}'
                stats = capture(command, folder, library if name != 'baseline' else None,
                                configs.get(name), args.timeout)
                row = measurement((folder / 'run.log').read_text())
                if checksum is None:
                    checksum = row['checksum']
                if row['checksum'] != checksum:
                    raise ValueError('the sampled workload checksum differs from the baseline')
                if stats is not None and stats['error']:
                    raise ValueError(f'sampler initialization or I/O failed: {stats}')
                if stats is not None and not stats['valid']:
                    invalid.add(name)
                    continue
                if stats is not None:
                    reserved[name] = max(reserved.get(name, 0), stats['reserved_bytes'])
                if round_index >= 0:
                    observations[name]['native'].append(row)
        if not invalid:
            break
        # Discard the incomplete timing batch and grow only the configurations that lost samples.
        # Every final comparison uses complete paired rounds from the same measurement generation.
        for name in invalid:
            config = configs.pop(name)
            config['capacity'] *= 2
            if config['capacity'] <= args.max_capacity:
                replacement = f'p{config["poll_ns"]}-c{config["capacity"]}'
                configs[replacement] = config
                print(name, 'lost samples, grow to', replacement, flush=True)
        if not configs:
            raise ValueError('no buffer passed the supplied size bound during validation')
        generation += 1
    analysis = analyze_candidates(observations, 'baseline', None, args.bootstrap_samples, 1, args.seed)
    speeds = analysis['comparisons']
    fastest = max(speeds, key=lambda name: speeds[name]['weighted_geometric_speedup']['ratio'])
    relative = analyze_candidates(observations, fastest, None, args.bootstrap_samples, 1, args.seed + 1)
    equivalent = [fastest] + [name for name, row in relative['comparisons'].items() if name != 'baseline' and
                             row['weighted_geometric_speedup']['bootstrap_95_interval'][0] >= 1 - args.tolerance]
    # Equal aligned footprints get the larger ring without another byte of memory.
    winner = min(equivalent, key=lambda name: (reserved[name], -configs[name]['capacity'],
                 -speeds[name]['weighted_geometric_speedup']['ratio']))
    result = dict(config=configs[winner], candidate=winner, fastest=fastest, equivalent=equivalent,
                  rounds=args.rounds, no_ring_drops=True, reserved_bytes=reserved, observations=observations, analysis=analysis,
                  relative_to_fastest=relative, probes=checks,
                  library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                  limit='Smallest measured buffer within the supplied speed tolerance, on this native workload.')
    (args.out / 'results.json').write_text(json.dumps(result, indent=2))
    (args.out / 'winner.json').write_text(json.dumps({'config': configs[winner], 'library_sha256':
                                                    result['library_sha256'], 'limit': result['limit']}, indent=2))
    print(json.dumps({'candidate': winner, 'config': configs[winner], 'report': str(args.out / 'results.json')}))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    search = sub.add_parser('tune')
    search.add_argument('--iterations', type=int, default=160000000)
    search.add_argument('--threads', type=int, default=4)
    search.add_argument('--rounds', type=int, default=9)
    search.add_argument('--max-capacity', type=int, default=4096)
    search.add_argument('--poll-ns', type=int, nargs='+', default=[500000, 1000000, 2000000, 4000000, 8000000, 16000000])
    search.add_argument('--bootstrap-samples', type=int, default=10000)
    search.add_argument('--seed', type=int, default=20261103)
    search.add_argument('--tolerance', type=float, default=.02)
    run = sub.add_parser('run')
    run.add_argument('--config', type=Path, required=True)
    run.add_argument('command', nargs=argparse.REMAINDER)
    for command in (search, run):
        command.add_argument('--out', type=Path, required=True)
        command.add_argument('--timeout', type=float, default=300)
        command.add_argument('--interval-ns', type=int, default=1000000)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or not 100000 <= args.interval_ns <= 1000000000:
        parser.error('invalid timeout or sampling interval')
    if args.mode == 'tune' and (args.rounds < 5 or not 1 <= args.threads <= 256 or args.iterations < 1 or
            args.max_capacity < 1 or args.max_capacity > 65536 or not args.poll_ns or
            any(not 1 <= n <= 100000000 for n in args.poll_ns) or not 0 <= args.tolerance < 1 or
            not 1000 <= args.bootstrap_samples <= 100000):
        parser.error('invalid search bounds')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    try:
        library, workload = compile_tools(args.out)
        if args.mode == 'tune':
            tune(args, library, workload)
        else:
            command = args.command[1:] if args.command[:1] == ['--'] else args.command
            if not command:
                raise ValueError('supply the command after --')
            config = json.loads(args.config.read_text())['config']
            stats = capture(command, args.out / 'capture', library, config, args.timeout, monitor=True)
            print(json.dumps(stats))
            if not stats['valid']:
                return 1
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError, TimeoutError) as error:
        print(f'profile_cpu.py: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
