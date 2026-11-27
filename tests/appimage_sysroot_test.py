#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/appimage-sysroot.sh takes one ROOT: anything more is refused before it installs a thing, naming what it
was given. Offline, without root."""

import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'appimage-sysroot.sh'
failures = []
for args, said in ((['root', '--linux-only'], 'was given 2: root --linux-only'), ([], 'was given 0: ')):
    done = subprocess.run(['sh', str(SCRIPT), *args], capture_output=True, text=True, timeout=60)
    if done.returncode != 2 or said not in done.stderr or 'scripts/appimage-sysroot.sh ROOT' not in done.stdout:
        failures.append(f'{args}: exit {done.returncode}, "{done.stderr.strip()}", not 2 and "{said}" with the usage')
if failures:
    sys.exit('appimage_sysroot:\n  ' + '\n  '.join(failures))
print('appimage_sysroot: its arguments other than one ROOT refused, each named')
