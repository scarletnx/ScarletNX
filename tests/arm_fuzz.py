#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Random-encoding differential of a native back end's A32/Thumb blocks against Unicorn 2.1.4.

The catalogues of arm_test.py, arm_vector_test.py and thumb_test.py pick their encodings by hand. This draws
them at random from each encoding class and runs each from random state: GPRs as addresses or edge values, NZCV/Q/GE,
FPSCR's RMode/FZ/DN/AHP and cumulative bits, D registers full of NaNs (quiet and signalling, with payloads),
infinities, zeros and denormals. Exclusive pairs (LDREX* then STREX* of the same size) start from a cleared monitor in
both. The decoder has the Cortex-A57's encodings, none UNPREDICTABLE (Unicorn has no A57 model: its "max" CPU takes
v8.2's too), so a word it gives to the back end runs and the state must be Unicorn's.

    python3 tests/arm_fuzz.py --blocks build/tests/snx-test-arm-blocks-native

Disagreements are grouped by what the decoder says the word is: wrong state (both executed), refused (the back end made
no block), missing (ours undefined, Unicorn ran it), extra (ours ran, Unicorn raised), and different faults. Unicorn is
QEMU's translator, not the hardware: a disagreement is checked against the Arm ARM before either side is called wrong.
The missing and the extra are v8.2's and the UNPREDICTABLE encodings (RdLo == RdHi, writeback to the loaded register,
the PC where it isn't allowed, register lists past d31, should-be-zero bits set), where either answer is the Arm ARM's,
and QEMU 5 checks no alignment for LDRD, LDM, VLDM or VLDn, which the A57 faults on: the run fails on wrong state and on
refused blocks, and reports the rest. No game assets.

