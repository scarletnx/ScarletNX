# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Run a Vulkan contract test with Khronos synchronization validation.

Runs the test executable under VK_LAYER_KHRONOS_validation with synchronization
validation enabled, and fails when it reports any validation error."""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

VALIDATION_ERROR = re.compile(r'Validation Error:|SYNC-HAZARD', re.IGNORECASE)
VALIDATION_LAYER_INSERTED = re.compile(r'Insert instance layer\s+"VK_LAYER_KHRONOS_validation"', re.IGNORECASE)


def main(argv):
    if not argv:
        print('usage: 3ds_vk_validation.py TEST_EXECUTABLE [ARG ...]', file=sys.stderr)
        return 2

    env = {
        **os.environ,
        'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation',
        'VK_LAYER_VALIDATE_SYNC': '1',
        'VK_LOADER_DEBUG': 'layer',
    }
    # a folder of its own, so no settings file beside the caller is read
    with tempfile.TemporaryDirectory(prefix='snx-vk-contracts-') as cwd:
        result = subprocess.run(
            [str(Path(argv[0]).absolute()), *argv[1:]],
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors='replace',
        )
    sys.stdout.write(result.stdout)

    if VALIDATION_ERROR.search(result.stdout):
        print('Vulkan validation reported an error', file=sys.stderr)
        return 1
    if result.returncode == 0 and not VALIDATION_LAYER_INSERTED.search(result.stdout):
        print('Khronos validation layer was not inserted; validation is inconclusive', file=sys.stderr)
        return 77
    return result.returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
