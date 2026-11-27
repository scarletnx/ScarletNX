#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A32 VFP/NEON blocks of a native back end vs ARMv8 AArch32 (Unicorn 2.1.4).

Every GPR, D register, CPSR, FPSCR and memory after one instruction, by the blocks of this host's back end or (run under
Unicorn) another machine's, with all its host registers and with three, so that values spill. The instructions are
arm_vfp_cases.s's and arm_neon_cases.s's, whose words stand beside their text, and forms of them made here by their
fields. No host floating-point comparisons or tolerances. No skipped mismatches.
"""

import argparse
import ctypes as C
import pathlib
import random
import re

from arm_blocks import CPU, PC, SIZE, Blocks, add_variants, arm, variants
from arm_test import unicorn_dies

# a register's field: its four bits and the fifth (a D or Q register's highest, an S register's lowest)
FIELDS = {'d': (12, 22), 'n': (16, 7), 'm': (0, 5)}


def place(word, field, kind, number):
    """word with the register of kind (s, d or q) and number in field (d, n or m)"""
    four, fifth = FIELDS[field]
    if kind == 's':
        high, low = number >> 1, number & 1
    else:
        number <<= kind == 'q'
        high, low = number & 15, number >> 4
    return word & ~(15 << four | 1 << fifth) | high << four | low << fifth


REGISTER = re.compile(r'(?<![.\w])([sdq])(\d+)\b')
# the conditional words (VFP's) whose one vector register is not the d field's: VMOV from and to a core register and
# VDUP of one (n), VMOV from and to two (m, of S(n) and S(n+1) the first's); and the fixed-point VCVT's, which names it
# twice
ONE = {
    'n': ((0x0FE00F7F, 0x0E000A10), (0x0F900F5F, 0x0E800B10)),
    'm': ((0x0FE00ED0, 0x0C400A10),),
    'd': ((0x0FBA0E50, 0x0EBA0A40),),
}


def operands(word, text):
    """the vector registers text names, each (kind, number), with its field of word"""
    found = [(m[1], int(m[2])) for m in REGISTER.finditer(text)]
    if word >> 28 != 15:
        for field, forms in ONE.items():
            if any(word & mask == value for mask, value in forms):
                return [(found[0], field)]
    if len(found) == 3:  # a shift by a register is the one whose second operand is the last field
        return list(zip(found, 'dmn' if text.startswith(('vshl', 'vrshl', 'vqshl', 'vqrshl')) else 'dnm'))
    return list(zip(found, 'dm'))


def high(kind, number):
    """the register past the ones the cases name: D16 to D31 must not be confused with S32 to S63"""
    return (number + {'s': 16, 'd': 16, 'q': 8}[kind]) % {'s': 32, 'd': 32, 'q': 16}[kind]


def cases():
    """the words of the case files and their text: a line's instruction, then @ and its word"""
    here = pathlib.Path(__file__).parent
    for name in ('arm_vfp_cases.s', 'arm_neon_cases.s'):
        for line in (here / name).read_text().splitlines():
            line = line.strip()
            if not line or line[0] in './*' or line.startswith('Copyright'):
                continue
            text, _, word = line.partition('@')
            yield int(word, 16), text.strip()


