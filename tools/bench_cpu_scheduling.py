#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Probe Linux task scheduling controls and measure a synthetic wake/work workload under CPU contention.

Every timing runs in a disposable process. No sysctls, governors, cgroups, privileges or game settings change.
This measures scheduler behavior, not game FPS. Use actual scene measurements before accepting a game speedup.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import resource
import signal
import statistics
import subprocess
import sys
import time


class Attr(ctypes.Structure):
    _fields_ = [('size', ctypes.c_uint32), ('policy', ctypes.c_uint32), ('flags', ctypes.c_uint64),
                ('nice', ctypes.c_int32), ('priority', ctypes.c_uint32), ('runtime', ctypes.c_uint64),
                ('deadline', ctypes.c_uint64), ('period', ctypes.c_uint64),
                ('util_min', ctypes.c_uint32), ('util_max', ctypes.c_uint32)]


def kernel_api():
    numbers = {'x86_64': (314, 315), 'aarch64': (274, 275), 'arm64': (274, 275)}
    if platform.system() != 'Linux' or platform.machine().lower() not in numbers or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise RuntimeError('This probe supports Linux x86-64 and AArch64.')
    put, get = numbers[platform.machine().lower()]
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long

    def read():
        attr = Attr()
        if libc.syscall(ctypes.c_long(get), ctypes.c_int(0), ctypes.byref(attr),
                        ctypes.c_uint(ctypes.sizeof(attr)), ctypes.c_uint(0)):
            raise OSError(ctypes.get_errno(), 'sched_getattr')
        return attr

    def write(attr):
        if libc.syscall(ctypes.c_long(put), ctypes.c_int(0), ctypes.byref(attr), ctypes.c_uint(0)):
            return os.strerror(ctypes.get_errno())
        return None

    return read, write


def apply(mode):
    read, write = kernel_api()
    attr = read()
    before = {name: getattr(attr, name) for name, _ in Attr._fields_}
    errors = {}
    if mode and attr.policy == 0:
        if attr.size >= ctypes.sizeof(attr):
            boost = Attr.from_buffer_copy(attr)
            boost.flags |= 0x60
            boost.util_min = boost.util_max = 1024
            error = write(boost)
            if error:
                errors['utilization_clamp'] = error
            else:
                attr = boost
        else:
            errors['utilization_clamp'] = 'kernel ABI predates utilization clamping'
        if mode == 2:
            attr.runtime = 100_000
            limit, _ = resource.getrlimit(resource.RLIMIT_NICE)
            if limit != 0:
                attr.nice = min(attr.nice, -20 if limit == resource.RLIM_INFINITY else max(-20, 20 - limit))
            error = write(attr)
            if error:
                errors['short_slice'] = error
    after = read()
    effective = {name: getattr(after, name) for name, _ in Attr._fields_}
    if mode == 2 and after.runtime != 100_000:
        errors['short_slice'] = errors.get('short_slice', 'kernel did not apply the requested slice')
    return {'before': before, 'effective': effective, 'unavailable': errors}


def quantile(values, fraction):
    return sorted(values)[min(len(values) - 1, int((len(values) - 1) * fraction))]


