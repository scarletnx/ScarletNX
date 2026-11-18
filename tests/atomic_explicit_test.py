#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""cmake/atomic_explicit.cmake as the build tree has it: every C source of src/ that the tree compiles is compiled with
-Werror=atomic-implicit-seq-cst, and no test's own source is (tests keep C's plain accesses). Reads the tree's
compile_commands.json; offline."""

import json
import sys
from pathlib import Path

tree, repo = Path(sys.argv[1]), Path(sys.argv[2]).resolve()
FLAG = '-Werror=atomic-implicit-seq-cst'
commands = json.loads((tree / 'compile_commands.json').read_text())
ours, tests, missing, extra = 0, 0, [], []
for entry in commands:
    source = Path(entry['file']).resolve()
    command = entry.get('command') or ' '.join(entry.get('arguments', []))
    if source.suffix != '.c' or ' -x c++' in command:
        continue
    relative = source.relative_to(repo) if source.is_relative_to(repo) else None
    if relative is None:
        continue
    if relative.parts[0] == 'src' and not {'.cache', '_deps'} & set(relative.parts):
        ours += 1
        if FLAG not in command:
            missing.append(str(relative))
    elif relative.parts[0] == 'tests':
        tests += 1
        if FLAG in command:
            extra.append(str(relative))
failed = False
if not ours:
    print('atomic_explicit: the tree compiles no C source of src/: nothing to check', file=sys.stderr)
    failed = True
for name in sorted(set(missing))[:10]:
    print(f'atomic_explicit: {name} is compiled without {FLAG}', file=sys.stderr)
    failed = True
for name in sorted(set(extra))[:10]:
    print(f'atomic_explicit: the test source {name} is compiled with {FLAG}', file=sys.stderr)
    failed = True
print(
    f'atomic_explicit: {ours} compiles of src/ with {FLAG}, {tests} of tests/ without'
    if not failed
    else 'atomic_explicit: FAILED'
)
sys.exit(1 if failed else 0)
