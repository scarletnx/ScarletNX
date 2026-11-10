#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Build workflow keeps each LLVM it builds. A step of .github/workflows/build.yml that runs one of the LLVM
scripts (scripts/linux-llvm.sh, android-llvm.sh, apple-llvm.sh) runs only when its cache missed; the cache is read by
actions/cache/restore, and the step right after the build saves it under the key the restore named. actions/cache
alone saves when the job ends and only if it succeeded: a failed test or a cancelled run threw away a build of up to
an hour, and the next run made it again. Then the same check on copies of the workflow made to fail it. Offline.

    tests/build_caches_test.py [REPO]   (default: the checkout this file is in)"""

import re
import sys
from pathlib import Path

WORKFLOW = Path('.github/workflows/build.yml')
SCRIPT = re.compile(r'scripts/(linux|android|apple)-llvm\.sh')
MISSED = re.compile(r"steps\.([\w-]+)\.outputs\.cache-hit != 'true'")
# the LLVM builds of the workflow: fewer found means this check no longer reads the file as it is written
BUILDS = 6


def indent(line):
    return len(line) - len(line.lstrip(' '))


def jobs(text):
    """{job: [step]} of a workflow, each step {key: value} with the keys of its with: as with.KEY. A value on
    several lines (a block, a folded if) is its lines joined by a newline"""
    lines = text.splitlines()
    out, job, step, key = {}, None, None, None
    in_jobs = False
    for line in lines:
        if not line.strip():
            continue
        col = indent(line)
        text_ = line.strip()
        if col == 0:
            in_jobs = text_ == 'jobs:'
            continue
        if not in_jobs:
            continue
        if col == 2 and not text_.startswith('#'):
            job, step, key = text_.rstrip(':'), None, None
            out[job] = []
            continue
        if job is None:
            continue
        if col == 6 and text_.startswith('- '):
            step, key = {}, None
            out[job].append(step)
            text_ = text_[2:].strip()
            col = 8
            if text_.startswith(('&', '*')) and ':' not in text_:  # an anchor's name alone, or its alias
                continue
        if step is None or text_.startswith('#') and col <= 8:
            continue
        if col == 8 and re.match(r'[\w-]+:', text_):
            key, _, value = text_.partition(':')
            step[key] = value.strip()
            scope = ''
        elif col == 10 and key == 'with' and re.match(r'[\w-]+:', text_):
            name, _, value = text_.partition(':')
            scope = 'with.' + name
            step[scope] = value.strip()
        elif key:
            at = scope or key
            step[at] = (step[at] + '\n' + text_).strip()
    return out


def check(text):
    """What is wrong with the LLVM caches of a workflow's text, one line each; none when they are kept"""
    wrong, builds = [], 0
    for job, steps in jobs(text).items():
        for i, step in enumerate(steps):
            if not SCRIPT.search(step.get('run', '')):
                continue
            builds += 1
            what = f'{job}: "{step.get("name", step["run"].splitlines()[0])}"'
            missed = MISSED.search(step.get('if', ''))
            if not missed:
                wrong.append(
                    f"{what} has no if: of a cache's miss (steps.ID.outputs.cache-hit != 'true'): it builds"
                    ' LLVM on every run'
                )
                continue
            cache = missed.group(1)
            restore = [s for s in steps[:i] if s.get('id') == cache]
            if not restore:
                wrong.append(f'{what} waits for the cache "{cache}", which no step before it in the job has as its id')
                continue
            uses = restore[0].get('uses', '')
            if not uses.startswith('actions/cache/restore@'):
                wrong.append(
                    f'{what}: its cache "{cache}" is read by {uses or "no action"}, not actions/cache/restore:'
                    ' actions/cache saves at the end of a job that succeeded, so a later failure or a'
                    ' cancelled run loses the build'
                )
                continue
            after = steps[i + 1] if i + 1 < len(steps) else {}
            if not after.get('uses', '').startswith('actions/cache/save@'):
                wrong.append(
                    f'{what} is followed by "{after.get("name") or after.get("uses") or "no step"}", not by'
                    f' the actions/cache/save of "{cache}": a step between them that fails loses the build'
                )
                continue
            saved = MISSED.search(after.get('if', ''))
            if not saved or saved.group(1) != cache:
                wrong.append(f'{what}: the save after it runs if "{after.get("if", "")}", not on the miss of "{cache}"')
            want = f'${{{{ steps.{cache}.outputs.cache-primary-key }}}}'
            if after.get('with.key') != want:
                wrong.append(f'{what}: the save after it has key "{after.get("with.key", "")}", not {want}')
            if after.get('with.path') != restore[0].get('with.path'):
                wrong.append(
                    f'{what}: the save after it has path "{after.get("with.path", "")}", and the restore'
                    f' "{restore[0].get("with.path", "")}"'
                )
    if builds < BUILDS:
        wrong.append(
            f'{builds} steps run an LLVM script, and the workflow had {BUILDS}: this check reads fewer than'
            ' are there, or one was taken out (then BUILDS goes down with it)'
        )
    return wrong


