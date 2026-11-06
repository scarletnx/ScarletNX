#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare sets, descriptor buffers and descriptor heaps on the real Vulkan driver.

Require exact renderer checks and verify the selected backend before timing.
Measure repeated and unique buffer offsets, scalar images and full indexed tails.
Frame retirement and recording completion are timed. Buffer creation is outside
those measurements. This measures CPU submission cost, not game FPS.

  tools/bench_gpu_descriptors.py --out /local/scratch/descriptors
"""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys

BACKENDS = {0: 'sets', 1: 'buffers', 2: 'heaps'}
COUNTERS = ('buffer_requests', 'buffer_driver_calls', 'buffer_hits', 'buffer_evictions',
            'image_requests', 'image_driver_calls', 'image_hits', 'image_replicas',
            'address_queries', 'full_layout_bytes', 'arena_bytes', 'payload_bytes',
            'frame_advances', 'arena_allocated_bytes', 'cache_allocated_bytes',
            'peak_used_bytes', 'workload_buffer_bytes', 'heap_table_bytes',
            'heap_image_lookups', 'heap_sampler_requests', 'heap_sampler_lookups', 'heap_sampler_replicas',
            'heap_sampler_driver_calls', 'heap_sampler_hits',
            'heap_reserved_bytes', 'heap_resource_range_bytes', 'heap_sampler_range_bytes')
MEMORY = ('arena_allocated_bytes', 'cache_allocated_bytes', 'peak_used_bytes',
          'workload_buffer_bytes', 'heap_reserved_bytes', 'heap_resource_range_bytes', 'heap_sampler_range_bytes')
DEVICE_LINE = re.compile(r'vulkan: (.+), Vulkan [0-9]+\.[0-9]+\.[0-9]+, driver [0-9a-fA-F]{8}, '
                         r'graphics queue family [0-9]+, compute [0-9]+(?: \([^\n]*\))?\Z')


class BinaryChangedError(RuntimeError):
    """A renderer gate and its measurements must use the same executable bytes."""


def binary_identity(path):
    before = path.stat()
    with path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    after = path.stat()
    fields = ('st_dev', 'st_ino', 'st_mtime_ns', 'st_size')
    if any(getattr(before, field) != getattr(after, field) for field in fields):
        raise BinaryChangedError('benchmark binary changed while hashing. Stop concurrent builds')
    return {'device': after.st_dev, 'inode': after.st_ino, 'mtime_ns': after.st_mtime_ns,
            'size': after.st_size, 'sha256': digest}


def unchanged(path, signature):
    if binary_identity(path) != signature:
        raise BinaryChangedError('benchmark binary changed. Stop concurrent builds')


def execute_checked(command, log, signature, timeout=180):
    """Check even a skipped or failed process, and the final process in a batch."""
    binary = Path(command[0])
    unchanged(binary, signature)
    try:
        return execute(command, log, timeout)
    finally:
        unchanged(binary, signature)


def device_name(text):
    # device.c prints this line before either the exact suite or a benchmark's early return.
    devices = [match[1] for line in text.splitlines() if (match := DEVICE_LINE.fullmatch(line))]
    if len(devices) > 1 or (devices and not devices[0]):
        raise ValueError('benchmark must identify one Vulkan device')
    return devices[0] if devices else None


def check_device(text, expected):
    device = device_name(text)
    if expected is not None and device != expected:
        raise ValueError('benchmark changed Vulkan devices or omitted its device name')
    return device


def execute(command, log, timeout=180):
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


def selected_backend(row):
    buffers, heaps = row.get('vk_descriptors'), row.get('vk_descriptor_heaps', 0)
    if type(buffers) is not int or buffers not in (0, 1) or type(heaps) is not int or heaps not in (0, 1):
        raise ValueError('descriptor backend flags must be integer booleans')
    return 2 if heaps else buffers


def renderer_gate(text, backend):
    if 'vulkan validation:' in text or 'failed: VK_ERROR' in text:
        raise ValueError('exact renderer reported a validation/API failure')
    if 'PASS headless Vulkan:' not in text:
        raise ValueError('exact renderer checks did not finish')
    selected = re.findall(r'graphics descriptors use (sets|buffers|heaps)\b', text)
    if len(selected) != 1 or selected[0] != BACKENDS[backend]:
        raise ValueError(f'exact renderer used a fallback instead of {BACKENDS[backend]}')
    return {'backend': backend, 'renderer_exact': True}


def identity(row, sets, count, backend, recording, images, indexed=0):
    for key, expected in [('sets', sets), ('descriptors', count), ('images', images),
                          ('vk_record', recording), ('indexed', indexed)]:
        actual = row.get(key, 0 if key == 'indexed' else None)
        if type(actual) is not int or actual != expected:
            raise ValueError(f'benchmark did not use requested {key}={expected}')
    if selected_backend(row) != backend or type(row.get('backend', backend)) is not int or row.get('backend', backend) != backend:
        raise ValueError(f'benchmark did not use requested backend={BACKENDS[backend]}')
    value = row.get('elapsed_ns')
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise ValueError('elapsed_ns must be positive and finite')


def measurement(text, sets, count, backend, recording, images=0):
    """Read the legacy repeated-only line; the current runner uses pattern_measurements."""
    lines = [line.removeprefix('DESCRIPTOR_BENCH ') for line in text.splitlines()
             if line.startswith('DESCRIPTOR_BENCH ')]
    if len(lines) != 1:
        raise ValueError('benchmark must produce exactly one measurement')
    row = json.loads(lines[0])
    identity(row, sets, count, backend, recording, images)
    return row


def pattern_measurements(text, sets, count, backend, recording, images=0, indexed=0):
    rows = [json.loads(line.removeprefix('DESCRIPTOR_PATTERN_BENCH ')) for line in text.splitlines()
            if line.startswith('DESCRIPTOR_PATTERN_BENCH ')]
    if len(rows) != 2 or {r.get('pattern') for r in rows} != {'repeated', 'unique'}:
        raise ValueError('benchmark must produce exactly one repeated and one unique measurement')
    for row in rows:
        identity(row, sets, count, backend, recording, images, indexed)
        if type(row.get('fallbacks')) is not int or row['fallbacks'] != 0:
            raise ValueError('timed benchmark used a fallback')
        if type(row.get('counters_available')) is not int or row['counters_available'] != int(backend != 0):
            raise ValueError('descriptor counter availability does not match backend')
        for key in COUNTERS:
            if type(row.get(key)) is not int or row[key] < 0:
                raise ValueError(f'{key} must be a nonnegative integer')
        if 'heap_buffer_native_calls' in row:
            calls = row['heap_buffer_native_calls']
            if type(calls) is not int or calls < 0 or (backend != 2 and calls):
                raise ValueError('heap buffer API calls do not match the selected backend')
        elements = images + (512 if indexed else 0)
        if backend:
            if row['buffer_requests'] != sets * count or row['image_requests'] != sets * elements:
                raise ValueError('descriptor request counters do not cover the complete workload')
            for group in ('buffer', 'image'):
                for field in ('driver_calls', 'hits'):
                    if row[f'{group}_{field}'] > row[f'{group}_requests']:
                        raise ValueError('descriptor calls/hits exceed requests')
            if row['buffer_driver_calls'] + row['buffer_hits'] != row['buffer_requests']:
                raise ValueError('buffer calls and hits do not account for every request')
            image_lookups = row['image_driver_calls'] + row['image_hits']
            if image_lookups + row['image_replicas'] != row['image_requests']:
                raise ValueError('image lookups and replicas do not account for every request')
            if not row['arena_allocated_bytes'] or not row['cache_allocated_bytes']:
                raise ValueError('descriptor memory allocation gauges are missing')
        if backend == 2:
            # Older baseline binaries omit this field; retain its absence instead of inventing a zero.
            if 'heap_buffer_native_calls' in row:
                calls = row['heap_buffer_native_calls']
                encodings = row['buffer_driver_calls']
                if type(calls) is not int or calls < 0 or not calls <= encodings <= 8 * calls:
                    raise ValueError('heap buffer API calls do not account for encoded elements in bounded batches')
            if row['heap_sampler_requests'] != sets * elements:
                raise ValueError('sampler request counter does not cover the heap workload')
            if row['heap_image_lookups'] != image_lookups:
                raise ValueError('heap image resolver calls do not match encoding calls and hits')
            if row['heap_sampler_lookups'] + row['heap_sampler_replicas'] != row['heap_sampler_requests']:
                raise ValueError('sampler lookups and replicas do not account for every request')
            # A byte-interning hit follows a driver call, so those counters can overlap for one lookup.
            if max(row['heap_sampler_hits'], row['heap_sampler_driver_calls']) > row['heap_sampler_lookups']:
                raise ValueError('sampler calls/hits exceed resolver lookups')
            ranges = row['heap_resource_range_bytes'] + row['heap_sampler_range_bytes']
            if not row['heap_resource_range_bytes'] or not row['heap_sampler_range_bytes'] or \
                    row['arena_allocated_bytes'] < ranges or row['heap_reserved_bytes'] >= ranges:
                raise ValueError('heap allocation does not contain the bound and reserved ranges')
    return rows


def rankings(rows, variants, rounds):
    """Pair the same workload and round; do not rank variants with missing observations."""
    workloads = sorted({(r['pattern'], r['descriptors'], r['images'], r.get('indexed', 0)) for r in rows})
    result = []
    for pattern, count, images, indexed in workloads:
        samples = {}
        for variant in variants:
            observed = [r for r in rows if (selected_backend(r), r['vk_record']) == variant and
                        (r['pattern'], r['descriptors'], r['images'], r.get('indexed', 0)) == (pattern, count, images, indexed)]
            if len(observed) == rounds and {r['round'] for r in observed} == set(range(rounds)):
                samples[variant] = {r['round']: r for r in observed}
        if not samples:
            continue
        reference = (0, 0) if (0, 0) in samples else min(samples)
        ranked = []
        for variant, batches in samples.items():
            values = [r['elapsed_ns'] / r['sets'] for r in batches.values()]
            speedups = [samples[reference][n]['elapsed_ns'] / batches[n]['elapsed_ns'] for n in range(rounds)]
            ranked.append({'backend': variant[0], 'backend_name': BACKENDS[variant[0]], 'vk_record': variant[1],
                           'median_ns_per_table': statistics.median(values),
                           'paired_speedup_median': statistics.median(speedups),
                           'frame_advances_median': statistics.median(r['frame_advances'] for r in batches.values()),
                           'memory_bytes': {key: max(r[key] for r in batches.values()) for key in MEMORY},
                           'counter_medians': {key: statistics.median(r[key] for r in batches.values())
                                               for key in COUNTERS if key not in MEMORY}})
        ranked.sort(key=lambda item: item['median_ns_per_table'])
        result.append({'workload': {'pattern': pattern, 'descriptors': count, 'images': images, 'indexed': indexed},
                       'paired_reference': {'backend': reference[0], 'vk_record': reference[1]}, 'ranking': ranked})
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bench', type=Path, default=Path('build/src/nx/snx-test-vulkan'))
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--sets', type=int, default=4096)
    ap.add_argument('--counts', type=int, nargs='+', default=[8, 32])
    ap.add_argument('--images', type=int, nargs='+', default=[0, 8])
    ap.add_argument('--indexed', type=int, nargs='+', default=[0, 1, 16])
    ap.add_argument('--backends', type=int, nargs='+', choices=tuple(BACKENDS), default=[0, 1, 2])
    a = ap.parse_args()
    if a.rounds < 3 or not 1 <= a.sets <= 16384 or any(not 1 <= n <= 34 for n in a.counts) or \
            any(not 0 <= n <= 16 for n in a.images) or any(not 0 <= n <= 256 for n in a.indexed):
        ap.error('require at least3 rounds,1..16384 sets,1..34 buffers,0..16 images and0..256 indexed handles')
    bench = a.bench.resolve()
    if not bench.is_file():
        ap.error('build the snx-test-vulkan target first')
    a.out.mkdir(parents=True, exist_ok=True)
    if (a.out / 'results.json').exists():
        ap.error('output already contains results. Choose a new directory')
    signature = binary_identity(bench)
    metadata = {'executable': str(bench), 'sha256': signature['sha256'], 'binary_identity': signature,
                'exact_renderer': []}
    variants, rejected, rows = [], [], []
    for backend, recording in itertools.product(dict.fromkeys(a.backends), (0, 1)):
        flags = [f'--vk_descriptors={backend}', f'--vk_record={recording}']
        name = f'{BACKENDS[backend]}-record{recording}'
        print('Exact renderer checks: ' + name, flush=True)
        try:
            text = execute_checked([str(bench), '-H', *flags], a.out / f'{name}-check.log', signature)
            if text is None:
                raise ValueError('unsupported backend')
            gate = renderer_gate(text, backend)
            gate['vk_record'] = recording
            device = check_device(text, metadata.get('device'))
            if device:
                metadata['device'] = device
                gate['device'] = device
            metadata['exact_renderer'].append(gate)
            variants.append((backend, recording))
        except BinaryChangedError:
            raise
        except (RuntimeError, ValueError) as error:
            rejected.append({'variant': name, 'reason': str(error)})
    if not variants:
        raise RuntimeError('no backend passed the exact renderer checks')
    rng = random.Random(142)
    for repeat in range(a.rounds):
        work = list(itertools.product(variants, dict.fromkeys(a.counts), dict.fromkeys(a.images), dict.fromkeys(a.indexed)))
        rng.shuffle(work)
        for (backend, recording), count, images, indexed in work:
            name = f'{BACKENDS[backend]}-record{recording}-d{count}-i{images}-a{indexed}-r{repeat}'
            command = [str(bench), '-H', f'--vk_descriptors={backend}', f'--vk_record={recording}',
                       f'--bench-descriptors={a.sets}', f'--descriptor-count={count}', f'--descriptor-images={images}',
                       f'--descriptor-indexed={indexed}']
            try:
                text = execute_checked(command, a.out / (name + '.log'), signature)
                if text is None:
                    raise ValueError('a previously verified backend became unavailable')
                check_device(text, metadata.get('device'))
                batch = pattern_measurements(text, a.sets, count, backend, recording, images, indexed)
                for row in batch:
                    row['round'] = repeat
                    rows.append(row)
                    print(f'{name}/{row["pattern"]}: {row["elapsed_ns"] / a.sets:.1f} ns/table, '
                          f'{row["frame_advances"]} retirements', flush=True)
            except BinaryChangedError:
                raise
            except (RuntimeError, ValueError) as error:
                rejected.append({'variant': name, 'reason': str(error)})
            (a.out / 'results.json').write_text(json.dumps({'metadata': metadata, 'measurements': rows, 'rejected': rejected}, indent=2))
    unchanged(bench, signature)
    ranking = rankings(rows, variants, a.rounds)
    if not ranking:
        raise RuntimeError('no workload has complete paired measurements')
    result = {'metadata': metadata, 'workloads': ranking, 'rejected': rejected,
              'scope': 'CPU descriptor write/bind and retirement on synthetic data; buffer creation is excluded.'}
    (a.out / 'winner.json').write_text(json.dumps(result, indent=2))
    print('Paired workload rankings and memory counters: ' + str(a.out / 'winner.json'), flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, ValueError, KeyboardInterrupt) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