With --mc and --objdump (LLVM's, as the LLVM blocks are tested with: --blocks snx-test-arm-blocks-llvm --opt 2) their
verdict is had too: the words run are only those llvm-objdump has for the Cortex-A57, and a difference on one that
llvm-mc warns of (SoftFail: the Arm ARM calls it UNPREDICTABLE) is not counted. Where they are not there, it says so and
exits with 77.
"""

import argparse
import collections
import ctypes as C
import os
import pathlib
import random
import re
import subprocess
import sys
import tempfile

from arm_blocks import CPU, PC, SIZE, SKIP, Blocks
from arm_test import unicorn_dies
from thumb_test import catalogue as thumb_catalogue

EXITS = ('ok', 'undefined', 'thumb16-unsupported', 'thumb32-unsupported', 'svc', 'bkpt', 'data-abort',
         'alignment', 'bad-entry', 'endian', 'unsupported', 'prefetch-abort', 'code-changed', 'it-entry')
UNDEF = {1, 2, 3, 10}

# (isa, [(mask, value)]): the bits under mask are value, the others random. A32 words take cond AL three times
# in four.
CLASSES = {
    'a32-dp': ('arm', [(0x0C000000, 0x00000000)]),
    'a32-mul': ('arm', [(0x0F0000F0, 0x00000090), (0x0F8000F0, 0x01000000), (0x0F900090, 0x01000080)]),
    'a32-ldst': ('arm', [(0x0E000000, 0x04000000), (0x0E000010, 0x06000000), (0x0E000090, 0x00000090)]),
    'a32-media': ('arm', [(0x0E000010, 0x06000010), (0x0FF000F0, 0x07000010), (0x0F8000F0, 0x07800050)]),
    'a32-ldm': ('arm', [(0x0E000000, 0x08000000)]),
    'a32-misc': ('arm', [(0x0F900000, 0x01000000), (0x0FB00000, 0x03200000), (0xFF000000, 0xF5000000),
                         (0xFE000000, 0xF6000000)]),
    'a32-vfp': ('arm', [(0x0F000E10, 0x0E000A00), (0x0F000E10, 0x0E000A10), (0x0E000E00, 0x0C000A00),
                        (0xFF000E10, 0xFE000A00)]),
    'a32-neon': ('arm', [(0xFE000000, 0xF2000000)]),
    'a32-neon-fp': ('arm', [(0xFE800F10, 0xF2000D00), (0xFE800F10, 0xF2000F00), (0xFE800E10, 0xF2800900),
                            (0xFFB30600, 0xF3B30400), (0xFFB30E00, 0xF3B30600), (0xFFB30F00, 0xF3B30500),
                            (0xFE800E90, 0xF2800E10), (0xFFB00000, 0xF3B00000)]),
    'a32-neon-ls': ('arm', [(0xFF100000, 0xF4000000)]),
    't16': ('thumb16', [(0x0000, 0x0000)]),
    # any word; then by family, as few of any word are one: loads and stores, LDM and LDRD, data processing, branches
    # and control, register shifts and extends, multiplies, and the bitfields, MLA, MLS, TBB and TBH by their own bits
    't32': ('thumb32', [(0x00000000, 0x00000000), (0xFE000000, 0xF8000000), (0xFE000000, 0xE8000000),
                        (0xFE008000, 0xEA000000), (0xFA008000, 0xF0000000), (0xFA008000, 0xF2000000),
                        (0xF8008000, 0xF0008000), (0xFF000000, 0xFA000000), (0xFF800000, 0xFB000000),
                        (0xFF800000, 0xFB800000), (0xFFF08020, 0xF3400000), (0xFFF08020, 0xF3C00000),
                        (0xFFF08020, 0xF3600000), (0xFFF000F0, 0xFB000000), (0xFFF000F0, 0xFB000010),
                        (0xFFF0FFE0, 0xE8D0F000)]),
    't32-vfp': ('thumb32', [(0xEF000E10, 0xEE000A00), (0xEF000E10, 0xEE000A10), (0xEE000E00, 0xEC000A00),
                            (0xFF000E10, 0xFE000A00)]),
    't32-neon': ('thumb32', [(0xEF000000, 0xEF000000), (0xFF100000, 0xF9000000)]),
}
EXCLUSIVE = ('a32-exclusive', 't32-exclusive', 'a32-monitor', 't-it')

F32 = (0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001,
       0x7F800001, 0xFFA00000, 0x00000001, 0x807FFFFF, 0x00800000, 0x7F7FFFFF, 0x4F000000, 0xCF000000,
       0x3F000000, 0x3FC00000, 0x40490FDB, 0x4B800000, 0x34000000, 0x2F800000)
F64 = (0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
       0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000123, 0x7FF0000000000001, 0xFFF4000000000000,
       0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x41E0000000000000,
       0xC1E0000000000000, 0x3FE0000000000000, 0x3FF8000000000000, 0x43E0000000000000, 0x47EFFFFFE0000000,
       0x36A0000000000000, 0x3810000000000000, 0x41DFFFFFFFC00000)
F16 = (0x0000, 0x8000, 0x3C00, 0x7C00, 0xFC00, 0x7E00, 0x7C01, 0xFE01, 0x0001, 0x03FF, 0x0400, 0x7BFF)
INT = (0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678)


def draw(rng, mask, value, bits):
    return (rng.getrandbits(bits) & ~mask | value) & ((1 << bits) - 1)


def generate(rng, name, count):
    """Encodings of one class: A32 words, a Thumb16 halfword, or a Thumb32 word as hw1 << 16 | hw2."""
    isa, templates = CLASSES[name]
    out = []
    while len(out) < count:
        mask, value = rng.choice(templates)
        if isa == 'arm':
            word = draw(rng, mask, value, 32)
            if word >> 28 != 15 and rng.random() < 0.75:
                word = word & 0x0FFFFFFF | 0xE0000000
            out.append([word])
        elif isa == 'thumb16':
            half = rng.getrandbits(16)
            if half >> 11 < 0b11101:
                out.append([half])
        else:
            word = draw(rng, mask, value, 32)
            if word >> 27 >= 0b11101:
                out.append([word >> 16, word & 0xFFFF])
    return out


def exclusive_pairs(rng, isa, count):
    """LDREX*/LDAEX* then STREX*/STLEX* of the same size on the same base, as A32 words or Thumb halfwords."""
    out = []
    while len(out) < count:
        size = rng.choice(('word', 'double', 'byte', 'half'))
        acquire = rng.randrange(2)
        n = rng.choice((1, 2, 3, 12))
        t, t2, s, u, u2 = rng.sample([r for r in range(13) if r != n], 5)
        if isa == 'arm':
            # cccc 0001 1ss1 nnnn tttt 11a0 1001 1111, the store's status register in tttt and its Rt in the low bits
            t, u = t & ~1, u & ~1
            if s in (u, u + 1):
                continue
            ss = {'word': 0, 'double': 1, 'byte': 2, 'half': 3}[size]
            kind = 0xE90 if acquire else 0xF90
            out.append([0xE1900000 | ss << 21 | n << 16 | t << 12 | kind | 0xF,
                        0xE1800000 | ss << 21 | n << 16 | s << 12 | kind | u])
            continue
        if s in (u, u2):
            continue
        if size == 'word' and not acquire:  # T1, with an offset of imm8 words
            imm = rng.choice((0, 0, 1, 2, rng.randrange(256)))
            out.append([0xE850 | n, t << 12 | 0x0F00 | imm, 0xE840 | n, u << 12 | s << 8 | imm])
            continue
        # E8Dn tttt TTTT 01ss 1111 (11ss for acquire), TTTT the doubleword's second register or 1111
        op = {'byte': 0, 'half': 1, 'word': 2, 'double': 3}[size] | (0xC if acquire else 0x4)
        second, second_u = (t2, u2) if size == 'double' else (15, 15)
        out.append([0xE8D0 | n, t << 12 | second << 8 | op << 4 | 0xF, 0xE8C0 | n, u << 12 | second_u << 8 | op << 4 | s])
    return out


def monitor_sequences(rng, count):
    """A32 LDREX*, something that may change what the monitor holds, then STREX* on the same base."""
    out = []
    while len(out) < count:
        n, n2 = rng.sample((1, 2, 3, 12), 2)
        ss = rng.randrange(4)  # word, doubleword, byte, halfword
        store_ss = ss if rng.random() < 0.8 else rng.choice([x for x in range(4) if x != ss])
        load = 0xE1900F9F | ss << 21 | n << 16 | 4 << 12
        store = 0xE1800F90 | store_ss << 21 | n << 16 | 8 << 12 | 6
        middle = rng.choice((
            [],
            [0xF57FF01F],  # CLREX
            [0xE5800000 | n << 16 | 4 << 12],  # STR r4, [n]: the value it loaded
            [0xE5800000 | n << 16 | 10 << 12],  # STR r10, [n]: another
            [0xE5C00000 | n << 16 | 10 << 12],  # STRB r10, [n]
            [0xE1900F9F | n2 << 16 | 11 << 12],  # LDREX r11, [n2]
            [0xE1D00F9F | n << 16 | 11 << 12],  # LDREXB r11, [n]
            [0xF57FF05B],  # DMB ISH
            [0xE2800001 | 10 << 16 | 10 << 12],  # ADD r10, r10, #1
        ))
        out.append([load] + middle + [store])
    return out


def implementation_defined(name, item):
    """A monitor sequence whose STREX may pass or fail: a size other than the last LDREX's (CONSTRAINED
    UNPREDICTABLE), or an ordinary store between them, which the backend lets clear the monitor (writemem in llvm.c)."""
    if name != 'a32-monitor':
        return False
    last = [w for w in item[:-1] if w & 0x0F900FFF == 0x01900F9F][-1]
    return (last >> 21 & 3) != (item[-1] >> 21 & 3) or any(w & 0x0E100000 == 0x04000000 for w in item[1:-1])


def instructions(isa, item):
    """how many of them the item has: A32 words, Thumb halfwords, two of each T32 word"""
    if isa == 'arm':
        return len(item)
    count = i = 0
    while i < len(item):
        i += 2 if item[i] >> 11 >= 0b11101 else 1
        count += 1
    return count


# what may not be in an IT block or depends on where it is: a branch, the PC, a hint, the system's
NOT_IN_IT = ('UNDEFINED', 'BRANCH', 'TABLE', 'IT', 'HINT', 'LEGACY_BL', 'A32.SVC', 'A32.BKPT', 'A32.BX', 'A32.SETEND',
             'A32.HINT', 'A32.COPROC', 'A32.TLS')


def allowed_in_it(item, name):
    if name.endswith('/pc') or name in NOT_IN_IT:
        return False
    if unpredictable('thumb', item) or writes_e('thumb', item):  # MSR ends the block where it writes E
        return False
    if len(item) == 2:
        return True
    # LDR (literal), ADR, CPS and SETEND, POP with the PC in its list, MOVS (UNPREDICTABLE there)
    return item[0] >> 11 not in (0b01001, 0b10100) and item[0] >> 8 not in (0xB6, 0xBD) and item[0] >> 6 != 0


def it_blocks(rng, runner, count, tools):
    """An IT instruction and as many Thumb instructions as its mask covers, none of them a branch: drawn at random from
    the encodings, and from the hand-picked ones of thumb_test.py, that the decoder has (and llvm-objdump, with
    tools)."""
    pool = generate(rng, 't16', 3000) + generate(rng, 't32', 12000)
    pool += [hws for _, n, hws in thumb_catalogue() if n == 1]
    blocks = Blocks(runner, [('thumb', PC, item) for item in pool])
    texts = disassemble(tools, 'thumb', pool) if tools else pool
    picked = [
        item for item, names, text in zip(pool, blocks.names, texts) if names and text and allowed_in_it(item, names[0])
    ]
    blocks.close()
    out = []
    while len(out) < count:
        firstcond = rng.randrange(14)
        mask = rng.randrange(1, 16)
        n = 5 - (mask & -mask).bit_length()
        out.append([0xBF00 | firstcond << 4 | mask] + [h for item in rng.choices(picked, k=n) for h in item])
    return out


SLOT = 32


def disassemble(tools, isa, items):
    """Each item's text, or None for an encoding the Cortex-A57 doesn't have. llvm-mc's disassembler resynchronizes
    a byte at a time after an invalid Thumb32 encoding, so the items are assembled as .inst directives into slots of
    their own and disassembled by llvm-objdump, which steps over an unknown one by its size."""
    mc, objdump = tools
    triple = 'armv8a-linux-gnueabi' if isa == 'arm' else 'thumbv8a-linux-gnueabi'
    source = ['.syntax unified', '.arm' if isa == 'arm' else '.thumb']
    for item in items:
        source.append(f'.balign {SLOT}')
        i = 0
        while i < len(item):
            if isa == 'arm':
                source.append(f'.inst 0x{item[i]:08x}')
                i += 1
            elif item[i] >> 11 >= 0b11101:
                source.append(f'.inst.w 0x{item[i]:04x}{item[i + 1]:04x}')
                i += 2
            else:
                source.append(f'.inst.n 0x{item[i]:04x}')
                i += 1
    with tempfile.TemporaryDirectory(prefix='snx-arm-fuzz-mc-') as path:
        asm, obj = pathlib.Path(path) / 'items.s', pathlib.Path(path) / 'items.o'
        asm.write_text('\n'.join(source) + '\n')
        subprocess.run(
            [mc, f'-triple={triple}', '-mcpu=cortex-a57', '-filetype=obj', str(asm), '-o', str(obj)], check=True
        )
        listing = subprocess.run(
            [objdump, '-d', '--mcpu=cortex-a57', '--no-show-raw-insn', str(obj)],
            check=True,
            text=True,
            capture_output=True,
        ).stdout
    found = collections.defaultdict(list)
    for line in listing.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s*(.*)$', line)
        if m:
            address = int(m.group(1), 16)
            found[address // SLOT].append((address % SLOT, m.group(2).strip().split('@')[0].strip()))
    out = []
    width = 4 if isa == 'arm' else 2
    for index, item in enumerate(items):
        texts = [t for offset, t in found[index] if offset < width * len(item)]
        out.append(None if not texts or any(t == '<unknown>' for t in texts) else '; '.join(texts))
    return out


def soft_fail(mc, isa, item):
    """llvm-mc's warning for an encoding it decodes but the Arm ARM calls UNPREDICTABLE (SoftFail)."""
    data = b''.join(w.to_bytes(4 if isa == 'arm' else 2, 'little') for w in item)
    triple = 'armv8a-linux-gnueabi' if isa == 'arm' else 'thumbv8a-linux-gnueabi'
    proc = subprocess.run(
        [mc, '--disassemble', f'-triple={triple}', '-mcpu=cortex-a57'],
        input=' '.join(f'0x{b:02x}' for b in data) + '\n',
        text=True,
        capture_output=True,
        check=False,  # an encoding it has not is not a SoftFail
    )
    return 'potentially undefined' in proc.stderr


