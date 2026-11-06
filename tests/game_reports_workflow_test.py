#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Game reports workflow's log summary, built from what the workflow checks out and nothing else: the files of its
sparse list copied alone into an empty folder, and the commands of its "Build the log summary" step run there as GitHub
runs them (bash -e), so a file the summary comes to include that the list lacks fails here, not on the next issue. The
snx-logscan it makes then summarizes a log, and scripts/game-reports.py loads from the same folder. Linux, with the cc
and the C++ runtime the step links. Offline.

    tests/game_reports_workflow_test.py [REPO]   (default: the checkout this file is in)"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

WORKFLOW = Path('.github/workflows/game-reports.yml')
STEP = 'Build the log summary'
PROGRAM = 'snx-logscan'
# the build step's keys this check applies; any other (shell, working-directory, env, if) is refused
RUN_KEYS = {'name', 'run'}
LOG = """[   0.00] ScarletNX - Some Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   2.25]
*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.
"""


def fail(what):
    sys.exit(f'game_reports_workflow: {what}')


def indent(line):
    return len(line) - len(line.lstrip(' '))


def mapping(lines, col, what):
    """The mapping whose keys sit at column col: {key: (the text after its colon, the lines under it)}. A comment at
    or left of col is the YAML's; deeper, it is a block's text."""
    out = {}
    key = None
    for line in lines:
        text = line.strip()
        if text.startswith('#') and indent(line) <= col:
            continue
        if not text:
            if key is not None:
                out[key][1].append(line)
            continue
        if indent(line) < col:
            fail(f'{WORKFLOW}: {what}: "{text}" sits left of its keys (column {col})')
        if indent(line) > col:
            if key is None:
                fail(f'{WORKFLOW}: {what}: "{text}" comes before any key')
            out[key][1].append(line)
            continue
        key, colon, value = text.partition(':')
        if not colon:
            fail(f'{WORKFLOW}: {what}: "{text}" is not a key')
        if key in out:
            fail(f'{WORKFLOW}: {what}: {key} twice')
        out[key] = (value.strip(), [])
    return out


def literal(entry, what):
    """A literal block's text (key: |), its lines less the first one's indentation"""
    value, lines = entry
    if value != '|':
        fail(f'{WORKFLOW}: {what} is "{value}", not a literal block (|), the one form this check reads')
    while lines and not lines[-1].strip():
        lines = lines[:-1]
    if not lines:
        fail(f'{WORKFLOW}: {what} is empty')
    col = indent(lines[0])
    return ''.join(line[col:] + '\n' for line in lines)


def steps(lines):
    """Each step of the workflow's one steps: list, as a mapping"""
    at = [i for i, line in enumerate(lines) if line.strip() == 'steps:']
    if len(at) != 1:
        fail(f'{WORKFLOW} has {len(at)} steps: lists; this check reads a workflow of one job')
    under = []
    for line in lines[at[0] + 1 :]:
        if line.strip() and not line.lstrip().startswith('#') and indent(line) <= indent(lines[at[0]]):
            break
        under.append(line)
    items = [line for line in under if line.strip() and not line.lstrip().startswith('#')]
    if not items or not items[0].lstrip().startswith('- '):
        fail(f'{WORKFLOW}: its steps: holds no list')
    dash = indent(items[0])
    found = []
    for line in under:
        if indent(line) == dash and line.lstrip().startswith('- '):
            found.append([line[:dash] + '  ' + line[dash + 2 :]])
        elif found:
            found[-1].append(line)
    return [mapping(step, dash + 2, f'step {n + 1}') for n, step in enumerate(found)]


def one(found, what):
    if len(found) != 1:
        fail(f'{WORKFLOW} has {len(found)} {what}, not one')
    return found[0]


