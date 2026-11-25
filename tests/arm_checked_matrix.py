#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check the opt-in bus against flat blocks over the existing integer catalogue.

This is a memory-codegen differential, not an independent ISA oracle. The
A57 tests separately compare against Unicorn. Both blocks are the native back
end's, or LLVM's, made for a profile and a memory mode (arm_blocks.py).
"""

import argparse
import ctypes as C

import arm_test as A
from arm_blocks import CHECKED_ABI, Access, Blocks, Checked, add_variants, variants


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


def make(runner, profile, mode, variant):
    programs = [('arm', pc, words) for pc, words in zip(A.PCS, A.PROGRAMS)]
    return Blocks(runner, programs, profile=profile, memory=mode, **variant).refuse()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blocks', required=True, help='snx-test-arm-blocks, for this host or another machine')
    parser.add_argument('--every', type=int, default=1, help='of the states, one in so many, from the encoding on')
    add_variants(parser)
    args = parser.parse_args()
    A.catalogue()
    # A57 checked exclusives remain unsupported. ARM11 reservations have an
    # independent ISA oracle and real CTR bus witnesses, not a flat-value oracle.
    cases = [(i, group) for i, group in A.CASES if group != 'atomic']
    total = 0
    for profile in ('a57', 'arm11'):
        for variant in variants(parser, args):
            flat = make(args.blocks, profile, 'flat', variant)
            checked = make(args.blocks, profile, 'checked', variant)
            name = f'{flat.name}, {profile}'
            count = 0
            for index, group in cases:
                for seed in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 31, 63)[index % args.every :: args.every]:
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
                    flat.functions[index](cpu)
                    checked.functions[index](state)
                    label = f'{name} {A.PROGRAMS[index]} {group} seed={seed}'
                    assert not errors, (label, errors)
                    assert snapshot(cpu) == snapshot(state.cpu), (label, snapshot(cpu), snapshot(state.cpu))
                    assert bytes(memory) == bytes(bus_memory), (label, 'memory mismatch')
                    count += 1
            total += count
            print(f'{name}: {count} checked/flat comparisons passed', flush=True)
            flat.close()
            checked.close()
    print(f'{total} exact checked-memory comparisons passed')


if __name__ == '__main__':
    main()
