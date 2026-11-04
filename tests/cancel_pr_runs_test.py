#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/cancel-pr-runs.py's choice of the runs a closed pull request left going"""

import importlib.util
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location(
    'cancel_pr_runs', Path(__file__).resolve().parent.parent / 'scripts' / 'cancel-pr-runs.py'
)
cancel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cancel)

failures = []


def check(what, got, want):
    if got != want:
        failures.append(f'{what}: got {got!r}, want {want!r}')


def run(id, status='in_progress', event='pull_request', repo='scarletnx/ScarletNX', prs=(1137,)):
    return {
        'id': id,
        'name': 'Build',
        'status': status,
        'event': event,
        'head_sha': f'{id:040x}',
        'head_repository': {'full_name': repo} if repo else None,
        'pull_requests': [{'number': n} for n in prs],
    }


def ids(runs):
    return [r['id'] for r in runs]


# a Build still going, queued or waiting goes; one done stays
runs = [run(1), run(2, 'queued'), run(3, 'waiting'), run(4, 'completed'), run(5, 'pending'), run(6, 'requested')]
check('the ones not done', ids(cancel.plan(runs, 1137, 'scarletnx/ScarletNX')), [1, 2, 3, 5, 6])

# only pull_request events: a push to the same branch name, or a link run after a Build, stays
runs = [run(1, event='push'), run(2, event='workflow_run'), run(3, event='pull_request_target'), run(4)]
check('pull_request events only', ids(cancel.plan(runs, 1137, 'scarletnx/ScarletNX')), [4])

# a fork's branch of the same name (main, say) is another branch: it stays
runs = [run(1, repo='someone/ScarletNX', prs=()), run(2)]
check('the head repository only', ids(cancel.plan(runs, 1137, 'scarletnx/ScarletNX')), [2])
check("a fork's own head", ids(cancel.plan(runs, 1054, 'someone/ScarletNX')), [1])

# a run another open pull request of the same branch lists stays; one listing none (a fork's) goes
runs = [run(1, prs=(1140,)), run(2, prs=(1137, 1140)), run(3, prs=())]
check('another pull request of the branch', ids(cancel.plan(runs, 1137, 'scarletnx/ScarletNX')), [2, 3])

# a removed head repository: any repository's run of the branch that lists this pull request, or none, goes
runs = [run(1, repo=None, prs=()), run(2, repo='someone/ScarletNX', prs=(1054,)), run(3, prs=(99,))]
check('a removed head repository', ids(cancel.plan(runs, 1054, '')), [1, 2])

check('nothing to cancel', cancel.plan([], 1137, 'scarletnx/ScarletNX'), [])

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('cancel_pr_runs: ok')
