#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/pr-builds.sh --print against a fake gh that has a run's artifacts and jobs: the builds linked, the APK's
halves and edenprobe's report left out, the jobs that did not succeed named by their id in build.yml, the checks as
installed, the sanitizers' build, the A64 differential's QEMU reference, Nightly due and the linux job a pull request's
run skips never among them, and edenprobe's line, its notice linked to the run. Then the pull request found from the
commit when none is given, and the workflow that runs it: publish.yml's last step, for a Build of a pull request of
this repository, with the permissions it reads and writes with, and no other workflow. Offline."""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARTIFACTS = {
    'artifacts': [
        {'id': 2, 'name': 'ScarletNX-windows-x86_64-abc1234', 'size_in_bytes': 3 << 20, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
        {'id': 1, 'name': 'ScarletNX-android-arm64-v8a-abc1234-stage', 'size_in_bytes': 1 << 20, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
        {'id': 3, 'name': 'edenprobe-abc1234-1', 'size_in_bytes': 1 << 20, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
        {'id': 4, 'name': 'fail-fast-1', 'size_in_bytes': 1 << 10, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
    ]
}  # fmt: skip
RUN = 'https://github.com/owner/repo/actions/runs/7'
TITLE = 'edenprobe a7f2beb: 906 passed, 20 failed, 3 skipped, 1 crashed'
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


def jobs(*named):
    """the run's jobs as the API lists them, ids from 100 in order"""
    return {'jobs': [{'id': 100 + k, 'name': name, 'conclusion': conclusion} for k, (name, conclusion) in
                     enumerate(named)]}  # fmt: skip


# every job of a run out of draft, as the API names them
WHOLE = [
    ('Linux (x86_64)', 'success'),
    ('Linux (aarch64)', 'success'),
    ('Linux (x86_64, ASan + UBSan)', 'failure'),
    ('Linux AppImage (x86_64)', 'success'),
    ('Linux AppImage (aarch64)', 'success'),
    ('Linux AppImage (ppc64le)', 'success'),
    ('Linux AppImage (riscv64)', 'failure'),
    ('Windows (Clang + LLVM)', 'success'),
    ('Android APK', 'success'),
    ('macOS and iOS (arm64)', 'success'),
    ('Linux AppImage (x86_64), as installed', 'failure'),
    ('Linux AppImage (riscv64), as installed', 'skipped'),
    ('Windows zip, as installed', 'success'),
    ('Amberlith (Linux x86_64, lavapipe)', 'success'),
    # the scheduled run's check, which every other event skips
    ('Nightly due', 'skipped'),
]
# a draft's: the cross AppImages skipped whole, under their name before the matrix filled it in, and macOS's; the
# probe's job under its name before Amberlith 1.0.0
DRAFT = [
    ('Linux (x86_64)', 'success'),
    ('Linux (x86_64, ASan + UBSan)', 'success'),
    ('Linux AppImage (x86_64)', 'success'),
    ('Linux AppImage (${{ matrix.arch }})', 'skipped'),
    ('Windows (Clang + LLVM)', 'failure'),
    ('Android APK', 'success'),
    ('macOS and iOS (arm64)', 'skipped'),
    ('Linux AppImage (${{ matrix.arch }}), as installed', 'skipped'),
    ('edenprobe (Linux x86_64, lavapipe)', 'failure'),
]