def replace_once(text, old, new):
    if text.count(old) != 1:
        sys.exit(f'build_caches: the self-test edits "{old}", which {WORKFLOW} has {text.count(old)} times')
    return text.replace(old, new)


def self_test(text):
    """Copies of the workflow each made wrong in one way: the check names each"""
    android = "key: android-llvm-x86_64-${{ hashFiles('scripts/android-llvm.sh') }}-${{ env.ANDROID_NDK_ROOT }}"
    save = (
        "      - if: steps.llvm-x86_64.outputs.cache-hit != 'true'\n        uses: actions/cache/save@v5\n"
        '        with:\n          path: android-llvm/x86_64\n'
        '          key: ${{ steps.llvm-x86_64.outputs.cache-primary-key }}\n'
    )
    build = (
        "      - name: Build LLVM's libraries for x86_64\n        if: steps.llvm-x86_64.outputs.cache-hit != 'true'\n"
    )
    cases = [
        (
            'the cache read and saved by actions/cache',
            replace_once(
                text,
                f'      - uses: actions/cache/restore@v5\n        id: llvm-x86_64\n        with:\n'
                f'          path: android-llvm/x86_64\n          {android}\n',
                f'      - uses: actions/cache@v5\n        id: llvm-x86_64\n        with:\n'
                f'          path: android-llvm/x86_64\n          {android}\n',
            ),
            'read by actions/cache@v5, not actions/cache/restore',
        ),
        ('no save after the build', replace_once(text, save, ''), 'not by the actions/cache/save of "llvm-x86_64"'),
        (
            'the save after the next build',
            replace_once(
                replace_once(text, save, ''),
                "      - if: steps.llvm-arm64.outputs.cache-hit != 'true'\n",
                save + "      - if: steps.llvm-arm64.outputs.cache-hit != 'true'\n",
            ),
            'not by the actions/cache/save of "llvm-x86_64"',
        ),
        (
            "the save under the other ABI's key",
            replace_once(
                text,
                'path: android-llvm/x86_64\n          key: ${{ steps.llvm-x86_64.outputs.cache-primary-key }}',
                'path: android-llvm/x86_64\n          key: ${{ steps.llvm-arm64.outputs.cache-primary-key }}',
            ),
            'has key "${{ steps.llvm-arm64.outputs.cache-primary-key }}"',
        ),
        (
            "the save of the other ABI's folder",
            replace_once(
                text,
                'path: android-llvm/x86_64\n          key: ${{ steps.llvm-x86_64.outputs.cache-primary-key }}',
                'path: android-llvm/arm64-v8a\n          key: ${{ steps.llvm-x86_64.outputs.cache-primary-key }}',
            ),
            'has path "android-llvm/arm64-v8a", and the restore "android-llvm/x86_64"',
        ),
        (
            'the save on every run',
            replace_once(
                text, save, save.replace(" if: steps.llvm-x86_64.outputs.cache-hit != 'true'\n        uses", ' uses')
            ),
            'the save after it runs if ""',
        ),
        (
            'the build on every run',
            replace_once(text, build, "      - name: Build LLVM's libraries for x86_64\n"),
            'it builds LLVM on every run',
        ),
        (
            'the build waiting for a cache the job has not',
            replace_once(text, build, build.replace('steps.llvm-x86_64.', 'steps.llvm-x86.')),
            'which no step before it in the job has as its id',
        ),
        (
            'a build taken out',
            replace_once(text, 'run: scripts/android-llvm.sh x86_64 "$PWD/android-llvm/x86_64"\n', 'run: true\n'),
            f'5 steps run an LLVM script, and the workflow had {BUILDS}',
        ),
    ]
    failed = []
    for name, changed, want in cases:
        wrong = check(changed)
        if not any(want in line for line in wrong):
            failed.append(f'{name}: the check says {wrong or "nothing"}, not "{want}"')
    return failed, len(cases)


def main():
    if len(sys.argv) > 2:
        sys.exit('usage: tests/build_caches_test.py [REPO]')
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    text = (repo / WORKFLOW).read_text(encoding='utf-8')
    wrong = check(text)
    if wrong:
        sys.exit(f'build_caches: {WORKFLOW}:\n  ' + '\n  '.join(wrong))
    failed, cases = self_test(text)
    if failed:
        sys.exit('build_caches: the check misses a workflow made wrong:\n  ' + '\n  '.join(failed))
    print(f'build_caches: {BUILDS} LLVM builds each saved by the step after them; {cases} wrong copies named')
    return 0


if __name__ == '__main__':
    sys.exit(main())
