#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Rebuild the embedded ASTC decoder SPIR-V (src/nx/gpu/texture/astc_spv.h) from astc.comp.

Needs glslang: glslangValidator from .cache/tools/glslang-16.6.0/bin (an official KhronosGroup release, unpacked
there), from PATH, or given with --glslang. spirv-val validates the result when it is found the same way
(.cache/tools/spirv-tools-*/bin or PATH).

The checked-in header is the compiled shader, so normal and cross builds need no shader compiler. --check needs
none either: it compares the sources' hash (this script and astc.comp) with the one the header holds.
--spv PATH also saves the standalone binary.
"""

import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

GLSLANG_VERSION = '16.6.0'


def find_tool(root, name, pattern):
    for candidate in sorted((root / '.cache/tools').glob(pattern)):
        if (candidate / 'bin' / name).is_file():
            return str(candidate / 'bin' / name)
    return shutil.which(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--spv', type=Path)
    parser.add_argument('--glslang', help='glslangValidator to use')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'src/nx/gpu/texture/astc.comp'
    header = source.with_name('astc_spv.h')
    # the bytes as the repository holds them: a Windows checkout turns the line ends into CR LF
    lf = lambda path: path.read_bytes().replace(b'\r\n', b'\n')
    digest = hashlib.sha256(lf(Path(__file__)) + lf(source)).hexdigest()
    if args.check:
        if f'sources {digest}' not in header.read_text():
            raise SystemExit('astc_spv.h is not what astc.comp makes: run scripts/build-astc-shader.py')
        return
    glslang = args.glslang or find_tool(root, 'glslangValidator', f'glslang-{GLSLANG_VERSION}')
    if not glslang:
        raise SystemExit(f'glslangValidator not found: unpack glslang {GLSLANG_VERSION} into .cache/tools')
    version = subprocess.check_output([glslang, '--version'], text=True).splitlines()[0].rsplit(':', 1)[-1].strip()
    if version != GLSLANG_VERSION:
        print(f'warning: glslang {version}, the header was last made with {GLSLANG_VERSION}')
    spirv_val = find_tool(root, 'spirv-val', 'spirv-tools-*')
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'astc.spv'
        subprocess.run(
            [glslang, '-V', '--target-env', 'vulkan1.1', '-o', str(binary), source.name], cwd=source.parent, check=True
        )
        if spirv_val:
            subprocess.run([spirv_val, '--target-env', 'vulkan1.1', str(binary)], check=True)
        else:
            print('warning: spirv-val not found, the SPIR-V was not validated')
        data = binary.read_bytes()
    if args.spv:
        args.spv.write_bytes(data)
    words = struct.unpack(f'<{len(data) // 4}I', data)
    text = f"""/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Generated from astc.comp by scripts/build-astc-shader.py (glslang {version}).
   sources {digest}
   Ported from Eden (src/video_core/host_shaders/astc_decoder.comp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   Do not edit. */
// clang-format off
static const u32 astc_spv[] = {{
"""
    for i in range(0, len(words), 8):
        text += '    ' + ', '.join(f'0x{v:08x}' for v in words[i : i + 8]) + ',\n'
    text += '};\n// clang-format on\n'
    header.write_text(text, newline='\n')


if __name__ == '__main__':
    main()
