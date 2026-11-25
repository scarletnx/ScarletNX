#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Which of the recompiler's long tests a change calls for: the paths it changes, on standard input, a line each.

    git diff --name-only BASE | scripts/recompiler-changes.py >> "$GITHUB_OUTPUT"

It prints an answer a line, as a step's outputs, and on standard error which paths called for it:

    differential   the A64 differential against QEMU, on the cross builds
    unicorn        the native back ends of the machines that no job runs on, under Unicorn (ctest -L unicorn)

.github/workflows/build.yml asks in a pull request; a push, a nightly and a run by hand run them all. unicorn's paths are
the differential's, what makes the back ends' code, and those tests. tests/recompiler_changes_test.py runs it.
"""

import sys

# a path that starts as one of these
DIFFERENTIAL = (
    'src/common/cpu/',
    'tests/llvm_qemu_',
    'tests/llvm_steps.c',
    'tests/qemu_ref/',
    'cmake/cross.cmake',
    'cmake/decodetree.cmake',
    'cmake/llvm.cmake',
    'cmake/llvm_components.cmake',
    'cmake/qemu_ref.cmake',
    'scripts/linux-llvm.sh',
    'scripts/llvm-libraries/',
    '.github/workflows/build.yml',
)
UNICORN = (
    'tools/decodetree/',
    'tools/sailgen/',
    'cmake/sailgen.cmake',
    'cmake/sdm.cmake',
    'src/nx/loader/module_arm.',
    'src/nx/loader/module_x64.',
    'tests/arm_call_abi',
    'tests/arm_native_modes.cmake',
    'tests/arm_x64.c',
    'tests/module_desc.c',
    'tests/native_unwind.c',
    'tests/recompiler_backends.c',
    'tests/sail_mach.c',
    'tests/sail_mach_emit.c',
    'tests/support/arm64_unwinder.h',
    'tests/support/host_near.h',
    'tests/support/host_unicorn.',
    'tests/support/la64_interp.',
    'scripts/recompiler-changes.py',
    'scripts/unicorn-tests.py',
)
ANSWERS = {'differential': DIFFERENTIAL, 'unicorn': DIFFERENTIAL + UNICORN}


def main():
    changed = [line.strip() for line in sys.stdin if line.strip()]
    for name, starts in ANSWERS.items():
        matched = [path for path in changed if path.startswith(starts)]
        if matched:
            print(f'{name}: {len(matched)} of the {len(changed)} files changed call for it:', file=sys.stderr)
            for path in matched:
                print(f'  {path}', file=sys.stderr)
        else:
            print(f'{name}: none of the {len(changed)} files changed calls for it', file=sys.stderr)
        print(f'{name}={"true" if matched else "false"}')


if __name__ == '__main__':
    main()
