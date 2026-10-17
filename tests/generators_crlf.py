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
    'scripts/build-ui-shaders.py',
    'src/nx/runtime/platform/controls_atlas.h',
    'src/nx/gpu/vk/ui_spv.h',
    'src/nx/gpu/vk/shaders/ui.vert',
    'src/nx/gpu/vk/shaders/ui.frag',
    'scripts/build-vic-shader.py',
    'src/nx/gpu/video/vic.comp',
    'src/nx/gpu/video/vic_spv.h',
    'scripts/build-astc-shader.py',
    'src/nx/gpu/texture/astc.comp',
    'src/nx/gpu/texture/astc_spv.h',
]


def main():
    with tempfile.TemporaryDirectory() as temporary:
        copy = Path(temporary)
        for name in TEXT + [str(p.relative_to(ROOT)) for p in (ROOT / 'dist/controls').glob('*.xml')]:
            (copy / name).parent.mkdir(parents=True, exist_ok=True)
            (copy / name).write_bytes((ROOT / name).read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        shutil.copy(ROOT / 'dist/controls/controls.png', copy / 'dist/controls/controls.png')
        for script in ('scripts/build-controls.py', 'scripts/build-ui-shaders.py', 'scripts/build-vic-shader.py',
                       'scripts/build-astc-shader.py'):
            subprocess.run([sys.executable, str(copy / script), '--check'], check=True)
    print('the generators check a CR LF checkout as an LF one')


if __name__ == '__main__':
    main()
