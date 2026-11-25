#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/recompiler-changes.py's answers for the paths a change has, and that every path it names by its file is there"""

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


for paths, differential, unicorn, why in (
    ((), False, False, 'no file'),
    (('src/gui/main.c', 'docs/user/gui.md', 'tests/gui_steps.c'), False, False, 'the menu'),
    (('src/common/cpu/recompiler/mach/mach_codegen.inc',), True, True, 'a back end'),
    (('src/common/cpu/aarch64/cpu_helpers.c',), True, True, 'the helpers the code calls'),
    (('tests/llvm_qemu_words.c',), True, True, 'the words both run'),
    (('cmake/llvm.cmake',), True, True, 'the LLVM both compare with'),
    (('.github/workflows/build.yml',), True, True, 'the workflow'),
    (('tools/sailgen/mach_a32.h',), False, True, "what generates a back end's code"),
    (('cmake/sailgen.cmake',), False, True, 'how it is generated'),
    (('src/nx/loader/module_x64.c',), False, True, "a module's descriptor"),
    (('tests/sail_mach.c',), False, True, 'a test of theirs'),
    (('tests/support/host_unicorn.c',), False, True, 'their host'),
    (('tests/arm_native_modes.cmake',), False, True, 'their modes'),
    (('scripts/unicorn-tests.py',), False, True, 'what counts them'),
    (
        ('src/nx/loader/module_llvm.c', 'tests/support/host_win64.c'),
        False,
        False,
        "another module's and another host's",
    ),
    (('src/gui/main.c', 'tests/arm_x64.c'), False, True, 'one of two'),
):
    status, said, _ = answers(*paths)
    check(
        why,
        status == 0 and said == f'differential={str(differential).lower()}\nunicorn={str(unicorn).lower()}\n',
        (status, said),
    )
status, said, why_said = answers('src/common/cpu/arm/aot.c', 'docs/a.md')
check(
    'it says which path calls for it, and how many do not',
    'unicorn: 1 of the 2 files changed call for it:\n  src/common/cpu/arm/aot.c\n' in why_said,
    why_said,
)
check('none is said', 'unicorn: none of the 1 files changed calls for it' in answers('docs/a.md')[2])

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
for start in module.DIFFERENTIAL + module.UNICORN:
    check(f'{start} starts the path of a file of the checkout', any(f.startswith(start) for f in files))
print('recompiler changes: ' + (f'{failures} failures' if failures else 'ok'))
sys.exit(1 if failures else 0)
