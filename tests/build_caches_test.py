#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""What the Build workflow (.github/workflows/build.yml) saves in the repository's caches, and when.

A pull request's run saves no compiler cache (.cache/ccache): every save of one is skipped on pull_request, and none
is left to actions/cache alone, which saves whatever the event. Only that pull request would read it, and the caches
share 10 GB: with one of every run in them, main's system roots and dependencies are evicted and built again.

Then the same checks on copies of the workflow made to fail them. Offline.

    tests/build_caches_test.py [REPO]   (default: the checkout this file is in)"""

import re
import sys
from pathlib import Path

WORKFLOW = Path('.github/workflows/build.yml')
NOT_A_PULL_REQUEST = "github.event_name != 'pull_request'"
# the workflow's saves of a compiler cache: fewer found means this check no longer reads the file as it is written
COMPILER_CACHES = 7


def indent(line):
    return len(line) - len(line.lstrip(' '))


def jobs(text):
    """{job: [step]} of a workflow, each step {key: value} with the keys of its with: as with.KEY. A value on
    several lines (a block, a folded if) is its lines joined by a newline"""
    lines = text.splitlines()
    out, job, step, key = {}, None, None, None
    in_jobs = False
    anchors = {}  # a step anchored &NAME, which each *NAME stands for
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
            text_ = text_[2:].strip()
            col = 8
            if text_.startswith('*') and ':' not in text_:  # an alias: the anchored step again
                out[job].append(anchors[text_[1:]])
                step = None
                continue
            out[job].append(step)
            if text_.startswith('&') and ':' not in text_:  # an anchor's name alone
                anchors[text_[1:]] = step
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
    return check_compiler(jobs(text))


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


def command(step, repo):
    """what a step runs: its run, or the function of a scripts/ci file it calls (`scripts/ci/FILE.sh STEP`)"""
    run = step.get('run', '')
    called = re.fullmatch(r'(?:bash )?(scripts/ci/[\w-]+\.sh)(?: (\w+))?', run.strip())
    if not called or not repo:
        return run
    text = (repo / called[1]).read_text(encoding='utf-8')
    if not called[2]:
        return text
    body = re.search(rf'^{called[2]}\(\) \{{\n(.*?)^\}}$', text, re.M | re.S)
    if not body:
        sys.exit(f'build_caches: {step.get("name")} runs {run.strip()}, and {called[1]} has no function {called[2]}')
    return body[1]


def replace_once(text, old, new):
    if text.count(old) != 1:
        sys.exit(f'build_caches: the self-test edits "{old}", which {WORKFLOW} has {text.count(old)} times')
    return text.replace(old, new)


def self_test(text):
    """Copies of the workflow each made wrong in one way: the check names each"""
    linux = (
        f'      - &ccache-save-arch\n        if: {NOT_A_PULL_REQUEST}\n        uses: actions/cache/save@v5\n'
        '        with:\n          path: .cache/ccache\n'
        '          key: ccache-${{ github.job }}-${{ matrix.arch }}-${{ github.run_id }}-${{ github.run_attempt }}\n'
    )
    reference = (
        '      - uses: actions/cache/restore@v5\n        with:\n          path: .cache/ccache\n'
        '          key: ccache-appimage-x86_64-${{ github.run_id }}-${{ github.run_attempt }}\n'
    )
    cases = [
        (
            "a compiler cache saved on a pull request's run too",
            replace_once(text, linux, linux.replace(f'\n        if: {NOT_A_PULL_REQUEST}\n', '\n')),
            'appimage: the save of .cache/ccache runs if ""',
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
            replace_once(text, '      - *ccache-trim\n      - *ccache-save\n', '      - *ccache-trim\n'),
            f'{COMPILER_CACHES - 1} steps save .cache/ccache, and the workflow had {COMPILER_CACHES}',
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
    print(f"build_caches: {COMPILER_CACHES} compiler caches saved by no pull request's run; {cases} wrong copies named")
    return 0


if __name__ == '__main__':
    sys.exit(main())