def worker(mode, cpu, jobs):
    os.sched_setaffinity(0, {cpu})
    # Start the competitor before changing our policy so it keeps the host defaults.
    burner = subprocess.Popen([sys.executable, __file__, '--burn', str(cpu)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        policy = apply(mode)
        time.sleep(0.2)
        data = bytes(range(256)) * 32
        for _ in range(50):
            hashlib.sha256(data).digest()
        samples = []
        usage = resource.getrusage(resource.RUSAGE_SELF)
        cpu_start, wall_start = time.process_time_ns(), time.perf_counter_ns()
        for _ in range(jobs):
            due = time.perf_counter_ns() + 1_000_000
            time.sleep(0.001)
            for _ in range(32):
                result = hashlib.sha256(data).digest()
            samples.append((time.perf_counter_ns() - due) / 1e6)
        elapsed = time.perf_counter_ns() - wall_start
        cpu_time = time.process_time_ns() - cpu_start
        end_usage = resource.getrusage(resource.RUSAGE_SELF)
        return {'mode': mode, 'policy': policy, 'jobs': jobs, 'checksum': result.hex(),
                'wall_ms': elapsed / 1e6, 'cpu_ms': cpu_time / 1e6,
                'jobs_per_s': jobs * 1e9 / elapsed, 'p50_ms': quantile(samples, 0.5),
                'p95_ms': quantile(samples, 0.95), 'p99_ms': quantile(samples, 0.99),
                'context_switches': end_usage.ru_nvcsw + end_usage.ru_nivcsw - usage.ru_nvcsw - usage.ru_nivcsw}
    finally:
        burner.terminate()
        burner.wait(timeout=10)


def run_worker(command, timeout=120):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        output, error = process.communicate(timeout=timeout)
        if process.returncode:
            raise RuntimeError(f'worker exited {process.returncode}: {error[-2000:]}')
        return json.loads(output)
    finally:
        # Own the complete process group, including a competitor left behind by an exited worker.
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--jobs', type=int, default=600)
    parser.add_argument('--cpu', type=int)
    parser.add_argument('--worker', type=int, choices=(0, 1, 2))
    parser.add_argument('--burn', type=int)
    args = parser.parse_args()
    if args.burn is not None:
        os.sched_setaffinity(0, {args.burn})
        apply(0)
        data = bytes(range(256)) * 32
        while True:
            hashlib.sha256(data).digest()
    if args.worker is not None:
        print(json.dumps(worker(args.worker, args.cpu, args.jobs)))
        return
    if not args.out or args.rounds < 3 or args.jobs < 100:
        parser.error('--out, at least 3 rounds and at least 100 jobs are required')
    kernel_api()
    cpus = sorted(os.sched_getaffinity(0))
    cpu = cpus[-1] if args.cpu is None else args.cpu
    if cpu not in cpus:
        parser.error('selected CPU is outside the process affinity mask')
    args.out.mkdir(parents=True, exist_ok=False)
    report = {'kernel': platform.release(), 'machine': platform.machine(), 'cpu': cpu,
              'scope': 'Synthetic scheduler wake/work latency under single-CPU contention. Not game FPS.', 'runs': []}
    governor = Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor')
    report['governor'] = governor.read_text().strip() if governor.exists() else None
    order = random.Random(481)
    expected_checksum = None
    for round_number in range(args.rounds):
        modes = [0, 1, 2]
        order.shuffle(modes)
        for mode in modes:
            row = run_worker([sys.executable, __file__, '--worker', str(mode), '--cpu', str(cpu),
                              '--jobs', str(args.jobs)])
            if expected_checksum is not None and row['checksum'] != expected_checksum:
                raise RuntimeError('workload output changed between candidates')
            expected_checksum = row['checksum']
            row['round'] = round_number + 1
            report['runs'].append(row)
            print(f"round {round_number + 1} mode {mode}: p95 {row['p95_ms']:.3f} ms, "
                  f"p99 {row['p99_ms']:.3f} ms, {row['jobs_per_s']:.1f} jobs/s", flush=True)
            (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    groups = {mode: [row for row in report['runs'] if row['mode'] == mode] for mode in (0, 1, 2)}
    baseline = groups[0]
    baseline_keys = [tuple(row['policy']['effective'][k] for k in ('policy', 'nice', 'runtime', 'util_min', 'util_max'))
                     for row in baseline]
    qualified = [0]
    for mode in (1, 2):
        rows = groups[mode]
        changed = all(tuple(row['policy']['effective'][k] for k in ('policy', 'nice', 'runtime', 'util_min', 'util_max'))
                      != baseline_keys[i] for i, row in enumerate(rows))
        faster = all(row['p95_ms'] < baseline[i]['p95_ms'] * 0.97 and
                     row['jobs_per_s'] >= baseline[i]['jobs_per_s'] * 0.97 for i, row in enumerate(rows))
        if changed and faster:
            qualified.append(mode)
    winner = min(qualified, key=lambda mode: statistics.median(row['p95_ms'] for row in groups[mode]))
    report['recommended_synthetic_mode'] = winner
    report['recommendation_rule'] = 'Changed controls and p95 latency >3% better in every paired round, with throughput within 3%.'
    (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Synthetic recommendation: cpu_scheduling={winner}. Verify in the same game scene before claiming an FPS gain.')


if __name__ == '__main__':
    main()
