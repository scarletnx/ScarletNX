#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare exact synthetic GPU preparation workloads in serial paired rounds.

No game or CUDA is required. This measures a named CPU workload, not game FPS.
Build immutable baseline and candidate executables first. The script does not
build, install packages, alter settings, or select one strategy for all games.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PREFIX = {'inline': 'NVN_INLINE_BENCH', 'indexed': 'INDEXED_HANDLES_BENCH',
          'hotpath': 'GPU_HOTPATH_BENCH'}
NAME = re.compile(r'[A-Za-z0-9_.-]+\Z')
DIMENSIONS = ('bytes', 'results', 'stages', 'range_bytes')
SCOPE = ('Synthetic CPU preparation workloads with exact-output gates. '
         'Results are specific to the measured workload and machine, not game FPS or a universal fastest strategy.')


def identity(path):
    """A content hash also catches in-place rewrites which preserve size or timestamps."""
    stat = path.stat()
    with path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    return {'device': stat.st_dev, 'inode': stat.st_ino, 'size': stat.st_size,
            'mtime_ns': stat.st_mtime_ns, 'sha256': digest}


def unchanged(binary):
    if identity(binary['path']) != binary['identity']:
        raise RuntimeError(f"benchmark binary changed: {binary['path']}; stop builds during measurement")


def parse_samples(text, kind, indexed_limit=8, dimensions=()):
    """Accept only complete, exact, positive-time records for a fixed workload."""
    samples = {}
    for line in text.splitlines():
        if not line.startswith(PREFIX[kind] + ' '):
            continue
        fields = {}
        for token in line.split()[1:]:
            key, separator, value = token.partition('=')
            if not separator or not NAME.fullmatch(key) or key in fields:
                raise ValueError('malformed or duplicate benchmark field')
            fields[key] = value
        try:
            words = {'parameter'} if kind == 'inline' else {'workload'} if kind == 'hotpath' else set()
            numbers = {key: int(value) for key, value in fields.items() if key not in words}
            if numbers['exact'] != 1 or numbers['ns'] <= 0:
                raise ValueError('exact=1 and positive ns are required')
            if kind == 'inline':
                name = fields['parameter']
                operations = numbers['draws']
                shape = {'draws': operations}
                workload = 'inline-' + name
            elif kind == 'indexed':
                unique, tables, lookups = (numbers[key] for key in ('unique', 'tables', 'lookups'))
                if min(unique, tables, lookups) < 1 or unique > lookups:
                    raise ValueError('invalid indexed workload dimensions')
                operations = tables * lookups
                shape = {'unique': unique, 'tables': tables, 'lookups': lookups}
                workload = f'indexed-{unique}-keys'
                name = workload
                if numbers['linear_limit'] != indexed_limit:
                    continue
            else:
                name = fields['workload']
                workload = name
                operations = numbers['operations']
                shape = {'operations': operations,
                         **{key: numbers[key] for key in DIMENSIONS if key in numbers}}
                for key in dimensions:
                    shape[key] = numbers[key]
            if not NAME.fullmatch(name) or operations < 1:
                raise ValueError('invalid workload name or operation count')
            if any(value < 0 for key, value in numbers.items() if key != 'linear_limit'):
                raise ValueError('negative benchmark counter')
            if numbers['ns'] > 0xffffffffffffffff or operations > 0xffffffffffffffff:
                raise ValueError('benchmark time and operation count must fit uint64')
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f'invalid {PREFIX[kind]} record: {error}') from error
        if workload in samples:
            raise ValueError('duplicate workload result: ' + workload)
        samples[workload] = {'workload': workload, 'operations': operations, 'shape': shape,
                             'elapsed_ns': numbers['ns'], 'ns_per_operation': numbers['ns'] / operations,
                             'exact': True, 'counters': {key: value for key, value in numbers.items()
                                                         if key not in ('exact', 'ns')}}
    if not samples:
        raise ValueError('no exact measurements for the requested benchmark strategy')
    return samples


def validate_shapes(reference, samples):
    if samples.keys() != reference.keys():
        raise ValueError('workload set changed between runs or candidates')
    for name, sample in samples.items():
        if sample['shape'] != reference[name]['shape']:
            raise ValueError('workload dimensions changed: ' + name)


