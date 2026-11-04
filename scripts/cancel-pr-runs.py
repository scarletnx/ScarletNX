#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Cancel the runs a closed pull request left going, so they free the runners the organization's jobs share.

A pull request merged or closed while its Build still ran keeps that run going to the end, though nothing will merge
what it builds. A push to a pull request already cancels its run before (build.yml's concurrency); this is for the
close. The runs cancelled are those of the pull request's head branch, in its head repository, started by a
pull_request event, that are not done; one that another pull request of the same branch lists stays. A pull request
open again when this reads it, reopened seconds after its close, keeps every run: the reopen's Build is among them, and
build.yml's concurrency drops the one before it. Its runs started since the close that ended cancelled are named in a
warning, as nothing here runs them again.

.github/workflows/pr-closed.yml runs it when a pull request closes: cancel-pr-runs.py NUMBER HEAD_REPO HEAD_BRANCH
--closed-at TIME, TIME the event's closed_at. It needs gh, with GH_REPO set and a token allowed actions: write;
--dry-run only lists.
"""

import argparse
import json
import re
import subprocess
import sys
import urllib.parse

# GitHub's statuses of a run that has not ended
GOING = {'queued', 'in_progress', 'waiting', 'requested', 'pending'}


def ours(run, number, head_repo):
    """a pull_request run of the head repository (any, when the pull request's head repository is gone) that lists no
    other pull request"""
    if run.get('event') != 'pull_request':
        return False
    repo = (run.get('head_repository') or {}).get('full_name')
    if head_repo and repo != head_repo:
        return False
    listed = [p.get('number') for p in run.get('pull_requests') or []]
    return not listed or number in listed


def plan(runs, number, head_repo):
    """The runs to cancel, from GitHub's listing of the head branch's pull_request runs: those not done, of the head
    repository (any, when the pull request's head repository is gone), that list no other pull request"""
    return [run for run in runs if run.get('status') in GOING and ours(run, number, head_repo)]


def cancelled_since(runs, number, head_repo, closed_at):
    """the pull request's runs started since its close that ended cancelled"""
    return [
        run
        for run in runs
        if run.get('conclusion') == 'cancelled'
        and run.get('created_at', '') >= closed_at
        and ours(run, number, head_repo)
    ]


def gh(*args):
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout


def listing(path):
    """every run of a listing of runs, 100 a page"""
    runs, page = [], 1
    while True:
        got = json.loads(gh('api', f'{path}&per_page=100&page={page}'))['workflow_runs']
        runs += got
        if len(got) < 100:
            return runs
        page += 1


def named(run):
    return f'run {run["id"]} ({run.get("name")}, {run["status"]}, {run.get("head_sha", "")[:9]})'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('number', type=int, help='the pull request that closed')
    parser.add_argument('head_repo', help='its head repository (owner/name), empty when it is gone')
    parser.add_argument('head_branch', help='its head branch')
    parser.add_argument('--closed-at', required=True, help="the close's time, as the event's closed_at has it")
    parser.add_argument('--dry-run', action='store_true', help='list what would be cancelled, cancel nothing')
    args = parser.parse_args()
    query = urllib.parse.urlencode({'branch': args.head_branch, 'event': 'pull_request'})
    runs = listing(f'repos/{{owner}}/{{repo}}/actions/runs?{query}')
    doomed = plan(runs, args.number, args.head_repo)
    print(
        f'#{args.number}: {len(runs)} run(s) of {args.head_repo or "a removed repository"}:{args.head_branch}, '
        f'{len(doomed)} still going'
    )
    # read after the listing: a reopen that comes after this read starts runs the listing does not hold
    try:
        state = json.loads(gh('api', f'repos/{{owner}}/{{repo}}/pulls/{args.number}')).get('state')
    except subprocess.CalledProcessError as e:
        found = re.findall(r'HTTP (\d{3})', e.stderr or '')
        sys.stderr.write(
            f'cannot read #{args.number} ({"HTTP " + found[-1] if found else "no HTTP status"}): '
            f'{(e.stderr or "").strip()}. Nothing cancelled: its {len(doomed)} run(s) still going go on\n'
        )
        return 1
    if state == 'open':
        print(f'#{args.number} is open again: nothing cancelled, its {len(doomed)} run(s) still going go on')
        for run in cancelled_since(runs, args.number, args.head_repo, args.closed_at):
            print(
                f'::warning::{named(run)} of #{args.number}, started since its close at {args.closed_at}, was '
                'cancelled before this run, which runs nothing again: re-run it from its page if it is wanted'
            )
        return 0
    failed = 0
    for run in doomed:
        what = named(run)
        if args.dry_run:
            print(f'would cancel {what}')
            continue
        try:
            gh('api', '-X', 'POST', f'repos/{{owner}}/{{repo}}/actions/runs/{run["id"]}/cancel')
            print(f'cancelled {what}')
        except subprocess.CalledProcessError as e:
            # a run that ended between the listing and the cancel answers 409: it is done, as wanted
            if '409' in (e.stderr or ''):
                print(f'{what} ended before its cancel')
            else:
                failed += 1
                sys.stderr.write(f'could not cancel {what}: {(e.stderr or "").strip()}\n')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
