#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The native x86-64 back end's instruction density, one op group at a time.

The --counts mode of snx-test-recompiler-backends writes one native object per op of the decoder: every sample word
of the op, a RET after each, made by the back end. This reads those objects, counts the host instructions in each
with llvm-objdump, and divides by the group's guest words. The objects are made without the host's features past
SSE4.2, PCLMULQDQ and POPCNT (--without), so every x86-64 host counts the same code: with them a host with GFNI or AVX2
makes shorter code for some ops than one without. A group's mean must stay below its limit in
x64_density_targets.txt. The limits leave room for a change of the toolchain, so the check catches a real growth of
the generated code instead of every difference of a byte.

    x64_density.py --backend PROGRAM --objdump LLVM_OBJDUMP --targets FILE [--limits [--also PROGRAM]]
    x64_density.py --sail PROGRAM --mc LLVM_MC


A run where a group is over its limit prints every group's count after the failures (op, guest words, host
instructions, mean, limit), so a host whose code the limits do not hold gives in its log what to make them again of.
--limits holds nothing: it prints FILE again with each group's limit a quarter above this build's count, rounded up,
the words FILE's. That is how the limits are made again after a change of the back end. --also names the program
of the other calling convention (snx-test-recompiler-win64 beside snx-test-recompiler-backends on Linux): a group's
limit is then of the larger count of the two, so the one file holds a Windows host too.

