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


def make_nro(code=None, text=0x100):
    """an NRO of text bytes of code (its words from 0x80 on), then 0x80 of .rodata, which the marker starts, and
    0x80 of .data"""
    image = bytearray(text + 0x100)

    def word(offset, value):
        struct.pack_into('<I', image, offset, value)

    word(0, 0x14000020)  # B +0x80, skipping NRO headers.
    image[0x10:0x14] = b'NRO0'
    word(0x18, len(image))
    for offset, value in ((0x20, 0), (0x24, text), (0x28, text), (0x2C, 0x80), (0x30, text + 0x80), (0x34, 0x80)):
        word(offset, value)
    marker = b'NX runtime smoke\n'
    image[text : text + len(marker)] = marker
    for i, value in enumerate((code or smoke_code)(len(marker))):
        word(0x80 + 4 * i, value)
    return image


def smoke_code(length):
    return (
        0x10000400,  # ADR X0, +0x80 (the marker in .rodata).
        0xD2800001 | length << 5,  # MOV X1, marker length.
        0xD40004E1,  # SVC #0x27: OutputDebugString.
        0xD40000E1,  # SVC #7: ExitProcess.
        0xD4200000,  # Unreachable BRK if exit dispatch is broken.
    )


# a call through a pointer that holds 0, which no code answers: the run stops with the caller in its report. Where it
# would return is a NOP, a word NCE runs as it is (it patches an SVC there), so the report's code at lr holds it on
# every host
NULL_CALL_LR = 0xD503201F


def null_call_code(length):
    return (
        0x10000809,  # ADR X9, +0x100 (.data, all zeros).
        0xF9400128,  # LDR X8, [X9].
        0xD63F0100,  # BLR X8: to 0.
        NULL_CALL_LR,  # NOP, where it would return.
        0xD40000E1,  # SVC #7: ExitProcess.
        0xD4200000,  # BRK.
    )


def core_order_code(length):
    """the main thread starts a thread on its own core at its own priority, gives it the time to line up there, then
    makes an SVC that changes memory: the thread has not run when the SVC returns, as on the console, and has once
    the main thread waited for it. The marker and the exit only then, a BRK otherwise. For make_nro's text=0x180"""
    return (
        0xD10403F4,  # SUB X20, SP, #0x100: the thread's word, in the main thread's stack.
        0xF900029F,  # STR XZR, [X20].
        0x52900001,  # MOV W1, #0x8000.
        0x72BFFFE1,  # MOVK W1, #0xFFFF, LSL #16: the current thread.
        0xD4000181,  # SVC #0xC: GetThreadPriority, into W1.
        0x2A0103E4,  # MOV W4, W1.
        0x10000401,  # ADR X1, +0x80 (the thread's code).
        0xAA1403E2,  # MOV X2, X20.
        0xD14007E3,  # SUB X3, SP, #0x1000: its stack, which it never uses.
        0x12800025,  # MOV W5, #-2: the process's core, the main thread's.
        0xD4000101,  # SVC #8: CreateThread, the handle into W1.
        0x35000340,  # CBNZ W0, +0x68 (the BRK).
        0xB9000A81,  # STR W1, [X20, #8].
        0x2A0103E0,  # MOV W0, W1.
        0xD4000121,  # SVC #9: StartThread.
        0x350002C0,  # CBNZ W0, +0x58 (the BRK).
        0xD40003C1,  # SVC #0x1E: GetSystemTick.
        0xAA0003EA,  # MOV X10, X0.
        0xD40003C1,  # SVC #0x1E: GetSystemTick, until 2^20 ticks (55 ms) passed: the thread's host thread has
        0xCB0A0009,  # SUB X9, X0, X10      started by then, and it is lined up behind the main thread, which keeps
        0xD354FD29,  # LSR X9, X9, #20      the core through these SVCs.
        0xB4FFFFA9,  # CBZ X9, -0xC.
        0xD2A20000,  # MOV X0, #0x10000000.
        0xD2A00401,  # MOV X1, #0x200000.
        0xD4000581,  # SVC #0x2C: MapPhysicalMemory (refused, a homebrew has no system resource).
        0xF9400288,  # LDR X8, [X20].
        0xB5000168,  # CBNZ X8, +0x2C (the BRK): the thread ran during the SVC.
        0x91002281,  # ADD X1, X20, #8.
        0x52800022,  # MOV W2, #1.
        0x92800003,  # MOV X3, #-1.
        0xD4000301,  # SVC #0x18: WaitSynchronization, until the thread ends.
        0xF9400288,  # LDR X8, [X20].
        0xB40000A8,  # CBZ X8, +0x14 (the BRK): it never ran.
        0x100003E0,  # ADR X0, +0x7C (the marker in .rodata).
        0xD2800001 | length << 5,  # MOV X1, marker length.
        0xD40004E1,  # SVC #0x27: OutputDebugString.
        0xD40000E1,  # SVC #7: ExitProcess.
        0xD4200000,  # BRK.
        0xF9000000,  # The thread: STR X0, [X0], its word no longer zero.
        0xD4000141,  # SVC #0xA: ExitThread.
    )


