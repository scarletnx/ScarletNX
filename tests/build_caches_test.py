#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""What the Build workflow (.github/workflows/build.yml) saves in the repository's caches, and when.

It keeps each LLVM it builds. A step that runs one of the LLVM scripts (scripts/linux-llvm.sh, android-llvm.sh,
apple-llvm.sh) runs only when its cache missed; the cache is read by actions/cache/restore, and the step right after
the build saves it under the key the restore named. actions/cache alone saves when the job ends and only if it
succeeded: a failed test or a cancelled run threw away a build of up to an hour, and the next run made it again.

A pull request's run saves no compiler cache (.cache/ccache): every save of one is skipped on pull_request, and none
is left to actions/cache alone, which saves whatever the event. Only that pull request would read it, and the caches
share 10 GB: with one of every run in them, main's LLVM builds and system roots are evicted and built again.

Then the same checks on copies of the workflow made to fail them. Offline.

    tests/build_caches_test.py [REPO]   (default: the checkout this file is in)"""

import re
import sys
from pathlib import Path

WORKFLOW = Path('.github/workflows/build.yml')
SCRIPT = re.compile(r'scripts/(linux|android|apple)-llvm\.sh')
MISSED = re.compile(r"steps\.([\w-]+)\.outputs\.cache-hit != 'true'")
NOT_A_PULL_REQUEST = "github.event_name != 'pull_request'"
# the LLVM builds of the workflow and its saves of a compiler cache: fewer found means this check no longer reads the
# file as it is written
BUILDS = 6
COMPILER_CACHES = 7


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
    """What is wrong with the caches of a workflow's text, one line each; none when they are as the top says"""
    found = jobs(text)
    return check_llvm(found) + check_compiler(found)


def check_compiler(found):
    wrong, saves = [], 0
    for job, steps in found.items():
        for step in steps:
            if step.get('with.path') != '.cache/ccache':
                continue
            uses = step.get('uses', '')
            if uses.startswith('actions/cache@'):
                wrong.append(
                    f'{job}: {uses} on .cache/ccache saves it when the job ends, on a pull request too: restore it'
                    ' with actions/cache/restore and save it with an actions/cache/save a pull request skips'
                )
            elif uses.startswith('actions/cache/save@'):
                saves += 1
                if NOT_A_PULL_REQUEST not in step.get('if', ''):
                    wrong.append(
                        f'{job}: the save of .cache/ccache runs if "{step.get("if", "")}", so on a pull request too:'
                        f' its if: needs {NOT_A_PULL_REQUEST}'
                    )
    if saves < COMPILER_CACHES:
        wrong.append(
            f'{saves} steps save .cache/ccache, and the workflow had {COMPILER_CACHES}: this check reads fewer than'
            ' are there, or one was taken out (then COMPILER_CACHES goes down with it)'
        )
    return wrong


def check_llvm(found):
    wrong, builds = [], 0
    for job, steps in found.items():
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
    sanitizers = (
        f'      - if: {NOT_A_PULL_REQUEST}\n        uses: actions/cache/save@v5\n'
        '        with:\n          path: .cache/ccache\n'
        '          key: ccache-${{ github.job }}-${{ github.run_id }}-${{ github.run_attempt }}\n'
        '      - name: Test under the sanitizers\n'
    )
    reference = (
        '      - uses: actions/cache/restore@v5\n        with:\n          path: .cache/ccache\n'
        '          key: ccache-linux-x86_64-${{ github.run_id }}-${{ github.run_attempt }}\n'
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
        (
            "a compiler cache saved on a pull request's run too",
            replace_once(
                text, sanitizers, sanitizers.replace(f'      - if: {NOT_A_PULL_REQUEST}\n        uses', '      - uses')
            ),
            'linux-sanitizers: the save of .cache/ccache runs if ""',
        ),
        (
            'a compiler cache saved whenever the run is not cancelled',
            replace_once(text, f'${{{{ !cancelled() && {NOT_A_PULL_REQUEST} }}}}', '${{ !cancelled() }}'),
            'apple: the save of .cache/ccache runs if "${{ !cancelled() }}"',
        ),
        (
            'a compiler cache left to actions/cache',
            replace_once(text, reference, reference.replace('actions/cache/restore@v5', 'actions/cache@v5')),
            'qemu-reference: actions/cache@v5 on .cache/ccache saves it when the job ends',
        ),
        (
            "a compiler cache's save taken out",
            replace_once(text, sanitizers, '      - name: Test under the sanitizers\n'),
            f'6 steps save .cache/ccache, and the workflow had {COMPILER_CACHES}',
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
    print(
        f'build_caches: {BUILDS} LLVM builds each saved by the step after them, {COMPILER_CACHES} compiler caches saved'
        f" by no pull request's run; {cases} wrong copies named"
    )
    return 0


if __name__ == '__main__':
    sys.exit(main())
