# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The AArch32 dispatch loop runs once per block a 32-bit game leaves through an indirect branch, millions of times a
second, so its lookup must stay a leaf: compile(), the slow path with its error buffer, out of line. Inlined, the buffer
gave every dispatch a 0x160-byte frame, five saved registers and the stack protector's canary (on arm64 a 0x1b0 frame
and twelve registers). This reads arm.c's object with llvm-objdump and fails when arm_thread_run, next_block or lookup
(whichever the compiler kept as functions) checks a stack canary or moves the stack pointer by more than 128 bytes.

    arm_dispatch_frame.py --objdump LLVM_OBJDUMP OBJECTS
    arm_dispatch_frame.py --objdump LLVM_OBJDUMP --program PROGRAM --nm LLVM_NM --symbolizer LLVM_SYMBOLIZER

OBJECTS: snx_core's objects, one argument or a ;-separated list; the one built from runtime/arm.c is read.

PROGRAM: the linked program, which a ThinLTO build reads instead (cmake/lto.cmake): its objects are LLVM bitcode, and
the machine code comes from the link, which can inline across files. There a static function can carry a .llvm.<hash>
suffix, and another file can have a static function of the same name (dns.c's lookup): the line table says which
static symbols are arm.c's, and each is read by its address, up to the next symbol."""

import argparse
import json
import re
import subprocess
import sys

FUNCTIONS = ('arm_thread_run', 'next_block', 'lookup')
CANARY = re.compile(r'__stack_chk_fail|__stack_chk_guard|__security_check_cookie')
# x86-64: subq $N, %rsp; arm64: sub sp, sp, #N, or a pre-indexed stp/str with sp, #-N
FRAME = (
    re.compile(r'\bsub[lq]?\s+\$(0x[0-9a-f]+|\d+),\s*%rsp'),
    re.compile(r'\bsub\s+sp,\s*sp,\s*#(0x[0-9a-f]+|\d+)'),
    re.compile(r'\[sp,\s*#-(0x[0-9a-f]+|\d+)\]!'),
)
LIMIT = 128  # room for Windows x64's 32 bytes of shadow space and a few spills


def disassemble_objects(a):
    """arm.c's object: its symbols are arm.c's alone"""
    objs = [o for arg in a.objects for o in arg.split(';') if re.search(r'runtime[/\\]arm\.c\.(o|obj)$', o)]
    if len(objs) != 1:
        given = sum(len(x.split(';')) for x in a.objects)
        sys.exit(f'FAIL: {len(objs)} objects of runtime/arm.c among {given} given')
    names = ','.join(FUNCTIONS + tuple('_' + f for f in FUNCTIONS))  # Mach-O prefixes C names with _
    out = subprocess.run(
        [a.objdump, '-d', '-r', '--no-show-raw-insn', '--disassemble-symbols=' + names, objs[0]],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode:
        sys.exit(f'FAIL: {a.objdump} said: {out.stderr.strip()}')
    return objs[0], out.stdout


def disassemble_program(a):
    """the program's functions of arm.c, each from its address to the next symbol's"""
    out = subprocess.run([a.nm, '-n', '--defined-only', a.program], capture_output=True, text=True, check=False)
    if out.returncode:
        sys.exit(f'FAIL: {a.nm} said: {out.stderr.strip()}')
    syms = []
    for line in out.stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms.append((int(p[0], 16), p[1], p[2]))
    found = [
        (i, addr, kind, name)
        for i, (addr, kind, name) in enumerate(syms)
        if re.fullmatch(r'_?(%s)(\.llvm\.\d+)?' % '|'.join(FUNCTIONS), name)
    ]
    if not found:
        return a.program, ''
    out = subprocess.run(
        [a.symbolizer, '--obj=' + a.program, '--no-inlines', '--output-style=JSON'],
        input=''.join(f'{addr:#x}\n' for _, addr, _, _ in found),
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode:
        sys.exit(f'FAIL: {a.symbolizer} said: {out.stderr.strip()}')
    files = [
        (r.get('Symbol') or [{}])[0].get('FileName', '') for r in map(json.loads, out.stdout.splitlines())
    ]
    text = []
    for (i, addr, kind, name), file in zip(found, files):
        # a global name is the program's one: arm_thread_run's, whether or not the line table names its file
        own = kind.isupper() and name.lstrip('_') == 'arm_thread_run'
        if not own and not re.search(r'runtime[/\\]arm\.c$', file):
            continue
        stop = next((s for s, _, _ in syms[i + 1 :] if s > addr), None)
        if stop is None:
            sys.exit(f'FAIL: no symbol after {name} in {a.program} to end it')
        cmd = [a.objdump, '-d', '--no-show-raw-insn', f'--start-address={addr:#x}', f'--stop-address={stop:#x}']
        out = subprocess.run(cmd + [a.program], capture_output=True, text=True, check=False)
        if out.returncode:
            sys.exit(f'FAIL: {a.objdump} said: {out.stderr.strip()}')
        text.append(f'{addr:016x} <{name}>:')
        # the header line objdump gives the start is ours above
        text += [x for x in out.stdout.splitlines() if not re.match(r'^[0-9a-f]+ <', x)]
    return a.program, '\n'.join(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--objdump', required=True)
    p.add_argument('--program')
    p.add_argument('--nm')
    p.add_argument('--symbolizer')
    p.add_argument('objects', nargs='*')
    a = p.parse_args()
    if a.program:
        if not a.nm or not a.symbolizer:
            sys.exit('FAIL: --program needs --nm and --symbolizer')
        source, text = disassemble_program(a)
    else:
        source, text = disassemble_objects(a)
    fails, seen, cur = [], [], None
    for line in text.splitlines():
        m = re.match(r'^[0-9a-f]+ <_?(\w+?)(\.llvm\.\d+)?>:', line)
        if m:
            cur = m.group(1)
            seen.append(cur)
            continue
        if cur is None:
            continue
        if CANARY.search(line):
            fails.append(f'{cur} checks a stack canary: {line.strip()}')
        for f in FRAME:
            n = f.search(line)
            if n and int(n.group(1), 0) > LIMIT:
                fails.append(
                    f'{cur} moves the stack pointer by {int(n.group(1), 0)} bytes, more than {LIMIT}: {line.strip()}'
                )
    if 'arm_thread_run' not in seen:
        fails.append(f'no arm_thread_run in {source} (functions read: {", ".join(seen) or "none"})')
    for f in fails:
        print('FAIL: ' + f)
    if fails:
        return 1
    print(f'ok: {", ".join(seen)} keep no canary and no frame over {LIMIT} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
