#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compare exact captured pipeline compilation with and without optimization in fresh headless-test processes.

Run while no game is active. Inputs come from prepare_pipeline_probe.py. Each case/flag/round starts a new process,
requests disabled NVIDIA/Mesa disk caches and uses an empty application pipeline cache. This measures CPU/driver
compile latency only: no draws are submitted and rendering equivalence is NOT established. Results never enable a
flag or modify runtime settings, caches or saves. Example:
  python3 tools/bench_pipeline_compile.py --binary build/src/nx/snx-test-vulkan \
      --manifest /private/pipeline-probes/manifest.json --rounds 3 --out /tmp/pipeline-compile-ab
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys

SAMPLE = re.compile(r'^PIPELINE_COMPILE_PROBE (\{[^\r\n]+\})$', re.MULTILINE)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1 << 20), b''):
            digest.update(data)
    return digest.hexdigest()


def parse(text, disable, words):
    matches = SAMPLE.findall(text)
    if len(matches) != 1:
        raise ValueError('expected exactly one compile probe result')
    row = json.loads(matches[0])
    if (row.get('disable_optimization') != disable or row.get('shader_words') != words or
        row.get('empty_application_cache') != 1 or row.get('rendered') != 0 or row.get('validation_messages') != 0 or
        row.get('application_cache_hits') != 0 or type(row.get('ns')) is not int or row['ns'] <= 0):
        raise ValueError('compile flags, input, cache or validation result does not match the requested experiment')
    if any(type(row.get(k)) is not int for k in ('vendor', 'device', 'driver')):
        raise ValueError('compile probe has no device/driver metadata')
    return row


def summarize(runs):
    rows = []
    for index in sorted({r['index'] for r in runs}):
        case = [r for r in runs if r['index'] == index]
        ratios = []
        for number in sorted({r['round'] for r in case}):
            pair = {r['disable_optimization']: r for r in case if r['round'] == number}
            if set(pair) != {0, 1}:
                raise ValueError('incomplete compile-flag pair')
            ratios.append(pair[0]['ns'] / pair[1]['ns'])
        rows.append({'index': index, 'paired_normal_over_disabled': ratios,
                     'median_normal_over_disabled': statistics.median(ratios),
                     'disabled_at_least_3_percent_faster_every_pair': all(r > 1.03 for r in ratios),
                     'normal_median_ns': statistics.median(r['ns'] for r in case if not r['disable_optimization']),
                     'disabled_median_ns': statistics.median(r['ns'] for r in case if r['disable_optimization'])})
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args(argv)
    if args.rounds < 3 or not 0 < args.timeout < float('inf'):
        parser.error('at least three paired rounds and a finite positive timeout are required')
    binary = args.binary.resolve(strict=True)
    manifest = json.loads(args.manifest.read_text())
    packages = manifest['packages']
    if not packages:
        parser.error('manifest has no pipeline packages')
    for package in packages:
        path = Path(package['path'])
        if sha256(path) != package['sha256']:
            parser.error(f'pipeline package hash changed: {path}')
    output = args.out.resolve()
    report = {'scope': 'Captured pipeline compile latency only. No rendering-equivalence claim or gameplay change.',
              'binary_sha256': sha256(binary), 'manifest_sha256': sha256(args.manifest), 'runs': [],
              'cache_policy': 'Fresh processes and empty application caches; requested __GL_SHADER_DISK_CACHE=0 '
                              'and MESA_SHADER_CACHE_DISABLE=true. Driver-internal behavior is not independently verified.',
              'eligible_for_gameplay': False}
    try:
        output.mkdir(parents=True, exist_ok=False, mode=0o700)
    except OSError as error:
        print(f'compile experiment rejected: {error}', file=sys.stderr)
        return 2
    rng = random.Random(781)
    try:
        for number in range(1, args.rounds + 1):
            order = list(packages)
            rng.shuffle(order)
            for package in order:
                modes = [0, 1]
                rng.shuffle(modes)
                for disable in modes:
                    work = output / f'round{number}-pipeline{package["index"]}-disable{disable}'
                    work.mkdir()
                    cache, temporary = work / 'driver-cache', work / 'tmp'
                    cache.mkdir(); temporary.mkdir()
                    env = dict(os.environ, __GL_SHADER_DISK_CACHE='0', MESA_SHADER_CACHE_DISABLE='true',
                               XDG_CACHE_HOME=str(cache), TMPDIR=str(temporary), TEMP=str(temporary), TMP=str(temporary))
                    command = [str(binary), '-H', '--vk_descriptors_mode=3', f'--pipeline-probe={package["path"]}',
                               f'--pipeline-probe-disable-opt={disable}']
                    with (work / 'stdout.log').open('w') as stdout, (work / 'stderr.log').open('w') as stderr:
                        result = subprocess.run(command, cwd=work, env=env, stdout=stdout, stderr=stderr,
                                                timeout=args.timeout)
                    if result.returncode:
                        raise ValueError(f'pipeline{package["index"]} disable{disable} exited {result.returncode}; see {work}')
                    text = (work / 'stdout.log').read_text() + (work / 'stderr.log').read_text()
                    row = parse(text, disable, package['spv_words'])
                    if sha256(binary) != report['binary_sha256'] or sha256(Path(package['path'])) != package['sha256']:
                        raise ValueError('probe binary or package changed during the experiment')
                    if report['runs'] and any(row[k] != report['runs'][0][k] for k in ('vendor', 'device', 'driver')):
                        raise ValueError('device/driver changed between probe processes')
                    row.update({'round': number, 'index': package['index'], 'directory': str(work)})
                    report['runs'].append(row)
                    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
                    print(f'pipeline{package["index"]} round{number} disable{disable}: {row["ns"] / 1e6:.2f}ms', flush=True)
        report['summary'] = summarize(report['runs'])
        report['status'] = 'complete'
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report['summary'], indent=2))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report.update({'status': 'rejected', 'reason': str(error)})
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'compile experiment rejected: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
