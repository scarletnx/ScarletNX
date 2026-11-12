#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The keys an older version's settings had, held to what a run tells of them, each failure a line that names its file
and line:

- every key src/nx/runtime/settings.txt had in a state of main (git log --first-parent, its first state on) and the
  working tree's file has not is named by src/nx/runtime/ini.c's INI_DEPRECATED, which keeps it under a game's
  [Deprecated] with what became of it, and by a `.ini` key row of docs/user/deprecations.md's table under ## Now
- the two name the same keys, and none of them is a key settings.txt has now: the runtime never reads a deprecated one

A key is read as settingsgen makes it: <N> of a repeat and <ID> of an each written out, a vulkan table's @NAME as name
in lowercase. An llvm table's keys are LLVM's passes and options, which LLVM keeps, so they are not read. A state is the
file in a commit of HEAD's first-parent history that changed it; --since REV reads REV's and those after it (a pull
request's base: what the pull request drops). A shallow clone reads the states it has, and says from which.

    scripts/check-deprecations.py [--root DIR] [--since REV]

Exit 0, 1 for a failure, 2 for a file it cannot read as it should, 77 without git or a checkout (ctest's skip).
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = 'src/nx/runtime/settings.txt'
INI_C = 'src/nx/runtime/ini.c'
DOC = 'docs/user/deprecations.md'
SKIP = 77

KEY = re.compile(r'^[a-z_][a-z0-9_]*$')
DEPRECATED_TABLE = re.compile(r'\bINI_DEPRECATED\[\]\s*=\s*\{(.*?)\{NULL,\s*NULL\}\};', re.DOTALL)
DEPRECATED_ROW = re.compile(r'\{"([^"]*)",')
CODE = re.compile(r'`([^`]*)`')


class Failures(list):
    def add(self, path, line, text):
        self.append(f'{path}:{line}: {text}' if line else f'{path}: {text}')


class Unreadable(Exception):
    pass


def settings_keys(text):
    """{key} of a settings.txt's text, as settingsgen makes them"""
    keys, lists, blocks = set(), {}, []
    kind = 'runtime'
    current = None  # the setting being read: [key, each]

    def done():
        if current is None or current[0] is None or kind == 'llvm':
            return
        key, each = current
        repeats = [n for n in blocks if n]
        names = [key]
        if '<N>' in key:
            names = [key.replace('<N>', str(n)) for n in range(1, (repeats[-1] if repeats else 1) + 1)]
        if '<ID>' in key and each in lists:
            names = [name.replace('<ID>', i) for name in names for i in lists[each]]
        keys.update(names)

    for raw in text.split('\n'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        word, _, rest = line.partition(' ')
        rest = rest.strip()
        if word in ('table', 'section', 'include', 'list', 'repeat', 'needs', 'end'):
            done()
            current = None
            if word == 'table':
                parts = rest.split()
                kind = parts[1] if len(parts) > 1 else 'runtime'
            elif word == 'list':
                name, *items = rest.split()
                lists[name] = [item.partition('=')[0] for item in items]
            elif word == 'repeat':
                blocks.append(int(rest.split()[0]) if rest.split() and rest.split()[0].isdigit() else 1)
            elif word == 'needs':
                blocks.append(0)
            elif word == 'end' and blocks:
                blocks.pop()
            continue
        if word == 'setting':
            done()
            current = [rest[1:].lower() if kind == 'vulkan' and rest.startswith('@') else None, None]
        elif current is not None and word == 'key':
            current[0] = rest
        elif current is not None and word == 'each':
            current[1] = rest
    done()
    return keys


def deprecated_keys(root):
    """({key: line} of ini.c's INI_DEPRECATED, the line it starts on)"""
    path = root / INI_C
    try:
        text = path.read_text(encoding='utf-8')
    except OSError as e:
        raise Unreadable(f'{INI_C}: {e.strerror}') from e
    table = DEPRECATED_TABLE.search(text)
    if not table:
        raise Unreadable(f'{INI_C}: no INI_DEPRECATED[] = {{...{{NULL, NULL}}}}; in it')
    keys = {}
    for row in DEPRECATED_ROW.finditer(table.group(1)):
        keys.setdefault(row.group(1), text.count('\n', 0, table.start(1) + row.start()) + 1)
    return keys, text.count('\n', 0, table.start()) + 1


def doc_keys(root):
    """({key: line} of the `.ini` key rows of the table under ## Now, the line of ## Now). A row's keys are the names in
    code in its first cell outside parentheses: `.ini` keys `a`, `b` (what `0` was)"""
    path = root / DOC
    try:
        lines = path.read_text(encoding='utf-8').split('\n')
    except OSError as e:
        raise Unreadable(f'{DOC}: {e.strerror}') from e
    keys, now, inside = {}, 0, False
    for number, line in enumerate(lines, 1):
        if line.startswith('#'):
            inside = line.strip() == '## Now'
            now = number if inside else now
            continue
        if not inside or not line.startswith('|'):
            continue
        cell = line.split('|')[1].strip()
        if not re.match(r'`\.ini` keys? ', cell):
            continue
        while True:  # the parentheses' words, innermost first, are no key
            bare = re.sub(r'\([^()]*\)', '', cell)
            if bare == cell:
                break
            cell = bare
        for name in CODE.findall(cell):
            if KEY.match(name):
                keys.setdefault(name, number)
    if not now:
        raise Unreadable(f'{DOC}: no ## Now section')
    return keys, now


def git(root, *args, data=None):
    run = subprocess.run(['git', '-C', str(root), *args], input=data, capture_output=True, check=False)
    if run.returncode:
        raise Unreadable(f'git {" ".join(args)}: {run.stderr.decode(errors="replace").strip()}')
    return run.stdout


def states(root, since):
    """[(commit, short hash, subject, text)] of settings.txt, the oldest first: each first-parent commit of HEAD that
    changed it (since's own state first, with since), then the working tree's file"""
    span = [f'{since}..HEAD'] if since else ['HEAD']
    log = git(root, 'log', '--first-parent', '--format=%H%x09%h%x09%cs %s', *span, '--', SETTINGS).decode()
    commits = [line.split('\t', 2) for line in reversed(log.splitlines()) if line]
    if since:
        base = git(root, 'log', '-1', '--format=%H%x09%h%x09%cs %s', since).decode().strip()
        commits.insert(0, base.split('\t', 2))
    batch = git(root, 'cat-file', '--batch', data=''.join(f'{c[0]}:{SETTINGS}\n' for c in commits).encode())
    out, at = [], 0
    for commit, short, subject in commits:
        head_end = batch.index(b'\n', at)
        head = batch[at:head_end].split()
        at = head_end + 1
        if head[-1] == b'missing':  # not there in this commit: before the file was made, or since's
            continue
        size = int(head[2])
        out.append((commit, short, subject, batch[at : at + size].decode('utf-8', errors='replace')))
        at += size + 1
    try:
        out.append((None, 'the working tree', '', (root / SETTINGS).read_text(encoding='utf-8')))
    except OSError as e:
        raise Unreadable(f'{SETTINGS}: {e.strerror}') from e
    return out


def gone_keys(read):
    """{key: (short hash, subject) of the state that dropped it} of the keys a state had and the last has not"""
    now = settings_keys(read[-1][3])
    gone, before = {}, set()
    for _, short, subject, text in read:
        keys = settings_keys(text)
        for key in before - keys:
            gone[key] = (short, subject)
        before = keys
    return {key: why for key, why in gone.items() if key not in now}, now


def check(root, since=None):
    """(failures, what was read) of the checkout at root"""
    failures = Failures()
    read = states(root, since)
    gone, now = gone_keys(read)
    deprecated, table_line = deprecated_keys(root)
    documented, now_line = doc_keys(root)
    for key in sorted(gone):
        short, subject = gone[key]
        dropped = f'{SETTINGS} dropped it in {short}' + (f' ({subject})' if subject else '')
        if key not in deprecated:
            failures.add(INI_C, table_line, f'{key}: {dropped}, and INI_DEPRECATED does not name it')
        if key not in documented:
            failures.add(DOC, now_line, f'{key}: {dropped}, and no `.ini` key row of ## Now names it')
    for key in sorted(set(deprecated) - set(documented)):
        failures.add(DOC, now_line, f'{key}: INI_DEPRECATED ({INI_C}:{deprecated[key]}) names it, no row of ## Now')
    for key in sorted(set(documented) - set(deprecated)):
        failures.add(INI_C, table_line, f'{key}: {DOC}:{documented[key]} names it, INI_DEPRECATED does not')
    for key in sorted(set(deprecated) & now):
        failures.add(INI_C, deprecated[key], f'{key}: deprecated, and {SETTINGS} has it: the runtime never reads it')
    first = read[0]
    shallow = git(root, 'rev-parse', '--is-shallow-repository').decode().strip() == 'true'
    said = (
        f'{len(read)} states of {SETTINGS} read, from {first[1]} ({first[2]}) to the working tree'
        + (': a shallow clone, the states before it not read' if shallow and not since else '')
        + f'; {len(gone)} keys it dropped, {len(deprecated)} deprecated'
    )
    return failures, said


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--root', type=Path, default=ROOT, help="the checkout (this script's by default)")
    parser.add_argument('--since', metavar='REV', help="read REV's state of settings.txt and the ones after it")
    args = parser.parse_args()
    if not shutil.which('git'):
        print("git is not on PATH: no state of settings.txt but the working tree's can be read", file=sys.stderr)
        return SKIP
    probe = subprocess.run(['git', '-C', str(args.root), 'rev-parse'], capture_output=True, check=False)
    if probe.returncode:
        why = probe.stderr.decode(errors='replace').strip()
        print(f'{args.root} is no git checkout git can read ({why}): its history cannot be read', file=sys.stderr)
        return SKIP
    try:
        failures, said = check(args.root, args.since)
    except Unreadable as e:
        print(e, file=sys.stderr)
        return 2
    for failure in failures:
        print(failure, file=sys.stderr)
    print(said if not failures else f'{said}: {len(failures)} failures', file=sys.stderr if failures else sys.stdout)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
