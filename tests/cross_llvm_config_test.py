#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/cross-llvm-config.sh on a system root whose llvm-config is a program of each machine a linux-*-cross preset
builds for: it runs under the qemu-user of that machine and byte order, with its arguments as given and the root put
in front of the paths it prints; a machine it has no qemu for is refused with its number. Fakes for both, offline."""

import os
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'cross-llvm-config.sh'
QEMUS = ('ppc64le', 'ppc64', 'riscv64', 'loongarch64', 'mips64el', 'mips64')
CASES = [  # e_machine, EI_DATA (1 little-endian, 2 big-endian), EI_CLASS (2 64-bit), the qemu expected (None: refused)
    (21, 1, 2, 'ppc64le'),
    (21, 2, 2, 'ppc64'),
    (243, 1, 2, 'riscv64'),
    (258, 1, 2, 'loongarch64'),
    (8, 1, 2, 'mips64el'),
    (8, 2, 2, 'mips64'),
    (8, 2, 1, None),  # 32-bit MIPS, the same e_machine
    (62, 1, 2, None),
]


def executable(path, text):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def main():
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        root, bin_dir = Path(tmp) / 'root', Path(tmp) / 'bin'
        (root / 'usr' / 'bin').mkdir(parents=True)
        bin_dir.mkdir()
        for q in QEMUS:  # -L ROOT PROGRAM ARGS...: what it ran, and a path of the target's for the script to rewrite
            executable(bin_dir / f'qemu-{q}', f'#!/bin/sh\necho "qemu-{q} $3 $4 $5" /usr/lib/llvm\n')
        config = root / 'usr' / 'bin' / 'llvm-config'
        env = dict(os.environ, PATH=f'{bin_dir}{os.pathsep}{os.environ.get("PATH", "")}', SNX_SYSROOT=str(root))
        for machine, data, elf_class, qemu in CASES:
            header = bytearray(64)
            header[:6] = b'\x7fELF' + bytes([elf_class, data])
            header[18:20] = struct.pack('<H' if data == 1 else '>H', machine)
            config.write_bytes(bytes(header))
            config.chmod(0o755)
            out = subprocess.run(
                ['sh', str(SCRIPT), '--libdir', '--link-static'], env=env, capture_output=True, text=True
            )
            if qemu:
                want = f'qemu-{qemu} {config} --libdir --link-static {root}/usr/lib/llvm'
                ok = out.returncode == 0 and out.stdout.strip() == want
            else:
                want = f'refused, naming machine {machine}'
                ok = out.returncode != 0 and f'machine {machine}' in out.stderr
            if not ok:
                print(
                    f'FAIL: machine {machine}, EI_DATA {data}, EI_CLASS {elf_class}: '
                    f'{out.stdout.strip() or out.stderr.strip()!r}, '
                    f'want {want!r}'
                )
                failed += 1
    if failed:
        return 1
    print(f'PASS cross-llvm-config.sh: {len(CASES)} machines')
    return 0


if __name__ == '__main__':
    sys.exit(main())
