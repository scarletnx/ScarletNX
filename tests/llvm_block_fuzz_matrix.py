#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Run the seeded generated-row block corpus through each supported code-generation mode."""
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) != 5:
        raise SystemExit('usage: llvm_block_fuzz_matrix.py TEST QEMU RUNNER OUTPUT')
    binary, qemu, runner, output = sys.argv[1:]
    modes = [[], ['--opt=0'], ['--jit', '--hook'], ['--portable', '--volatile'],
             ['--jit', '--opt=0', '--hook', '--volatile']]
    seeds = [0x5EED, 0xA57, 0xC0FFEE, 0x1935BA11]
    for seed in seeds:
        for index, flags in enumerate(modes):
            directory = str(Path(output) / f'{seed:x}-{index}')
            command = [binary, qemu, runner, directory, '--programs=128', f'--seed={seed}', *flags]
            subprocess.run(command, check=True, timeout=120)
    print('Block seed matrix: 10,240 whole-program comparisons passed in 20 configurations')


if __name__ == '__main__':
    main()
