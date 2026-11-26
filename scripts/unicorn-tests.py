#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""What each machine's tests under Unicorn did, from ctest's JUnit file, and whether every machine's ran.

    ctest --test-dir build/default -L '^unicorn$' --no-tests=error --output-junit "$PWD/unicorn.xml"
    scripts/unicorn-tests.py unicorn.xml

The tests of a native back end that no job runs on (tests/CMakeLists.txt: label unicorn, and the machine's) are run by
Unicorn, and a build that did not find Unicorn, or a machine that its Unicorn does not have, has none of them: ctest
passes all the same. So this fails where a machine has no test that passed of a family (sail, recompiler, arm), or not
each of its A32 and Thumb blocks' against Unicorn's own ARM core, and for a test that did not run or was skipped. The one skip it takes is PowerPC64's, for the reason the test prints: the pip wheel's
Unicorn 2.1.4 does not hold the MSR of a 64-bit PowerPC. The table it prints goes to GITHUB_STEP_SUMMARY too, where it is
set. tests/unicorn_tests_test.py runs it.
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET

# the machines, as tests/CMakeLists.txt labels their tests, and the families each has
ALL = ('sail', 'recompiler', 'arm')
MACHINES = {
    'arm64': ALL,
    'arm64win': ('recompiler', 'arm'),
    'rv64': ALL,
    'ppc64': ALL,
    'ppc64be': ALL,
    'mips64': ALL,
    'mips64be': ALL,
    'la64': ALL,
    's390x': ALL,
}
# each machine's blocks against Unicorn's ARM core (tests/arm_blocks.cmake: arm_<machine>_<test>), which a build without
# SNX_TEST_ARM_DIFF has none of
BLOCKS = ('differential', 'vector', 'thumb', 'fuzz')
# who may skip, and what the test says when it does
MAY_SKIP = ('ppc64', 'ppc64be')
SAYS = "doesn't hold (an unpatched Unicorn 2.1.4)"


class Machine:
    def __init__(self):
        self.passed = self.skipped = self.failed = 0
        self.seconds = 0.0
        self.families = {}
        self.names = set()


def read(path):
    """({machine: Machine}, [what is wrong]) of a JUnit file of ctest's"""
    machines, wrong = {name: Machine() for name in MACHINES}, []
    cases = list(ET.parse(path).getroot().iter('testcase'))
    if not cases:
        wrong.append(f'{path} has no test')
    for case in cases:
        name = case.get('name')
        labels = [p.get('value') for p in case.iter('property') if p.get('name') == 'cmake_labels']
        mine = [m for m in MACHINES if m in ';'.join(labels).split(';')]
        if len(mine) != 1:
            wrong.append(f'{name}: {len(mine)} machines among its labels {";".join(labels) or "(none)"}')
            continue
        machine = machines[mine[0]]
        family = name.split('_', 1)[0]
        machine.seconds += float(case.get('time') or 0)
        status, output = case.get('status'), case.findtext('system-out') or ''
        why = next((e.get('message') for e in case.iter('skipped')), None)
        if status == 'run':
            machine.passed += 1
            machine.families[family] = machine.families.get(family, 0) + 1
            machine.names.add(name)
        elif status == 'fail':
            machine.failed += 1
        elif why == 'SKIP_RETURN_CODE=77' and mine[0] in MAY_SKIP and SAYS in output:
            machine.skipped += 1
        else:
            machine.skipped += 1
            said = output.strip().splitlines()
            wrong.append(f'{name}: not run ({why or status}): {said[-1] if said else "it said nothing"}')
    for name, families in MACHINES.items():
        machine = machines[name]
        if not machine.passed + machine.skipped + machine.failed:
            wrong.append(f'{name}: no test of it')
        elif name not in MAY_SKIP or not machine.skipped:
            for family in families:
                if not machine.families.get(family) and not machine.failed:
                    wrong.append(f'{name}: no {family}_ test of it passed')
            for test in BLOCKS:
                if 'arm' in families and f'arm_{name}_{test}' not in machine.names and not machine.failed:
                    wrong.append(f'{name}: arm_{name}_{test} of it did not pass')
    return machines, wrong


def table(machines):
    rows = ['| machine | passed | skipped | failed | seconds |', '| --- | ---: | ---: | ---: | ---: |']
    for name, m in machines.items():
        rows.append(f'| {name} | {m.passed} | {m.skipped} | {m.failed} | {m.seconds:.0f} |')
    total = [sum(getattr(m, key) for m in machines.values()) for key in ('passed', 'skipped', 'failed', 'seconds')]
    rows.append(f'| all | {total[0]} | {total[1]} | {total[2]} | {total[3]:.0f} |')
    return '\n'.join(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('junit', help="ctest's --output-junit file")
    path = parser.parse_args().junit
    try:
        machines, wrong = read(path)
    except (OSError, ET.ParseError) as e:
        raise SystemExit(f'unicorn-tests: {path}: {e}') from None
    text = table(machines)
    print(text)
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(summary, 'a', encoding='utf-8') as f:
            print(f'### The native back ends under Unicorn\n\n{text}\n', file=f)
    for line in wrong:
        print(f'unicorn-tests: {line}', file=sys.stderr)
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