--sail counts the other way, the generated Sail step of each guest word alone (snx-test-sail-x64 --counts: no
prologue, no pins, none of x64_vec.c's whole-vector forms, so an upper bound of the generated code), on this host's
features and on the plain host's, with llvm-mc, and prints each group's mean and the whole's. It holds no limit.

Exit 77 says this host has no x86-64 back end. The program then writes no objects and says so on its standard output.
"""

import argparse
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

NO_BACKEND = 'no x86-64 back end'
# --without's mask, sailgen's SGX_F_* past 2008's processors: BMI1, BMI2, LZCNT, F16C, AVX, FMA, AES-NI, SHA-NI, AVX2,
# GFNI and SM4. SSE2 to SSE4.2, PCLMULQDQ and POPCNT stay: without those the back end makes its fallbacks' code, or none
NO_OPTIONAL = '0x3BD07'
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
    out = subprocess.run(
        [objdump, '-d', '--no-show-raw-insn', *[str(o) for o in objects]], capture_output=True, text=True, check=False
    )
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


def counted(backend, objdump, folder):
    """each op's host instructions in the objects backend --counts writes, in the order written; None with no back end"""
    folder.mkdir()
    listing = folder / 'counts.txt'
    run = subprocess.run(
        [str(backend), '--without', NO_OPTIONAL, '--counts', str(listing)],
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode == 77 or NO_BACKEND in run.stdout:
        return None
    if run.returncode:
        sys.exit(f'FAIL: {backend} --counts: {run.stderr.strip() or run.stdout.strip()}')
    groups = records(listing)
    if not groups:
        sys.exit(f'FAIL: {backend} --counts wrote no objects')
    objects = []
    for op, size, data in groups:
        if len(data) != size:
            sys.exit(f'FAIL: {op}: {len(data)} bytes of object, not {size}')
        path = folder / (op + '.o')
        path.write_bytes(data)
        objects.append(path)
    count = instructions(objdump, objects)
    return [(op, count[obj.name]) for (op, _, _), obj in zip(groups, objects)]


SENTINEL = '0x0f 0x1f 0x80 0xde 0xc0 0xad 0x0b'  # nopl 0xbadc0de(%rax): between two words' code, in sync
# fifteen one-byte NOPs before it: what an instruction cut by the end of a word's bytes (its constants') takes of them,
# the rest decode as NOPs, not counted
PAD = ' '.join(['0x90'] * 15)


def disassembled(mc, codes):
    """the host instructions of each hex byte string, by llvm-mc: a batch at once, between sentinels; a batch whose
    count does not come out (bytes that swallow the sentinel, such as a constant pool's) one string at a time"""

    def run(batch):
        lines = []
        for code in batch:
            lines.append(' '.join('0x' + code[k : k + 2] for k in range(0, len(code), 2)))
            lines.append(PAD)
            lines.append(SENTINEL)
        dis = subprocess.run(
            [mc, '--disassemble', '-triple=x86_64'],
            input='\n'.join(lines) + '\n',
            capture_output=True,
            text=True,
            check=False,
        )
        counts, n, trailing = [], 0, 0
        for line in dis.stdout.splitlines():
            line = line.strip()
            if not line or line.startswith('.'):
                continue
            if line.startswith('nopl') and '195936478(%rax)' in line:
                counts.append(n - trailing)
                n = trailing = 0
            elif line == 'nop':
                n += 1
                trailing += 1
            else:
                n += 1
                trailing = 0
        return counts

    out = []
    for at in range(0, len(codes), 256):
        batch = codes[at : at + 256]
        counts = run(batch)
        if len(counts) != len(batch):
            counts = []
            for code in batch:
                one = run([code])
                if len(one) != 1:
                    sys.exit(f'FAIL: {mc} could not count {code[:32]}...')
                counts.append(one[0])
        out.extend(counts)
    return out


def sail(program, mc, folder):
    """each op's words and host instructions of its Sail steps, on this host's features and the plain host's"""
    folder.mkdir()
    listing = folder / 'sail.txt'
    run = subprocess.run([str(program), '--counts', str(listing)], capture_output=True, text=True, check=False)
    if run.returncode == 77 or 'not an x86-64 host' in run.stdout:
        return None
    if run.returncode:
        sys.exit(f'FAIL: {program} --counts: {run.stderr.strip() or run.stdout.strip()}')
    rows = [line.split('\t') for line in listing.read_text().splitlines() if line]
    host = disassembled(mc, [row[2] if row[2] != '-' else '' for row in rows])
    plain = disassembled(mc, [row[3] if row[3] != '-' else '' for row in rows])
    out = {}
    for row, h, p in zip(rows, host, plain):
        words, hs, ps = out.get(row[1], (0, 0, 0))
        out[row[1]] = (words + 1, hs + h, ps + p)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--backend', type=Path, help='the test program that has --counts')
    parser.add_argument('--objdump', help="the build's llvm-objdump")
    parser.add_argument('--targets', type=Path, help='the checked-in limits')
    parser.add_argument('--sail', type=Path, help='snx-test-sail-x64, whose --counts are the Sail steps')
    parser.add_argument('--mc', help="with --sail, the build's llvm-mc")
    parser.add_argument('--limits', action='store_true', help="print the limits file of this build's counts")
    parser.add_argument('--also', type=Path, help="with --limits, the other calling convention's program")
    args = parser.parse_args()
    if args.sail:
        if args.backend or not args.mc:
            parser.error('--sail takes --mc, and no --backend')
        with tempfile.TemporaryDirectory(prefix='snx-x64-density-') as scratch:
            groups = sail(args.sail, args.mc, Path(scratch) / 's')
        if groups is None:
            print('SKIP: no x86-64 host: nothing to count')
            return 77
        for op in sorted(groups):
            words, host, plain = groups[op]
            print(f'{op}\t{words}\t{host / words:.2f}\t{plain / words:.2f}')
        words = sum(g[0] for g in groups.values())
        host = sum(g[1] for g in groups.values())
        plain = sum(g[2] for g in groups.values())
        print(
            f'x64 sail steps: {host} host instructions / {words} guest words, mean {host / words:.2f} '
            f'(plain host: {plain / words:.2f}), {len(groups)} groups'
        )
        return 0
    if not (args.backend and args.objdump and args.targets):
        parser.error('--backend, --objdump and --targets go together')
    allowed = limits(args.targets)
    with tempfile.TemporaryDirectory(prefix='snx-x64-density-') as scratch:
        groups = counted(args.backend, args.objdump, Path(scratch) / 'a')
        if groups is None:
            print('SKIP: no x86-64 back end: nothing to count')
            return 77
        if args.limits:
            most = dict(groups)
            for op, n in (counted(args.also, args.objdump, Path(scratch) / 'b') or []) if args.also else []:
                most[op] = max(most.get(op, 0), n)
            for line in args.targets.read_text().splitlines():
                if line.startswith('#'):
                    print(line)
            for op in sorted(most):
                if op not in allowed:
                    sys.exit(f'FAIL: {op}: no words in {args.targets}')
                words = allowed[op][0]
                print(f'{op}\t{words}\t{math.ceil(most[op] * 1.25 / words)}')
            return 0
        bad = 0
        total_insns = total_words = 0
        seen = set()
        rows = []
        for op, n in groups:
            if op in seen:
                print(f'FAIL: {op}: two records', file=sys.stderr)
                bad = 1
            seen.add(op)
            if op not in allowed:
                print(f'FAIL: {op}: no limit in {args.targets}', file=sys.stderr)
                bad = 1
                continue
            words, limit = allowed[op]
            total_insns += n
            total_words += words
            rows.append(f'{op}\t{words}\t{n}\t{n / words:.2f}\t{limit:g}')
            if n > limit * words:
                print(
                    f'FAIL: {op}: {n} host instructions for {words} guest words, mean {n / words:.2f}, limit {limit:g}',
                    file=sys.stderr,
                )
                bad = 1
        for op in sorted(set(allowed) - seen):
            print(f'FAIL: {op}: no object from --counts', file=sys.stderr)
            bad = 1
        if bad:
            print('every group: op, guest words, host instructions, mean, limit')
            print('\n'.join(rows))
        mean = total_insns / total_words if total_words else 0
        print(
            f'x64 density: {total_insns} host instructions / {total_words} guest words, mean {mean:.2f}, '
            f'{len(groups)} groups'
        )
        return bad


if __name__ == '__main__':
    sys.exit(main())