def read(repo):
    """The files the workflow checks out, and the build step's commands"""
    every = steps((repo / WORKFLOW).read_text(encoding='utf-8').splitlines())
    checkout = one([s for s in every if s.get('uses', ('',))[0].startswith('actions/checkout@')], 'checkout steps')
    if 'with' not in checkout:
        fail(f'{WORKFLOW}: its checkout has no with:, so no sparse list to build from')
    body = checkout['with'][1]
    options = mapping(body, min(indent(line) for line in body if line.strip()), 'the checkout')
    if 'sparse-checkout' not in options:
        fail(f'{WORKFLOW}: its checkout has no sparse-checkout list')
    cone = options.get('sparse-checkout-cone-mode', ('true',))[0]
    if cone != 'false':
        fail(
            f'{WORKFLOW}: sparse-checkout-cone-mode is {cone}: cone mode checks out whole folders and the files at the'
            ' top, and this check copies the files listed alone'
        )
    files = [line.strip() for line in literal(options['sparse-checkout'], 'sparse-checkout').splitlines()]
    files = [name for name in files if name]
    build = one([s for s in every if s.get('name', ('',))[0] == STEP], f'steps named "{STEP}"')
    other = sorted(set(build) - RUN_KEYS)
    if other:
        fail(
            f'{WORKFLOW}: "{STEP}" sets {", ".join(other)}, which this check does not apply: it runs the commands as'
            ' bash -e in the checkout, with nothing else'
        )
    return files, literal(build['run'], f'the run of "{STEP}"')


def main():
    if len(sys.argv) > 2:
        fail('usage: tests/game_reports_workflow_test.py [REPO]')
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    files, commands = read(repo)
    with tempfile.TemporaryDirectory(prefix='snx-game-reports-') as tmp:
        tree = Path(tmp) / 'checkout'
        tree.mkdir()
        for name in files:
            if any(c in name for c in '*?[!#\\') or name.startswith('/'):
                fail(f'{WORKFLOW}: "{name}" in the sparse list is a pattern; this check copies files named by path')
            source = repo / name
            if not source.is_file():
                fail(f'{WORKFLOW}: "{name}" in the sparse list is no file of {repo}')
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, tree / name)
        script = Path(tmp) / 'build-step.sh'
        script.write_text(commands, encoding='utf-8')
        built = subprocess.run(['bash', '-e', str(script)], cwd=tree, capture_output=True, text=True, check=False)
        if built.returncode:
            sys.stdout.write(built.stdout + built.stderr)
            fail(
                f'"{STEP}" exits {built.returncode} in a folder of only the {len(files)} files its checkout lists'
                f' (its output above): a file it needs goes into the sparse-checkout list of {WORKFLOW}'
            )
        program = tree / PROGRAM
        if not program.is_file():
            fail(f'"{STEP}" made no {PROGRAM}, which the Summarize step runs')
        scanned = subprocess.run(
            [str(program), '-'], input=LOG, cwd=tree, capture_output=True, text=True, timeout=60, check=False
        )
        if scanned.returncode:
            fail(f'{PROGRAM} exits {scanned.returncode} on a log: {scanned.stderr.strip()}')
        summary = scanned.stdout
        try:
            title = json.loads(summary).get('title')
        except ValueError as e:
            fail(f"{PROGRAM}'s summary is no JSON ({e}):\n{summary}")
        if title != 'Some Game' or 'nn::diag::detail::VAbortImpl()' not in summary:
            fail(f"{PROGRAM}'s summary lacks the log's title or its frame demangled:\n{summary}")
        loaded = subprocess.run(
            [sys.executable, 'scripts/game-reports.py', '--help'], cwd=tree, capture_output=True, text=True, check=False
        )
        if loaded.returncode:
            fail(f'scripts/game-reports.py --help exits {loaded.returncode} in the checkout: {loaded.stderr.strip()}')
    print(f'game_reports_workflow: "{STEP}" builds {PROGRAM} from the {len(files)} files of the sparse list')
    return 0


if __name__ == '__main__':
    sys.exit(main())
