#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Keep a pull request a draft until someone with write access to the repository marks it ready for review.

GitHub has no setting for it: the author of a pull request may always open it ready or mark it ready. So one opened,
reopened or marked ready goes back to a draft, with one comment saying why, unless the one who did it has write access
(write, maintain or admin, as GitHub's permission endpoint reports it), or the one who made it ready has: a
maintainer's release stands until the pull request is a draft again, through a close and a reopen by its author too.
Who made it ready is the one who marked it ready, or its author for one opened ready. A re-run (ATTEMPT over 1) may
come after a newer release, so it reads that one from the pull request's events.

.github/workflows/pr-ready.yml runs it on those events: pr-ready.py --repo OWNER/NAME NUMBER ACTION SENDER --attempt N.
It reads the pull request as it is now: a draft, or one closed, stays as it is. It needs gh, with a token allowed
pull-requests: write and contents: write (GitHub refuses the draft to a token without it); --dry-run only says what it
would do.
"""

import argparse
import json
import re
import subprocess
import sys

# the permission endpoint's legacy roles, all it answers: maintain reads as write there, triage as read
WRITE = {'admin', 'write'}
PERMISSIONS = WRITE | {'read', 'none'}
DID = {'opened': 'opened it', 'reopened': 'reopened it', 'ready_for_review': 'marked it ready'}
LOGIN = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.-]*(\[bot\])?')
BOT = 'github-actions[bot]'
MARK = '<!-- pr-ready:draft -->'
COMMENT = (
    f'{MARK}\n'
    'This pull request is a draft again. In this repository a pull request leaves draft when a maintainer, someone '
    'with write access to it, marks it ready for review. A draft is built and tested like any other pull request. '
    'When yours is ready, say so in a comment and a maintainer will mark it ready. Marking it ready yourself makes it '
    'a draft again.'
)
# PROBE ONLY, dropped before the pull request: logins taken as without write access
AS_OUTSIDER = set()
CONVERT = 'mutation($id: ID!) { convertPullRequestToDraft(input: {pullRequestId: $id}) { pullRequest { isDraft } } }'


class Stop(Exception):
    """a step that could not be done: the run fails with this message"""


class GitHubError(RuntimeError):
    """a gh call that failed, with what gh said"""


def gh(args, data=None):
    run = subprocess.run(
        ['gh', *args],
        input=json.dumps(data) if data is not None else None,
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode:
        raise GitHubError((run.stderr or run.stdout).strip() or f'gh exited with {run.returncode} and said nothing')
    return json.loads(run.stdout) if run.stdout.strip() else {}


def status(error):
    """the HTTP status gh named in its error, or that it named none"""
    found = re.findall(r'HTTP (\d{3})', str(error))
    return f'HTTP {found[-1]}' if found else 'no HTTP status'


def pages(path):
    """every item of a listing, 100 a page"""
    items, page = [], 1
    while True:
        got = gh(['api', f'{path}?per_page=100&page={page}'])
        items += got
        if len(got) < 100:
            return items
        page += 1


def permission(repo, login, number):
    """login's permission on repo, one of PERMISSIONS"""
    try:
        got = gh(['api', f'repos/{repo}/collaborators/{login}/permission'])
    except GitHubError as error:
        raise Stop(
            f'cannot read the permission of {login} on {repo} ({status(error)}): {error}. '
            f'#{number} stays ready for review, nothing was changed'
        ) from error
    value = got.get('permission')
    if value not in PERMISSIONS:
        raise Stop(
            f'the permission of {login} on {repo} reads {value!r}, none of {", ".join(sorted(PERMISSIONS))}. '
            f'#{number} stays ready for review, nothing was changed'
        )
    if login in AS_OUTSIDER:
        print(f'probe: {login} has {value}, taken as read')
        return 'read'
    return value


def made_ready(repo, pr, action, sender, attempt):
    """who made the pull request ready, and how: the sender of a first run of the event that did, else the actor of
    its newest ready_for_review event, else its author, who opened it ready"""
    if attempt == 1 and action in ('opened', 'ready_for_review'):
        return sender, DID[action]
    try:
        events = pages(f'repos/{repo}/issues/{pr["number"]}/events')
    except GitHubError as error:
        raise Stop(
            f'cannot read the events of #{pr["number"]} ({status(error)}): {error}. It stays ready for review, '
            'nothing was changed'
        ) from error
    marks = [e for e in events if e.get('event') == 'ready_for_review']
    if not marks:
        return pr['user']['login'], 'opened it ready'
    newest = max(marks, key=lambda e: (e.get('created_at') or '', e.get('id') or 0))
    # GitHub's name for a removed account
    return (newest.get('actor') or {}).get('login') or 'ghost', f'marked it ready at {newest.get("created_at")}'


def hold(repo, pr, dry_run):
    """the comment, unless the pull request has it, then the pull request turned into a draft. The comment goes first:
    a run whose conversion failed is run again and finds the pull request still ready and its comment there, while one
    converted whose comment failed would find a draft and never post it"""
    number = pr['number']
    try:
        comments = pages(f'repos/{repo}/issues/{number}/comments')
    except GitHubError as error:
        raise Stop(
            f'cannot read the comments of #{number} ({status(error)}): {error}. It stays ready for review'
        ) from error
    there = next(
        (c for c in comments if (c.get('user') or {}).get('login') == BOT and (c.get('body') or '').startswith(MARK)),
        None,
    )
    if there:
        print(f'#{number} has the comment already: {there.get("html_url") or there.get("id")}')
    elif dry_run:
        print(f'would comment on #{number}:\n{COMMENT}')
    else:
        try:
            gh(['api', '-X', 'POST', f'repos/{repo}/issues/{number}/comments', '--input', '-'], {'body': COMMENT})
        except GitHubError as error:
            raise Stop(f'cannot comment on #{number} ({status(error)}): {error}. It stays ready for review') from error
        print(f'commented on #{number}')
    if dry_run:
        print(f'would turn #{number} into a draft')
        return
    try:
        got = gh(['api', 'graphql', '-f', f'query={CONVERT}', '-f', f'id={pr["node_id"]}'])
    except GitHubError as error:
        raise Stop(
            f'cannot turn #{number} into a draft ({status(error)}): {error}. It stays ready for review'
        ) from error
    done = ((got.get('data') or {}).get('convertPullRequestToDraft') or {}).get('pullRequest') or {}
    if done.get('isDraft') is not True:
        raise Stop(f'GitHub answered the conversion of #{number} with {json.dumps(got)}: it stays ready for review')
    print(f'#{number} is a draft again')


def decide(args):
    repo, number, sender = args.repo, args.number, args.sender
    try:
        pr = gh(['api', f'repos/{repo}/pulls/{number}'])
    except GitHubError as error:
        raise Stop(f'cannot read #{number} of {repo} ({status(error)}): {error}. Nothing was changed') from error
    if pr.get('state') != 'open':
        print(f'#{number} is {pr.get("state")}: nothing to hold')
        return
    if pr.get('draft'):
        print(f'#{number} is a draft: nothing to hold')
        return
    have = permission(repo, sender, number)
    acted = f'{sender}, who {DID[args.action]}, has {have}'
    if have in WRITE:
        print(f'#{number} stays ready for review: {acted}')
        return
    who, how = made_ready(repo, pr, args.action, sender, args.attempt)
    if who != sender:
        theirs = permission(repo, who, number)
        released = f'{who}, who {how}, has {theirs}'
        if theirs in WRITE:
            print(f'#{number} stays ready for review: {released} ({acted})')
            return
        acted += f', and {released}'
    print(f'#{number} goes back to a draft: {acted}')
    hold(repo, pr, args.dry_run)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repo', required=True, help='the repository, OWNER/NAME')
    parser.add_argument('number', type=int, help='the pull request')
    parser.add_argument('action', choices=sorted(DID), help="the pull_request_target event's action")
    parser.add_argument('sender', help="the event's sender, who acted")
    parser.add_argument('--attempt', type=int, default=1, help="the run's attempt, 1 for its first run (default 1)")
    parser.add_argument('--dry-run', action='store_true', help='say what it would do, change nothing')
    parser.add_argument('--as-outsider', action='append', default=[], help='PROBE ONLY: take LOGIN as read')
    args = parser.parse_args()
    AS_OUTSIDER.update(args.as_outsider)
    if not LOGIN.fullmatch(args.sender):
        parser.error(f'the sender {args.sender!r} is not a GitHub login')
    if args.attempt < 1:
        parser.error(f'--attempt {args.attempt} is not a run attempt: they count from 1')
    try:
        decide(args)
    except Stop as error:
        sys.stdout.flush()  # what it did before, in the log before the error
        sys.stderr.write(f'pr-ready.py: {error}\n')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