def recompile(runtime, repo, fixture, name, output, scratch, env, timeout):
    """the program's folder, once it is recompiled; its launcher has the folder's name"""
    compiled = subprocess.run(
        [str(runtime), '--repo', str(repo), '-r', str(fixture), name, '--runtime', str(runtime), '--jobs', '2']
        + ['--outdir', str(output)],
        cwd=scratch,
        env=env,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    (scratch / f'compile {name}.stdout').write_bytes(compiled.stdout)
    (scratch / f'compile {name}.stderr').write_bytes(compiled.stderr)
    if compiled.returncode:
        raise RuntimeError(f'NRO recompilation of {name} returned {compiled.returncode}')
    return output / name


def run(folder, name, arguments, scratch, env, timeout):
    """the run's exit status and its log"""
    log = folder / f'{name}.log'
    log.unlink(missing_ok=True)  # each run judged on its own log
    launcher = folder / (f'{name}.exe' if os.name == 'nt' else name)
    result = subprocess.run(
        [str(launcher), '-H', *arguments], cwd=folder, env=env, capture_output=True, timeout=timeout, check=False
    )
    label = ' '.join([name, *arguments])
    (scratch / f'runtime {label}.stdout').write_bytes(result.stdout)
    (scratch / f'runtime {label}.stderr').write_bytes(result.stderr)
    text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
    (scratch / f'runtime {label}.log').write_text(text, encoding='utf-8')
    return result.returncode, text


def null_call(text):
    """what a report of a call to 0 lacks, or None: the stop, the caller's call marked at lr, the fatal"""
    if "*** The game's thread " not in text or 'stopped at 0: code the recompiler missed (0) ***' not in text:
        return "the report's header"
    if not any('code at lr ' in line and f' d63f0100 [{NULL_CALL_LR:08x}]' in line for line in text.splitlines()):
        return 'the BLR before lr in the code at lr'
    if 'FATAL: The game reached code at 0 that the recompiler missed' not in text:
        return 'the fatal line'
    return None


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
        fixture.write_bytes(make_nro(smoke_code))
        env = os.environ.copy()
        env.pop('SNX_GAME', None)
        try:
            folder = recompile(runtime, repo, fixture, 'NX Smoke', output, scratch, env, args.timeout)
            # each way of making the game's shaders: before it starts (the default, which the pass's line says ended
            # before the game ran), none, and in the background, which the game's exit may cut short
            for mode in ('2', '0', '1'):
                code, text = run(
                    folder, 'NX Smoke', [f'--prepare_shaders={mode}', *args.runtime_arg], scratch, env, args.timeout
                )
                if code or '[game] NX runtime smoke' not in text or 'The game exited (code 0)' not in text:
                    raise RuntimeError(
                        f'NX runtime returned {code} or did not execute the Horizon SVC markers'
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
            fixture = scratch / 'null call.nro'
            fixture.write_bytes(make_nro(null_call_code))
            folder = recompile(runtime, repo, fixture, 'NX Null Call', output, scratch, env, args.timeout)
            code, text = run(
                folder, 'NX Null Call', ['--prepare_shaders=0', *args.runtime_arg], scratch, env, args.timeout
            )
            lacks = null_call(text)
            if not code or lacks:
                raise RuntimeError(f'a call to 0 returned {code}, and its log lacks {lacks}')
            fixture = scratch / 'core order.nro'
            fixture.write_bytes(make_nro(core_order_code, text=0x180))
            folder = recompile(runtime, repo, fixture, 'NX Core Order', output, scratch, env, args.timeout)
            code, text = run(
                folder, 'NX Core Order', ['--prepare_shaders=0', *args.runtime_arg], scratch, env, args.timeout
            )
            if code or '[game] NX runtime smoke' not in text or 'The game exited (code 0)' not in text:
                raise RuntimeError(
                    f"a thread lined up on its starter's core ran during the starter's svcMapPhysicalMemory, or"
                    f' never ran: the run returned {code}'
                )
            print(
                'Synthetic NRO recompiled and ran Horizon OutputDebugString/ExitProcess with headless Vulkan, '
                'with each prepare_shaders mode; a call to 0 stopped with its caller in the report; a thread lined '
                "up on its starter's core ran only once the starter waited, not during its memory SVC."
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
