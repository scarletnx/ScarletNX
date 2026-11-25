#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""How many of each whole program's functions and blocks the host's native back end made, from ctest's JUnit file.

    ctest --test-dir build/default --output-on-failure --output-junit "$PWD/tests.xml"
    scripts/recompile-census.py tests.xml

The tests recompile_census_* (tests/CMakeLists.txt: label census) recompile a whole program of each kind and fail where
the native back end left some of it, and ctest prints nothing of a test that passes. This prints the line each said, what
its recompile counted, and fails for a program with no test, or one that did not run or was skipped: a host that has no
native back end skips them, and every job that calls this is of a host that has one. The table goes to
GITHUB_STEP_SUMMARY too, where it is set. tests/recompile_census_test.py runs it.
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

# the programs, as tests/recompile_census.c names them
PROGRAMS = ('switch', 'ounce', 'nro', 'switch32', '3ds')
TEST = 'recompile_census_'
LINE = re.compile(r'^census: (\S+): (\S+) back end: (\d+) of (\d+) (.+?)(?:, LLVM the rest)?$', re.M)


def read(path):
    """({program: (back end, made, of, what, seconds)}, [what is wrong]) of a JUnit file of ctest's"""
    rows, wrong = {}, []
    cases = {case.get('name'): case for case in ET.parse(path).getroot().iter('testcase')}
    for program in PROGRAMS:
        case = cases.get(TEST + program)
        if case is None:
            wrong.append(f'{program}: no test of it')
            continue
        status, output = case.get('status'), case.findtext('system-out') or ''
        line = next((m for m in LINE.finditer(output) if m[1] == program), None)
        if line:
            rows[program] = (line[2], int(line[3]), int(line[4]), line[5], float(case.get('time') or 0))
        if status not in ('run', 'fail'):
            why = next((e.get('message') for e in case.iter('skipped')), None) or status
            said = output.strip().splitlines()
            wrong.append(f'{program}: not run ({why}): {said[-1] if said else "it said nothing"}')
        elif not line:
            wrong.append(f'{program}: it said no count')
        elif status == 'run' and line[3] != line[4]:
            wrong.append(f'{program}: it passed with {line[3]} of {line[4]} {line[5]}')
    return rows, wrong


def table(rows):
    lines = ['| program | back end | made | of | what | seconds |', '| --- | --- | ---: | ---: | --- | ---: |']
    for program, (back_end, made, of, what, seconds) in rows.items():
        lines.append(f'| {program} | {back_end} | {made} | {of} | {what} | {seconds:.1f} |')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('junit', help="ctest's --output-junit file")
    path = parser.parse_args().junit
    try:
        rows, wrong = read(path)
    except (OSError, ET.ParseError) as e:
        raise SystemExit(f'recompile-census: {path}: {e}') from None
    for program, (back_end, made, of, what, seconds) in rows.items():
        print(f'census: {program}: {back_end} back end: {made} of {of} {what} ({seconds:.1f} s)')
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(summary, 'a', encoding='utf-8') as f:
            print(f'### What the native back end made of whole programs\n\n{table(rows)}\n', file=f)
    for line in wrong:
        print(f'recompile-census: {line}', file=sys.stderr)
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
