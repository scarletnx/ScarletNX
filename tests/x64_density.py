#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The native x86-64 back end's instruction density, one op group at a time.

The --counts mode of snx-test-recompiler-backends writes one native object per op of the decoder: every sample word
of the op, a RET after each, made by the back end. This reads those objects, counts the host instructions in each
with llvm-objdump, and divides by the group's guest words. A group's mean must stay below its limit in
x64_density_targets.txt. The limits leave room for a change of the toolchain, so the check catches a real growth of
the generated code instead of every difference of a byte.

    x64_density.py --backend PROGRAM --objdump LLVM_OBJDUMP --targets FILE

Exit 77 says this host has no x86-64 back end. The program then writes no objects and says so on its standard output.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

NO_BACKEND = 'no x86-64 back end'
HEADER = re.compile(r'^(.+):\s+file format\b')
INSN = re.compile(r'^\s*[0-9a-fA-F]+:\s+\S')
RECORD = re.compile(r'^OP\t([^\t]+)\t([0-9]+)$')


def limits(path):
    """each op's guest words and density limit"""
    out = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        op, words, limit = line.split()
        out[op] = (int(words), float(limit))
    return out


def records(path):
    """each op's name, declared object size and object bytes"""
    lines = path.read_text().splitlines()
    out = []
    i = 0
    while i < len(lines):
        match = RECORD.match(lines[i])
        if match and i + 1 < len(lines):
            out.append((match.group(1), int(match.group(2)), bytes.fromhex(lines[i + 1].strip())))
            i += 2
        else:
            i += 1
    return out


def instructions(objdump, objects):
    """one disassembly of all objects, the host instructions of each"""
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', *[str(o) for o in objects]],
                         capture_output=True, text=True, check=False)
    if out.returncode:
        sys.exit(f'FAIL: {objdump} said: {out.stderr.strip()}')
    counts = {o.name: 0 for o in objects}
    name = None
    for line in out.stdout.splitlines():
        head = HEADER.match(line)
        if head:
            name = Path(head.group(1)).name
        elif name is not None and INSN.match(line):
            counts[name] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--backend', type=Path, required=True, help='the test program that has --counts')
    parser.add_argument('--objdump', required=True, help="the build's llvm-objdump")
    parser.add_argument('--targets', type=Path, required=True, help='the checked-in limits')
    args = parser.parse_args()
    allowed = limits(args.targets)
    with tempfile.TemporaryDirectory(prefix='snx-x64-density-') as scratch:
        folder = Path(scratch)
        listing = folder / 'counts.txt'
        run = subprocess.run([str(args.backend), '--counts', str(listing)], capture_output=True, text=True, check=False)
        if run.returncode == 77 or NO_BACKEND in run.stdout:
            print('SKIP: no x86-64 back end: nothing to count')
            return 77
        if run.returncode:
            sys.exit(f'FAIL: {args.backend} --counts: {run.stderr.strip() or run.stdout.strip()}')
        groups = records(listing)
        if not groups:
            sys.exit(f'FAIL: {args.backend} --counts wrote no objects')
        objects = []
        for op, size, data in groups:
            if len(data) != size:
                sys.exit(f'FAIL: {op}: {len(data)} bytes of object, not {size}')
            path = folder / (op + '.o')
            path.write_bytes(data)
            objects.append(path)
        count = instructions(args.objdump, objects)
        bad = 0
        total_insns = total_words = 0
        seen = set()
        for (op, _, _), obj in zip(groups, objects):
            if op in seen:
                print(f'FAIL: {op}: two records', file=sys.stderr)
                bad = 1
            seen.add(op)
            if op not in allowed:
                print(f'FAIL: {op}: no limit in {args.targets}', file=sys.stderr)
                bad = 1
                continue
            words, limit = allowed[op]
            n = count[obj.name]
            total_insns += n
            total_words += words
            if n > limit * words:
                print(f'FAIL: {op}: {n} host instructions for {words} guest words, mean {n / words:.2f}, '
                      f'limit {limit:g}', file=sys.stderr)
                bad = 1
        for op in sorted(set(allowed) - seen):
            print(f'FAIL: {op}: no object from --counts', file=sys.stderr)
            bad = 1
        mean = total_insns / total_words if total_words else 0
        print(f'x64 density: {total_insns} host instructions / {total_words} guest words, mean {mean:.2f}, '
              f'{len(groups)} groups')
        return bad


if __name__ == '__main__':
    sys.exit(main())
