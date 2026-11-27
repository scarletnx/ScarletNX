#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Whether a pull request calls for the A64 differential against QEMU: the paths it changes, on standard input, a line
each.

    git diff --name-only HEAD^1 HEAD | scripts/recompiler-changes.py >> "$GITHUB_OUTPUT"

It prints two outputs for a step: differential=true or false, and why=, a line saying which of the changed files call
for it, or that none does; on standard error each of those files. The differential is qemu-reference's recording and
appimage-cross's run of it (.github/workflows/build.yml), and the recording holds QEMU's ends of sail_x64's words, which
the Windows job reads: its paths are what makes either. A pull request asks; a run by hand and a version's tag run it
whatever changed. tests/recompiler_changes_test.py runs it.
"""

import sys

# a path that starts as one of these
DIFFERENTIAL = (
    'src/common/cpu/',
    'tests/native_qemu_',
    'tests/native_steps',
    'tests/qemu_ref/',
    'tests/sail_x64',
    'tools/sailgen/',
    'cmake/cross.cmake',
    'cmake/decodetree.cmake',
    'cmake/qemu_ref.cmake',
    'cmake/sailgen.cmake',
    'cmake/sdm.cmake',
    'scripts/ci/linux.sh',
    'scripts/ci/cross.sh',
    'scripts/recompiler-changes.py',
    '.github/workflows/build.yml',
)


def main():
    changed = [line.strip() for line in sys.stdin if line.strip()]
    matched = [path for path in changed if path.startswith(DIFFERENTIAL)]
    if matched:
        why = f'{len(matched)} of the {len(changed)} files changed call for it, {matched[0]} first'
        print(f'differential: {len(matched)} of the {len(changed)} files changed call for it:', file=sys.stderr)
        for path in matched:
            print(f'  {path}', file=sys.stderr)
    else:
        why = f'none of the {len(changed)} files changed calls for it (scripts/recompiler-changes.py)'
        print(f'differential: {why}', file=sys.stderr)
    print(f'differential={"true" if matched else "false"}')
    print(f'why={why}')


if __name__ == '__main__':
    main()
