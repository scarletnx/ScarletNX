#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Records recompiler_backends' frozen files (test-references' recompiler_backends/) from a build that has LLVM: for
each file a ctest test reads (--frozen FILE), snx-test-recompiler-backends runs that test's programs with --record FILE,
and then every test that reads one of the files runs as ctest has it, and must pass or skip. A file of the folder that no
test of this build reads (another machine's, under the Unicorn this build lacks) is recorded again from its args line.

    recompiler_backends_record.py BUILD --out DIR [FILE...]

DIR is the recompiler_backends folder of a test-references clone, the one BUILD's tests read: BUILD configured with
-DCPM_test-references_SOURCE=<the clone>. The files written there go up by a commit of that repository and its new pin
in cpmfile.json.

FILE names some of the files (default.txt, or a path); with none, all of them. The generator's options are the test's
command line's; a test that gives none (recompiler_backends itself) takes the file's args line, so a new file of no
options is recorded once by hand: snx-test-recompiler-backends --record FILE --functions 600 --states 8. It prints each
file with the test whose programs it holds, what was drawn, and how many functions the native back end ran otherwise."""

import json
import os
import re
import subprocess
import sys

# recompiler_backends.c's generator_option: the options that decide what is drawn, with how many words each takes (-1:
# the words up to the next option)
GENERATOR = {
    '--functions': 1,
    '--states': 1,
    '--seed': 1,
    '--target': 1,
    '--long': 2,
    '--ops': -1,
    '--words': -1,
    '--programs': 0,
    '--calls': 0,
    '--calls-back': 0,
}


def generator_words(words):
    """the generator's options among words, in their order"""
    out, i = [], 0
    while i < len(words):
        n = GENERATOR.get(words[i])
        if n is None:
            i += 1
            continue
        end = (
            i + 1 + n if n >= 0 else next((k for k in range(i + 1, len(words)) if words[k].startswith('-')), len(words))
        )
        out += words[i:end]
        i = end
    return out


def main():
    args = sys.argv[1:]
    if len(args) < 3 or args[0].startswith('-') or args[1] != '--out':
        sys.exit(__doc__)
    build, out, only = args[0], os.path.realpath(args[2]), args[3:]
    listing = subprocess.run(
        ['ctest', '--test-dir', build, '--show-only=json-v1'], capture_output=True, text=True, check=False
    )
    if listing.returncode:
        sys.exit(f'ctest --show-only in {build}: {listing.stderr.strip()}')
    files = {}  # file -> [(test, command, working directory)]
    for test in json.loads(listing.stdout)['tests']:
        command = test.get('command') or []
        if '--frozen' not in command or not os.path.basename(command[0]).startswith('snx-test-recompiler-'):
            continue
        at = command.index('--frozen')
        cwd = next((p['value'] for p in test.get('properties', []) if p['name'] == 'WORKING_DIRECTORY'), build)
        files.setdefault(command[at + 1], []).append((test['name'], command[:at] + command[at + 2 :], cwd))
    elsewhere = sorted(f for f in files if os.path.dirname(os.path.realpath(f)) != out)
    if elsewhere:
        sys.exit(
            f"--out {out}: {build}'s tests read {elsewhere[0]}, not a file there: configure it with "
            '-DCPM_test-references_SOURCE=<the clone>'
        )
    if not os.path.exists(os.path.join(out, os.pardir, '.git')):
        sys.exit(f"--out {out}: no .git beside it, so not a test-references clone (CPM's copy is every tree's)")
    # the folder's files no test of this build reads (another machine's, where Unicorn is missing): from their args line
    known = {os.path.realpath(f) for f in files}
    for entry in sorted(os.listdir(out)):
        if entry.endswith('.txt') and os.path.realpath(os.path.join(out, entry)) not in known:
            files[os.path.join(out, entry)] = []
    if only:
        wanted = {f for f in files if os.path.basename(f) in only or f in only}
        unknown = [o for o in only if not any(os.path.basename(f) == o or f == o for f in files)]
        if unknown:
            sys.exit(f'{" ".join(unknown)}: no test of {build} reads such a file; they read {len(files)} files')
        files = {f: files[f] for f in wanted}
    host = next(
        (c[0] for tests in files.values() for _, c, _ in tests if c[0].endswith('snx-test-recompiler-backends')), None
    )
    if not host:
        sys.exit(f'{build} has no snx-test-recompiler-backends test that reads a frozen file')
    failed = 0
    for path in sorted(files):
        tests = files[path]
        # the host program's test where there is one, else another machine's, run here by the host's program
        name, command, cwd = (
            next((t for t in tests if t[1][0] == host), tests[0]) if tests else ('no test of this build', [host], build)
        )
        drawn = generator_words(command[1:])
        if not drawn:
            try:
                with open(path) as f:
                    header = f.read().split('\n# args ', 1)[1].split('\n', 1)[0]
            except (OSError, IndexError):
                print(f'{path}: {name} gives no generator option and the file has no args line to take them from')
                failed += 1
                continue
            drawn = header.split()
        # LLVM alone's ends depend on what is drawn alone: the native back end's options (--without, --hook, --jit)
        # are left out
        run = [host, '--record', path] + drawn
        done = subprocess.run(run, cwd=cwd, capture_output=True, text=True, timeout=3600, check=False)
        said = re.search(r'recorded .*', done.stdout)
        disagree = re.search(r'(\d+) failures', done.stdout)
        if not said:
            print(f"{path}: {name}'s programs not recorded (exit {done.returncode}):\n{done.stderr.strip()[-2000:]}")
            failed += 1
            continue
        print(
            f"{os.path.basename(path)}: {name}'s programs ({' '.join(drawn)}), {said.group(0)}; the native back end "
            f'ended otherwise in {disagree.group(1) if disagree else "?"} of its runs here; read by '
            f'{", ".join(t[0] for t in tests) or "no test of this build: another machine's, checked where it is built"}'
        )
    names = sorted({t[0] for tests in files.values() for t in tests})
    if not names:
        sys.exit(1 if failed else 0)
    check = subprocess.run(
        [
            'ctest',
            '--test-dir',
            build,
            '--output-on-failure',
            '-R',
            '^(' + '|'.join(re.escape(n) for n in names) + ')$',
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    summary = [line for line in check.stdout.splitlines() if 'tests passed' in line or '***' in line]
    print('\n'.join(summary) or check.stdout[-2000:])
    sys.exit(1 if failed or check.returncode else 0)


if __name__ == '__main__':
    main()
