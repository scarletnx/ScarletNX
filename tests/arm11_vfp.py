#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Scalar VFPv2 blocks of a native back end vs Unicorn's explicit ARM11 MPCore, no tolerances."""

import argparse
import ctypes as C

import arm_vector_test as V
from arm_blocks import CHECKED_ABI, CPU, PC, SIZE, Access, Blocks, Checked, add_variants, arm, variants


def catalogue():
    """(word, text) of each instruction"""
    listed = []
    for width, prefix in [(32, 's'), (64, 'd')]:

        def put(base, text, wide=(width == 64) << 8, **registers):  # its registers' fields, each (kind, number)
            word = base | wide
            for field, (kind, number) in registers.items():
                word = V.place(word, field, kind, number)
            listed.append((word, text))

        three = {
            'vadd': 0xEE300A00, 'vsub': 0xEE300A40, 'vmul': 0xEE200A00, 'vdiv': 0xEE800A00, 'vmla': 0xEE000A00,
            'vmls': 0xEE000A40, 'vnmla': 0xEE100A40, 'vnmls': 0xEE100A00, 'vnmul': 0xEE200A40,
        }  # fmt: skip
        for op, base in three.items():
            for d, n, m in [(0, 2, 4), (2, 2, 4), (4, 2, 4), (15, 15, 14)]:
                text = f'{op}.f{width} {prefix}{d},{prefix}{n},{prefix}{m}'
                put(base, text, d=(prefix, d), n=(prefix, n), m=(prefix, m))
        for op, base in [('vmov', 0xEEB00A40), ('vabs', 0xEEB00AC0), ('vneg', 0xEEB10A40), ('vsqrt', 0xEEB10AC0)]:
            put(base, f'{op}.f{width} {prefix}0,{prefix}2', d=(prefix, 0), m=(prefix, 2))
        for op, base in [('vcmp', 0xEEB40A40), ('vcmpe', 0xEEB40AC0)]:
            put(base, f'{op}.f{width} {prefix}0,{prefix}2', d=(prefix, 0), m=(prefix, 2))
            put(base | 0x10000, f'{op}.f{width} {prefix}0,#0', d=(prefix, 0))
        for signed in ['s', 'u']:
            sign = signed == 's'
            text = f'.{signed}32.f{width} s0,{prefix}2'
            put(0xEEB80A40 | sign << 7, f'vcvt.f{width}.{signed}32 {prefix}0,s2', d=(prefix, 0), m=('s', 2))
            put(0xEEBC0AC0 | sign << 16, f'vcvt{text}', d=('s', 0), m=(prefix, 2))
            put(0xEEBC0A40 | sign << 16, f'vcvtr{text}', d=('s', 0), m=(prefix, 2))
    return listed + [
        (0xEEB70AC1, 'vcvt.f64.f32 d0,s2'),
        (0xEEB70BC2, 'vcvt.f32.f64 s0,d2'),
        (0xEE0F0A90, 'vmov s31,r0'),
        (0xEE1F0A90, 'vmov r0,s31'),
        (0xEC410B1F, 'vmov d15,r0,r1'),
        (0xEC510B1F, 'vmov r0,r1,d15'),
        (0xEC410A1F, 'vmov s30,s31,r0,r1'),
        (0xEC510A1F, 'vmov r0,r1,s30,s31'),
        (0xEEF10A10, 'vmrs r0,fpscr'),
        (0xEEF1FA10, 'vmrs APSR_nzcv,fpscr'),
        (0xEEE10A10, 'vmsr fpscr,r0'),
        (0xED94FB01, 'vldr d15,[r4,#4]'),
        (0xED84FB01, 'vstr d15,[r4,#4]'),
        (0xEDD4FA01, 'vldr s31,[r4,#4]'),
        (0xEDC4FA01, 'vstr s31,[r4,#4]'),
        (0xED9F0B02, 'vldr d0,[pc,#8]'),
        (0xED2D8B10, 'vpush {d8-d15}'),
        (0xECBD8B10, 'vpop {d8-d15}'),
        (0xECB48A10, 'vldmia r4!,{s16-s31}'),
        (0xECA48A10, 'vstmia r4!,{s16-s31}'),
        (0xED340B08, 'vldmdb r4!,{d0-d3}'),
        (0xED240B08, 'vstmdb r4!,{d0-d3}'),
        (0xED2D0B20, 'vpush {d0-d15}'),
    ]


