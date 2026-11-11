#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The generators' --check on a Windows checkout: every text source with CR LF line ends, as git writes it there."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEXT = [
    'scripts/build-controls.py',
    'scripts/build-skins.py',
    'src/common/utilities/font.c',
    'src/nx/runtime/platform/controls_atlas.h',
]


def main():
    with tempfile.TemporaryDirectory() as temporary:
        copy = Path(temporary)
        skins = ROOT / 'dist/controls/skins'
        sources = [p for p in skins.rglob('*') if p.suffix in ('.cfg', '.svg')]
        for name in TEXT + [str(p.relative_to(ROOT)) for p in list((ROOT / 'dist/controls').glob('*.xml')) + sources]:
            (copy / name).parent.mkdir(parents=True, exist_ok=True)
            (copy / name).write_bytes((ROOT / name).read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        shutil.copy(ROOT / 'dist/controls/controls.png', copy / 'dist/controls/controls.png')
        for png in skins.rglob('*.png'):
            shutil.copy(png, copy / png.relative_to(ROOT))
        for script in ('scripts/build-controls.py', 'scripts/build-skins.py'):
            subprocess.run([sys.executable, str(copy / script), '--check'], check=True)
    print('the generators check a CR LF checkout as an LF one')


if __name__ == '__main__':
    main()
