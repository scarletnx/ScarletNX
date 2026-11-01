#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare descriptor strategies on deterministic synthetic data and the real Vulkan driver.

First run exact renderer checks for every backend/recording combination. Then
measure descriptor creation, update and command recording in shuffled serial
rounds. This measures CPU submission cost, not game FPS. No settings are applied.

  tools/bench_gpu_descriptors.py --out /local/scratch/descriptors
"""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys


def execute(command, log, timeout=90):
    with log.open('w') as stream:
        try:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f'timed out: {log}') from error
    text = log.read_text(errors='replace')
    if result.returncode == 77:
        return None
    if result.returncode or 'vulkan validation:' in text or 'failed: VK_ERROR' in text:
        raise RuntimeError(f'failed with exit {result.returncode}: {log}')
    return text


def measurement(text, sets, count, backend, recording, images=0):
    lines = [line[len('DESCRIPTOR_BENCH '):] for line in text.splitlines() if line.startswith('DESCRIPTOR_BENCH ')]
    if len(lines) != 1:
        raise ValueError('benchmark must produce exactly one measurement')
    row = json.loads(lines[0])
    for key, expected in [('sets', sets), ('descriptors', count), ('images', images), ('vk_descriptors', backend), ('vk_record', recording)]:
        if row.get(key) != expected:
            raise ValueError(f'benchmark did not use requested {key}={expected}')
    value = row.get('elapsed_ns')
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise ValueError('elapsed_ns must be positive and finite')
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bench', type=Path, default=Path('build/src/nx/snx-test-vulkan'))
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--sets', type=int, default=4096)
    ap.add_argument('--counts', type=int, nargs='+', default=[8, 32])
    ap.add_argument('--images', type=int, nargs='+', default=[0, 8])
    a = ap.parse_args()
    if a.rounds < 3 or not 1 <= a.sets <= 16384 or not a.counts or any(not 1 <= n <= 34 for n in a.counts) or not a.images or any(not 0 <= n <= 16 for n in a.images):
        ap.error('require at least 3 rounds, 1..16384 sets, 1..34 buffer descriptors and 0..16 image descriptors')
    bench = a.bench.resolve()
    if not bench.is_file():
        ap.error('build the snx-test-vulkan target first')
    a.out.mkdir(parents=True, exist_ok=True)
    if (a.out / 'results.json').exists():
        ap.error('output already contains results. Choose a new directory')
    signature = (bench.stat().st_ino, bench.stat().st_mtime_ns, bench.stat().st_size)
    metadata = {'executable': str(bench), 'sha256': hashlib.sha256(bench.read_bytes()).hexdigest()}
    variants, rejected, rows = [], [], []
    for backend, recording in itertools.product((0, 1), repeat=2):
        flags = [f'--vk_descriptors={backend}', f'--vk_record={recording}']
        name = f'buffers{backend}-record{recording}'
        print('Exact renderer checks: ' + name, flush=True)
        try:
            text = execute([str(bench), '-H', *flags], a.out / f'{name}-check.log')
            if text is None:
                rejected.append({'variant': name, 'reason': 'unsupported'})
            elif 'PASS headless Vulkan:' not in text:
                rejected.append({'variant': name, 'reason': 'renderer checks did not finish'})
            else:
                device = next((line.split(': ', 1)[1] for line in text.splitlines()
                               if line.startswith('Headless Vulkan device: ')), None)
                if device:
                    metadata['device'] = device
                variants.append((backend, recording))
        except RuntimeError as error:
            rejected.append({'variant': name, 'reason': str(error)})
    if not variants:
        raise RuntimeError('no backend passed the exact renderer checks')
    rng = random.Random(142)
    for repeat in range(a.rounds):
        work = list(itertools.product(variants, a.counts, a.images))
        rng.shuffle(work)
        for (backend, recording), count, images in work:
            if (bench.stat().st_ino, bench.stat().st_mtime_ns, bench.stat().st_size) != signature:
                raise RuntimeError('benchmark binary changed during measurement. Stop concurrent builds')
            name = f'buffers{backend}-record{recording}-d{count}-i{images}-r{repeat}'
            command = [str(bench), '-H', f'--vk_descriptors={backend}', f'--vk_record={recording}',
                       f'--bench-descriptors={a.sets}', f'--descriptor-count={count}', f'--descriptor-images={images}']
            text = execute(command, a.out / (name + '.log'))
            if text is None:
                raise RuntimeError('a previously verified backend became unavailable')
            row = measurement(text, a.sets, count, backend, recording, images)
            row['round'] = repeat
            rows.append(row)
            print(f'{name}: {row["elapsed_ns"] / a.sets:.1f} ns/table', flush=True)
            (a.out / 'results.json').write_text(json.dumps({'metadata': metadata, 'measurements': rows, 'rejected': rejected}, indent=2))
    ranking = []
    for backend, recording in variants:
        medians = {f'{n} buffers + {images} images': statistics.median(r['elapsed_ns'] / a.sets for r in rows
                   if r['vk_descriptors'] == backend and r['vk_record'] == recording and
                   r['descriptors'] == n and r['images'] == images)
                   for n, images in itertools.product(a.counts, a.images)}
        score = math.exp(statistics.mean(math.log(v) for v in medians.values()))
        ranking.append({'vk_descriptors': backend, 'vk_record': recording,
                        'ns_per_table': medians, 'score': score})
    ranking.sort(key=lambda row: row['score'])
    result = {'metadata': metadata, 'fastest_measured': ranking[0], 'ranking': ranking, 'rejected': rejected,
              'scope': 'CPU descriptor submission on synthetic data. Not a game FPS prediction.'}
    (a.out / 'winner.json').write_text(json.dumps(result, indent=2))
    print('\nFastest measured CPU descriptor path:', ranking[0], flush=True)
    print('Exact rendering checks passed. Results: ' + str(a.out), flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, ValueError, KeyboardInterrupt) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
