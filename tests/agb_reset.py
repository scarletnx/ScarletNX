#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""SoftReset (SWI 0) restarts the machine as the BIOS does: from EWRAM's start when the byte at 0x03007FFA says so,
as a game that copied a program there expects, with r0 to r12 zero, the top 0x200 bytes of IWRAM cleared and each
mode's stack set (system 0x03007F00, irq 0x03007FA0, svc 0x03007FE0). It never returns to the code after the call.

A ROM made here copies ten instructions to 0x02000000, sets the flag, leaves 0x55 at 0x03007E00 and 7 in r6, then
calls SoftReset. The EWRAM program puts 42 in r5, reads 0x03007E00 into r7 and the irq and svc stacks into r8 and
r9, and spins. The code, assembled with llvm-mc for armv4t at 0x080000C0:

        adr   r1, ram
        mov   r0, #0x02000000
        mov   r2, #10
    copy:
        ldr   r3, [r1], #4
        str   r3, [r0], #4
        subs  r2, r2, #1
        bne   copy
        ldr   r0, =0x03007FFA
        mov   r1, #1
        strb  r1, [r0]              @ start again from EWRAM
        ldr   r0, =0x03007E00
        mov   r1, #0x55
        str   r1, [r0]              @ a word the reset clears
        mov   r6, #7                @ a register it zeroes
        swi   #0                    @ SoftReset
        mov   r5, #1                @ never reached
    spin:
        b     spin
    ram:
        mov   r5, #42
        mov   r0, #0x03000000
        add   r0, r0, #0x7E00
        ldr   r7, [r0]
        msr   cpsr_c, #0xD2         @ irq mode
        mov   r8, sp
        msr   cpsr_c, #0xD3         @ svc mode
        mov   r9, sp
        msr   cpsr_c, #0x1F         @ system mode
    ramspin:
        b     ramspin

The machine starts at the cartridge's first word, as the BIOS jumps there: a second ROM's header branches past a
word at 0x080000C0 (mov r5, #0xFF) to code at 0x080000C8 that puts 0x5A in r5 and spins.

A header whose complement check is not its title block's runs, as mGBA runs it, and the runner names both values (the
console's BIOS stops there): a fourth ROM with the check flipped puts 42 in r5 and spins.

A software interrupt whose number the BIOS does not have returns at once, as mGBA's HLE BIOS answers it, and the
runner says which and where: a third ROM calls swi 0xFF0099 at 0x080000C0, then puts 42 in r5 and spins.

A MultiBoot ROM (.mb) plays with no cartridge from EWRAM, where the BIOS receives one through the link cable: from
0x020000C0 when that word is not zero (a program that puts 42 in r5 there and spins), else from EWRAM's start (one
that puts 0x5A in r5 at 0x02000000 and spins), as mGBA's machine starts one. An image past EWRAM's 256 KiB is
refused, the message naming its size and what EWRAM holds.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

CODE = [
    0xE28F1044, 0xE3A00402, 0xE3A0200A, 0xE4913004, 0xE4803004, 0xE2522001, 0x1AFFFFFB, 0xE59F0020,
    0xE3A01001, 0xE5C01000, 0xE59F0018, 0xE3A01055, 0xE5801000, 0xE3A06007, 0xEF000000, 0xE3A05001,
    0xEAFFFFFE, 0x03007FFA, 0x03007E00, 0xE3A0502A, 0xE3A00403, 0xE2800C7E, 0xE5907000, 0xE321F0D2,
    0xE1A0800D, 0xE321F0D3, 0xE1A0900D, 0xE321F01F, 0xEAFFFFFE,
]  # fmt: skip

EXPECTED = {'r5': 42, 'r6': 0, 'r7': 0, 'r8': 0x03007FA0, 'r9': 0x03007FE0, 'r13': 0x03007F00, 'cpsr': 0x1F}


