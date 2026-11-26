#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Which events of the Build workflow (.github/workflows/build.yml) test, and which only build.

A pull request, a run by hand and a version's tag run every test; a push to main and the nightly's schedule build,
package and publish, and run none but the AppImage and the zip as a user would, which every event runs. SNX_TESTS, in
the workflow's env, says which: each test step below is `if: env.SNX_TESTS == 'true'`, with nothing in its condition
that leaves out a pull request or picks one by what it changes. Every step that runs ctest is one of them, and the
steps that make and publish the packages run on every event, a pull request's too. The one test a pull request runs
by what it changes is the A64 differential against QEMU: its reference, a job of the testing events, records on a pull
request only where the recompiler-changes job (scripts/recompiler-changes.py) says the pull request changes what it
tests, and the cross jobs' run of it follows the reference. No other job or step reads that job's answer. Then the
same checks on copies of the workflow made to fail them. Offline.

    tests/build_tests_test.py [REPO]   (default: the checkout this file is in)"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_caches_test import command, jobs

WORKFLOW = Path('.github/workflows/build.yml')
TESTS = "env.SNX_TESTS == 'true'"
TESTING = (
    "github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch' || "
    "startsWith(github.ref, 'refs/tags/')"
)
# the steps that test, by job: every one is there, and each runs where SNX_TESTS alone
TEST_STEPS = {
    'appimage': [
        'Test headless Vulkan and NX runtime',
        'Test native tooling and indexed filesystems',
        'Check optional Python diagnostics',
        'Make the x86-64 instruction selection again',
    ],
    'linux-sanitizers': ['Test under the sanitizers'],
    # a job of its own, beside the appimage job, whose longest test it was
    'linux-unicorn': ['Test the native back ends under Unicorn'],
    'appimage-cross': [
        'Install qemu-user',
        'Run the A64 differential against the recorded QEMU reference',
        "Run the launcher's tests through qemu",
        'Recompile whole programs through qemu',
        'Run ${{ matrix.arch }} AppImages through qemu-user',
        'Run the AppImage as a user would',
    ],
    'windows': ['Test native tooling and indexed filesystems'],
    'android': ["Test the app's Java"],
    'apple': ['Test native tooling and indexed filesystems'],
}
# the steps that make the packages, which every event runs: a pull request's run makes what main's publishes
BUILD_STEPS = {
    'appimage': ['Build and install into /usr', 'Make the AppImage', 'Build the tests and the headless Vulkan tree'],
    'linux-unicorn': ['Build the tests under Unicorn'],
    'appimage-cross': [
        "Install the target container's tools and libraries",
        'Install the packaging tools',
        'Cross-build and install',
        'Bundle the AppImage',
        'Make the AppImage',
        'Check the AppImage',
    ],
    'windows': ['Build the runtime and pinned dependencies'],
    'android': ["Build each ABI's native libraries and APK"],
    'apple': ['Build the runtime for macOS', 'Build the app for iOS', 'Make the macOS app', 'Make the IPA'],
}
# the packages run as a user would, on every event: seconds each, on what main's push and the nightly publish
USER_RUNS = {'appimage': 'Run the AppImage as a user would', 'windows': "Run the zip's scarletnx.exe as a user would"}
# what picked a pull request's tests by what it changes
FILTERS = re.compile(r'recompiler-changes|steps\.unicorn\.|cmake-changed')
# the one test a pull request runs only where it changes what it is made from: it downloads Intel's manual, whose
# link a pull request that changes nothing of it would fail on as often as the site does
CHANGE_FILTERED = {('appimage', 'Make the x86-64 instruction selection again')}
# when the A64 differential's QEMU reference records: on a pull request that changes what it tests, a run by hand and a
# tag; and the job that decides, a pull request's alone
REFERENCE = (
    "!cancelled() && (github.event_name == 'pull_request' && needs.recompiler-changes.outputs.differential == 'true' "
    "|| github.event_name == 'workflow_dispatch' || startsWith(github.ref, 'refs/tags/'))"
)
DECIDES = "github.event_name == 'pull_request'"
# the checks of what is published that run a test of ctest on every event: the zip's build line, which a nightly
# carries
PACKAGE_CHECKS = {('windows', 'Check the build line names the checkout as it is')}


