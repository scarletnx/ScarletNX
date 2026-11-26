#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Each stop of the Switch runtime that the person playing can do something about names its kind of error
(src/common/utilities/error_kind.h), so its panel offers the action that fixes it: a fatal() whose message says one of
the words below is refused, with its file and line. A plain fatal() is INTERNAL (or MEMORY, by its words). Offline.

    tests/error_kinds.py SOURCE_DIR"""

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1])
# the words of a stop that has a kind of its own, and the kind it takes
KINDS = {
    'device was lost': 'GPU_LOST',
    'stopped responding': 'GPU_LOST',
    'answers only in the SDK': 'SETTING, SDK32 or NOT_YET',
    'svcBreak': 'GAME_STOPPED',
    'which the SDK stops the game on': 'GAME_STOPPED',
    'the SDK aborts here': 'GAME_STOPPED',
    'The game crashed': 'CRASH',
    'Unsupported instruction': 'NOT_YET',
    "doesn't implement": 'NOT_YET',
    'from another ScarletNX version': 'CODE_OLD',
    'Cannot find the recompiled code': 'CODE_MISSING',
    "game's dump": 'DUMP_MISSING',
    'needs keys': 'KEYS',
    'Could not index the saves': 'SAVE',
    'settings: ': 'SETTING_VALUE',
    'Eden room': 'NETWORK or SETTING_VALUE',
}
CALL = re.compile(r'(?<![A-Za-z0-9_>.])(fatal|fatal_code)\s*\(')


def literal(code, start):
    """the string literals of the call's arguments from start, joined"""
    depth, i, out, cur, instr = 0, start, [], [], False
    while i < len(code):
        c = code[i]
        if instr:
            if c == '\\':
                cur.append(code[i : i + 2])
                i += 2
                continue
            if c == '"':
                instr = False
                out.append(''.join(cur))
                cur = []
            else:
                cur.append(c)
        elif c == '"':
            instr = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if not depth:
                break
        i += 1
    return ''.join(out)


failures = 0
for path in sorted((ROOT / 'src' / 'nx').rglob('*.c')):
    code = path.read_text(encoding='utf-8', errors='replace')
    code = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), code, flags=re.S)
    for m in CALL.finditer(code):
        line_start = code.rfind('\n', 0, m.start()) + 1
        if re.match(r'\s*(\[\[.*\]\]\s*)?(static\s+)?void\b', code[line_start : m.start()]):
            continue  # its definition
        msg = literal(code, m.end() - 1)
        for words, kind in KINDS.items():
            if words in msg:
                line = code.count('\n', 0, m.start()) + 1
                print(f'{path.relative_to(ROOT)}:{line}: "{msg[:80]}" stops with {m.group(1)}(): its kind is {kind}')
                failures += 1
                break
print(
    f'error_kinds: {failures} stop(s) with no kind'
    if failures
    else 'error_kinds: every stop the player meets has its kind'
)
sys.exit(1 if failures else 0)
