#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare CPU candidates after exactness checks, with quiet serial timings.

The JSON manifest defines item, candidates, build/check argv, and bench argv by
workload. Each timed command prints a final JSON line with elapsed_ns/checksum.
Optional code_bytes adds code size to the Pareto comparison. No code is applied.
"""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import signal
import statistics
import subprocess


def available_cpus():
    return sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else list(range(os.cpu_count() or 1))


def validate(manifest):
    if not isinstance(manifest, dict):
        raise ValueError('the manifest must be a JSON object')
    candidates = manifest.get('candidates', [])
    if not isinstance(candidates, list) or not candidates:
        raise ValueError('candidates must be a nonempty list')
    names = set()
    workloads = None
    for candidate in candidates:
        if not isinstance(candidate, dict):
            raise ValueError('each candidate must be an object')
        name = candidate.get('name', '')
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name) or name in names:
            raise ValueError(f'invalid or duplicate candidate name: {name!r}')
        names.add(name)
        bench = candidate.get('bench')
        if not isinstance(bench, dict) or not bench:
            raise ValueError(f'{name}: bench must name workloads')
        if any(not isinstance(w, str) or not re.fullmatch(r'[A-Za-z0-9_-]+', w) for w in bench):
            raise ValueError(f'{name}: invalid workload name')
        if 'cwd' in candidate and not isinstance(candidate['cwd'], str):
            raise ValueError(f'{name}: cwd must be a directory string')
        if workloads is not None and set(bench) != workloads:
            raise ValueError('every candidate must benchmark the same workloads')
        workloads = set(bench)
        for command in [candidate.get('build'), candidate.get('check'), *bench.values()]:
            if not isinstance(command, list) or not command or any(not isinstance(a, str) for a in command):
                raise ValueError(f'{name}: commands must be nonempty argv lists')
        if 'code_bytes' in candidate and (type(candidate['code_bytes']) is not int or candidate['code_bytes'] < 0):
            raise ValueError(f'{name}: invalid code_bytes')
    if any('code_bytes' in c for c in candidates) and not all('code_bytes' in c for c in candidates):
        raise ValueError('code_bytes must be provided for every candidate or none')
    return candidates


def execute(command, cwd, log, timeout):
    # Write directly to disk: worker processes can inherit stdout, and a driver timeout must not wait on their pipes.
    with log.open('w') as output:
        with subprocess.Popen(command, cwd=cwd, text=True, stdout=output,
                              stderr=subprocess.STDOUT, start_new_session=os.name == 'posix') as process:
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # A compiler driver can leave workers behind. Stop our own group before any later measurement.
                if os.name == 'posix':
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    process.kill()
                process.wait()
                raise
    if process.returncode:
        raise RuntimeError(f'{command[0]} returned {process.returncode}: {log}')
    return log.read_text()


def prepare(candidate, root, timeout):
    name = candidate['name']
    try:
        for phase in ('build', 'check'):
            execute(candidate[phase], candidate.get('cwd'), root / f'{name}-{phase}.log', timeout)
        return name, {'valid': True}
    # A timeout aborts the entire search, including on systems where workers cannot be killed as one group.
    except (RuntimeError, OSError) as error:
        return name, {'valid': False, 'error': str(error)}


def measurement(output):
    try:
        row = json.loads(output.strip().splitlines()[-1])
        value = row['elapsed_ns']
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError('elapsed_ns must be positive and finite')
        if type(row['checksum']) not in (str, int):
            raise ValueError('checksum must be a string or integer')
        return row
    except (IndexError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise ValueError('benchmark must end with JSON containing elapsed_ns and checksum') from error


def summarize(observations):
    summary = {}
    for name, workloads in observations.items():
        summary[name] = {}
        for workload, samples in workloads.items():
            ns = [sample['elapsed_ns'] for sample in samples]
            median = statistics.median(ns)
            summary[name][workload] = {
                'median_ns': median, 'min_ns': min(ns), 'max_ns': max(ns),
                'relative_mad': statistics.median(abs(v - median) for v in ns) / median,
                'samples': samples,
            }
    return summary


def pareto(summary, sizes):
    def dimensions(name):
        values = [summary[name][w]['median_ns'] for w in sorted(summary[name])]
        return values + ([sizes[name]] if sizes else [])
    front = []
    for name in summary:
        values = dimensions(name)
        dominated = False
        for other in summary:
            if other == name:
                continue
            pairs = list(zip(dimensions(other), values))
            if all(a <= b for a, b in pairs) and any(a < b for a, b in pairs):
                dominated = True
                break
        if not dominated:
            front.append(name)
    return front


def speed_interval(log_ratios, samples, seed):
    """Paired bootstrap interval for the median log speedup; it describes this workload and these observations."""
    rng = random.Random(seed)
    estimates = sorted(statistics.median(rng.choices(log_ratios, k=len(log_ratios))) for _ in range(samples))
    return {'ratio': math.exp(statistics.median(log_ratios)),
            'bootstrap_95_interval': [math.exp(estimates[int(samples * .025)]),
                                      math.exp(estimates[min(samples - 1, int(samples * .975))])]}


def compare_candidate(job):
    name, baseline, candidate, weights, samples, seed = job
    ratios = {}
    columns = {}
    for index, workload in enumerate(sorted(baseline)):
        a, b = baseline[workload], candidate[workload]
        if len(a) != len(b) or len(a) < 5:
            raise ValueError('paired analysis needs equal counts of at least five observations')
        values = [math.log(x['elapsed_ns']) - math.log(y['elapsed_ns']) for x, y in zip(a, b)]
        columns[workload] = values
        ratios[workload] = speed_interval(values, samples, seed + index)
    total = sum(weights.values())
    aggregate = [sum(columns[w][i] * weights[w] / total for w in columns)
                 for i in range(len(next(iter(columns.values()))))]
    return name, {'workloads': ratios, 'weighted_geometric_speedup': speed_interval(aggregate, samples, seed + 65537)}


def analyze_candidates(observations, baseline, weights, samples, workers, seed):
    workloads = set(observations[baseline])
    if weights is None:
        weights = {workload: 1.0 for workload in sorted(workloads)}
    if not isinstance(weights, dict) or set(weights) != workloads or any(
            type(v) not in (int, float) or not math.isfinite(v) or v <= 0 for v in weights.values()):
        raise ValueError('workload_weights must give a positive finite weight for every workload')
    if not math.isfinite(sum(weights.values())):
        raise ValueError('workload weight sum is not finite')
    jobs = [(name, observations[baseline], candidate, weights, samples, seed + index * 131071)
            for index, (name, candidate) in enumerate(observations.items()) if name != baseline]
    workers = min(workers, len(available_cpus()), len(jobs))
    if workers > 1:
        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
            results = dict(pool.map(compare_candidate, jobs))
    else:
        results = dict(compare_candidate(job) for job in jobs)
    return {'baseline': baseline, 'weights': weights, 'bootstrap_samples': samples, 'analysis_processes': workers,
            'comparisons': results, 'limit': 'Paired per-comparison intervals, not a proof of a global optimum or game FPS.'}


def recommend(analysis, sizes, min_speedup, max_regression, max_bytes):
    eligible = []
    for name, row in analysis['comparisons'].items():
        if max_bytes is not None and sizes.get(name, max_bytes + 1) > max_bytes:
            continue
        if any(v['bootstrap_95_interval'][0] < 1 - max_regression for v in row['workloads'].values()):
            continue
        score = row['weighted_geometric_speedup']
        if score['bootstrap_95_interval'][0] >= 1 + min_speedup:
            eligible.append((score['ratio'], name))
    winner = max(eligible)[1] if eligible else analysis['baseline']
    if not eligible and max_bytes is not None and sizes.get(winner, max_bytes + 1) > max_bytes:
        winner = None
    return {'candidate': winner, 'min_speedup': min_speedup, 'max_regression': max_regression,
            'max_code_bytes': max_bytes,
            'reason': 'Best estimated score among candidates passing the supplied thresholds.' if eligible else
                      'No candidate cleared the thresholds; retain the reference if it fits the size budget.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=11)
    parser.add_argument('--workers', type=int, default=len(available_cpus()))
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('--seed', type=int, default=902)
    parser.add_argument('--baseline', help='reference candidate; defaults to the first valid candidate')
    parser.add_argument('--bootstrap-samples', type=int, default=4000)
    parser.add_argument('--min-speedup', type=float, default=.01, help='required lower-bound improvement, as a fraction')
    parser.add_argument('--max-regression', type=float, default=.02, help='allowed lower-bound workload loss, as a fraction')
    parser.add_argument('--max-code-bytes', type=int)
    args = parser.parse_args(argv)
    if args.rounds < 5 or args.workers < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('need at least five rounds, one worker, and a positive timeout')
    if not 1000 <= args.bootstrap_samples <= 100000 or not math.isfinite(args.min_speedup) or args.min_speedup < 0 or \
            not math.isfinite(args.max_regression) or not 0 <= args.max_regression < 1 or \
            (args.max_code_bytes is not None and args.max_code_bytes < 1):
        parser.error('invalid bootstrap count, speed/regression threshold or code-size budget')
    try:
        data = args.manifest.read_bytes()
        manifest = json.loads(data)
        candidates = validate(manifest)
        args.out.mkdir(parents=True, exist_ok=False)
        rows = {}
        # Parallel builds require separate working directories. Without them, serialize.
        directories = [str(Path(c.get('cwd', '.')).resolve()) for c in candidates]
        workers = args.workers if len(set(directories)) == len(directories) else 1
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(prepare, c, args.out, args.timeout) for c in candidates]
            for future in concurrent.futures.as_completed(futures):
                name, row = future.result()
                rows[name] = row
                print(name, 'exactness passed' if row['valid'] else row['error'], flush=True)
        (args.out / 'prepared.json').write_text(json.dumps(rows, indent=2))
        candidates = [c for c in candidates if rows[c['name']]['valid']]
        if not candidates:
            raise ValueError('no candidate passed its independent correctness check')
        baseline = args.baseline or candidates[0]['name']
        if baseline not in {c['name'] for c in candidates}:
            raise ValueError('the baseline candidate did not pass its correctness check or does not exist')
        if args.max_code_bytes is not None and any('code_bytes' not in c for c in candidates):
            raise ValueError('a code-size budget requires measured code_bytes for every candidate')
        observations = {c['name']: {w: [] for w in c['bench']} for c in candidates}
        checksums = {}
        rng = random.Random(args.seed)
        for round_index in range(-2, args.rounds):
            order = [(c, w) for c in candidates for w in c['bench']]
            rng.shuffle(order)
            for candidate, workload in order:
                name = candidate['name']
                log = args.out / f'{name}-{workload}-{round_index + 2}.log'
                output = execute(candidate['bench'][workload], candidate.get('cwd'), log, args.timeout)
                row = measurement(output)
                if workload in checksums and checksums[workload] != row['checksum']:
                    raise ValueError(f'benchmark checksum mismatch: {log}')
                checksums[workload] = row['checksum']
                if round_index >= 0:
                    observations[name][workload].append(row)
            # Preserve raw progress if a subsequent run fails or is interrupted.
            (args.out / 'observations.json').write_text(json.dumps(observations, indent=2))
            print(f'Round {round_index + 3}/{args.rounds + 2}', flush=True)
        summary = summarize(observations)
        sizes = {c['name']: c['code_bytes'] for c in candidates if 'code_bytes' in c}
        front = pareto(summary, sizes)
        analysis = analyze_candidates(observations, baseline, manifest.get('workload_weights'), args.bootstrap_samples,
                                      args.workers, args.seed)
        choice = recommend(analysis, sizes, args.min_speedup, args.max_regression, args.max_code_bytes)
        result = {'manifest': manifest, 'manifest_sha256': hashlib.sha256(data).hexdigest(),
                  'host': {'platform': platform.platform(), 'cpu': platform.processor(), 'affinity': available_cpus()},
                  'prepared': rows, 'rounds': args.rounds, 'summary': summary, 'code_bytes': sizes, 'pareto': front,
                  'analysis': analysis, 'recommendation': choice,
                  'claim': 'Best among tested exact candidates, not a global optimum. Median differences within variation are inconclusive.'}
        (args.out / 'results.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'pareto': front, 'recommendation': choice, 'report': str(args.out / 'results.json')}, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'cpu-tune: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