def job_if(text, job):
    """a job's if: of the workflow's text, its lines joined; '' where it has none"""
    start = text.find(f'\n  {job}:\n')
    if start < 0:
        return None
    rest = text[start + len(job) + 5 :]
    end = re.search(r'\n  [\w-]+:\n', rest)
    block = rest[: end.start() + 1] if end else rest
    found = re.search(r'^    if: >-\n((?:      .*\n)+)', block, re.MULTILINE) or re.search(
        r'^    if: (.*)$', block, re.MULTILINE
    )
    return ' '.join(found[1].split()) if found else ''


def check(text, repo=None):
    """what is wrong with when the workflow's text tests, one line each; none when it is as the top says"""
    wrong = []
    env = re.search(r'\n  SNX_TESTS: >-\n((?:    .*\n)+)', text)
    said = ' '.join(env[1].split()) if env else ''
    if said != '${{ ' + TESTING + ' }}':
        wrong.append(f'SNX_TESTS is "{said}", not the testing events: ${{{{ {TESTING} }}}}')
    found = jobs(text)
    for job, names in TEST_STEPS.items():
        steps = {step.get('name'): step for step in found.get(job, [])}
        for name in names:
            step = steps.get(name)
            if step is None:
                wrong.append(f'{job}: no step "{name}": a pull request no longer runs that test')
                continue
            cond = step.get('if', '')
            if TESTS not in cond:
                wrong.append(f'{job}: "{name}" runs if "{cond}", on main\'s pushes too: its if: needs {TESTS}')
            if 'event_name' in cond or FILTERS.search(cond):
                wrong.append(f'{job}: "{name}" runs if "{cond}": a pull request is held to SNX_TESTS alone')
            if (job, name) not in CHANGE_FILTERED and 'git diff' in command(step, repo):
                wrong.append(f'{job}: "{name}" looks at what a pull request changes: it runs on every one')
    for job, steps in found.items():
        if job == 'recompiler-changes':
            continue
        for step in steps:
            name = step.get('name', step.get('uses', ''))
            ctest = re.search(r'(^|\s)ctest\s', command(step, repo))
            if ctest and TESTS not in step.get('if', '') and (job, name) not in PACKAGE_CHECKS:
                wrong.append(f'{job}: "{name}" runs ctest if "{step.get("if", "")}": its if: needs {TESTS}')
            if FILTERS.search(step.get('if', '') + '\n' + command(step, repo)):
                wrong.append(f'{job}: "{name}" picks the tests by what a pull request changes')
    for job, names in BUILD_STEPS.items():
        steps = {step.get('name'): step for step in found.get(job, [])}
        for name in names:
            step = steps.get(name)
            if step is None:
                wrong.append(f'{job}: no step "{name}": this check reads the workflow as it was')
            elif 'SNX_TESTS' in step.get('if', '') or "event_name != 'pull_request'" in step.get('if', ''):
                wrong.append(f'{job}: "{name}" runs if "{step["if"]}": what main publishes a pull request makes')
    for job, name in USER_RUNS.items():
        step = {step.get('name'): step for step in found.get(job, [])}.get(name)
        if step is None:
            wrong.append(f'{job}: no step "{name}": this check reads the workflow as it was')
        elif step.get('if'):
            wrong.append(
                f'{job}: "{name}" runs if "{step["if"]}": every event runs it, main\'s push and the nightly too'
            )
    reference = job_if(text, 'qemu-reference')
    if reference != REFERENCE:
        wrong.append(f'qemu-reference runs if "{reference}", not as the testing events and the filter say: {REFERENCE}')
    decides = job_if(text, 'recompiler-changes')
    if decides != DECIDES:
        wrong.append(f'recompiler-changes runs if "{decides}", not on a pull request alone: {DECIDES}')
    return wrong