def a57_words(tools, isa, items):
    """the items llvm-objdump has for the Cortex-A57"""
    return [item for item, text in zip(items, disassemble(tools, isa, items)) if text]


def uncounted(tools, isa, item):
    """whether a difference on the item is left uncounted: the Arm ARM leaves its result open (unpredictable), or, with
    LLVM's tools, llvm-mc warns of it (SoftFail)"""
    return unpredictable(isa, item) or bool(tools and soft_fail(tools[0], isa, item))


def unpredictable(isa, item):
    """A word the decoder has whose result the Arm ARM leaves open, so that either answer is its: VSWP, VTRN, VUZP or
    VZIP of one register with itself (UNKNOWN), and T32's data processing (of a shifted register, a modified or a plain
    immediate) into the PC, MUL, MLA or MLS of it, and a byte's or a halfword's load of it (UNPREDICTABLE; the rest of
    those the decoder refuses)."""
    if isa == 'arm':
        if len(item) != 1:
            return False
        word = item[0]
    elif len(item) == 2 and item[0] >> 11 >= 0b11101:
        word = item[0] << 16 | item[1]
        first, second = item
        into_pc = (second >> 8 & 15) == 15 and not second & 0x8000
        if first >> 11 == 0b11110 or first >> 9 == 0b1110101:
            plain = first >> 11 == 0b11110 and first & 0x200 != 0  # no flags: no TST, TEQ, CMN or CMP, whose Rd is 15
            return into_pc and (plain or not first & 0x10 or first >> 5 & 15 not in (0, 4, 8, 13))
        if first & 0xFE10 == 0xF810 and first >> 5 & 3 != 2 and second >> 12 == 15:
            # a byte's or a halfword's load of it, but for PLD, PLDW and PLI: imm12, a register, imm8 down
            return not first & 0x80 and second & 0x0800 != 0 and second & 0x0F00 != 0x0C00
        if first & 0xFFF0 == 0xFB00:  # MUL, MLA (Ra 15), MLS
            regs = [first & 15, second >> 8 & 15, second & 15] + [second >> 12] * (second & 0x10 != 0)
            return 15 in regs
        if first >> 8 not in (0xEF, 0xFF):
            return False
        word = word & 0x00FFFFFF | (0xF3000000 if first >> 8 == 0xFF else 0xF2000000)  # NEON's as A32's
    else:
        return False
    same = (word >> 12 & 15 | word >> 18 & 16) == (word & 15 | word >> 1 & 16)
    return word & 0xFFB30E10 == 0xF3B20000 and same