if not shutil.which('bash') or not shutil.which('jq'):
    print('pr-builds: skipped, it needs bash and jq')
    sys.exit(77)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    answers = root / 'answers.json'
    # the fake gh answers each API path from answers.json, and fails on one it has no answer for
    gh = root / 'gh'
    gh.write_text(
        f'#!{sys.executable}\n'
        'import json, sys\n'
        f'answers = json.load(open({str(answers)!r}))\n'
        "path = next(a for a in sys.argv[1:] if a.startswith('repos/'))\n"
        'if path not in answers:\n'
        "    sys.exit(f'fake gh: no answer for {path}')\n"
        'print(json.dumps(answers[path]))\n'
    )
    gh.chmod(0o755)

    def comment(needs=None, run_jobs=None, notice=None, pr='5', pulls=None):
        given = {'repos/owner/repo/actions/runs/7/artifacts?per_page=100': ARTIFACTS}
        if run_jobs is not None:
            given['repos/owner/repo/actions/runs/7/jobs?per_page=100'] = run_jobs
        if notice is not None:
            given |= {
                f'repos/owner/repo/check-runs/{100 + k}/annotations': notice for k in range(len(run_jobs['jobs']))
            }
        if pulls is not None:
            given['repos/owner/repo/commits/abc1234def/pulls'] = pulls
        answers.write_text(json.dumps(given))
        env = dict(os.environ, PATH=f'{root}{os.pathsep}{os.environ["PATH"]}')
        env.pop('NEEDS', None)
        if needs is not None:
            env['NEEDS'] = json.dumps(needs)
        command = ['bash', str(ROOT / 'scripts' / 'pr-builds.sh'), 'owner/repo', pr, '7', 'abc1234def', '--print']
        done = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30, check=False)
        check('it prints the comment', done.returncode == 0, done.stderr)
        return done.stdout

    # NEEDS given, in the shape of toJSON(needs)
    built = {'linux': {'result': 'success', 'outputs': {}}, 'windows': {'result': 'success', 'outputs': {}}}
    text = comment(built | {'edenprobe': {'result': 'success', 'outputs': {'result': TITLE}}})
    check(
        "the builds linked, the halves of the APK, edenprobe's report and a failed job's mark left out",
        f'- [ScarletNX-windows-x86_64-abc1234]({RUN}/artifacts/2), 3 MiB' in text
        and '-stage' not in text
        and 'edenprobe-abc1234' not in text
        and 'fail-fast' not in text,
        text,
    )
    check('kept until the first to expire', 'Builds of abc1234, kept until 2026-11-01:' in text, text)
    check(
        "edenprobe's result linked to the run",
        f'\n\n[{TITLE}]({RUN}): every case of the baseline passes\n' in text and 'Not built' not in text,
        text,
    )
    text = comment(
        built | {'windows': {'result': 'failure', 'outputs': {}}, 'edenprobe': {'result': 'failure', 'outputs': {}}}
    )
    check(
        'a failed edenprobe says so, without a result of its own',
        f'[Amberlith]({RUN}): failed, see its summary' in text,
    )
    check('a failed build named, edenprobe not among them', text.endswith('\n\nNot built: windows (failure)\n'), text)
    text = comment(built | {'edenprobe': {'result': 'skipped', 'outputs': {}}})
    check('a skipped edenprobe', text.endswith(f'\n\n[Amberlith]({RUN}): skipped\n') and 'Not built' not in text)

    # the run's jobs, as publish.yml finds them once the Build ends
    notice = [{'title': 'something else', 'message': 'no'}, {'title': 'edenprobe', 'message': TITLE}]
    text = comment(run_jobs=jobs(*WHOLE), notice=notice)
    check(
        "a leg's failure under its job's id, the checks as installed and the sanitizers' build never named",
        text.endswith(
            f'\n\n[{TITLE}]({RUN}): every case of the baseline passes\n\nNot built: appimage-cross (failure)\n'
        ),
        text,
    )
    apk = "edenprobe v0.11.0's APK: 640 passed, 13 failed, 0 skipped, 0 crashed"
    text = comment(run_jobs=jobs(*WHOLE), notice=[{'title': 'edenprobe', 'message': apk}] + notice)
    check(
        "the NRO's and the APK's notices on edenprobe's line, the NRO's first whatever order the API gives",
        f'\n\n[{TITLE}; {apk}]({RUN}): every case of the baseline passes\n' in text,
        text,
    )
    text = comment(run_jobs=jobs(*DRAFT), notice=[{'title': 'edenprobe', 'message': TITLE}])
    check(
        "a draft's: the jobs skipped whole named in build.yml's order, edenprobe's failure with its notice",
        text.endswith(
            f'\n\n[{TITLE}]({RUN}): failed, see its summary\n\nNot built: appimage-cross (skipped), windows (failure), '
            'apple (skipped)\n'
        ),
        text,
    )
    text = comment(
        run_jobs=jobs(('Recompiler changes', 'success'), ("A64 differential's QEMU reference", 'skipped'), *WHOLE[1:2])
    )
    check(
        'a pull request that skips the A64 differential: its QEMU reference and the job that skips it never named',
        'Not built' not in text,
        text,
    )
    text = comment(
        run_jobs=jobs(('Linux (${{ matrix.arch }})', 'skipped'), ('Linux AppImage (x86_64)', 'success'), *WHOLE[4:5])
    )
    check(
        "a pull request's run: the linux job it skips never named, its Linux built in the appimage job",
        'Not built' not in text,
        text,
    )
    text = comment(run_jobs=jobs(*WHOLE[:2], ('Lint the shaders', 'failure')))
    check('a job it does not know keeps its name', text.endswith('\n\nNot built: Lint the shaders (failure)\n'), text)
    text = comment(run_jobs=jobs(('Linux (x86_64)', 'success'), ('Linux (aarch64)', 'cancelled')))
    check('a leg cancelled', text.endswith('\n\nNot built: linux (cancelled)\n'), text)
    text = comment(run_jobs=jobs(('Amberlith (Linux x86_64, lavapipe)', 'success')), notice=[])
    check('Amberlith without its notice', text.endswith(f'\n\n[Amberlith]({RUN}): every case of the baseline passes\n'))
    text = comment(run_jobs={'jobs': []})
    check(
        'a run without jobs: the builds alone',
        'edenprobe' not in text and 'Amberlith' not in text and 'Not built' not in text,
        text,
    )

    # a pull request that changes nothing either run loads: the job ends at once, passed, its notice saying why
    why = 'not run, none of the 3 files changed is one the NRO or the APK loads'
    text = comment(
        run_jobs=jobs(('Amberlith (Linux x86_64, lavapipe)', 'success')),
        notice=[{'title': 'edenprobe', 'message': f'Amberlith: {why}'}],
    )
    check(
        'Amberlith not run: the line says why, and nothing about a baseline',
        text.endswith(f'\n\n[Amberlith]({RUN}): {why}\n') and 'baseline' not in text,
        text,
    )

    # no pull request given: the open one whose head is the commit, else nothing
    pulls = [
        {'number': 9, 'state': 'closed', 'head': {'sha': 'abc1234def'}},
        {'number': 11, 'state': 'open', 'head': {'sha': 'abc1234def'}},
    ]
    text = comment(run_jobs={'jobs': []}, pr='', pulls=pulls)
    check('the pull request found from its head', text.startswith('<!-- scarletnx-builds -->\nBuilds of abc1234'), text)
    text = comment(run_jobs={'jobs': []}, pr='', pulls=pulls[:1])
    check(
        'no open pull request: nothing linked, said with the commit',
        text == 'no open pull request of owner/repo has abc1234 as its head: nothing to link\n',
        text,
    )