def bus(memory, fail=False):
    events = []

    @Access
    def access(context, operation, address, size, data):
        events.append((operation, address, size))
        if operation not in (3, 0x101, 0x102) or not 0 < size <= 128 or address + size > SIZE:
            return 1
        if fail and operation != 3:
            return 1
        if operation == 0x102:
            C.memmove(C.addressof(memory) + address, data, size)
        else:
            C.memmove(data, C.addressof(memory) + address, size)
        return 0

    return access, events


def main():
    import unicorn as U
    from unicorn import arm_const as R

    assert U.__version__ == '2.1.4'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blocks', required=True, help='snx-test-arm-blocks, for this host or another machine')
    parser.add_argument('--every', type=int, default=1, help='of the states, one in so many, from the encoding on')
    add_variants(parser)
    args = parser.parse_args()
    listed = catalogue()
    words, names = [word for word, _ in listed], [text for _, text in listed]
    words.append(words[names.index('vldmdb r4!,{d0-d3}')] | 1)
    names.append('unsupported legacy FLDMX format word')
    # ARM condition codes must precede VFP guards and memory access.
    for index in [0, names.index('vmov s31,r0'), names.index('vstr d15,[r4,#4]')]:
        for cond in range(14):
            words.append((words[index] & 0x0FFFFFFF) | cond << 28)
            names.append(f'condition={cond} {names[index]}')
    u = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    u.ctl_set_cpu_model(R.UC_CPU_ARM_11MPCORE)
    u.mem_map(0, SIZE)
    regs = [getattr(R, f'UC_ARM_REG_R{i}') for i in range(13)] + [R.UC_ARM_REG_SP, R.UC_ARM_REG_LR, R.UC_ARM_REG_PC]
    for variant in variants(parser, args):
        for mode in ['flat', 'checked']:
            listed = arm([(word,) for word in words])
            blocks = Blocks(args.blocks, listed, profile='arm11', memory=mode, **variant).refuse()
            functions = blocks.functions
            total = 0
            for index, (word, text) in enumerate(zip(words, names)):
                for seed in range(index % args.every, 80, args.every):
                    cpu, memory = V.initial(word, seed)
                    cpu.fpscr &= 0xF3C0009F
                    if text == 'vmsr fpscr,r0':
                        cpu.r[0] = cpu.fpscr
                    upper = list(cpu.d)[16:]
                    before = bytes(cpu)
                    u.mem_write(0, bytes(memory))
                    u.ctl_remove_cache(PC, PC + 4)
                    u.reg_write(R.UC_ARM_REG_CPSR, cpu.cpsr)
                    u.reg_write(R.UC_ARM_REG_FPEXC, 0x40000000)
                    u.reg_write(R.UC_ARM_REG_FPSCR, cpu.fpscr)
                    for reg, value in zip(regs, cpu.r):
                        u.reg_write(reg, value)
                    for i in range(16):
                        u.reg_write(R.UC_ARM_REG_D0 + i, cpu.d[i])
                    access, accesses = bus(memory)
                    state = Checked()
                    C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                    state.abi, state.access = CHECKED_ABI, access
                    actual = cpu if mode == 'flat' else state.cpu
                    if mode == 'checked':
                        actual.mem = C.cast(1, C.POINTER(C.c_uint8))
                        actual.mem_size = 2**64 - 1
                    functions[index](actual if mode == 'flat' else state)
                    label = f'{blocks.name} {mode} {text} seed={seed}'
                    if text.startswith('unsupported legacy'):
                        assert actual.exit == 10 and actual.r[15] == PC, label
                        assert list(actual.d) == list(CPU.from_buffer_copy(before).d), label
                        assert all(op == 3 for op, _, _ in accesses), label
                        continue
                    u.emu_start(PC, SIZE, count=1)
                    expected = [u.reg_read(reg) for reg in regs] + [u.reg_read(R.UC_ARM_REG_D0 + i) for i in range(16)]
                    expected += [u.reg_read(R.UC_ARM_REG_CPSR), u.reg_read(R.UC_ARM_REG_FPSCR)]
                    values = list(actual.r) + list(actual.d)[:16] + [actual.cpsr, actual.fpscr]
                    assert actual.exit == 0 and values == expected, (
                        label,
                        actual.exit,
                        [(i, hex(x), hex(y)) for i, (x, y) in enumerate(zip(values, expected)) if x != y],
                    )
                    assert list(actual.d)[16:] == upper and bytes(memory) == bytes(u.mem_read(0, SIZE)), label
                    total += 1
            # Rejected vector length/stride/trap controls cannot alter FP state.
            for control in [0x10000, 0x70000, 0x100000, 0x300000, 0x100, 0x8000]:
                cpu, memory = V.initial(words[0], 0)
                cpu.fpscr = control
                old = bytes(cpu)
                if mode == 'checked':
                    state = Checked()
                    C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                    access, accesses = bus(memory)
                    state.abi, state.access = CHECKED_ABI, access
                    actual = state.cpu
                else:
                    actual = cpu
                functions[0](actual if mode == 'flat' else state)
                assert actual.exit == 10 and actual.r[15] == PC and actual.fpscr == control
                assert list(actual.d) == list(CPU.from_buffer_copy(old).d)
            # LEN and STRIDE are written; an enabled trap, and AHP and QC, which VFPv2 lacks, stop first
            for control in [0x10000, 0x70000, 0x100000, 0x300000, 0x100, 0x8000, 0x4000000, 0x8000000]:
                index = names.index('vmsr fpscr,r0')
                cpu, memory = V.initial(words[index], 0)
                cpu.r[0], cpu.fpscr = control, 0
                if mode == 'checked':
                    state = Checked()
                    C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                    access, accesses = bus(memory)
                    state.abi, state.access = CHECKED_ABI, access
                    actual = state.cpu
                else:
                    actual = cpu
                functions[index](actual if mode == 'flat' else state)
                if control & 0x370000:
                    assert actual.exit == 0 and actual.r[15] == PC + 4 and actual.fpscr == control
                else:
                    assert actual.exit == 10 and actual.r[15] == PC and actual.fpscr == 0
            for text in ['condition=0 vadd.f32 s0,s2,s4', 'condition=0 vstr d15,[r4,#4]']:
                index = names.index(text)
                cpu, memory = V.initial(words[index], 0)
                cpu.fpscr = 0x10000
                cpu.r[4] = SIZE + 4  # EQ is false: no guard or bad data access
                old_d, old_memory = list(cpu.d), bytes(memory)
                if mode == 'checked':
                    state = Checked()
                    C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                    access, accesses = bus(memory, True)
                    state.abi, state.access = CHECKED_ABI, access
                    actual = state.cpu
                else:
                    actual = cpu
                functions[index](actual if mode == 'flat' else state)
                assert actual.exit == 0 and actual.r[15] == PC + 4 and actual.fpscr == 0x10000
                assert list(actual.d) == old_d and bytes(memory) == old_memory
                if mode == 'checked':
                    assert all(op == 3 for op, _, _ in accesses)
            if mode == 'checked':
                for text, register in [('vldr d15,[r4,#4]', 4), ('vstr d15,[r4,#4]', 4), ('vpop {d8-d15}', 13)]:
                    index = names.index(text)
                    for failure in ['range', 'permission', 'alignment']:
                        cpu, memory = V.initial(words[index], 0)
                        cpu.fpscr = 0
                        if failure == 'range':
                            cpu.r[register] = SIZE - 8 if register == 4 else SIZE - 32
                        if failure == 'alignment':
                            cpu.r[register] += 2
                        old_regs, old_d, old_memory = list(cpu.r), list(cpu.d), bytes(memory)
                        state = Checked()
                        C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                        access, accesses = bus(memory, failure == 'permission')
                        state.abi, state.access = CHECKED_ABI, access
                        state.cpu.mem = C.cast(1, C.POINTER(C.c_uint8))
                        state.cpu.mem_size = 2**64 - 1
                        functions[index](state)
                        assert state.cpu.exit == (7 if failure == 'alignment' else 6), (
                            text,
                            failure,
                            state.cpu.exit,
                        )
                        assert list(state.cpu.r) == old_regs and list(state.cpu.d) == old_d and not state.cpu.fpscr
                        assert bytes(memory) == old_memory
                        assert accesses[0][0] == 3 and all(op in (0x101, 0x102) for op, _, _ in accesses[1:])
            print(
                f'ARM11 VFPv2, {blocks.name}, {mode}: {total} exact comparisons; '
                'unsupported controls/wide transfers stop',
                flush=True,
            )
            blocks.close()


if __name__ == '__main__':
    main()