def writes_e(isa, item):
    """MSR to CPSR's x field, which has E: EL0 writes it (CPSRWriteByInstr), Unicorn (QEMU's user mask) doesn't, and
    arm_test.py's msr_x witness has it"""
    if isa == 'arm':
        return len(item) == 1 and item[0] & 0x0DF00000 == 0x01200000 and item[0] & 0x00020000 != 0
    return len(item) == 2 and item[0] & 0xFFF0 == 0xF380 and item[1] & 0xF0FF == 0x8000 and item[1] & 0x0200 != 0


def special(rng):
    kind = rng.randrange(6)
    if kind == 0:
        return rng.choice(F32) | rng.choice(F32) << 32
    if kind == 1:
        return rng.choice(F64)
    if kind == 2:
        return sum(rng.choice(F16) << 16 * i for i in range(4))
    if kind == 3:
        return rng.choice(INT) | rng.choice(INT) << 32
    if kind == 4:
        return rng.choice((0x01, 0x7F, 0x80, 0xFF, 0x08, 0x10, 0x1F, 0x20, 0xE0)) * 0x0101010101010101
    return rng.getrandbits(64)


def initial(rng, isa, item):
    thumb = isa != 'arm'
    cpu = CPU()
    mem = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu.mem = mem
    cpu.mem_size = SIZE
    cpu.cpsr = rng.getrandbits(4) << 28 | rng.getrandbits(1) << 27 | rng.getrandbits(4) << 16 | thumb << 5 | 0x10
    cpu.fpscr = rng.getrandbits(4) << 28 | rng.getrandbits(1) << 27 | rng.getrandbits(3) << 24
    cpu.fpscr |= rng.getrandbits(2) << 22 | rng.choice((0, 0, 0x9F, rng.getrandbits(8) & 0x9F))
    addresses = rng.random() < 0.6
    for i in range(15):
        if addresses and rng.random() < 0.85:
            cpu.r[i] = 0x2000 + rng.randrange(0xC000) & ~rng.choice((0, 3, 7, 15))
        else:
            cpu.r[i] = rng.choice(INT) if rng.random() < 0.4 else rng.getrandbits(32)
    cpu.r[15] = PC
    for i in range(32):
        cpu.d[i] = special(rng)
    width = 2 if thumb else 4
    mem[PC - 4 : PC] = bytes((0xBF, 0xF3, 0x2F, 0x8F)) if thumb else (0xF57FF01F).to_bytes(4, 'little')  # CLREX
    for i, w in enumerate(item):
        mem[PC + width * i : PC + width * (i + 1)] = w.to_bytes(width, 'little')
    return cpu, mem


