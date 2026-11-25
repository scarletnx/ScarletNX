#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/unicorn-tests.py on JUnit files as ctest writes them: every machine run passes, and one with no test, with a
family of none that passed, with a test that did not run or was skipped, fails and is named; PowerPC64's skip is taken
for its one reason alone. And the machines are those tests/CMakeLists.txt labels."""

import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'unicorn-tests.py'
spec = importlib.util.spec_from_file_location('unicorn_tests', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
failures = 0


def check(what, ok, got=None):
    global failures
    if not ok:
        failures += 1
        print(f'FAIL: {what}' + ('' if got is None else f': {got}'))


SKIP = "ppc64le unicorn: MSR 9000000000000001 doesn't hold (an unpatched Unicorn 2.1.4)\nSKIP: no Unicorn of ppc64le\n"


def case(name, labels, status='run', skipped=None, output='', seconds='1.5'):
    skip = f'<skipped message={quoteattr(skipped)}/>' if skipped else ''
    labelled = f'<property name="cmake_labels" value={quoteattr(labels)}/>' if labels else ''
    return (
        f'<testcase name={quoteattr(name)} classname={quoteattr(name)} time="{seconds}" status="{status}">{skip}'
        f'<properties>{labelled}</properties><system-out>{escape(output)}</system-out></testcase>'
    )


def whole():
    """{test: its case} of a run where every machine's tests passed, PowerPC64's being skipped as the wheel's are"""
    out = {}
    for machine, families in module.MACHINES.items():
        for family in families:
            blocks = [f'arm_{machine}_{test}' for test in module.BLOCKS] if family == 'arm' else []
            for name in [f'{family}_{machine}', f'{family}_{machine}_spill', *blocks]:
                if machine in module.MAY_SKIP and family != 'recompiler':
                    out[name] = case(name, f'unicorn;{machine}', 'notrun', 'SKIP_RETURN_CODE=77', SKIP)
                else:
                    out[name] = case(name, f'unicorn;{machine}')
    return out


def run(cases, summary=None):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'unicorn.xml'
        path.write_text('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite>' + ''.join(cases) + '</testsuite>\n')
        env = {k: v for k, v in os.environ.items() if k != 'GITHUB_STEP_SUMMARY'}
        if summary:
            env['GITHUB_STEP_SUMMARY'] = str(summary)
        done = subprocess.run(
            [sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True, env=env, check=False
        )
    return done.returncode, done.stdout, done.stderr


def wrong(what, cases, *says):
    status, said, why = run(cases)
    check(what, status == 1 and all(s in why for s in says), (status, why, said))


all_ok = whole()
with tempfile.TemporaryDirectory() as folder:
    summary = Path(folder) / 'summary.md'
    status, said, why = run(all_ok.values(), summary)
    check('every machine ran', status == 0 and not why, (status, why))
    check('what each did is said', '| rv64 | 10 | 0 | 0 | 15 |' in said and '| ppc64 | 2 | 8 | 0 | 15 |' in said, said)
    check('and goes to the summary of the step', '| all |' in summary.read_text())
for machine in module.MACHINES:
    left = [c for n, c in all_ok.items() if f'{machine}' != n.split('_')[1]]
    wrong(f'{machine} with no test', left, f'{machine}: no test of it')
wrong('no test at all', [], 'has no test', 'rv64: no test of it')
for family in ('sail', 'recompiler', 'arm'):
    cases = {n: c for n, c in all_ok.items() if n.split('_')[:2] != [family, 'mips64']}
    wrong(f'a machine with no {family} test', cases.values(), f'mips64: no {family}_ test of it passed')
for test in module.BLOCKS:
    cases = {n: c for n, c in all_ok.items() if n != f'arm_rv64_{test}'}
    wrong(f'a machine with no {test} of its blocks', cases.values(), f'rv64: arm_rv64_{test} of it did not pass')
cases = dict(all_ok)
cases['arm_rv64'] = case('arm_rv64', 'unicorn;rv64', 'notrun', 'SKIP_RETURN_CODE=77', 'SKIP: no Unicorn riscv64\n')
wrong('another machine skipped', cases.values(), 'arm_rv64: not run (SKIP_RETURN_CODE=77): SKIP: no Unicorn riscv64')
cases = dict(all_ok)
cases['arm_ppc64'] = case('arm_ppc64', 'unicorn;ppc64', 'notrun', 'SKIP_RETURN_CODE=77', 'SKIP: no window\n')
wrong(
    "PowerPC64's skip for another reason", cases.values(), 'arm_ppc64: not run (SKIP_RETURN_CODE=77): SKIP: no window'
)
cases = dict(all_ok)
cases['sail_la64'] = case(
    'sail_la64', 'unicorn;la64', 'notrun', 'Unable to find executable', 'Unable to find executable: x'
)
wrong(
    'a test not built', cases.values(), 'sail_la64: not run (Unable to find executable): Unable to find executable: x'
)
cases = dict(all_ok)
cases['sail_la64'] = case('sail_la64', 'unicorn;la64', 'disabled', output='Disabled')
wrong('a test disabled', cases.values(), 'sail_la64: not run (disabled): Disabled')
cases = dict(all_ok)
cases['recompiler_s390x'] = case('recompiler_s390x', 'unicorn;s390x', 'fail')
status, said, why = run(cases.values())
check(
    "a failure is ctest's to say, and is counted",
    status == 0 and '| s390x | 9 | 0 | 1 | 15 |' in said,
    (status, said, why),
)
cases = dict(all_ok)
cases['arm_sparc'] = case('arm_sparc', 'unicorn')
wrong('a test of no machine', cases.values(), 'arm_sparc: 0 machines among its labels unicorn')
done = subprocess.run([sys.executable, str(SCRIPT), 'no-such.xml'], capture_output=True, text=True, check=False)
check('no file', done.returncode == 1 and 'no-such.xml' in done.stderr, done.stderr)

# the machines of the labels
text = (ROOT / 'tests' / 'CMakeLists.txt').read_text()
labelled = re.search(r'MATCHES "\^\(sail\|recompiler\|arm\)_\(([a-z0-9|]+)\)\(_\|\$\)"\)', text)
check('tests/CMakeLists.txt labels them', labelled)
if labelled:
    check('the same machines', set(labelled[1].split('|')) == set(module.MACHINES), labelled[1])
print('unicorn tests: ' + (f'{failures} failures' if failures else 'ok'))
sys.exit(1 if failures else 0)
