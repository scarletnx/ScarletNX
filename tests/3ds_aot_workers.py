#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The static ARM11 compile on one machine's worker threads, again and again: every chunk is built for the memory
model of the host, so the module links. A chunk built for the checked model on a host that is flat calls a stack probe
the module does not define, and the link fails."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

RUNS = 10
BRANCHES = 30000

spec = importlib.util.spec_from_file_location('ctr_aot', Path(__file__).with_name('3ds_aot.py'))
ctr_aot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctr_aot)


def main():
    driver = str(Path(sys.argv[1]).absolute())
    # a block for every branch: far more chunks than the host has cores, so every core starts a worker. The words are
    # 0xEA000000, B +8 (a branch to the word two after it), 0xE3A0002A, MOV r0, #42, and 0xEF000003, SVC #3
    blob = ctr_aot.application([0xEA000000] * BRANCHES + [0xE3A0002A, 0xEF000003])
    with tempfile.TemporaryDirectory(prefix='snx-ctr-aot-workers-') as temp:
        source = Path(temp) / 'game.cia'
        source.write_bytes(blob)
        for run in range(RUNS):
            output = Path(temp) / f'out {run}'
            p = subprocess.run([driver, '-r', '--output', str(output), str(source)], text=True, capture_output=True,
                               timeout=120)
            if p.returncode != 0:
                sys.exit(f'3ds_aot_workers: run {run} exited with {p.returncode}\n{p.stdout}{p.stderr}')
            if not ctr_aot.module_of(output).is_file():
                sys.exit(f'3ds_aot_workers: run {run} wrote no module\n{p.stdout}{p.stderr}')
    print(f'3ds_aot_workers: ok, {RUNS} compiles')


if __name__ == '__main__':
    main()
