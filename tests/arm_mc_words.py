#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""llvm-mc's verdict on the words the A32 and Thumb tests run: it assembles each catalogue's text (arm_vector_test.py's
VFP and NEON for the Cortex-A57, thumb_test.py's Thumb, arm11_vfp.py's VFPv2 for the ARM11 MPCore), and each word or
halfword must be the one the catalogue has.

    python3 tests/arm_mc_words.py --mc llvm-mc

Where llvm-mc is not there it says so and exits with 77. No game assets.
"""

import argparse
import os
import re
import subprocess
import sys

import arm11_vfp
import arm_vector_test
import thumb_test
from arm_blocks import SKIP


def encodings(output):
    """each instruction's encoding in llvm-mc's -show-encoding output, its bytes"""
    return [
        bytes(int(value, 16) for value in re.findall(r'0x[0-9a-f]+', line.split('encoding:')[1]))
        for line in output.splitlines()
        if 'encoding:' in line
    ]


def assemble(mc, arguments, isa, lines):
    """the encodings of the lines, one each; llvm-mc's status and their count are asserted"""
    process = subprocess.run(
        [mc, *arguments, '-show-encoding'],
        input=f'.syntax unified\n.{isa}\n' + '\n'.join(lines) + '\n',
        text=True,
        capture_output=True,
        check=False,  # its status is asserted with what it said
    )
    if process.returncode:
        raise AssertionError(f'llvm-mc ended with status {process.returncode}: {process.stderr}')
    out = encodings(process.stdout)
    assert len(out) == len(lines), f'llvm-mc made {len(out)} encodings of {len(lines)} lines'
    return out


def compare(mc, vector, thumb, vfp2):
    """what differs, a line each, of the catalogues: vector and vfp2 (word, text), thumb (text, instructions,
    halfwords)"""
    differ = []
    a57 = ['-triple=armv8-linux-gnueabi', '-mcpu=cortex-a57']
    for (word, text), data in zip(vector, assemble(mc, a57, 'arm', [text for _, text in vector])):
        if int.from_bytes(data, 'little') != word:
            differ.append(f"A57 {text}: {word:08x} here, {int.from_bytes(data, 'little'):08x} llvm-mc's")
    for (word, text), data in zip(
        vfp2,
        assemble(mc, ['-triple=armv6k-linux-gnueabi', '-mcpu=mpcore', '-mattr=+vfp2'], 'arm', [t for _, t in vfp2]),
    ):
        if int.from_bytes(data, 'little') != word:
            differ.append(f"ARM11 {text}: {word:08x} here, {int.from_bytes(data, 'little'):08x} llvm-mc's")
    for text, count, halfwords in thumb:
        lines = [part.strip() for part in text.split(';')]
        assert len(lines) == count, f'{text}: {len(lines)} instructions, the catalogue says {count}'
        theirs = [
            int.from_bytes(data[at : at + 2], 'little')
            for data in assemble(mc, a57, 'thumb', lines)
            for at in range(0, len(data), 2)
        ]
        if theirs != list(halfwords):
            ours, said = ' '.join(f'{h:04x}' for h in halfwords), ' '.join(f'{h:04x}' for h in theirs)
            differ.append(f"Thumb {text}: {ours} here, {said} llvm-mc's")
    return differ


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mc', required=True, help='llvm-mc, of the LLVM this was built with')
    args = parser.parse_args()
    if not os.access(args.mc, os.X_OK):
        print(f'SKIP: {args.mc} is not there (the LLVM this was built with has no llvm-mc)')
        return SKIP
    vector, thumb, vfp2 = arm_vector_test.catalogue(), thumb_test.catalogue(), arm11_vfp.catalogue()
    differ = compare(args.mc, vector, thumb, vfp2)
    for line in differ:
        print(line)
    print(
        f'{len(vector)} A57 VFP and NEON words, {len(thumb)} Thumb lines and {len(vfp2)} ARM11 VFPv2 words: '
        f"{len(differ)} differ from llvm-mc's"
    )
    return 1 if differ else 0


if __name__ == '__main__':
    sys.exit(main())