class Unicorn:
    def __init__(self, thumb):
        import unicorn as U
        from unicorn import arm_const as A

        self.U, self.A, self.thumb = U, A, thumb
        self.uc = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_THUMB if thumb else U.UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
        self.uc.mem_map(0, SIZE)
        self.regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)]
        self.regs += [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]

    def run(self, cpu, mem, count, end):
        """Run the CLREX before the code, then count instructions or up to end (Unicorn doesn't count an IT).
        Returns Unicorn's error or None."""
        A, uc = self.A, self.uc
        uc.mem_write(0, bytes(mem))
        uc.ctl_remove_cache(0, SIZE)
        uc.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        uc.reg_write(A.UC_ARM_REG_FPSCR, cpu.fpscr)
        for r, value in zip(self.regs, cpu.r):
            uc.reg_write(r, value)
        uc.reg_write(A.UC_ARM_REG_PC, (PC - 4) | self.thumb)
        for i, value in enumerate(cpu.d):
            uc.reg_write(A.UC_ARM_REG_D0 + i, value)
        try:
            uc.emu_start((PC - 4) | self.thumb, end, count=count + 1)
        except self.U.UcError as exc:
            return str(exc)
        return None

    def state(self):
        A, uc = self.A, self.uc
        values = [uc.reg_read(r) for r in self.regs] + [uc.reg_read(A.UC_ARM_REG_CPSR)]
        values += [uc.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)] + [uc.reg_read(A.UC_ARM_REG_FPSCR)]
        return values


