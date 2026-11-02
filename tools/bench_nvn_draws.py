#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare legal NVN storage-ordering strategies on identical synthetic GPU draws.

Each candidate must pass exact GPU-output checks before it can compete. Rendering quality stays fixed.
The result measures a specific draw workload, not game FPS. Run serially without a build or another game.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess

SAMPLE = re.compile(r'NVN_DRAW_BENCH ordering=(\d+) descriptors=(\d+) record=(\d+) draws=(\d+) rounds=(\d+) ns=(\d+) passes=(\d+) exact=(\d+)')


def parse_sample(text, candidate):
    if 'could not start the command recording thread' in text:
        raise ValueError('recording-thread fallback cannot compete')
    matches = list(SAMPLE.finditer(text))
    if len(matches) != 1:
        raise ValueError('expected exactly one benchmark result')
    ordering, descriptors, record, draws, rounds, ns, passes, exact = map(int, matches[0].groups())
    if (ordering, descriptors, record) != tuple(candidate):
        raise ValueError('the requested strategy was not active')
    if exact != 1 or draws != 256 or rounds != 8 or ns <= 0 or passes <= 0:
        raise ValueError('exact output checks or workload measurements are invalid')
    return {'ordering': ordering, 'descriptors': descriptors, 'record': record,
            'ns_per_draw': ns / (draws * rounds), 'passes_per_batch': passes / rounds,
            'draws': draws, 'rounds': rounds}


def run(binary, args, path):
    work = path.parent / (path.stem + '-work')
    work.mkdir()
    result = subprocess.run([str(binary), '-H', *args], cwd=work, env=dict(os.environ, TMPDIR=str(work)),
                            capture_output=True, text=True, timeout=120)
    text = result.stdout + result.stderr
    path.write_text(text)
    return result.returncode, text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/src/nx/snx-test-vulkan'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 3:
        parser.error('at least three paired rounds are required')
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error('build snx-test-vulkan first')
    args.out.mkdir(parents=True, exist_ok=False)
    report = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'scope': 'Fixed synthetic draws with exact output validation. Not game FPS.',
              'checks': [], 'runs': [], 'rejected': []}
    available = []
    for descriptors, record in itertools.product((0, 1), repeat=2):
        flags = [f'--vk_descriptors={descriptors}', f'--vk_record={record}']
        code, text = run(binary, flags, args.out / f'check-d{descriptors}-r{record}.log')
        if code == 77:
            report['rejected'].append({'descriptors': descriptors, 'record': record, 'reason': 'unsupported backend'})
            continue
        if code or 'PASS headless Vulkan' not in text:
            raise RuntimeError(f'exact renderer checks failed for descriptors={descriptors}, record={record}')
        available.extend((ordering, descriptors, record) for ordering in (0, 1, 2))
        report['checks'].append({'descriptors': descriptors, 'record': record, 'exact': True})
    if not available:
        raise RuntimeError("no renderer candidate passed exact checks")
    rng = random.Random(781)
    for round_number in range(args.rounds):
        order = list(available)
        rng.shuffle(order)
        for candidate in order:
            ordering, descriptors, record = candidate
            flags = [f'--nvn-draw-bench={ordering}', f'--vk_descriptors={descriptors}', f'--vk_record={record}']
            code, text = run(binary, flags, args.out / f'round{round_number + 1}-p{ordering}-d{descriptors}-r{record}.log')
            if code:
                raise RuntimeError(f'candidate {candidate} failed with {code}')
            row = parse_sample(text, candidate)
            row['round'] = round_number + 1
            report['runs'].append(row)
            print(f"round {round_number + 1} {candidate}: {row['ns_per_draw']:.1f} ns/draw, "
                  f"{row['passes_per_batch']:.1f} passes/batch", flush=True)
            (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    summaries = []
    for candidate in available:
        ordering, descriptors, record = candidate
        rows = [r for r in report['runs'] if (r['ordering'], r['descriptors'], r['record']) == candidate]
        baseline = [r for r in report['runs'] if (r['ordering'], r['descriptors'], r['record']) == (0, descriptors, record)]
        paired = all(row['ns_per_draw'] < base['ns_per_draw'] * .97 for row, base in zip(rows, baseline))
        summaries.append({'ordering': ordering, 'descriptors': descriptors, 'record': record,
                          'median_ns_per_draw': statistics.median(r['ns_per_draw'] for r in rows),
                          'median_passes': statistics.median(r['passes_per_batch'] for r in rows),
                          'beats_conservative_every_round': bool(ordering and paired)})
    qualified = [row for row in summaries if not row['ordering'] or row['beats_conservative_every_round']]
    winner = min(qualified, key=lambda row: row['median_ns_per_draw'])
    report['summaries'] = summaries
    report['winner'] = winner
    report['rule'] = 'Explicit ordering must beat its matching conservative baseline by 3% in every paired round.'
    (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.out / 'winner.json').write_text(json.dumps(winner, indent=2) + '\n')
    print('Synthetic winner: ' + json.dumps(winner), flush=True)


if __name__ == '__main__':
    main()
