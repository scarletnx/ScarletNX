#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A read of a write-only I/O register gives the open bus, the opcode the CPU fetched last, as mGBA reads it: from ARM
the low half of [$+8], from Thumb in the ROM [$+4], from Thumb in IWRAM [$+4] when $+4 starts a word and [$+2] when
it does not; a word reads that half twice, a byte its own byte of it. DMA0CNT_L reads 0, and DISPCNT what was written.
A game builds WIN0V from what it read there, so a stored value made its window empty.

The ROM's code, assembled with llvm-mc for armv4t at 0x080000C0 (mGBA 0.11 ends with the same r3 to r10):

        mov   r2, #0x04000000
        ldrh  r4, [r2, #0x44]       @ WIN0V from ARM: 0x1055, the low half of [$+8]
        mov   r0, r0
        mov   r1, #0x55
        mov   r0, #0x80
        strh  r0, [r2]
        ldrh  r7, [r2]              @ DISPCNT: 0x80
        mov   r0, #5
        strh  r0, [r2, #0xB8]
        ldrh  r8, [r2, #0xB8]       @ DMA0CNT_L: 0
        adr   r0, iwram
        mov   r1, #0x03000000
        mov   r3, #4
    copy:
        ldr   r12, [r0], #4
        str   r12, [r1], #4
        subs  r3, r3, #1
        bne   copy
        add   r2, r2, #0x44
        adr   r0, thumb + 1
        bx    r0
        .thumb
    thumb:
        ldrh  r3, [r2]              @ from the ROM: 0x2177, [$+4]
        mov   r8, r8
        movs  r1, #0x77
        ldr   r0, [r2]              @ a word: 0x21662166
        mov   r8, r8
        movs  r1, #0x66
        mov   r9, r0
        ldrb  r0, [r2, #1]          @ a byte: 0x21
        mov   r8, r8
        movs  r1, #0x44
        mov   r10, r0
        ldr   r0, =0x03000001
        bx    r0
        .pool
    iwram:                          @ run at 0x03000000
        ldrh  r5, [r2]              @ $+4 starts a word: 0x2011, [$+4]
        ldrh  r6, [r2]              @ it does not: 0x2011, [$+2] (0x2122 is [$+4])
        movs  r0, #0x11
        movs  r1, #0x22
    spin:
        b     spin
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

CODE = [
    0xE3A02301, 0xE1D244B4, 0xE1A00000, 0xE3A01055, 0xE3A00080, 0xE1C200B0, 0xE1D270B0, 0xE3A00005,
    0xE1C20BB8, 0xE1D28BB8, 0xE28F0040, 0xE3A01403, 0xE3A03004, 0xE490C004, 0xE481C004, 0xE2533001,
    0x1AFFFFFB, 0xE2822044, 0xE28F0001, 0xE12FFF10, 0x46C08813, 0x68102177, 0x216646C0, 0x78504681,
    0x214446C0, 0x48014682, 0x46C04700, 0x03000001, 0x88168815, 0x21222011, 0x46C0E7FE, 0x46C046C0,
]  # fmt: skip

EXPECTED = {'r3': 0x2177, 'r4': 0x1055, 'r5': 0x2011, 'r6': 0x2011, 'r7': 0x80, 'r8': 0, 'r9': 0x21662166, 'r10': 0x21}


def rom():
    """A cartridge the board takes (its branch, fixed byte and complement), its code at 0x080000C0."""
    image = bytearray(0xC0)
    image[0:4] = (0xEA00002E).to_bytes(4, 'little')  # b 0x080000C0
    image[0xA0:0xAC] = b'OPENBUS'.ljust(12, b'\0')
    image[0xAC:0xB2] = b'AOPB01'
    image[0xB2] = 0x96
    image[0xBD] = -(sum(image[0xA0:0xBD]) + 0x19) & 0xFF
    for word in CODE:
        image += word.to_bytes(4, 'little')
    return bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-agb-open-bus-') as temporary:
        work = pathlib.Path(temporary)
        (work / 'open_bus.gba').write_bytes(rom())
        run = subprocess.run(
            [args.runner, '--frames=10', '--registers', '--outdir', str(work), str(work / 'open_bus.gba')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        found = dict(re.findall(r'\b(r\d+|cpsr)=([0-9A-F]{8})', run.stdout))
        if run.returncode or 'cpsr' not in found:
            sys.exit('agb_open_bus: the runner exited with %d\n%s%s' % (run.returncode, run.stdout, run.stderr))
        wrong = [
            '%s=%08X, not %08X' % (name, int(found[name], 16), value)
            for name, value in EXPECTED.items()
            if int(found[name], 16) != value
        ]
        if wrong:
            sys.exit('agb_open_bus: %s\n%s' % (', '.join(wrong), run.stdout))
    print('agb_open_bus: passed')


if __name__ == '__main__':
    main()