FIELDS = [f'r{i}' for i in range(16)] + ['cpsr'] + [f'd{i}' for i in range(32)] + ['fpscr']


def fault_kind(error):
    if error is None or 'FETCH_UNMAPPED' in error:
        return 'ok'  # an indirect branch completed, to a random target
    if 'UNMAPPED' in error or 'PROT' in error:
        return 'abort'
    return 'exception'


def agree(ours, theirs):
    if ours == 0:
        return theirs == 'ok'
    if ours in UNDEF or ours in (4, 5):
        return theirs == 'exception'
    if ours in (6, 7):
        return theirs in ('abort', 'exception')
    return False


def key_of(name, names):
    if name == 't-it':
        return 'it-block'
    if name.endswith('exclusive') or name == 'a32-monitor':
        return '/'.join(names)
    return names[0]


def run_one(fn, uni, isa, item, rng):
    """One state: (kind, detail), kind pass, endian, wrong, missing, extra, fault or alignment."""
    cpu, mem = initial(rng, isa, item)
    before = list(cpu.r) + [cpu.cpsr] + list(cpu.d) + [cpu.fpscr]
    error = uni.run(cpu, mem, instructions(isa, item), PC + (2 if isa != 'arm' else 4) * len(item))
    theirs = fault_kind(error)
    fn(cpu)
    ours = cpu.exit
    if ours == 9:
        return 'endian', ''
    if not agree(ours, theirs):
        kind = 'missing' if ours in UNDEF and theirs == 'ok' else 'extra' if ours == 0 else 'fault'
        if kind == 'fault' and ours == 7 and theirs == 'ok':
            kind = 'alignment'  # QEMU 5, Unicorn's base, doesn't check LDRD/LDM/VLDM alignment
        return kind, f'ours={EXITS[ours]} syndrome={cpu.syndrome:x} unicorn={error}'
    if ours != 0 or theirs != 'ok':
        return 'pass', ''
    actual = list(cpu.r) + [cpu.cpsr] + list(cpu.d) + [cpu.fpscr]
    expected = uni.state()
    if writes_e(isa, item):
        actual[16] &= ~0x200
        expected[16] &= ~0x200
    diffs = [f'{FIELDS[i]}: {b:x} -> {a:x} (unicorn {e:x})'
             for i, (b, a, e) in enumerate(zip(before, actual, expected)) if a != e]
    if diffs:
        return 'wrong', f'cpsr0={before[16]:08x} fpscr0={before[49]:08x} ' + ', '.join(diffs[:6])
    if error is None:
        theirs_mem = bytes(uni.uc.mem_read(0, SIZE))
        if bytes(mem) != theirs_mem:
            at = next(i for i in range(SIZE) if mem[i] != theirs_mem[i])
            return 'wrong', f'memory at {at:x}: {bytes(mem[at:at + 8]).hex()} (unicorn {theirs_mem[at:at + 8].hex()})'
    return 'pass', ''


