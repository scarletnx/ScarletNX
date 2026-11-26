#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""build.yml's Nightly due, run as Actions runs its step (bash -eo pipefail) against a fake curl and date: a scheduled
run cancels itself on a day (UTC) the newest nightly is of, and when the newest nightly was built from its commit, and
goes on otherwise; GitHub not answering fails the step. Offline."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_caches_test import jobs  # noqa: E402

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

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('nightly due: ok')