def catalogue():
    """(word, text) of each instruction, once"""
    instructions = list(cases())
    # Register aliases and D16..D31 must not be confused with S32..S63.
    for word, text in list(instructions):
        if '{' not in text and '[' not in text and text.startswith(('v', 'aes', 'sha')):
            # By-element operands have restricted register ranges, leave them alone.
            other = word
            for (kind, number), field in operands(word, text):
                other = place(other, field, kind, high(kind, number))
            instructions.append((other, REGISTER.sub(lambda m: f'{m[1]}{high(m[1], int(m[2]))}', text)))
        named = re.findall(r'\b([dq]\d+)\b', text)
        if (
            len(named) in (2, 3)
            and '{' not in text
            and '[' not in text
            and not text.startswith(('vtrn', 'vzip', 'vuzp', 'vswp'))
        ):
            dest, source = named[0], named[-1]
            if dest[0] == source[0]:
                renamed = re.sub(r'\b' + dest + r'\b', source, text, count=1)
                instructions.append((place(word, 'd', source[0], int(source[1:])), renamed))
    same = {  # three registers of a length: bits 24, 11:8 and 4, and whether the shifted register is the last field's
        'vadd': (0xF2000800, 'i', False),
        'vsub': (0xF3000800, 'i', False),
        'vqadd': (0xF2000010, 'su', False),
        'vqsub': (0xF2000210, 'su', False),
        'vshl': (0xF2000400, 'su', True),
        'vrshl': (0xF2000500, 'su', True),
        'vqshl': (0xF2000410, 'su', True),
        'vqrshl': (0xF2000510, 'su', True),
    }
    for op, (base, signs, swapped) in same.items():
        for size in (8, 16, 32, 64):
            for sign in signs:
                for reg in ('d', 'q'):
                    word = base | (sign == 'u') << 24 | (size.bit_length() - 4) << 20 | (reg == 'q') << 6
                    for number, field in ((0, 'd'), (1, 'm' if swapped else 'n'), (2, 'n' if swapped else 'm')):
                        word = place(word, field, reg, number)
                    instructions.append((word, f'{op}.{sign}{size} {reg}0,{reg}1,{reg}2'))
    shifts = {  # by an immediate: bits 24 and 11:8, left
        'vshr': (0xF2800050, 'su', False),
        'vrshr': (0xF2800250, 'su', False),
        'vsra': (0xF2800150, 'su', False),
        'vrsra': (0xF2800350, 'su', False),
        'vshl': (0xF2800550, 'i', True),
        'vqshl': (0xF2800750, 'su', True),
        'vqshlu': (0xF2800650, 's', True),
        'vsli': (0xF3800550, 'i', True),
        'vsri': (0xF3800450, 'i', False),
    }
    for op, (base, signs, left) in shifts.items():
        for size in (8, 16, 32, 64):
            for amount in (0, size - 1) if left else (1, size):
                for sign in signs:
                    unsigned = sign == 'u' or op == 'vqshlu'  # VQSHLU is the one whose U says unsigned results
                    field = size + amount if left else 2 * size - amount  # imm6, whose leading one is the size's
                    # (64's L bit 7 alone, imm6 the amount itself)
                    word = base | unsigned << 24 | (size == 64) << 7 | (field - (64 if size == 64 else 0)) << 16
                    word = place(place(word, 'd', 'q', 0), 'm', 'q', 2)
                    instructions.append((word, f'{op}.{sign}{size} q0,q2,#{amount}'))
    # Single, multiple, structure, lane, replicate and post-indexed memory forms.
    posts = (('', 15), ('!', 13), (',r6', 6))
    for load in ('vld', 'vst'):
        memory = 0xF4040000 | (load == 'vld') << 21
        for count in range(1, 5):
            # the registers, and the bits 11:8 that say how many and how far apart
            lists = {
                1: [([4], 7), ([4, 5], 10), ([4, 5, 6], 6), ([4, 5, 6, 7], 2)],
                2: [([4, 5], 8), ([4, 6], 9), ([4, 5, 6, 7], 3)],
                3: [([4, 5, 6], 4), ([4, 6, 8], 5)],
                4: [([4, 5, 6, 7], 0), ([4, 6, 8, 10], 1)],
            }[count]
            for regs, kind in lists:
                for width in (8, 16, 32, 64) if count == 1 else (8, 16, 32):
                    names = ','.join(f'd{r}' for r in regs)
                    for post, index in posts:
                        for alignment in ('', ':64'):
                            word = memory | kind << 8 | (width.bit_length() - 4) << 6 | (alignment != '') << 4 | index
                            text = f'{load}{count}.{width} {{{names}}},[r4{alignment}]{post}'
                            instructions.append((place(word, 'd', 'd', 4), text))
            for size, width in enumerate((8, 16, 32)):
                lane = 64 // width - 1
                for stride in (1,) if count == 1 or width == 8 else (1, 2):
                    names = ','.join(f'd{4 + i * stride}[{lane}]' for i in range(count))
                    place_of = lane << 1 + size | (stride == 2) << size  # bits 7:4: the lane, then the stride's
                    for post, index in posts:
                        word = memory | 1 << 23 | size << 10 | (count - 1) << 8 | place_of << 4 | index
                        instructions.append((place(word, 'd', 'd', 4), f'{load}{count}.{width} {{{names}}},[r4]{post}'))
        if load == 'vld':
            for count in range(1, 5):
                for size, width in enumerate((8, 16, 32)):
                    for stride in (1, 2):
                        names = ','.join(f'd{4 + i * stride}[]' for i in range(count))
                        for post, index in posts:
                            word = memory | 1 << 23 | 0xC00 | (count - 1) << 8 | size << 6 | index
                            word |= (stride == 2 and count > 1) << 5
                            text = f'vld{count}.{width} {{{names}}},[r4]{post}'
                            instructions.append((place(word, 'd', 'd', 4), text))
            instructions += [
                (0xF4A44C6D, 'vld1.16 {d4[],d5[]},[r4]!'),
                (0xF4A44FFD, 'vld4.32 {d4[],d6[],d8[],d10[]},[r4:128]!'),
            ]
    once = {}
    for word, text in instructions:
        once.setdefault(text, word)
    return [(word, text) for text, word in once.items()]


