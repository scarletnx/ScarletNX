#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/cancel-pr-runs.py's choice of the runs a closed pull request left going, then the script against
tests/fake_github.py's gh: a pull request still closed has them cancelled, one reopened seconds after its close keeps
them, and its runs cancelled already since the close are named. Offline."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / 'scripts' / 'cancel-pr-runs.py'


def load(name, path):
    """a module by its path, as python -I puts no folder of the test on the path"""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cancel = load('cancel_pr_runs', SCRIPT)
FAKE_GH = load('fake_github', HERE / 'fake_github.py').FAKE_GH

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

# The script, from the close's event on: the listing, the pull request read after it, then the cancels
CLOSED = '2026-11-04T04:45:26Z'


def going(id, status='in_progress', conclusion=None, created='2026-11-04T04:40:00Z'):
    """a run of the head branch, as the runs listing has it"""
    return dict(run(id, status), head_branch='feature', conclusion=conclusion, created_at=created)


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    bin_dir = root / 'bin'
    bin_dir.mkdir()
    (bin_dir / 'gh').write_text(FAKE_GH)
    (bin_dir / 'gh').chmod(0o755)
    state_file, log_file = root / 'state.json', root / 'gh.log'

    def start(pr_state, runs, **extra):
        """a new fake GitHub where pull request 1137 is pr_state when the script reads it"""
        state = {'releases': {}, 'tags': {}, 'next': 0, 'runs': runs, 'pulls': [{'number': 1137, 'state': pr_state}]}
        state_file.write_text(json.dumps(dict(state, **extra)))
        log_file.write_text('')

    def script(*extra):
        env = dict(
            os.environ,
            PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            FAKE_GH_STATE=str(state_file),
            FAKE_GH_LOG=str(log_file),
        )
        return subprocess.run(
            [sys.executable, str(SCRIPT), '1137', 'scarletnx/ScarletNX', 'feature', '--closed-at', CLOSED, *extra],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

    def ended(id):
        found = next(r for r in json.loads(state_file.read_text())['runs'] if r['id'] == id)
        return found['status'], found['conclusion']

    def writes():
        return [c['args'] for c in map(json.loads, log_file.read_text().splitlines()) if '-X' in c['args']]

    # closed, and still closed when it reads: the run going is cancelled, the one done stays
    start('closed', [going(1), going(2, 'completed', 'success')])
    done = script()
    check('still closed: exit', done.returncode, 0)
    check('still closed: the run going', ended(1), ('completed', 'cancelled'))
    check('still closed: the run done', ended(2), ('completed', 'success'))
    check('still closed: one cancel', writes(), [['api', '-X', 'POST', 'repos/{owner}/{repo}/actions/runs/1/cancel']])

    # closed, then reopened 2 s later, before the job reads it: the reopen's Build and the one before it go on
    start('open', [going(1), going(2, 'queued', created='2026-11-04T04:45:28Z')])
    done = script()
    check('reopened: exit', done.returncode, 0)
    check('reopened: nothing cancelled', writes(), [])
    check('reopened: the runs as they were', [ended(1), ended(2)], [('in_progress', None), ('queued', None)])
    check(
        'reopened: says so',
        '#1137 is open again: nothing cancelled, its 2 run(s) still going go on' in done.stdout,
        True,
    )

    # reopened just after the job reads it closed: the run going before is cancelled, the reopen's Build, which the
    # listing made before that read can't hold, goes on
    start('closed', [going(1)], reopen_after_read=going(2, 'queued', created='2026-11-04T04:45:40Z'))
    done = script()
    check('reopened after the read: exit', done.returncode, 0)
    check('reopened after the read: the run before', ended(1), ('completed', 'cancelled'))
    check("reopened after the read: the reopen's run", ended(2), ('queued', None))

    # reopened, and a run of its since the close cancelled already: named in a warning, and run again by nothing; one
    # cancelled before the close is not its reopen's
    start(
        'open',
        [
            going(3, 'completed', 'cancelled', created='2026-11-04T04:45:28Z'),
            going(4, 'completed', 'cancelled', created='2026-11-04T04:00:00Z'),
            going(5, 'queued', created='2026-11-04T04:45:40Z'),
        ],
    )
    done = script()
    warnings = [line for line in done.stdout.splitlines() if line.startswith('::warning::')]
    check(
        'reopened, a run cancelled since the close: named',
        warnings,
        [
            (
                f'::warning::run 3 (Build, completed, 000000000) of #1137, started since its close at {CLOSED}, was '
                'cancelled before this run, which runs nothing again: re-run it from its page if it is wanted'
            )
        ],
    )
    check('reopened, a run cancelled since the close: nothing written', writes(), [])

    # the pull request can't be read after the listing: nothing cancelled, and the run fails naming it and the status
    start('closed', [going(1)], pull_errors={'1137': 'gh: Server Error (HTTP 502)'})
    done = script()
    check('unreadable: exit', done.returncode, 1)
    check(
        'unreadable: says so',
        'cannot read #1137 (HTTP 502): gh: Server Error (HTTP 502). Nothing cancelled: its 1 run(s) still going go on'
        in done.stderr,
        True,
    )
    check('unreadable: the run going goes on', ended(1), ('in_progress', None))
    check('unreadable: nothing written', writes(), [])

    # --dry-run lists and writes nothing
    start('closed', [going(1)])
    done = script('--dry-run')
    check('dry run: says it', 'would cancel run 1 (Build, in_progress, 000000000)' in done.stdout, True)
    check('dry run: nothing written', writes(), [])

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('cancel_pr_runs: ok')
