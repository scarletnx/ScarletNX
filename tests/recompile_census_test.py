# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/recompile-census.py on JUnit files as ctest writes them: the line of every program is printed, and one with no
test, that did not run, was skipped, said no count or passed with a part left, fails and is named. And the programs are
those of tests/CMakeLists.txt and tests/recompile_census.c."""

import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'recompile-census.py'
spec = importlib.util.spec_from_file_location('recompile_census', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
failures = 0


def check(what, ok, got=None):
    global failures
    if not ok:
        failures += 1
        print(f'FAIL: {what}' + ('' if got is None else f': {got}'))


def case(name, output='', status='run', skipped=None):
    skip = f'<skipped message={quoteattr(skipped)}/>' if skipped else ''
    return (
        f'<testcase name={quoteattr(name)} classname={quoteattr(name)} time="1.25" status="{status}">{skip}'
        f'<properties><property name="cmake_labels" value="census"/></properties>'
        f'<system-out>{escape(output)}</system-out></testcase>'
    )


WHAT = {'switch': 'functions', 'ounce': 'functions', 'nro': 'functions', 'switch32': 'AArch32 blocks', '3ds': 'blocks'}


def line(program, made=20349, of=20349):
    return f'program: 7 words\ncensus: {program}: arm64 back end: {made} of {of} {WHAT[program]}\n'


def whole():
    """{test: its case} of a run where each program's test passed, and the case of a test of another kind"""
    out = {module.TEST + program: case(module.TEST + program, line(program)) for program in module.PROGRAMS}
    out['arm_aot'] = case('arm_aot')
    return out


def run(cases, summary=None):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'tests.xml'
        path.write_text('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite>' + ''.join(cases) + '</testsuite>\n')
        env = {k: v for k, v in os.environ.items() if k != 'GITHUB_STEP_SUMMARY'}
        if summary:
            env['GITHUB_STEP_SUMMARY'] = str(summary)
        done = subprocess.run(
            [sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True, env=env, check=False
        )
    return done.returncode, done.stdout, done.stderr


def wrong(what, cases, says):
    status, said, why = run(cases.values())
    check(what, status == 1 and says in why, (status, why, said))


with tempfile.TemporaryDirectory() as folder:
    summary = Path(folder) / 'summary.md'
    status, said, why = run(whole().values(), summary)
    check('every program made', status == 0 and not why, (status, why))
    for program, what in WHAT.items():
        check(f"{program}'s line", f'census: {program}: arm64 back end: 20349 of 20349 {what} (1.2 s)' in said, said)
    check('and goes to the summary of the step', '| switch32 | arm64 | 20349 | 20349 |' in summary.read_text())
check('the programs', tuple(WHAT) == module.PROGRAMS, module.PROGRAMS)
for program in module.PROGRAMS:
    cases = whole()
    del cases[module.TEST + program]
    wrong(f'{program} with no test', cases, f'{program}: no test of it')
    cases = whole()
    cases[module.TEST + program] = case(
        module.TEST + program, 'SKIP: no native back end on this host: why\n', 'notrun', 'SKIP_RETURN_CODE=77'
    )
    wrong(
        f'{program} skipped',
        cases,
        f'{program}: not run (SKIP_RETURN_CODE=77): SKIP: no native back end on this host: why',
    )
cases = whole()
cases[module.TEST + 'nro'] = case(module.TEST + 'nro', '', 'notrun', 'Unable to find executable')
wrong('a test not built', cases, 'nro: not run (Unable to find executable): it said nothing')
cases = whole()
cases[module.TEST + 'ounce'] = case(module.TEST + 'ounce', 'program: 7 words\n')
wrong('a program with no count', cases, 'ounce: it said no count')
cases = whole()
cases[module.TEST + '3ds'] = case(module.TEST + '3ds', line('switch'))
wrong("another program's count", cases, '3ds: it said no count')
cases = whole()
cases[module.TEST + 'switch'] = case(module.TEST + 'switch', line('switch', 20348))
wrong('a part left in a test that passed', cases, 'switch: it passed with 20348 of 20349 functions')
cases[module.TEST + 'switch'] = case(module.TEST + 'switch', line('switch', 20348), 'fail')
status, said, why = run(cases.values())
check(
    "a failure is ctest's to say, and its count is printed",
    status == 0 and 'census: switch: arm64 back end: 20348 of 20349 functions (1.2 s)' in said,
    (status, said, why),
)
done = subprocess.run([sys.executable, str(SCRIPT), 'no-such.xml'], capture_output=True, text=True, check=False)
check('no file', done.returncode == 1 and 'no-such.xml' in done.stderr, done.stderr)

# the programs' names
cmake = (ROOT / 'tests' / 'CMakeLists.txt').read_text()
registered = re.search(r'set\(snx_census ([^)]*)\)', cmake)
named = re.search(r'kind_names\[KINDS\] = \{([^}]*)\}', (ROOT / 'tests' / 'recompile_census.c').read_text())
check('tests/CMakeLists.txt registers them', registered and (*registered[1].split(), '3ds') == module.PROGRAMS)
check('tests/recompile_census.c names them', named and tuple(re.findall(r'"(\w+)"', named[1])) == module.PROGRAMS)
print('recompile census: ' + (f'{failures} failures' if failures else 'ok'))
sys.exit(1 if failures else 0)
