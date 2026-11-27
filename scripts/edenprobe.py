#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The old name of scripts/amberlith.py, from when Amberlith was edenprobe (before 1.0.0): it says the new name and
runs that with the same arguments."""

import runpy
import sys
from pathlib import Path

print(
    'scripts/edenprobe.py is the old name of scripts/amberlith.py: it runs that; call scripts/amberlith.py',
    file=sys.stderr,
)
sys.argv[0] = str(Path(__file__).with_name('amberlith.py'))
runpy.run_path(sys.argv[0], run_name='__main__')
