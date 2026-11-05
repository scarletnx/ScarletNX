#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/pr-ready.py against tests/fake_github.py's gh: a pull request made ready by someone without write access
goes back to a draft with one comment, however often it comes back; one opened or marked ready by someone with write
access stays, and so does a maintainer's release through a reopen or a re-run; a draft or a closed one is not even
looked at past the pull request; a permission GitHub can't give stops the run, naming the login and the status, with
nothing changed. Offline."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / 'scripts' / 'pr-ready.py'


def load(name, path):
    """a module by its path, as python -I puts no folder of the test on the path"""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ready = load('pr_ready', SCRIPT)
FAKE_GH = load('fake_github', HERE / 'fake_github.py').FAKE_GH

REPO = 'owner/repo'
PERMISSIONS = {'maintainer': 'admin', 'writer': 'write', 'outsider': 'read', 'other': 'read', 'app[bot]': 'none'}
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


def pull(number, author='outsider', draft=False, state='open'):
    return {
        'number': number,
        'state': state,
        'draft': draft,
        'user': {'login': author},
        'node_id': f'PR_node{number}',
        'labels': [],
    }


def at(hour):
    return f'2026-11-01T{hour:02d}:00:00Z'


def marked(id, login, hour):
    """a ready_for_review event of a pull request's timeline, by login"""
    return {'id': id, 'event': 'ready_for_review', 'actor': {'login': login}, 'created_at': at(hour)}


