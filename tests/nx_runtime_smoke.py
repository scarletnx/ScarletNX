# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Recompile synthetic homebrew and run its Horizon SVCs with headless Vulkan."""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def make_nro():
    image = bytearray(0x200)

    def word(offset, value):
        struct.pack_into('<I', image, offset, value)

    word(0, 0x14000020)  # B +0x80, skipping NRO headers.
    image[0x10:0x14] = b'NRO0'
    word(0x18, len(image))
    for offset, value in ((0x20, 0), (0x24, 0x100), (0x28, 0x100), (0x2C, 0x80), (0x30, 0x180), (0x34, 0x80)):
        word(offset, value)
    marker = b'NX runtime smoke\n'
    image[0x100 : 0x100 + len(marker)] = marker
    word(0x80, 0x10000400)  # ADR X0, +0x80 (the marker in .rodata).
    word(0x84, 0xD2800001 | len(marker) << 5)  # MOV X1, marker length.
    word(0x88, 0xD40004E1)  # SVC #0x27: OutputDebugString.
    word(0x8C, 0xD40000E1)  # SVC #7: ExitProcess.
    word(0x90, 0xD4200000)  # Unreachable BRK if exit dispatch is broken.
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--artifact-dir', type=Path, default=Path('build/nx-smoke-failures'))
    parser.add_argument(
        '--runtime-arg', action='append', default=[], help="an argument of the game's runs (--host_page=65536)"
    )
    parser.add_argument('--log-has', action='append', default=[], help="a line each run's log must hold")
    args = parser.parse_args()
    runtime, repo, artifacts = args.runtime.resolve(), args.repo.resolve(), args.artifact_dir.resolve()
    with tempfile.TemporaryDirectory(prefix='scarletnx-nx-smoke-') as temporary:
        scratch = Path(temporary)
        fixture, output = scratch / 'homebrew with spaces.nro', scratch / 'output with spaces'
        fixture.write_bytes(make_nro())
        env = os.environ.copy()
        env.pop('SNX_GAME', None)
        try:
            compile_command = [
                str(runtime),
                '--repo',
                str(repo),
                '-r',
                str(fixture),
                'NX Smoke',
                '--runtime',
                str(runtime),
                '--jobs',
                '2',
                '--outdir',
                str(output),
            ]
            compiled = subprocess.run(
                compile_command, cwd=scratch, env=env, capture_output=True, timeout=args.timeout, check=False
            )
            (scratch / 'compile.stdout').write_bytes(compiled.stdout)
            (scratch / 'compile.stderr').write_bytes(compiled.stderr)
            if compiled.returncode:
                raise RuntimeError(f'NRO recompilation returned {compiled.returncode}')
            folder = output / 'NX Smoke'  # the game's own
            launcher = folder / ('NX Smoke.exe' if os.name == 'nt' else 'NX Smoke')
            # each way of making the game's shaders: before it starts (the default, which the pass's line says ended
            # before the game ran), none, and in the background, which the game's exit may cut short
            log = folder / 'NX Smoke.log'
            for mode in ('2', '0', '1'):
                log.unlink(missing_ok=True)  # each mode judged on its own log
                result = subprocess.run(
                    [str(launcher), '-H', f'--prepare_shaders={mode}', *args.runtime_arg],
                    cwd=folder,
                    env=env,
                    capture_output=True,
                    timeout=args.timeout,
                    check=False,
                )
                (scratch / f'runtime{mode}.stdout').write_bytes(result.stdout)
                (scratch / f'runtime{mode}.stderr').write_bytes(result.stderr)
                text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
                (scratch / f'runtime{mode}.log').write_text(text, encoding='utf-8')
                if result.returncode or '[game] NX runtime smoke' not in text or 'The game exited (code 0)' not in text:
                    raise RuntimeError(
                        f'NX runtime returned {result.returncode} or did not execute the Horizon SVC markers'
                        f' (prepare_shaders={mode})'
                    )
                for line in args.log_has:
                    if line not in text:
                        raise RuntimeError(f'the log of prepare_shaders={mode} lacks {line!r}')
                passed = "vulkan: the game's own shaders:" in text
                if mode == '2' and (not passed or text.index("the game's own shaders:") > text.index('[game] NX')):
                    raise RuntimeError("prepare_shaders=2: the shader pass didn't end before the game ran")
                if mode == '0' and passed:
                    raise RuntimeError('prepare_shaders=0: the shader pass ran')
            print(
                'Synthetic NRO recompiled and ran Horizon OutputDebugString/ExitProcess with headless Vulkan, '
                'with each prepare_shaders mode.'
            )
            return 0
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            artifacts.mkdir(parents=True, exist_ok=True)
            saved = artifacts / f'nx-smoke-{time.time_ns()}'
            shutil.copytree(scratch, saved)
            print(f'NX runtime smoke: {error}; saved {saved}', file=sys.stderr)
            return 1


if __name__ == '__main__':
    raise SystemExit(main())
