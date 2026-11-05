#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Every RtAPI a test makes carries the runtime's 16-byte compare-and-swap.

An STXP or CASP of doublewords calls RtAPI.cas128 on RISC-V, LoongArch, MIPS64 and PowerPC64 before POWER8
(common/cpu/aarch64/helpers/integer.c), and only there, so a test whose RtAPI leaves it NULL passes on x86-64 and
AArch64 and dies on those hosts. In tests/, each RtAPI defined names .cas128 in its initializer, or is one
snx_runtime_defaults fills; an RtAPI made by a compound literal names it too, and none is cleared by memset after.

    rtapi_cas128.py [--repo DIR]
"""

import argparse
import re
import sys
from pathlib import Path

# comments, string and character literals, which say nothing about the code
NOISE = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)
# an RtAPI object (not a pointer), its initializer up to the statement's end: RtAPI has no member that takes a ';'
DEFINITION = re.compile(r'((?:\b(?:static|const|extern|volatile)\s+)*)\bRtAPI\s+(\w+)\s*(?:=([^;]*))?;')
POINTER = re.compile(r'\bRtAPI\s*\*\s*(?:const\s+)?(\w+)')
COMPOUND = re.compile(r'\(\s*(?:const\s+)?RtAPI\s*\)\s*\{([^;]*?)\}')
FIX = 'give it .cas128 = snx_runtime_cas128'


def blank(match):
    return re.sub(r'[^\n]', ' ', match.group(0))


def check_text(name, text):
    """the errors of one source file, each 'name:line: why'"""
    code = NOISE.sub(blank, text)
    errors = []

    def line(at):
        return code.count('\n', 0, at) + 1

    rtapis = set()
    for match in DEFINITION.finditer(code):
        prefix, var, init = match.group(1), match.group(2), match.group(3)
        if 'extern' in prefix.split():
            continue
        rtapis.add(var)
        if init is not None:
            if 'cas128' not in init:
                shown = ' '.join(init.split())
                errors.append(f'{name}:{line(match.start())}: RtAPI {var} = {shown} leaves cas128 NULL: {FIX}')
        elif not re.search(r'\bsnx_runtime_defaults\s*\(\s*&\s*' + re.escape(var) + r'\s*\)', code):
            errors.append(f'{name}:{line(match.start())}: RtAPI {var} has no initializer, so cas128 is NULL: {FIX}')
    for match in COMPOUND.finditer(code):
        if 'cas128' not in match.group(1):
            shown = ' '.join(match.group(1).split())
            errors.append(f'{name}:{line(match.start())}: (RtAPI){{{shown}}} leaves cas128 NULL: {FIX}')
    rtapis.update(match.group(1) for match in POINTER.finditer(code))
    for var in sorted(rtapis):
        for match in re.finditer(r'\bmemset\s*\(\s*&?\s*' + re.escape(var) + r'\s*,', code):
            errors.append(f'{name}:{line(match.start())}: memset clears RtAPI {var}, cas128 with it: {FIX}')
    return errors


# what the check must find and pass, so that a pattern that stops matching fails here and not in silence
SELF = [
    ('RtAPI rt = {0};', 1),
    ('static const RtAPI rt = {.miss = m,\n .poll = p};', 1),
    ('RtAPI rt;', 1),
    ('RtAPI rt; memset(&rt, 0, sizeof rt); rt.cas128 = snx_runtime_cas128;', 2),
    ('void f(RtAPI* rt) { memset(rt, 0, sizeof(*rt)); }', 1),
    ('void f(RtAPI* rt) { *rt = (RtAPI){.undef = u}; }', 1),
    ('RtAPI rt = {.undef = u, .cas128 = snx_runtime_cas128};', 0),
    ('static const RtAPI rt = {\n .poll = p, /* RtAPI x; */ .cas128 = snx_runtime_cas128};', 0),
    ('RtAPI rt; snx_runtime_defaults(&rt);', 0),
    ('extern const RtAPI g_rtapi;', 0),
    ('void f(RtAPI* rt) { *rt = (RtAPI){.cas128 = snx_runtime_cas128}; }', 0),
    ('const RtAPI* rt = &g_rtapi; printf("RtAPI x;");', 0),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    failed = 0
    for text, want in SELF:
        got = check_text('self', text)
        if len(got) != want:
            print(f'rtapi_cas128: the check found {len(got)} errors in {text!r}, not {want}: {got}', file=sys.stderr)
            failed = 1
    tests = args.repo / 'tests'
    files = sorted(p for p in tests.rglob('*') if p.suffix in {'.c', '.h', '.inc'} and p.is_file())
    if not files:
        print(f'rtapi_cas128: no C source under {tests}', file=sys.stderr)
        return 1
    errors = []
    for path in files:
        errors += check_text(str(path.relative_to(args.repo)), path.read_text(encoding='utf-8', errors='replace'))
    for error in errors:
        print(error, file=sys.stderr)
    if errors or failed:
        return 1
    print(f'rtapi_cas128: every RtAPI in the {len(files)} C sources of tests/ carries cas128')
    return 0


if __name__ == '__main__':
    sys.exit(main())