def summarize(rows, labels, rounds):
    """Rank each workload independently; use matching rounds for the paired ratio."""
    workloads = sorted({row['workload'] for row in rows})
    results = []
    for workload in workloads:
        baseline = {row['round']: row for row in rows
                    if row['label'] == 'baseline' and row['workload'] == workload}
        if set(baseline) != set(range(rounds)):
            raise ValueError('baseline rounds are incomplete')
        ranking = []
        for label in labels:
            samples = {row['round']: row for row in rows
                       if row['label'] == label and row['workload'] == workload}
            if set(samples) != set(baseline):
                raise ValueError('candidate rounds are incomplete')
            times = [samples[r]['ns_per_operation'] for r in range(rounds)]
            ratios = [baseline[r]['ns_per_operation'] / samples[r]['ns_per_operation'] for r in range(rounds)]
            median = statistics.median(times)
            base_median = statistics.median(row['ns_per_operation'] for row in baseline.values())
            counters = {key: statistics.median(row['counters'][key] for row in samples.values())
                        for key in samples[0]['counters']}
            ranking.append({'label': label, 'median_ns_per_operation': median,
                             'min_ns_per_operation': min(times), 'max_ns_per_operation': max(times),
                             'time_reduction_percent': 100 * (1 - median / base_median),
                             'median_paired_speedup': statistics.median(ratios),
                             'faster_paired_rounds': sum(value > 1 for value in ratios),
                             'median_counters': counters})
        ranking.sort(key=lambda result: result['median_ns_per_operation'])
        results.append({'workload': workload, 'shape': baseline[0]['shape'], 'ranking': ranking,
                        'fastest_measured': ranking[0]['label']})
    return results


def execute(binary, arguments, directory, timeout):
    unchanged(binary)
    directory.mkdir()
    log = directory / 'run.log'
    command = [str(binary['path']), *arguments]
    with log.open('w', encoding='utf-8') as stream:
        try:
            completed = subprocess.run(command, cwd=directory, env=dict(os.environ, TMPDIR=str(directory)),
                                       stdout=stream, stderr=subprocess.STDOUT, timeout=timeout, check=False)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f'benchmark timed out; inspect {log}') from error
    unchanged(binary)
    text = log.read_text(encoding='utf-8', errors='replace')
    if completed.returncode or 'vulkan validation:' in text or 'failed: VK_ERROR' in text:
        raise RuntimeError(f'benchmark failed with exit {completed.returncode}; inspect {log}')
    return text, command, str(log)


