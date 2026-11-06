# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The AArch32 dispatch loop runs once per block a 32-bit game leaves through an indirect branch, millions of times a
second, so its lookup must stay a leaf: compile(), the slow path with its error buffer, out of line. Inlined, the buffer
gave every dispatch a 0x160-byte frame, five saved registers and the stack protector's canary (on arm64 a 0x1b0 frame
and twelve registers). This reads arm.c's object with llvm-objdump and fails when arm_thread_run, next_block or lookup
(whichever the compiler kept as functions) checks a stack canary or moves the stack pointer by more than 128 bytes.

    arm_dispatch_frame.py --objdump LLVM_OBJDUMP OBJECTS

OBJECTS: snx_core's objects, one argument or a ;-separated list; the one built from runtime/arm.c is read."""

import argparse
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


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--objdump', required=True)
    p.add_argument('objects', nargs='+')
    a = p.parse_args()
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
    fails, seen, cur = [], [], None
    for line in out.stdout.splitlines():
        m = re.match(r'^[0-9a-f]+ <_?(\w+)>:', line)
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
        fails.append(f'no arm_thread_run in {objs[0]} (functions read: {", ".join(seen) or "none"})')
    for f in fails:
        print('FAIL: ' + f)
    if fails:
        return 1
    print(f'ok: {", ".join(seen)} keep no canary and no frame over {LIMIT} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
