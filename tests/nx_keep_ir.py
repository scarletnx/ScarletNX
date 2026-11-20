#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A recompile with --keep-ir whose module's IR cannot be saved says so, with the folder: the 64-bit program of
nx_runtime_smoke.py and the 32-bit one of nx_arm32_smoke.py, each recompiled into a folder where a folder already has
the IR file's name (<name>_build/module.ll). The recompile fails, and its last line names the IR and where it was to
go, where it printed whatever the error's buffer held. Nothing is run: no Vulkan, no window.

    nx_keep_ir.py --runtime build/scarletnx [--cc clang]
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import nx_arm32_smoke
import nx_runtime_smoke


def refused(runtime, repo, game, name, scratch, timeout):
    """the recompile of game into scratch/output, its module's IR path taken by a folder: its output when it fails"""
    kept = scratch / 'output' / name / f'{name}_build'
    (kept / 'module.ll').mkdir(parents=True)
    env = os.environ.copy()
    env.pop('SNX_GAME', None)
    for variable in ('HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME'):  # no file of the user's
        env[variable] = str(scratch / variable.lower())
    command = [str(runtime), '--repo', str(repo), '-r', str(game), name, '--runtime', str(runtime), '--jobs', '2']
    command += ['--outdir', str(scratch / 'output'), '--keep-ir']
    done = subprocess.run(command, cwd=scratch, env=env, capture_output=True, text=True, timeout=timeout, check=False)
    text = done.stdout + done.stderr
    if done.returncode == 0:
        raise RuntimeError(f'{name}: the recompile passed with its IR file a folder:\n{text}')
    want = f'cannot save the IR of module in {kept}'
    # Windows' runtime names the folder with backslashes, the path here has forward ones (MSYS2's Python)
    if want.replace('\\', '/') not in text.replace('\\', '/'):
        raise RuntimeError(f'{name}: the recompile did not say "{want}":\n{text}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    runtime, repo = args.runtime.resolve(), args.repo.resolve()
    with tempfile.TemporaryDirectory(prefix='scarletnx-nx-keep-ir-') as temporary:
        # by its real path, as the runtime names the folder: macOS's temporary folder is under a link (/var)
        scratch = Path(temporary).resolve()
        try:
            wide = scratch / 'wide'
            wide.mkdir()
            (wide / 'keep ir.nro').write_bytes(nx_runtime_smoke.make_nro(nx_runtime_smoke.smoke_code))
            refused(runtime, repo, wide / 'keep ir.nro', 'Keep IR', wide, args.timeout)
            narrow = scratch / 'narrow'
            exefs = narrow / 'exefs'
            exefs.mkdir(parents=True)
            source = Path(__file__).resolve().with_name('nx_arm32_program.c')
            obj = narrow / 'program.o'
            subprocess.run([args.cc, *nx_arm32_smoke.CFLAGS, '-marm', '-c', str(source), '-o', str(obj)], check=True)
            text, symbols = nx_arm32_smoke.link(nx_arm32_smoke.Elf32(obj.read_bytes()))
            (exefs / 'main').write_bytes(nx_arm32_smoke.make_nso(text, symbols))
            (exefs / 'main.npdm').write_bytes(nx_arm32_smoke.make_npdm())
            refused(runtime, repo, exefs, 'Keep IR 32', narrow, args.timeout)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            print(f'FAIL: {error}', file=sys.stderr)
            return 1
    print('PASS --keep-ir: a module whose IR cannot be saved is named with its folder, 64-bit and 32-bit')
    return 0


if __name__ == '__main__':
    sys.exit(main())