def items_of(args, name, tools):
    rng = random.Random(f'{args.seed}:{name}')
    if name == 'a32-exclusive':
        return 'arm', exclusive_pairs(rng, 'arm', args.count)
    if name == 't32-exclusive':
        return 'thumb', exclusive_pairs(rng, 'thumb', args.count)
    if name == 'a32-monitor':
        return 'arm', monitor_sequences(rng, args.count)
    if name == 't-it':
        return 'thumb', it_blocks(rng, args.blocks, args.count, tools)
    isa = 'arm' if CLASSES[name][0] == 'arm' else 'thumb'
    return isa, generate(rng, name, args.count)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--blocks', required=True, help='snx-test-arm-blocks, for this host or another machine')
    p.add_argument('--class', dest='classes', action='append', choices=sorted(CLASSES) + list(EXCLUSIVE))
    p.add_argument('--count', type=int, default=2000, help='encodings drawn per class')
    p.add_argument('--states', type=int, default=6, help='random states per encoding')
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--pool', type=int, help="a native back end's: the host registers its values are in (0: all)")
    p.add_argument('--opt', type=int, choices=(0, 2), help="LLVM's runner's: the passes' -O level")
    p.add_argument('--mc', help="llvm-mc, for LLVM's verdict (with --objdump)")
    p.add_argument('--objdump', help='llvm-objdump, which keeps the words the Cortex-A57 has')
    p.add_argument('--examples', type=int, default=3)
    p.add_argument('--only', metavar='ISA:HEX[,HEX...]',
                   help='one encoding (arm:e1a00001, thumb:4408 or thumb:e850,0f00), every state printed')
    args = p.parse_args()
    if bool(args.mc) != bool(args.objdump):
        p.error(f"--mc {args.mc} and --objdump {args.objdump}: LLVM's verdict takes both")
    tools = (args.mc, args.objdump) if args.mc else None
    for tool in tools or ():
        if not os.access(tool, os.X_OK):
            print(f"SKIP: {tool} is not there, which LLVM's verdict needs (the LLVM this was built with has none)")
            return SKIP
    if args.only:
        isa, words = args.only.split(':')
        item = [int(w, 16) for w in words.split(',')]
        blocks = Blocks(args.blocks, [(isa, PC, item)], pool=args.pool, opt=args.opt)
        blocks.refuse()
        uni = Unicorn(isa != 'arm')
        print(blocks.name, ' '.join(blocks.names[0]), *(disassemble(tools, isa, [item]) if tools else ()))
        for state in range(args.states):
            rng = random.Random(f'{args.seed}:only:{state}')
            cpu, _ = initial(random.Random(f'{args.seed}:only:{state}'), isa, item)
            kind, detail = run_one(blocks.functions[0], uni, isa, item, rng)
            print(f'  {state}: {kind} {detail}')
            if kind != 'pass':
                print('     r=' + ' '.join(f'{v:x}' for v in cpu.r) + f' cpsr={cpu.cpsr:x} fpscr={cpu.fpscr:x}')
                print('     d=' + ' '.join(f'{v:x}' for v in cpu.d))
        return 0
    wrong = 0
    for name in args.classes or list(CLASSES) + list(EXCLUSIVE):
        isa, items = items_of(args, name, tools)
        items = list({tuple(item): item for item in items}.values())
        drawn = len(items)
        if tools:
            items = a57_words(tools, isa, items)
        blocks = Blocks(args.blocks, [(isa, PC, item) for item in items], pool=args.pool, opt=args.opt)
        uni = Unicorn(isa != 'arm')
        report = collections.defaultdict(lambda: collections.defaultdict(list))
        runs = 0
        for index, (item, fn) in enumerate(zip(items, blocks.functions)):
            text = f'{" ".join(blocks.names[index])}  [{isa}:{",".join(f"{w:x}" for w in item)}]'
            if fn is None:
                report['refused']['refused'].append(f'{text} {blocks.why[index]}')
                continue
            entry = report[key_of(name, blocks.names[index])]
            if unicorn_dies(isa, item):  # left to another host, as arm_test.py says
                entry['unicorn-dies'].append(text)
                continue
            for state in range(args.states):
                rng = random.Random(f'{args.seed}:{name}:{index}:{state}')
                kind, detail = run_one(fn, uni, isa, item, rng)
                runs += 1
                if kind != 'pass' and implementation_defined(name, item):
                    kind = 'impl-defined'
                elif kind in ('wrong', 'missing', 'extra', 'fault') and uncounted(tools, isa, item):
                    kind = 'unpredictable'
                entry[kind].append(f'{text} {detail}')
                if kind not in ('pass', 'endian'):
                    break
        blocks.close()
        kinds = ('wrong', 'refused', 'missing', 'extra', 'fault', 'alignment', 'unpredictable', 'impl-defined',
                 'unicorn-dies')
        bad = {k: v for k, v in report.items() if any(v[x] for x in kinds)}
        kept = (
            f' ({drawn - len(items)} of {drawn} drawn left out: llvm-objdump has no A57 instruction)' if tools else ''
        )
        print(f'== {name}: {len(items)} encodings{kept}, {runs} runs, {len(bad)} of their kinds disagree', flush=True)
        for said in sorted(bad, key=lambda m: -sum(len(bad[m][x]) for x in kinds)):
            entry = bad[said]
            summary = ', '.join(f'{x} {len(entry[x])}' for x in kinds + ('endian',) if entry[x])
            print(f'  {said}: {summary} (of {sum(len(v) for v in entry.values())})')
            for x in kinds:
                for line in entry[x][: args.examples]:
                    print(f'    {x}: {line}')
        wrong += sum(len(v['wrong']) + len(v['refused']) for v in bad.values())
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
