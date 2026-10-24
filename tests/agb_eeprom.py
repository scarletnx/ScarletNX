#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A game's EEPROM save, as its DMA talks to it a bit a halfword at 0x0D000000: a write of block 5 on the 512 byte chip,
the chip busy (0) right after it and ready (1) later, its read (4 bits that mean nothing, then the block), then a
write and a read of block 0x123, whose 14 bits of number make it the 8 KiB chip. The registers end as mGBA 0.11's do
(r4 r5 and r8 r9 the blocks read, r6 the 4 bits, r7 the busy read), and the game's .sav is the 8 KiB chip with both
blocks in it, most significant bit first. A second ROM of the same name that only reads block 0x123 back reads it
from that .sav, which the runner loads as the 8 KiB chip's.

The ROM's code, its EEPROM_V tag at its end, assembled with llvm-mc for armv4t at 0x080000C0:

        .arm
        .text
    start:
        ldr   r10, =0x02000000
        ldr   r11, =0x040000D4
        mov   r0, r10                @ a write of block 5: 1 0, 6 bits, 64 bits, 0
        mov   r1, #1
        strh  r1, [r0], #2
        mov   r1, #0
        strh  r1, [r0], #2
        mov   r2, #5
        mov   r3, #6
        mov   lr, pc
        b     put_bits
        ldr   r2, =0x12345678
        mov   r3, #32
        mov   lr, pc
        b     put_bits
        ldr   r2, =0x9ABCDEF0
        mov   r3, #32
        mov   lr, pc
        b     put_bits
        mov   r1, #0
        strh  r1, [r0], #2
        mov   r0, #73
        mov   lr, pc
        b     dma_to_eeprom
        ldr   r1, =0x0D000000
        ldrh  r7, [r1]               @ busy: 0
    wait1:
        ldrh  r2, [r1]
        tst   r2, #1
        beq   wait1
        mov   r0, r10                @ a read of block 5: 1 1, 6 bits, 0
        mov   r1, #1
        strh  r1, [r0], #2
        strh  r1, [r0], #2
        mov   r2, #5
        mov   r3, #6
        mov   lr, pc
        b     put_bits
        mov   r1, #0
        strh  r1, [r0], #2
        mov   r0, #9
        mov   lr, pc
        b     dma_to_eeprom
        mov   r0, #68
        mov   lr, pc
        b     dma_from_eeprom
        add   r0, r10, #0x200
        mov   r6, #0                 @ the 4 bits before the data, ORed
        mov   r3, #4
    junk:
        ldrh  r2, [r0], #2
        orr   r6, r6, r2
        subs  r3, r3, #1
        bne   junk
        mov   lr, pc
        b     pack32
        mov   r4, r1
        mov   lr, pc
        b     pack32
        mov   r5, r1
        mov   r0, r10                @ a write of block 0x123: 14 bits of number
        mov   r1, #1
        strh  r1, [r0], #2
        mov   r1, #0
        strh  r1, [r0], #2
        ldr   r2, =0x123
        mov   r3, #14
        mov   lr, pc
        b     put_bits
        ldr   r2, =0xCAFEBABE
        mov   r3, #32
        mov   lr, pc
        b     put_bits
        ldr   r2, =0x0BADF00D
        mov   r3, #32
        mov   lr, pc
        b     put_bits
        mov   r1, #0
        strh  r1, [r0], #2
        mov   r0, #81
        mov   lr, pc
        b     dma_to_eeprom
        ldr   r1, =0x0D000000
    wait2:
        ldrh  r2, [r1]
        tst   r2, #1
        beq   wait2
        mov   r0, r10                @ its read
        mov   r1, #1
        strh  r1, [r0], #2
        strh  r1, [r0], #2
        ldr   r2, =0x123
        mov   r3, #14
        mov   lr, pc
        b     put_bits
        mov   r1, #0
        strh  r1, [r0], #2
        mov   r0, #17
        mov   lr, pc
        b     dma_to_eeprom
        mov   r0, #68
        mov   lr, pc
        b     dma_from_eeprom
        add   r0, r10, #0x208
        mov   lr, pc
        b     pack32
        mov   r8, r1
        mov   lr, pc
        b     pack32
        mov   r9, r1
    spin:
        b     spin
    put_bits:                        @ r3 bits of r2 at r0, most significant first
        subs  r3, r3, #1
        bxmi  lr
        mov   r1, r2, lsr r3
        and   r1, r1, #1
        strh  r1, [r0], #2
        b     put_bits
    pack32:                          @ 32 halfwords at r0 into r1, the first the most significant
        mov   r1, #0
        mov   r3, #32
    pack:
        ldrh  r2, [r0], #2
        and   r2, r2, #1
        orr   r1, r2, r1, lsl #1
        subs  r3, r3, #1
        bne   pack
        bx    lr
    dma_to_eeprom:                   @ r0 halfwords from r10
        str   r10, [r11]
        ldr   r1, =0x0D000000
        str   r1, [r11, #4]
        orr   r1, r0, #0x80000000
        str   r1, [r11, #8]
        bx    lr
    dma_from_eeprom:                 @ r0 halfwords to r10 + 0x200
        ldr   r1, =0x0D000000
        str   r1, [r11]
        add   r1, r10, #0x200
        str   r1, [r11, #4]
        orr   r1, r0, #0x80000000
        str   r1, [r11, #8]
        bx    lr
        .ltorg
        .ascii "EEPROM_V124\0"
        .align 2
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

CODE = [
    0xE3A0A402, 0xE59FB210, 0xE1A0000A, 0xE3A01001, 0xE0C010B2, 0xE3A01000, 0xE0C010B2, 0xE3A02005,
    0xE3A03006, 0xE1A0E00F, 0xEA000060, 0xE59F21EC, 0xE3A03020, 0xE1A0E00F, 0xEA00005C, 0xE59F21E0,
    0xE3A03020, 0xE1A0E00F, 0xEA000058, 0xE3A01000, 0xE0C010B2, 0xE3A00049, 0xE1A0E00F, 0xEA000061,
    0xE3A0140D, 0xE1D170B0, 0xE1D120B0, 0xE3120001, 0x0AFFFFFC, 0xE1A0000A, 0xE3A01001, 0xE0C010B2,
    0xE0C010B2, 0xE3A02005, 0xE3A03006, 0xE1A0E00F, 0xEA000046, 0xE3A01000, 0xE0C010B2, 0xE3A00009,
    0xE1A0E00F, 0xEA00004F, 0xE3A00044, 0xE1A0E00F, 0xEA000052, 0xE28A0C02, 0xE3A06000, 0xE3A03004,
    0xE0D020B2, 0xE1866002, 0xE2533001, 0x1AFFFFFB, 0xE1A0E00F, 0xEA00003B, 0xE1A04001, 0xE1A0E00F,
    0xEA000038, 0xE1A05001, 0xE1A0000A, 0xE3A01001, 0xE0C010B2, 0xE3A01000, 0xE0C010B2, 0xE59F2124,
    0xE3A0300E, 0xE1A0E00F, 0xEA000028, 0xE59F2118, 0xE3A03020, 0xE1A0E00F, 0xEA000024, 0xE59F210C,
    0xE3A03020, 0xE1A0E00F, 0xEA000020, 0xE3A01000, 0xE0C010B2, 0xE3A00051, 0xE1A0E00F, 0xEA000029,
    0xE3A0140D, 0xE1D120B0, 0xE3120001, 0x0AFFFFFC, 0xE1A0000A, 0xE3A01001, 0xE0C010B2, 0xE0C010B2,
    0xE59F20C0, 0xE3A0300E, 0xE1A0E00F, 0xEA00000F, 0xE3A01000, 0xE0C010B2, 0xE3A00011, 0xE1A0E00F,
    0xEA000018, 0xE3A00044, 0xE1A0E00F, 0xEA00001B, 0xE28A0F82, 0xE1A0E00F, 0xEA00000A, 0xE1A08001,
    0xE1A0E00F, 0xEA000007, 0xE1A09001, 0xEAFFFFFE, 0xE2533001, 0x412FFF1E, 0xE1A01332, 0xE2011001,
    0xE0C010B2, 0xEAFFFFF9, 0xE3A01000, 0xE3A03020, 0xE0D020B2, 0xE2022001, 0xE1821081, 0xE2533001,
    0x1AFFFFFA, 0xE12FFF1E, 0xE58BA000, 0xE3A0140D, 0xE58B1004, 0xE3801102, 0xE58B1008, 0xE12FFF1E,
    0xE3A0140D, 0xE58B1000, 0xE28A1C02, 0xE58B1004, 0xE3801102, 0xE58B1008, 0xE12FFF1E, 0x040000D4,
    0x12345678, 0x9ABCDEF0, 0x00000123, 0xCAFEBABE, 0x0BADF00D, 0x52504545, 0x565F4D4F, 0x00343231,
]  # fmt: skip

# the second ROM: the first's start replaced by a read of block 0x123 into r8 and r9, its subroutines and tag the same
READ_CODE = [
    0xE3A0A402, 0xE59FB0C8, 0xE1A0000A, 0xE3A01001, 0xE0C010B2, 0xE0C010B2, 0xE59F20B8, 0xE3A0300E,
    0xE1A0E00F, 0xEA00000F, 0xE3A01000, 0xE0C010B2, 0xE3A00011, 0xE1A0E00F, 0xEA000018, 0xE3A00044,
    0xE1A0E00F, 0xEA00001B, 0xE28A0F82, 0xE1A0E00F, 0xEA00000A, 0xE1A08001, 0xE1A0E00F, 0xEA000007,
    0xE1A09001, 0xEAFFFFFE, 0xE2533001, 0x412FFF1E, 0xE1A01332, 0xE2011001, 0xE0C010B2, 0xEAFFFFF9,
    0xE3A01000, 0xE3A03020, 0xE0D020B2, 0xE2022001, 0xE1821081, 0xE2533001, 0x1AFFFFFA, 0xE12FFF1E,
    0xE58BA000, 0xE3A0140D, 0xE58B1004, 0xE3801102, 0xE58B1008, 0xE12FFF1E, 0xE3A0140D, 0xE58B1000,
    0xE28A1C02, 0xE58B1004, 0xE3801102, 0xE58B1008, 0xE12FFF1E, 0x040000D4, 0x00000123, 0x52504545,
    0x565F4D4F, 0x00343231,
]  # fmt: skip

EXPECTED = {'r4': 0x12345678, 'r5': 0x9ABCDEF0, 'r6': 0, 'r7': 0, 'r8': 0xCAFEBABE, 'r9': 0x0BADF00D}
BLOCKS = {5: bytes.fromhex('123456789ABCDEF0'), 0x123: bytes.fromhex('CAFEBABE0BADF00D')}


def rom(code):
    """A cartridge the board takes (its branch, fixed byte and complement), code at 0x080000C0."""
    image = bytearray(0xC0)
    image[0:4] = (0xEA00002E).to_bytes(4, 'little')  # b 0x080000C0
    image[0xA0:0xAC] = b'EEPROM'.ljust(12, b'\0')
    image[0xAC:0xB2] = b'AEEP01'
    image[0xB2] = 0x96
    image[0xBD] = -(sum(image[0xA0:0xBD]) + 0x19) & 0xFF
    for word in code:
        image += word.to_bytes(4, 'little')
    return bytes(image + bytes(0x40))  # room after the tag, as a game's ROM has


def registers(runner, work, code):
    """the registers the runner ends code's ROM with, eeprom.gba in work"""
    (work / 'eeprom.gba').write_bytes(rom(code))
    run = subprocess.run(
        [runner, '--frames=30', '--registers', '--outdir', str(work), str(work / 'eeprom.gba')],
        capture_output=True,
        text=True,
        timeout=600,
    )
    found = dict(re.findall(r'\b(r\d+|cpsr)=([0-9A-F]{8})', run.stdout))
    if run.returncode or 'cpsr' not in found:
        sys.exit('agb_eeprom: the runner exited with %d\n%s%s' % (run.returncode, run.stdout, run.stderr))
    return found, run.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-agb-eeprom-') as temporary:
        work = pathlib.Path(temporary)
        found, said = registers(args.runner, work, CODE)
        wrong = [
            '%s=%08X, not %08X' % (name, int(found[name], 16), value)
            for name, value in EXPECTED.items()
            if int(found[name], 16) != value
        ]
        save = work / 'eeprom' / 'eeprom.sav'
        kept = save.read_bytes() if save.is_file() else b''
        if len(kept) != 0x2000:
            wrong.append("%s holds %d bytes, not the 8 KiB chip's 8192" % (save, len(kept)))
        else:
            wrong += [
                'block %#x is %s, not %s' % (block, kept[block * 8 : block * 8 + 8].hex(), data.hex())
                for block, data in BLOCKS.items()
                if kept[block * 8 : block * 8 + 8] != data
            ]
        if wrong:
            sys.exit('agb_eeprom: %s\n%s' % (', '.join(wrong), said))
        found, said = registers(args.runner, work, READ_CODE)
        wrong = [
            'read back from the .sav: %s=%08X, not %08X' % (name, int(found[name], 16), EXPECTED[name])
            for name in ('r8', 'r9')
            if int(found[name], 16) != EXPECTED[name]
        ]
        if wrong:
            sys.exit('agb_eeprom: %s\n%s' % (', '.join(wrong), said))
    print('agb_eeprom: passed')


if __name__ == '__main__':
    main()
