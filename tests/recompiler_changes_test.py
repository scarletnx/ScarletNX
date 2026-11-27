#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/recompiler-changes.py's answer for the paths a change has, the line that says why, and that every path it
names by its file is there"""

import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'recompiler-changes.py'
failures = 0


def check(what, ok, got=None):
    global failures
    if not ok:
        failures += 1
        print(f'FAIL: {what}' + ('' if got is None else f': {got}'))


def answers(*paths):
    done = subprocess.run(
        [sys.executable, str(SCRIPT)],
        input=''.join(f'{p}\n' for p in paths),
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    return done.returncode, done.stdout, done.stderr


for paths, differential, why in (
    ((), False, 'no file'),
    (('src/gui/main.c', 'docs/user/gui.md', 'tests/gui_steps.c'), False, 'the menu'),
    (('src/common/cpu/recompiler/mach/mach_codegen.inc',), True, 'a back end'),
    (('src/common/cpu/aarch64/cpu_helpers.c',), True, 'the helpers the code calls'),
    (('tests/native_qemu_words.c',), True, 'the words both run'),
    (('tests/native_steps.c',), True, 'the native steps the differential runs'),
    (('tests/sail_x64.c',), True, "sail_x64's words, whose QEMU ends the recording holds"),
    (('tools/sailgen/mach_a32.h',), True, 'what generates them'),
    (('scripts/ci/linux.sh',), True, 'the steps that build and record it'),
    (('.github/workflows/build.yml',), True, 'the workflow'),
    (('src/nx/loader/module_a64.c', 'tests/support/host_win64.c'), False, "another module's and another host's"),
    (('src/gui/main.c', 'src/common/cpu/arm/aot.c'), True, 'one of two'),
):
    status, said, _ = answers(*paths)
    check(why, status == 0 and said.startswith(f'differential={str(differential).lower()}\nwhy='), (status, said))
status, said, why_said = answers('src/common/cpu/arm/aot.c', 'docs/a.md')
check(
    'the line names how many call for it and the first',
    said.endswith('why=1 of the 2 files changed call for it, src/common/cpu/arm/aot.c first\n'),
    said,
)
check(
    'it says on standard error which path calls for it',
    'differential: 1 of the 2 files changed call for it:\n  src/common/cpu/arm/aot.c\n' in why_said,
    why_said,
)
status, said, _ = answers('docs/a.md', 'src/gui/main.c')
check(
    'a skip says none calls for it, and the script that decided',
    said.endswith('why=none of the 2 files changed calls for it (scripts/recompiler-changes.py)\n'),
    said,
)
# every file or folder it names is there, so that one moved does not stop calling for its tests
spec = importlib.util.spec_from_file_location('recompiler_changes', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
files = [
    f.relative_to(ROOT).as_posix()
    for top in ('src', 'tests', 'tools', 'cmake', 'scripts', '.github')
    for f in (ROOT / top).rglob('*')
    if f.is_file()
]
for start in module.DIFFERENTIAL:
    check(f'{start} starts the path of a file of the checkout', any(f.startswith(start) for f in files))
print('recompiler changes: ' + (f'{failures} failures' if failures else 'ok'))
sys.exit(1 if failures else 0)