def drafted(id, login, hour):
    return {'id': id, 'event': 'convert_to_draft', 'actor': {'login': login}, 'created_at': at(hour)}


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    bin_dir = root / 'bin'
    bin_dir.mkdir()
    (bin_dir / 'gh').write_text(FAKE_GH)
    (bin_dir / 'gh').chmod(0o755)
    state_file, log_file = root / 'state.json', root / 'gh.log'

    def start(pulls, **extra):
        """a new fake GitHub holding pulls"""
        state = {'releases': {}, 'tags': {}, 'next': 100, 'pulls': pulls, 'permissions': PERMISSIONS, 'events': {}}
        state_file.write_text(json.dumps(dict(state, **extra)))
        log_file.write_text('')

    def state():
        return json.loads(state_file.read_text())

    def change(**fields):
        state_file.write_text(json.dumps(dict(state(), **fields)))

    def the_pull(number):
        return next(p for p in state()['pulls'] if p['number'] == number)

    def comments(number):
        return state().get('comments', {}).get(str(number), [])

    def calls():
        return [json.loads(line)['args'] for line in log_file.read_text().splitlines()]

    def writes():
        return [c for c in calls() if '-X' in c and c[c.index('-X') + 1] != 'GET' or 'graphql' in c]

    def run(number, action, sender, *extra, merged=False):
        """the script run; merged, its errors in its output, as a workflow's log has them"""
        env = dict(
            os.environ,
            PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            FAKE_GH_STATE=str(state_file),
            FAKE_GH_LOG=str(log_file),
        )
        return subprocess.run(
            [sys.executable, str(SCRIPT), '--repo', REPO, str(number), action, sender, *extra],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT if merged else subprocess.PIPE,
            text=True,
            env=env,
            check=False,
        )

    # an outsider's pull request opened ready goes back to a draft, with the comment
    start([pull(5)])
    done = run(5, 'opened', 'outsider')
    check('opened ready by an outsider: exit 0', done.returncode == 0, done.stderr)
    check('opened ready by an outsider: a draft', the_pull(5)['draft'] is True)
    check(
        'opened ready by an outsider: the comment, from the job',
        [(c['user']['login'], c['body']) for c in comments(5)] == [('github-actions[bot]', ready.COMMENT)],
        comments(5),
    )
    check(
        'opened ready by an outsider: says who and their permission',
        '#5 goes back to a draft: outsider, who opened it, has read' in done.stdout,
        done.stdout,
    )
    # marked ready again by the outsider: a draft again, and still the one comment
    change(pulls=[dict(the_pull(5), draft=False)], events={'5': [drafted(1, 'github-actions[bot]', 1)]})
    done = run(5, 'ready_for_review', 'outsider')
    check('marked ready again by the outsider: exit 0', done.returncode == 0, done.stderr)
    check('marked ready again by the outsider: a draft', the_pull(5)['draft'] is True)
    check('marked ready again by the outsider: one comment still', len(comments(5)) == 1, comments(5))
    check('marked ready again: says the comment is there', 'has the comment already' in done.stdout, done.stdout)

    # someone else's comment that starts with the mark is not the job's: the job's is still posted
    start([pull(6)], comments={'6': [{'id': 1, 'user': {'login': 'outsider'}, 'body': ready.MARK + ' made up'}]})
    run(6, 'opened', 'outsider')
    check(
        "a look-alike comment by the outsider: the job's posted",
        [c['user']['login'] for c in comments(6)] == ['outsider', 'github-actions[bot]'],
        comments(6),
    )

    # someone with write access, opening it or marking an outsider's ready, leaves it ready and writes nothing
    for login, action in (('writer', 'opened'), ('maintainer', 'ready_for_review'), ('writer', 'reopened')):
        start([pull(7, author=login if action == 'opened' else 'outsider')])
        done = run(7, action, login)
        what = f'{action} by {login} ({PERMISSIONS[login]})'
        check(f'{what}: exit 0', done.returncode == 0, done.stderr)
        check(f'{what}: still ready', the_pull(7)['draft'] is False)
        check(f'{what}: nothing written', writes() == [], writes())
        check(f'{what}: says why', f'#7 stays ready for review: {login}, who ' in done.stdout, done.stdout)

    # a draft, or a closed pull request, is read and nothing more: no permission asked, nothing written
    for what, pr, action in (
        ("an outsider's draft", pull(8, draft=True), 'opened'),
        ("an outsider's draft reopened", pull(8, draft=True), 'reopened'),
        ('a pull request closed before its run', pull(8, state='closed'), 'reopened'),
    ):
        start([pr])
        done = run(8, action, 'outsider')
        check(f'{what}: exit 0', done.returncode == 0, done.stderr)
        check(f'{what}: as it was', the_pull(8) == pr, the_pull(8))
        check(f'{what}: only the pull request read', calls() == [['api', f'repos/{REPO}/pulls/8']], calls())

    # a permission GitHub can't give stops the run: the login and the status named, nothing changed
    for login, error, status in (
        ('outsider', 'gh: Server Error (HTTP 502)', 'HTTP 502'),
        ('gone', 'gh: gone is not a user (HTTP 404)', 'HTTP 404'),
        ('outsider', 'error connecting to api.github.com', 'no HTTP status'),
    ):
        start([pull(9, author=login)], permission_errors={login: error})
        done = run(9, 'opened', login)
        what = f'the permission of {login} failing with {status}'
        check(f'{what}: exit 1', done.returncode == 1, done.returncode)
        check(
            f'{what}: says so',
            f'cannot read the permission of {login} on {REPO} ({status}): {error}. #9 stays ready for review'
            in done.stderr,
            done.stderr,
        )
        check(f'{what}: still ready', the_pull(9)['draft'] is False)
        check(f'{what}: nothing written', writes() == [], writes())
    # and so does a permission that is none of the endpoint's answers
    start([pull(9)], permissions=dict(PERMISSIONS, outsider='triage'))
    done = run(9, 'opened', 'outsider')
    check('an unknown permission: exit 1', done.returncode == 1, done.returncode)
    check(
        'an unknown permission: says it', "reads 'triage', none of admin, none, read, write" in done.stderr, done.stderr
    )
    check('an unknown permission: nothing written', writes() == [], writes())

    # a bot without write access is held as anyone: the repository trusts no bot by name
    start([pull(10, author='app[bot]')])
    done = run(10, 'opened', 'app[bot]')
    check('a bot: a draft', the_pull(10)['draft'] is True, done.stdout + done.stderr)

    # a maintainer's release stands when the author closes and reopens it; the newest mark is the one that counts
    release = [marked(1, 'outsider', 1), drafted(2, 'github-actions[bot]', 2), marked(3, 'maintainer', 9)]
    for what, events, held in (
        ('reopened after a maintainer marked it ready', release, False),
        ('reopened, opened ready and never marked', [], True),
        ('reopened after its author marked it ready over a release',
         [*release, drafted(4, 'outsider', 10), marked(5, 'outsider', 11)], True),
    ):  # fmt: skip
        start([pull(11)], events={'11': events})
        done = run(11, 'reopened', 'outsider')
        check(f'{what}: exit 0', done.returncode == 0, done.stderr)
        check(f'{what}: {"a draft" if held else "still ready"}', the_pull(11)['draft'] is held, done.stdout)
        check(f'{what}: {"the" if held else "no"} comment', len(comments(11)) == int(held), comments(11))
    check(
        'reopened after its author marked it ready over a release: names both',
        '#11 goes back to a draft: outsider, who reopened it, has read' in done.stdout,
        done.stdout,
    )
    start([pull(11)], events={'11': release})
    done = run(11, 'reopened', 'outsider')
    check(
        'reopened after a release: names who released it and when',
        f'#11 stays ready for review: maintainer, who marked it ready at {at(9)}, has admin' in done.stdout,
        done.stdout,
    )

    # A first run of a mark trusts the sender: the events may not list that mark yet, and the newest they show is a
    # maintainer's older one. A re-run trusts the events: it may come after a newer release
    older = [marked(1, 'maintainer', 1), drafted(2, 'outsider', 2)]
    for what, attempt, events, held in (
        ('marked ready by the outsider, its mark not listed yet', '1', older, True),
        ('a re-run after a maintainer marked it ready', '2',
         [*older, marked(3, 'outsider', 3), drafted(4, 'github-actions[bot]', 4), marked(5, 'maintainer', 5)], False),
        ('a re-run, the outsider the newest to mark it', '2', [*older, marked(3, 'outsider', 3)], True),
    ):  # fmt: skip
        start([pull(12)], events={'12': events})
        done = run(12, 'ready_for_review', 'outsider', '--attempt', attempt)
        check(f'{what}: exit 0', done.returncode == 0, done.stderr)
        check(f'{what}: {"a draft" if held else "still ready"}', the_pull(12)['draft'] is held, done.stdout)
    # a re-run of an opened pull request's run, after a maintainer marked it ready
    start([pull(12)], events={'12': [drafted(1, 'github-actions[bot]', 1), marked(2, 'writer', 2)]})
    done = run(12, 'opened', 'outsider', '--attempt', '3')
    check('a re-run of its opening after a release: still ready', the_pull(12)['draft'] is False, done.stdout)

    # The comment goes first: a conversion that failed leaves it ready with its comment, and a re-run makes the draft
    # without a second comment
    start([pull(13)], fail_convert_once=True)
    done = run(13, 'opened', 'outsider')
    check('a conversion refused: exit 1', done.returncode == 1, done.returncode)
    check(
        'a conversion refused: says so',
        'cannot turn #13 into a draft (no HTTP status): GraphQL: Resource not accessible by integration' in done.stderr,
        done.stderr,
    )
    check('a conversion refused: still ready', the_pull(13)['draft'] is False)
    check('a conversion refused: the comment made', len(comments(13)) == 1, comments(13))
    done = run(13, 'opened', 'outsider', '--attempt', '2')
    check('its re-run: a draft', the_pull(13)['draft'] is True, done.stderr)
    check('its re-run: one comment still', len(comments(13)) == 1, comments(13))
    # in the log, what it did comes before the error that stopped it
    start([pull(13)], fail_convert_once=True)
    log = run(13, 'opened', 'outsider', merged=True).stdout
    check(
        'a conversion refused: the log in order',
        log.find('goes back to a draft') < log.find('commented on #13') < log.find('cannot turn #13'),
        log,
    )

    # --dry-run reads and says, and writes nothing
    start([pull(14)])
    done = run(14, 'opened', 'outsider', '--dry-run')
    check('dry run: exit 0', done.returncode == 0, done.stderr)
    check('dry run: says both', 'would comment on #14' in done.stdout and 'would turn #14 into a draft' in done.stdout)
    check('dry run: nothing written', writes() == [] and the_pull(14)['draft'] is False, writes())

    # what the workflow can't send is refused before anything is read
    for what, args in (
        ('another action', (15, 'synchronize', 'outsider')),
        ('a sender that is no login', (15, 'opened', 'a/../b')),
        ('attempt 0', (15, 'opened', 'outsider', '--attempt', '0')),
    ):
        start([pull(15)])
        done = run(*args)
        check(f'{what}: exit 2', done.returncode == 2, done.returncode)
        check(f'{what}: nothing read', calls() == [], calls())

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('pr_ready: ok')