def self_test(text, repo):
    """the checks on copies of the workflow made wrong, each named: (the copies they missed, how many)"""
    unicorn = "      - name: Test the native back ends under Unicorn\n        if: env.SNX_TESTS == 'true'\n"
    cases = {
        'a test step on every event': text.replace(
            "      - name: Test under the sanitizers\n        if: env.SNX_TESTS == 'true'\n",
            '      - name: Test under the sanitizers\n',
        ),
        'a test step gone': text.replace("      - name: Test the app's Java\n", '      - name: Test the Java\n'),
        'a test step a pull request skips': text.replace(
            "      - name: Test the app's Java\n        if: env.SNX_TESTS == 'true'\n",
            "      - name: Test the app's Java\n        if: env.SNX_TESTS == 'true' && github.event_name != 'pull_request'\n",
        ),
        'the zip run as a user would on the testing events alone': text.replace(
            "      - name: Run the zip's scarletnx.exe as a user would\n",
            "      - name: Run the zip's scarletnx.exe as a user would\n        if: env.SNX_TESTS == 'true'\n",
        ),
        'the AppImage run as a user would gone': text.replace(
            '      - name: Run the AppImage as a user would\n        uses:',
            '      - name: Run it as a user would\n        uses:',
        ),
        "the tests under Unicorn on a pull request's changes": text.replace(
            unicorn,
            '      - name: Test the native back ends under Unicorn\n'
            "        if: env.SNX_TESTS == 'true' && steps.unicorn.outputs.unicorn == 'true'\n",
        ),
        "the tests under Unicorn on main's push": text.replace(
            unicorn, '      - name: Test the native back ends under Unicorn\n'
        ),
        "main's push testing": text.replace(
            "github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch' ||\n",
            "github.event_name == 'pull_request' || github.event_name == 'push' ||\n",
            1,
        ),
        'the iOS app left to the nightlies': text.replace(
            "        if: ${{ !cancelled() && steps.ios-llvm-build.outcome != 'failure' }}\n",
            "        if: ${{ !cancelled() && github.event_name != 'pull_request' && steps.ios-llvm-build.outcome != 'failure' }}\n",
        ),
        'the QEMU reference on every event': re.sub(
            r'(\n  qemu-reference:\n(?:      .*\n|    (?!if:).*\n)*?)    if: >-\n(?:      .*\n)+',
            lambda found: found[1] + '    if: ${{ !cancelled() }}\n',
            text,
        ),
        'the QEMU reference on every pull request, whatever it changes': text.replace(
            "github.event_name == 'pull_request' && needs.recompiler-changes.outputs.differential == 'true' ||",
            "github.event_name == 'pull_request' ||",
        ),
        'the filter deciding on a push too': text.replace(
            "    name: Recompiler changes\n    if: github.event_name == 'pull_request'\n",
            '    name: Recompiler changes\n',
        ),
        "the tests under Unicorn by the differential's filter": text.replace(
            unicorn,
            '      - name: Test the native back ends under Unicorn\n'
            "        if: env.SNX_TESTS == 'true' && needs.recompiler-changes.outputs.differential == 'true'\n",
        ),
    }
    missed = [name for name, copy in cases.items() if copy == text or not check(copy, repo)]
    return missed, len(cases)


def main():
    if len(sys.argv) > 2:
        sys.exit('usage: tests/build_tests_test.py [REPO]')
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    text = (repo / WORKFLOW).read_text(encoding='utf-8')
    wrong = check(text, repo)
    if wrong:
        sys.exit(f'build_tests: {WORKFLOW}:\n  ' + '\n  '.join(wrong))
    missed, cases = self_test(text, repo)
    if missed:
        sys.exit('build_tests: the check misses a workflow made wrong:\n  ' + '\n  '.join(missed))
    count = sum(len(names) for names in TEST_STEPS.values())
    print(
        f'build_tests: {count} test steps on the testing events alone, every ctest among them, the packages made and'
        f' {len(USER_RUNS)} run as a user would on every event; {cases} wrong copies named'
    )
    return 0


if __name__ == '__main__':
    sys.exit(main())