EDGE32 = (
    0,
    0x80000000,
    1,
    0x007FFFFF,
    0x00800000,
    0x3F800001,
    0x3F800000,
    0x3F000000,
    0x7F800000,
    0xFF800000,
    0x7FC12345,
    0x7F812345,
    0xBF800000,
    0x7F7FFFFF,
    0x4EFFFFFF,
    0xCF000000,
)
EDGE64 = (
    0,
    0x8000000000000000,
    1,
    0xFFFFFFFFFFFFF,
    0x10000000000000,
    0x3FF0000000000001,
    0x3FF0000000000000,
    0x3FE0000000000000,
    0x7FF0000000000000,
    0xFFF0000000000000,
    0x7FF8123456789ABC,
    0x7FF0123456789ABC,
    0xBFF0000000000000,
    0x7FEFFFFFFFFFFFFF,
    0x41DFFFFFFFC00000,
    0xC1E0000000000000,
)


def initial(word, seed):
    rng = random.Random(seed)
    cpu = CPU()
    mem = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu.mem = mem
    cpu.mem_size = SIZE
    cpu.cpsr = (seed & 15) << 28 | 0x080A0010
    cpu.fpscr = (seed % 4) << 22 | ((seed // 4) % 16) << 24 | ((seed // 8) % 16) << 28
    if seed & 64:
        cpu.fpscr |= 0x9F  # cumulative exceptions must not be cleared
    for i in range(16):
        cpu.r[i] = 0x8000 + i * 16
    cpu.r[15] = PC
    for i in range(32):
        if seed < 32:
            cpu.d[i] = EDGE32[(seed + i) % 16] | EDGE32[(seed * 3 + i) % 16] << 32
        elif seed < 48:
            cpu.d[i] = EDGE64[(seed + i) % 16]
        elif seed < 64:
            # Identical byte/halfword/word signed endpoints, variable shifts and saturation.
            byte = (0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 129, 192, 224, 255)[(seed + i) % 16]
            cpu.d[i] = byte * 0x0101010101010101
        else:
            cpu.d[i] = rng.getrandbits(64)
    mem[PC : PC + 4] = word.to_bytes(4, 'little')
    return cpu, mem


def compare(libraries, words, names, states, every):
    import unicorn as U
    from unicorn import arm_const as A

    u = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    u.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
    u.mem_map(0, SIZE)
    regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]
    total, left = 0, []
    for index, (word, text) in enumerate(zip(words, names)):
        if unicorn_dies('arm', [word]):
            left.append(text)
            continue
        for seed in range(index % every, states, every):
            cpu, mem = initial(word, seed)
            u.mem_write(0, bytes(mem))
            u.ctl_remove_cache(PC, PC + 4)
            u.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
            u.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
            u.reg_write(A.UC_ARM_REG_FPSCR, cpu.fpscr)
            for r, value in zip(regs, cpu.r):
                u.reg_write(r, value)
            for i, value in enumerate(cpu.d):
                u.reg_write(A.UC_ARM_REG_D0 + i, value)
            u.emu_start(PC, SIZE, count=1)
            expected = [u.reg_read(r) for r in regs] + [u.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)]
            expected += [u.reg_read(A.UC_ARM_REG_CPSR), u.reg_read(A.UC_ARM_REG_FPSCR)]
            expected_mem = bytes(u.mem_read(0, SIZE))
            start, guest = bytes(cpu), bytes(mem)
            for blocks in libraries:
                C.memmove(C.byref(cpu), start, len(start))
                C.memmove(mem, guest, SIZE)
                blocks.functions[index](cpu)
                actual = list(cpu.r) + list(cpu.d) + [cpu.cpsr, cpu.fpscr]
                label = f'{blocks.name}: {word:08x} {text} seed={seed}'
                assert cpu.exit == 0, f'{label} exit={cpu.exit}'
                assert actual == expected, (
                    label
                    + ': '
                    + ', '.join(
                        f'field{i}: {x:016x} != {y:016x}' for i, (x, y) in enumerate(zip(actual, expected)) if x != y
                    )
                )
                assert bytes(mem) == expected_mem, label + ': memory differs'
                total += 1
    print(f'  {total} exact comparisons passed ({len(words) - len(left)} encodings)')
    if left:
        print(f'  {len(left)} left to another host, Unicorn 2.1.4 dying on them on this one: {", ".join(left)}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--blocks', required=True, help='snx-test-arm-blocks, for this host or another machine')
    p.add_argument('--states', type=int, default=80)
    p.add_argument('--every', type=int, default=1, help='of the states, one in so many, from the encoding on')
    add_variants(p)
    args = p.parse_args()
    words, names = zip(*catalogue())
    listed = arm([(word,) for word in words])
    libraries = [Blocks(args.blocks, listed, **variant).refuse() for variant in variants(p, args)]
    print(f'A32 VFP/NEON: {"; ".join(blocks.name for blocks in libraries)}', flush=True)
    compare(libraries, words, names, args.states, args.every)
    for blocks in libraries:
        blocks.close()


if __name__ == '__main__':
    main()
