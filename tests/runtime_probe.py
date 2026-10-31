#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scarletnx --probe, Recommended Settings' test of this computer, on the Vulkan device there is: every step's lines
in their order, each value of its kind. runtime_probe.py SCARLETNX"""

import re
import subprocess
import sys

STEPS = ['gpu', 'pipelines', 'memory', 'done']
INTS = {'gpu_bc', 'gpu_astc', 'gpu_heap_mb', 'gpu_budget_mb', 'pipelines_auto', 'cpus', 'cores', 'ram_available_mb'}


def main(scarletnx):
    run = subprocess.run([scarletnx, '--probe'], capture_output=True, text=True, timeout=120, check=False)
    assert run.returncode == 0, f'exit {run.returncode}: {run.stderr}'
    steps, keys = [], {}
    for line in run.stdout.splitlines():
        if line.startswith('probe: '):
            steps.append(line[7:])
            continue
        key, eq, value = line.partition('=')
        assert eq and re.fullmatch(r'[a-z0-9_]+', key), f'not key=value: {line!r}'
        assert key not in keys, f'{key} said twice'
        keys[key] = value
    assert steps == STEPS, f'steps {steps}, not {STEPS}'
    for key in INTS & keys.keys():
        assert re.fullmatch(r'\d+', keys[key]), f'{key}={keys[key]} is no whole number'
    assert 'gpu_error' not in keys, f'the device answered nothing: {keys["gpu_error"]}'
    assert keys['gpu'] and keys['gpu_type'] in ('discrete', 'integrated', 'other'), keys
    assert keys['gpu_bc'] in '01' and keys['gpu_astc'] in '01'
    assert keys['gpu_budget_from'] in ('driver', 'heap')
    assert 0 < int(keys['gpu_budget_mb']) <= int(keys['gpu_heap_mb']), (keys['gpu_budget_mb'], keys['gpu_heap_mb'])
    assert 'pipelines_error' not in keys, keys['pipelines_error']
    rates = [entry.split(':') for entry in keys['pipelines'].split(',')]
    assert rates and all(len(r) == 2 and int(r[0]) >= 1 and float(r[1]) > 0 for r in rates), keys['pipelines']
    threads = [int(r[0]) for r in rates]
    assert len(set(threads)) == len(threads), f'a number of threads tried twice: {keys["pipelines"]}'
    assert int(keys['pipelines_auto']) in threads, (keys['pipelines_auto'], keys['pipelines'])
    assert 1 <= int(keys['cores']) <= int(keys['cpus']), (keys['cores'], keys['cpus'])
    assert ('ram_available_mb' in keys) != ('ram_error' in keys), 'the memory step says neither or both'
    print(run.stdout, end='')
    print('runtime_probe passed')


if __name__ == '__main__':
    main(sys.argv[1])
