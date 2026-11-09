#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Every result the runtime answers comes from src/nx/hle/result_codes.inc (docs/platforms/nx/ipc.md, "Results").

Refused in the tracked sources' text, comments and string literals left out:
- RC(...) anywhere in src/ but src/nx/hle/result.h, which makes the table's constants with it: a result a file
  makes for itself;
- in src/nx/hle/, a number that is one of the table's results (0x200 and up, in hex or decimal) as a #define's value,
  the result of an R_THROW or an R_UNLESS, or a return value: a result written as a number. The table's own rows give
  module and description apart, so they are not numbers of that kind.
Tests may build results with RC(module, description): they check the codes against the sources' numbers.

    result_codes.py [--repo DIR]
"""

import argparse
import re
import sys
from pathlib import Path

from gnu_extensions import strip_comments

TABLE = 'src/nx/hle/result_codes.inc'
RC_HOME = 'src/nx/hle/result.h'
HLE = 'src/nx/hle/'
SOURCES = ('*.c', '*.h', '*.inc', '*.cpp', '*.hpp')
RC_CALL = re.compile(r'(?<![\w])RC\s*\(')
NUMBER = r'(0[xX][0-9A-Fa-f]+|[1-9][0-9]*)[uUlL]*'
NUMBERS = [
    ('a #define of a result', re.compile(r'^[ \t]*#[ \t]*define[ \t]+\w+[ \t]+\(?' + NUMBER + r'\)?[ \t]*$', re.M)),
    ('R_THROW of a result', re.compile(r'\bR_THROW\(\s*' + NUMBER + r'\s*\)')),
    ('R_UNLESS of a result', re.compile(r'\bR_UNLESS\((?:[^;]*),\s*' + NUMBER + r'\s*\)\s*;')),
    ('a return of a result', re.compile(r'\breturn\s+\(?' + NUMBER + r'\)?\s*;')),
]


def table_codes(text):
    """the table's results, {code: 'MODULE_NAME'}, from its rows"""
    rows = strip_comments(text)
    modules = {name: int(number) for name, number in re.findall(r'^SNX_RESULT_MODULE\((\w+), (\d+)\)', rows, re.M)}
    codes = {}
    for module, symbol, description in re.findall(r'^SNX_RESULT\((\w+), (\w+), (\d+),', rows, re.M):
        codes[modules[module] | int(description) << 9] = module + '_' + symbol
    return codes


def check_text(name, text, codes):
    """name's results made outside the table, as file:line: what"""
    found = []
    stripped = strip_comments(text)

    def at(match):
        return '%s:%d' % (name, stripped.count('\n', 0, match.start()) + 1)

    if name.startswith('src/') and name != RC_HOME:
        for m in RC_CALL.finditer(stripped):
            found.append('%s: RC() outside %s: a row of %s' % (at(m), RC_HOME, TABLE))
    if name.startswith(HLE) and name != TABLE:
        for what, pattern in NUMBERS:
            for m in pattern.finditer(stripped):
                value = int(m.group(1), 0)
                if value >= 0x200 and value in codes:
                    found.append('%s: %s, %s: SNX_RC_%s' % (at(m), what, m.group(1), codes[value]))
    return found


def self_test():
    """each rule finds its spelling, and none finds it in a comment, a string or a test"""
    codes = table_codes('SNX_RESULT_MODULE(SVC, 1)\nSNX_RESULT(SVC, INVALID_HANDLE, 114, InvalidHandle, SWITCHBREW)\n')
    sample = (
        '#define BAD RC(1, 114)\n'
        '#define R_BADHANDLE 0xE401\n'
        'int a(void) { R_THROW(0xE401); }\n'
        'int b(int x) { R_UNLESS(x, 58369); return 0; }\n'
        'int c(void) { return 0xE401u; }\n'
        '/* RC(1, 114) 0xE401 */ const char* d = "R_THROW(0xE401); RC(1, 2)";\n'
        '#define FINE 0xE402\n'
        'int e(void) { return 0x4; }\n'
    )
    found = [line.split(': ', 1)[1] for line in check_text('src/nx/hle/sample.c', sample, codes)]
    want = [
        'RC() outside %s: a row of %s' % (RC_HOME, TABLE),
        'a #define of a result, 0xE401: SNX_RC_SVC_INVALID_HANDLE',
        'R_THROW of a result, 0xE401: SNX_RC_SVC_INVALID_HANDLE',
        'R_UNLESS of a result, 58369: SNX_RC_SVC_INVALID_HANDLE',
        'a return of a result, 0xE401: SNX_RC_SVC_INVALID_HANDLE',
    ]
    numbers = sample.split('\n', 1)[1]  # all but the RC() line
    left_alone = check_text('tests/sample.c', sample, codes) + check_text('src/3ds/sample.c', numbers, codes)
    return found == want and not left_alone and len(codes) == 1, found + left_alone


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo', default=str(Path(__file__).resolve().parent.parent))
    args = parser.parse_args()
    ok, lines = self_test()
    if not ok:
        print('result_codes: the rules miss their own sample: %s' % lines)
        return 1
    repo = Path(args.repo)
    codes = table_codes((repo / TABLE).read_text())
    if not codes:
        print('result_codes: no result in %s' % TABLE)
        return 1
    # the tree as it is, not git's index: git refuses a checkout another user owns, as a CI container's root sees it
    files = sorted(
        str(path.relative_to(repo)).replace('\\', '/')
        for path in (repo / 'src').rglob('*')
        if path.is_file() and any(path.match(s) for s in SOURCES)
    )
    found = []
    for name in files:
        found += check_text(name, (repo / name).read_text(errors='replace'), codes)
    for line in found:
        print(line)
    if found:
        print('%d result(s) outside %s (tests/result_codes.py says which)' % (len(found), TABLE))
        return 1
    print('result_codes: %d files, %d results, every one from %s' % (len(files), len(codes), TABLE))
    return 0


if __name__ == '__main__':
    sys.exit(main())
