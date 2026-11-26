#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Checked A32 integer blocks of a native back end vs Unicorn's ARM11 MPCore model.

An independent ISA oracle, unlike arm_checked_matrix.py. Includes serialized
ARMv6K exclusive sequences. A separate C gate tests the real CTR bus against
aliases, external writes, ABA, permissions and mapping changes. Thumb legacy
BL halves have separate C golden witnesses.
"""

import argparse
import ctypes as C

import arm_test as A
from arm_blocks import CHECKED_ABI, Access, Checked, add_variants, variants
from arm_checked_matrix import make


def main():
    import unicorn as U
    from unicorn import arm_const as R

    assert U.__version__ == '2.1.4', 'Use the pinned unicorn==2.1.4'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blocks', required=True, help='snx-test-arm-blocks, for this host or another machine')
    parser.add_argument('--every', type=int, default=1, help='of the states, one in so many, from the encoding on')
    add_variants(parser)
    args = parser.parse_args()
    A.catalogue()
    for condition in range(15):
        for d, n, m in ((0, 1, 2), (1, 1, 2), (2, 1, 2), (14, 14, 13)):
            for opcode in (0x06500F90, 0x06800FB0):
                A.case(condition << 28 | opcode | n << 16 | d << 12 | m, 'packed')
    A.CASES.append((A.program(0xE6513F92, 0xE0233004, 0xE6815FB2), 'packed'))

    # ARMv5TE signed halfword multiplies: SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy, every x/y half. Fields are
    # Rd (19:16), Rn/RdLo (15:12), Rs (11:8), Rm (3:0). The 'mulh' group clears the sticky Q flag on half the
    # seeds, so an overflowing accumulate is seen setting it.
    def mulh(op, x, y, d, n, s, m):
        return 0x01000080 | op << 21 | y << 6 | x << 5 | d << 16 | n << 12 | s << 8 | m

    forms = [(0, x, y) for x in (0, 1) for y in (0, 1)] + [(1, 0, y) for y in (0, 1)] + [(1, 1, y) for y in (0, 1)]
    forms += [(2, x, y) for x in (0, 1) for y in (0, 1)] + [(3, x, y) for x in (0, 1) for y in (0, 1)]
    for op, x, y in forms:
        accumulates = op in (0, 2) or (op == 1 and not x)
        for condition in range(15) if (x, y) == (0, 0) else (14,):
            for d, n, s, m in ((3, 4, 2, 1), (1, 2, 1, 2), (14, 1, 2, 1)):
                if not accumulates:
                    n = 0
                elif op == 2 and d == n:
                    continue
                A.case(condition << 28 | mulh(op, x, y, d, n, s, m), 'mulh')
    # A32 base-in-list transfers with writeback: ARM11 MPCore (and ARMv8)
    # semantics, checked against Unicorn. STM stores the old base; LDM's
    # loaded base wins over writeback.
    for word in (
        0xE8A40050,
        0xE8B40050,
        0xE8A40010,
        0xE8B40010,
        0xE9244010,
        0xE9340050,
        0xE92D6000,
        0xE8BD4010,
        0xE8A48000,
    ):
        A.case(word, 'multiple')
    regs = [getattr(R, f'UC_ARM_REG_R{i}') for i in range(13)] + [R.UC_ARM_REG_SP, R.UC_ARM_REG_LR, R.UC_ARM_REG_PC]
    emu = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    emu.ctl_set_cpu_model(R.UC_CPU_ARM_11MPCORE)
    emu.mem_map(0, A.SIZE)
    for variant in variants(parser, args):
        blocks = make(args.blocks, 'arm11', 'checked', variant)
        total = invalid = 0
        for index, group in A.CASES:
            words = A.PROGRAMS[index]
            for seed in range(index % args.every, 32, args.every):
                cpu, memory = A.state(words, seed, group)
                if group == 'packed':
                    cpu.cpsr = (cpu.cpsr & ~0xF0000) | (seed & 15) << 16
                if group == 'mulh' and seed & 16:
                    cpu.cpsr &= ~(1 << 27)
                emu.mem_write(0, bytes(memory))
                emu.ctl_remove_cache(A.PC, A.PC + 4 * len(words))
                emu.reg_write(R.UC_ARM_REG_CPSR, cpu.cpsr)
                for reg, value in zip(regs, cpu.r):
                    emu.reg_write(reg, value)
                error = None
                try:
                    emu.emu_start(A.PC, A.SIZE, count=len(words))
                except U.UcError as exc:
                    error = exc
                faults = []
                epoch, reservation = [1], [0, 0, 0]

                @Access
                def access(context, operation, address, size, data):
                    if operation not in (1, 2, 3, 0x101, 0x102, 0x201, 0x202) or not 1 <= size <= 64:
                        faults.append((operation, size))
                        return 1
                    if address + size > A.SIZE:
                        return 1
                    if operation == 0x202:
                        valid = reservation == [epoch[0], address, size]
                        reservation[:] = [0, 0, 0]
                        if not valid:
                            return 2
                    if operation & 255 == 2:
                        C.memmove(C.addressof(memory) + address, data, size)
                        epoch[0] += 1
                    else:
                        C.memmove(data, C.addressof(memory) + address, size)
                        if operation == 0x201:
                            reservation[:] = [epoch[0], address, size]
                    return 0

                state = Checked()
                C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                state.abi, state.access = CHECKED_ABI, access
                state.cpu.mem = C.cast(1, C.POINTER(C.c_uint8))
                state.cpu.mem_size = 2**64 - 1
                blocks.functions[index](state)
                actual = state.cpu
                label = f'{blocks.name} {words} {group} seed={seed}'
                assert not faults, (label, faults)
                if error:
                    assert error.errno == U.UC_ERR_INSN_INVALID and actual.exit == 1, (label, error, actual.exit)
                    assert actual.r[15] == A.PC, (label, 'undefined PC')
                    invalid += 1
                    continue
                assert actual.exit == 0, (label, actual.exit)
                expected = [emu.reg_read(reg) for reg in regs] + [emu.reg_read(R.UC_ARM_REG_CPSR)]
                assert list(actual.r) + [actual.cpsr] == expected, (label, list(actual.r), expected)
                assert bytes(memory) == bytes(emu.mem_read(0, A.SIZE)), (label, 'memory mismatch')
                total += 1
        print(
            f'ARM11 checked, {blocks.name}: {total} exact comparisons, {invalid} matched undefined encodings',
            flush=True,
        )
        blocks.close()


if __name__ == '__main__':
    main()
