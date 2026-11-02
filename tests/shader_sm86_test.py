# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""shader_sm86's translations through spirv-val: the test writes each module it makes into a folder of its own
(sm86_<name>.spv), and each one goes to the validator as an argument, as shader_direct_test.py hands its fixtures."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--test', required=True)
    parser.add_argument('--validator', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-sm86-') as directory:
        subprocess.run([args.test, directory], check=True)
        modules = sorted(Path(directory).glob('sm86_*.spv'))
        if not modules:
            raise AssertionError('shader_sm86 wrote no module for spirv-val')
        for module in modules:
            subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(module)], check=True)
        print(f'sm86: spirv-val passes {len(modules)} modules')


if __name__ == '__main__':
    main()
