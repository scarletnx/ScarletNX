#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""build.yml's Nightly due, run as Actions runs its step (bash -eo pipefail) against a fake curl and date: a scheduled
run cancels itself on a day (UTC) the newest nightly is of, and when the newest nightly was built from its commit, and
goes on otherwise; GitHub not answering fails the step. And its Name the nightly, in every job that makes a package,
before it builds: SNX_BUILD_NAME, nightly-<the day the run began>, for cmake/commit.cmake, from GitHub's answer however
it is laid out, a run with no day failing the step. Offline."""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_caches_test import jobs

ROOT = Path(__file__).resolve().parent.parent
SHA = '5d85e96facf651ac17402c0e1faff196b4002288'
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


workflow = (ROOT / '.github' / 'workflows' / 'build.yml').read_text()
steps = jobs(workflow)['nightly-due']
script = next(step['run'] for step in steps if 'run' in step).removeprefix('|\n')  # the block's text alone
check(
    'the job runs only on the schedule',
    "if: github.event_name == 'schedule'" in workflow.split('nightly-due:')[1][:200],
)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'bin').mkdir()
    log = root / 'curl.log'
    (root / 'bin' / 'curl').write_text(
        '#!/bin/sh\n'
        f'echo "$*" >> {log}\n'
        'case "$*" in\n'
        '*"-X POST"*) exit 0 ;;\n'
        '*releases/latest*) [ -n "$LATEST" ] || exit 22; printf \'{"tag_name": "%s"}\\n\' "$LATEST" ;;\n'
        'esac\n'
    )
    (root / 'bin' / 'date').write_text('#!/bin/sh\n[ "$*" = "-u +%F" ] || exit 9\necho "$TODAY"\n')
    for tool in ('curl', 'date'):
        (root / 'bin' / tool).chmod(0o755)

    def due(latest, today='2026-11-26'):
        log.unlink(missing_ok=True)
        env = dict(
            os.environ,
            PATH=f'{root / "bin"}{os.pathsep}{os.environ["PATH"]}',
            LATEST=latest,
            TODAY=today,
            GH_TOKEN='t',
            GITHUB_SHA=SHA,
            GITHUB_API_URL='https://api.github.com',
            GITHUB_REPOSITORY='owner/repo',
            GITHUB_RUN_ID='77',
        )
        done = subprocess.run(
            ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        calls = log.read_text() if log.exists() else ''
        return done, '-X POST' in calls and 'repos/owner/repo/actions/runs/77/cancel' in calls

    done, cancelled = due('nightly-2026-11-26-abcdef1')
    check('a day that has its nightly: cancelled', done.returncode == 0 and cancelled, done.stdout + done.stderr)
    done, cancelled = due(f'nightly-2026-11-25-{SHA[:7]}')
    check(
        'the commit the newest nightly was built from, a day later: cancelled, as main did not change',
        done.returncode == 0 and cancelled,
        done.stdout + done.stderr,
    )
    done, cancelled = due('nightly-2026-11-25-abcdef1')
    check(
        'a new day and a new commit: the run goes on, saying both',
        done.returncode == 0 and not cancelled and 'this run: 2026-11-26, 5d85e96' in done.stdout,
        done.stdout + done.stderr,
    )
    done, cancelled = due('nightly-2026-11-260-abcdef1')
    check('a tag only like the day is not it', done.returncode == 0 and not cancelled, done.stdout)
    done, cancelled = due('')
    check('GitHub not answering fails the step', done.returncode != 0 and not cancelled, done.stdout + done.stderr)

# Name the nightly: defined once, in each job that makes a package, before the step that builds
blocks = {
    b.split(':', 1)[0].strip(): b for b in re.split(r'\n(?=  [\w-]+:\n)', workflow[workflow.index('\njobs:\n') :])[1:]
}
for job, builds_by in (
    ('appimage', 'scripts/ci/linux.sh release'),
    ('appimage-cross', 'scripts/ci/cross.sh build'),
    ('windows', 'scripts/ci/windows.sh build'),
    ('android', 'scripts/ci/android.sh build'),
    ('apple', 'scripts/ci/apple.sh macos_build'),
):
    block = blocks[job]
    at = max(block.find('- &nightly-name\n'), block.find('- *nightly-name\n'))
    check(f'{job} names its nightly before it builds', 0 <= at < block.find(builds_by), (at, block.find(builds_by)))
cross = (ROOT / 'scripts' / 'ci' / 'cross.sh').read_text()
check(
    'the cross build hands the name to its container',
    '-e SNX_BUILD_NAME build sh' in cross[cross.index('\nbuild() {') :].split('\n}\n', 1)[0],
)
naming = re.search(r'^      - &nightly-name\n((?:        .*\n)*)', workflow, re.MULTILINE)[1]
check(
    "the name is a scheduled run's, or one by hand of main's",
    "if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'"
    in naming,
)
name_script = '\n'.join(line[10:] for line in naming.split('run: |\n', 1)[1].splitlines())
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'bin').mkdir()
    (root / 'bin' / 'curl').write_text('#!/bin/sh\nprintf \'%s\' "$RUN_JSON"\n')
    (root / 'bin' / 'curl').chmod(0o755)
    for laid, answer in (
        (
            'compact',
            '{"id":77,"pull_requests":[],"created_at":"2026-11-26T00:23:11Z","updated_at":"2026-11-27T00:01:00Z"}',
        ),
        ('indented', '{\n  "id": 77,\n  "pull_requests": [],\n  "created_at": "2026-11-26T23:59:59Z",\n  "x": 1\n}'),
        ('without one', '{"id": 77}'),
    ):
        env_file = root / f'env-{laid}'
        env = dict(
            os.environ,
            PATH=f'{root / "bin"}{os.pathsep}{os.environ["PATH"]}',
            RUN_JSON=answer,
            GH_TOKEN='t',
            GITHUB_API_URL='https://api.github.com',
            GITHUB_REPOSITORY='owner/repo',
            GITHUB_RUN_ID='77',
            GITHUB_ENV=str(env_file),
        )
        done = subprocess.run(
            ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', name_script],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        written = env_file.read_text() if env_file.exists() else ''
        if laid == 'without one':
            check('a run with no day fails the step, naming none', done.returncode != 0 and not written, done.stderr)
        else:
            check(
                f'{laid}: the day the run began, not the one it was updated',
                done.returncode == 0 and written == 'SNX_BUILD_NAME=nightly-2026-11-26\n',
                (done.stdout, done.stderr, written),
            )

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('nightly due: ok')