def write_report(out, report):
    (out / 'results.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    lines = ['# Synthetic GPU preparation comparison', '', SCOPE, '', 'Status: ' + report['status'], '',
             '| Workload | Strategy | Median ns/operation | Time reduction | Faster paired rounds |',
             '| --- | --- | ---: | ---: | ---: |']
    for workload in report.get('summaries', []):
        for row in workload['ranking']:
            lines.append(f"| {workload['workload']} | {row['label']} | {row['median_ns_per_operation']:.3f} | "
                         f"{row['time_reduction_percent']:.1f}% | {row['faster_paired_rounds']}/{report['rounds']} |")
    lines.extend(['', 'Each executable must check exact output. Workloads and operation counts must match.',
                  'Rounds run serially in a seeded shuffled order; do not build or run other timing jobs meanwhile.',
                  'Binary hashes are checked before and after each process. Shared libraries are not hashed.',
                  'This script does not change emulator settings or apply a winner.'])
    if 'error' in report:
        lines.extend(['', 'Failed: ' + report['error']])
    (out / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', action='append', required=True, metavar='NAME=PATH')
    parser.add_argument('--kind', choices=tuple(PREFIX), default='hotpath')
    parser.add_argument('--arg', action='append', dest='arguments', help='common executable argument; default: --bench')
    parser.add_argument('--variant-arg', action='append', default=[], metavar='NAME=ARG')
    parser.add_argument('--indexed-limit', type=int, default=8)
    parser.add_argument('--dimension', action='append', default=[], metavar='KEY',
                        help='additional immutable generic input field; bytes/results/stages/range_bytes are automatic')
    parser.add_argument('--rounds', type=int, default=7)
    parser.add_argument('--seed', type=int, default=539)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--out', type=Path, default=ROOT / '.cache/gpu-hotpaths')
    parser.add_argument('--index', type=Path, default=ROOT / '.cache/nvn-gpu-index/index.json')
    args = parser.parse_args(argv)
    if not 3 <= args.rounds <= 99 or not 1 <= args.timeout <= 3600 or not 0 <= args.indexed_limit <= 256:
        parser.error('require 3..99 rounds, 1..3600 timeout seconds, and indexed limit 0..256')
    if any(not NAME.fullmatch(key) or key in ('ns', 'exact') for key in args.dimension):
        parser.error('dimensions must name benchmark input fields')
    binaries = {'baseline': {'path': args.baseline.resolve(), 'arguments': []}}
    for item in args.candidate:
        name, separator, path = item.partition('=')
        if not separator or not NAME.fullmatch(name) or name in binaries or not path:
            parser.error('candidates require distinct NAME=PATH entries; baseline is reserved')
        binaries[name] = {'path': Path(path).resolve(), 'arguments': []}
    for item in args.variant_arg:
        name, separator, argument = item.partition('=')
        if not separator or name not in binaries or not argument:
            parser.error('--variant-arg must name an existing strategy and a nonempty argument')
        binaries[name]['arguments'].append(argument)
    for binary in binaries.values():
        if not binary['path'].is_file() or not os.access(binary['path'], os.X_OK):
            parser.error('benchmark executable does not exist or is not executable: ' + str(binary['path']))
        binary['identity'] = identity(binary['path'])
    out = args.out.resolve()
    if out.exists():
        parser.error('output already exists; choose a new directory')
    out.mkdir(parents=True)
    report = {'format_version': 1, 'scope': SCOPE, 'status': 'running', 'rounds': args.rounds, 'seed': args.seed,
              'kind': args.kind, 'python': platform.python_version(), 'platform': platform.platform(),
              'binaries': {name: {**binary, 'path': str(binary['path'])} for name, binary in binaries.items()},
              'measurements': [], 'orders': []}
    if args.index.is_file():
        index = json.loads(args.index.read_text(encoding='utf-8'))
        report['reference_index'] = {'path': str(args.index.resolve()), 'sha256': identity(args.index)['sha256'],
                                     'api_coverage': index.get('api_coverage'), 'scope': 'Static provenance only.'}
        if index.get('classification') == 'static_contract_and_api_evidence':
            report['reference_index']['api_inventory_counts'] = index.get('api_inventory', {}).get('counts', {})
            report['reference_index']['contract_counts'] = index.get('counts', {})
    rng, reference = random.Random(args.seed), None
    try:
        for round_number in range(args.rounds):
            order = list(binaries)
            rng.shuffle(order)
            report['orders'].append(order)
            for name in order:
                binary = binaries[name]
                arguments = list(args.arguments if args.arguments is not None else ['--bench'])
                if args.kind == 'indexed':
                    arguments.append(str((args.seed + round_number) & 0xffffffff))
                arguments.extend(binary['arguments'])
                directory = out / f'round{round_number + 1}-{name}'
                text, command, log = execute(binary, arguments, directory, args.timeout)
                limit = -1 if name == 'baseline' else args.indexed_limit
                samples = parse_samples(text, args.kind, limit, args.dimension)
                if reference is None:
                    reference = samples
                validate_shapes(reference, samples)
                for sample in samples.values():
                    report['measurements'].append({**sample, 'label': name, 'round': round_number,
                                                    'command': command, 'log': log})
                write_report(out, report)
                print(f'round {round_number + 1}: {name}, {len(samples)} exact workloads', flush=True)
        for binary in binaries.values():
            unchanged(binary)
        report['summaries'] = summarize(report['measurements'], binaries, args.rounds)
        report['status'] = 'complete'
        write_report(out, report)
    except (RuntimeError, ValueError, OSError, KeyboardInterrupt) as error:
        report.update(status='failed', error=str(error))
        write_report(out, report)
        raise
    print('Workload-specific results: ' + str(out / 'report.md'), flush=True)
    return report


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, ValueError, OSError, KeyboardInterrupt) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
