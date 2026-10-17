#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Rebuild the embedded VIC SPIR-V (the aligned and the unaligned build of vic.comp).
Requires glslangValidator on PATH (spirv-val is used when it is there).

The checked-in header is the compiled shader, so normal and cross builds do not
need a host shader compiler. --check needs none either: it compares the sources'
hash (this script and vic.comp) with the one the header holds.
--spv PATH also saves the standalone binary of the aligned build.
"""

import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--spv', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'src/nx/gpu/video/vic.comp'
    header = source.with_name('vic_spv.h')
    # the bytes as the repository holds them: a Windows checkout turns the line ends into CR LF
    lf = lambda path: path.read_bytes().replace(b'\r\n', b'\n')
    digest = hashlib.sha256(lf(Path(__file__)) + lf(source)).hexdigest()
    if args.check:
        if f'sources {digest}' not in header.read_text():
            raise SystemExit('vic_spv.h is not what vic.comp makes: run scripts/build-vic-shader.py')
        return
    version = (
        subprocess.check_output(['glslangValidator', '--version'], text=True).splitlines()[0].rsplit(':', 1)[-1].strip()
    )
    builds = [('', 'vic_spv'), ('-DVIC_UNALIGNED', 'vic_unaligned_spv')]
    words_by_name = {}
    with tempfile.TemporaryDirectory() as tmp:
        for define, name in builds:
            binary = Path(tmp) / f'{name}.spv'
            cmd = ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', str(binary), source.name]
            if define:
                cmd.insert(1, define)
            subprocess.run(cmd, cwd=source.parent, check=True)
            if shutil.which('spirv-val'):
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)], check=True)
            data = binary.read_bytes()
            words_by_name[name] = struct.unpack(f'<{len(data) // 4}I', data)
            if args.spv and not define:
                args.spv.write_bytes(data)
    text = """/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Generated from vic.comp by scripts/build-vic-shader.py (glslang @VERSION@).
   sources @DIGEST@
   Ported from Eden (src/video_core/host1x/vic.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
   Do not edit. */
// clang-format off
"""
    text = text.replace('@VERSION@', version).replace('@DIGEST@', digest)
    for _, name in builds:
        words = words_by_name[name]
        text += f'static const u32 {name}[] = {{\n'
        for i in range(0, len(words), 8):
            text += '    ' + ', '.join(f'0x{v:08x}' for v in words[i : i + 8]) + ',\n'
        text += '};\n'
    text += '// clang-format on\n'
    header.write_text(text)


if __name__ == '__main__':
    main()
