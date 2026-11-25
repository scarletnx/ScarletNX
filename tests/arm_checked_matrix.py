#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check the opt-in bus against flat AOT over the existing integer catalogue.

This is a memory-codegen differential, not an independent ISA oracle. The
unchanged A57 tests separately compare against Unicorn. No execution engine:
LLVM objects are emitted ahead of time, host-linked, then loaded with ctypes.
"""

import argparse
import ctypes as C
import pathlib
import subprocess
import tempfile

import arm_test as A

Access = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_void_p)


# checked.h's SnxArmCheckedCPU: SNX_ARM_CHECKED_ABI, and the words a native block moves a transfer through
CHECKED_ABI = 2


class Checked(C.Structure):
    _fields_ = [
        ('cpu', A.CPU),
        ('abi', C.c_uint32),
        ('context', C.c_void_p),
        ('access', Access),
        ('words', C.c_uint32 * 32),
    ]


def snapshot(cpu):
    return (
        list(cpu.r),
        cpu.cpsr,
        cpu.exit,
        cpu.syndrome,
        cpu.exclusive_value,
        cpu.exclusive_address,
        cpu.exclusive_size,
        cpu.exclusive_valid,
        cpu.tpidrurw,
        cpu.tpidruro,
        cpu.fpscr,
        list(cpu.d),
    )


def compile_blocks(args, root, profile, mode, opt):
    stem = root / f'{profile}-{mode}-{opt}'
    obj, lib = stem.with_suffix('.o'), stem.with_suffix('.so')
    subprocess.run([args.emitter, str(root / 'words.txt'), str(obj), opt, profile, mode], check=True)
    subprocess.run([args.cc, '-shared', str(obj), args.runtime, '-o', str(lib)], check=True)
    handle = C.CDLL(str(lib))
    functions = []
    for i in range(len(A.PROGRAMS)):
        fn = getattr(handle, f'block_{i}')
        fn.argtypes, fn.restype = [C.POINTER(A.CPU)], None
        functions.append(fn)
    return functions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emitter', required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--runtime', required=True, help='libsnx_arm_rt.a')
    args = parser.parse_args()
    A.catalogue()
    # A57 checked exclusives remain unsupported. ARM11 reservations have an
    # independent ISA oracle and real CTR bus witnesses, not a flat-value oracle.
    cases = [(i, group) for i, group in A.CASES if group != 'atomic']
    total = 0
    with tempfile.TemporaryDirectory(prefix='snx-checked-matrix-') as tmp:
        root = pathlib.Path(tmp)
        (root / 'words.txt').write_text(
            ''.join(f'{pc:x} ' + ' '.join(f'{w:08x}' for w in words) + '\n' for pc, words in zip(A.PCS, A.PROGRAMS))
        )
        for profile in ('a57', 'arm11'):
            for opt in ('0', '2'):
                flat = compile_blocks(args, root, profile, 'flat', opt)
                checked = compile_blocks(args, root, profile, 'checked', opt)
                count = 0
                for index, group in cases:
                    for seed in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 31, 63):
                        cpu, memory = A.state(A.PROGRAMS[index], seed, group)
                        bus_memory = (C.c_uint8 * A.SIZE).from_buffer_copy(memory)
                        base, errors = C.addressof(bus_memory), []

                        @Access
                        def access(context, operation, address, size, data):
                            if operation not in (1, 2, 3, 0x101, 0x102) or not 1 <= size <= 128:
                                errors.append((operation, size))
                                return 1
                            if address + size > A.SIZE:
                                return 1
                            if operation & 255 == 2:
                                C.memmove(base + address, data, size)
                            else:
                                C.memmove(data, base + address, size)
                            return 0

                        state = Checked()
                        C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                        state.abi, state.access = CHECKED_ABI, access
                        state.cpu.mem = C.cast(1, C.POINTER(C.c_uint8))
                        state.cpu.mem_size = 2**64 - 1
                        flat[index](C.byref(cpu))
                        checked[index](C.byref(state.cpu))
                        label = f'{profile} O{opt} {A.PROGRAMS[index]} {group} seed={seed}'
                        assert not errors, (label, errors)
                        assert snapshot(cpu) == snapshot(state.cpu), (label, snapshot(cpu), snapshot(state.cpu))
                        assert bytes(memory) == bytes(bus_memory), (label, 'memory mismatch')
                        count += 1
                total += count
                print(f'{profile} O{opt}: {count} checked/flat comparisons passed', flush=True)
    print(f'{total} exact checked-memory comparisons passed')


if __name__ == '__main__':
    main()