def rom(code=CODE, branch=0xEA00002E):
    """A cartridge the board takes (its branch, b 0x080000C0 unless given, fixed byte and complement), its code at
    0x080000C0."""
    image = bytearray(0xC0)
    image[0:4] = branch.to_bytes(4, 'little')
    image[0xA0:0xAC] = b'RESET'.ljust(12, b'\0')
    image[0xAC:0xB2] = b'ARST01'
    image[0xB2] = 0x96
    image[0xBD] = -(sum(image[0xA0:0xBD]) + 0x19) & 0xFF
    for word in code:
        image += word.to_bytes(4, 'little')
    return bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-agb-reset-') as temporary:
        work = pathlib.Path(temporary)
        (work / 'reset.gba').write_bytes(rom())
        run = subprocess.run(
            [args.runner, '--frames=10', '--registers', '--outdir', str(work), str(work / 'reset.gba')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        found = dict(re.findall(r'\b(r\d+|cpsr)=([0-9A-F]{8})', run.stdout))
        if run.returncode or 'cpsr' not in found:
            sys.exit('agb_reset: the runner exited with %d\n%s%s' % (run.returncode, run.stdout, run.stderr))
        wrong = [
            '%s=%08X, not %08X' % (name, int(found[name], 16), value)
            for name, value in EXPECTED.items()
            if int(found[name], 16) != value
        ]
        if wrong:
            sys.exit('agb_reset: after SoftReset %s\n%s' % (', '.join(wrong), run.stdout))
        # b 0x080000C8 over mov r5, #0xFF and a spin, to mov r5, #0x5A and a spin
        (work / 'entry.gba').write_bytes(rom([0xE3A050FF, 0xEAFFFFFE, 0xE3A0505A, 0xEAFFFFFE], 0xEA000030))
        run = subprocess.run(
            [args.runner, '--frames=2', '--registers', '--outdir', str(work), str(work / 'entry.gba')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        found = re.search(r'\br5=([0-9A-F]{8})', run.stdout)
        if run.returncode or not found or int(found.group(1), 16) != 0x5A:
            sys.exit(
                'agb_reset: the header branch to 0x080000C8 was not where the run started (r5 %s, not 0000005A)'
                '\n%s%s' % (found.group(1) if found else 'missing', run.stdout, run.stderr)
            )
        # swi 0xFF0099, mov r5, #42, spin
        (work / 'unknown.gba').write_bytes(rom([0xEFFF0099, 0xE3A0502A, 0xEAFFFFFE]))
        run = subprocess.run(
            [args.runner, '--frames=2', '--registers', '--outdir', str(work), str(work / 'unknown.gba')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        found = re.search(r'\br5=([0-9A-F]{8})', run.stdout)
        said = 'The game called BIOS function 0xFF at 0x080000C0, which the BIOS does not have' in run.stdout
        if run.returncode or not found or int(found.group(1), 16) != 42 or not said:
            sys.exit(
                'agb_reset: after swi 0xFF0099 r5 is %s, not 0000002A, or the runner did not name the call 0xFF at '
                '0x080000C0\n%s%s' % (found.group(1) if found else 'missing', run.stdout, run.stderr)
            )
        # a header whose checksum is not its title block's runs, as mGBA runs it, and the runner says both
        image = bytearray(rom([0xE3A0502A, 0xEAFFFFFE]))
        title = image[0xBD]
        image[0xBD] ^= 0x40
        (work / 'checksum.gba').write_bytes(bytes(image))
        run = subprocess.run(
            [args.runner, '--frames=2', '--registers', '--outdir', str(work), str(work / 'checksum.gba')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        found = re.search(r'\br5=([0-9A-F]{8})', run.stdout)
        said = "The ROM header's checksum is 0x%02X and its title block sums to 0x%02X: it runs" % (title ^ 0x40, title)
        if run.returncode or not found or int(found.group(1), 16) != 42 or said not in run.stdout:
            sys.exit(
                'agb_reset: a header with checksum 0x%02X against 0x%02X ended with %d and r5 %s, not 0 and 0000002A '
                'with "%s"\n%s%s'
                % (
                    title ^ 0x40,
                    title,
                    run.returncode,
                    found.group(1) if found else 'missing',
                    said,
                    run.stdout,
                    run.stderr,
                )
            )
        # MultiBoot ROMs: the header's branch to 0xC0, mov r5, #42 and a spin there; mov r5, #0x5A and a spin at the
        # start with a zero word at 0xC0; and an image a word past EWRAM
        programs = {
            'entry.mb': (0xEA00002E).to_bytes(4, 'little')
            + bytes(0xBC)
            + b''.join(w.to_bytes(4, 'little') for w in (0xE3A0502A, 0xEAFFFFFE)),
            'start.mb': b''.join(w.to_bytes(4, 'little') for w in (0xE3A0505A, 0xEAFFFFFE)) + bytes(0xC0),
        }
        for name, (r5, pc) in {'entry.mb': (42, 0x020000C4), 'start.mb': (0x5A, 0x02000004)}.items():
            (work / name).write_bytes(programs[name])
            run = subprocess.run(
                [args.runner, '--frames=2', '--registers', '--outdir', str(work), str(work / name)],
                capture_output=True,
                text=True,
                timeout=600,
            )
            found = dict(re.findall(r'\b(r\d+)=([0-9A-F]{8})', run.stdout))
            if run.returncode or int(found.get('r5', '0'), 16) != r5 or int(found.get('r15', '0'), 16) != pc:
                sys.exit(
                    'agb_reset: the MultiBoot ROM %s ended with r5 %s and r15 %s, not %08X and %08X\n%s%s'
                    % (name, found.get('r5', 'missing'), found.get('r15', 'missing'), r5, pc, run.stdout, run.stderr)
                )
        (work / 'big.mb').write_bytes(programs['entry.mb'] + bytes(0x40000 + 4 - len(programs['entry.mb'])))
        run = subprocess.run(
            [args.runner, '--frames=2', '--outdir', str(work), str(work / 'big.mb')],
            capture_output=True,
            text=True,
            timeout=600,
        )
        said = 'the multiboot program is 262148 bytes: EWRAM, where the BIOS receives one, holds 1 to 262144'
        if run.returncode != 1 or said not in run.stderr:
            sys.exit(
                'agb_reset: a MultiBoot ROM of 262148 bytes ended with %d, not 1 with "%s"\n%s%s'
                % (run.returncode, said, run.stdout, run.stderr)
            )
    print('agb_reset: passed')


if __name__ == '__main__':
    main()