def workflow_checks(publish, others):
    """what is wrong with the workflows that run the script: publish.yml's text, and every other workflow's by name"""
    wrong = []
    steps = publish.split('\n      - ')
    step = next((x for x in steps if x.startswith('name: Link the builds from the pull request\n')), None)
    if step is None:
        return ["publish.yml has no step 'Link the builds from the pull request'"]
    if step is not steps[-1]:
        wrong.append("the comment is not publish.yml's last step, after the publishing")
    condition = ' '.join(step.split('run: ', 1)[0].split())
    for needed in (
        '!cancelled()',
        "github.event_name == 'workflow_run'",
        "github.event.workflow_run.event == 'pull_request'",
        'github.event.workflow_run.head_repository.full_name == github.repository',
    ):
        if needed not in condition:
            wrong.append(f'the comment step runs without {needed}: {condition}')
    if 'scripts/pr-builds.sh "$GITHUB_REPOSITORY" "$PR" "$RUN" "$SHA"' not in step:
        wrong.append(f'the comment step does not run the script: {step[:200]}')
    head = publish.split('\njobs:', 1)[0]
    for permission in ('actions: read', 'checks: read', 'pull-requests: write'):
        if f'\n  {permission}' not in head:
            wrong.append(f'publish.yml lacks {permission}')
    for name, text in others.items():
        # a command that runs it, not a comment that names it
        if re.search(r'^\s*(run:\s*)?(bash\s+)?scripts/pr-builds\.sh\s', text, re.MULTILINE):
            wrong.append(f'{name} runs scripts/pr-builds.sh too: two jobs would write the comment')
    return wrong


# the workflow that writes the comment, as it is, then copies of it made wrong one way each
flows = ROOT / '.github' / 'workflows'
publish_text = (flows / 'publish.yml').read_text()
others = {f.name: f.read_text() for f in flows.glob('*.yml') if f.name != 'publish.yml'}
check(
    'publish.yml writes the comment, and no other workflow does',
    not workflow_checks(publish_text, others),
    workflow_checks(publish_text, others),
)
wrong_copies = {
    'without the fork rule': publish_text.replace(
        '\n          github.event.workflow_run.head_repository.full_name == github.repository }}', ' true }}'
    ),
    'without checks: read': publish_text.replace('\n  checks: read', ''),
    'the comment first': publish_text.replace(
        '      - name: Download the x86-64 AppImage',
        '      - name: Link the builds from the pull request\n        run: x\n'
        '      - name: Download the x86-64 AppImage',
    ),
}
for what, text in wrong_copies.items():
    check(f'a copy of publish.yml {what} is named wrong', text != publish_text and workflow_checks(text, others), what)
check(
    'another workflow running the script is named, one that names it in a comment not',
    workflow_checks(publish_text, {'pr-checks.yml': '        run: scripts/pr-builds.sh "$GITHUB_REPOSITORY" "$PR"\n'})
    and not workflow_checks(publish_text, {'build.yml': '      # a notice scripts/pr-builds.sh reads back\n'}),
)

for failure in failures:
    print('FAIL:', failure)
print('pr-builds:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
