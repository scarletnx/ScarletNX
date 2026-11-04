#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Cancel the runs a closed pull request left going, so they free the runners the organization's jobs share.

A pull request merged or closed while its Build still ran keeps that run going to the end, though nothing will merge
what it builds. A push to a pull request already cancels its run before (build.yml's concurrency); this is for the
close. The runs cancelled are those of the pull request's head branch, in its head repository, started by a
pull_request event, that are not done; one that another pull request of the same branch lists stays.

.github/workflows/pr-closed.yml runs it when a pull request closes: cancel-pr-runs.py NUMBER HEAD_REPO HEAD_BRANCH.
It needs gh, with GH_REPO set and a token allowed actions: write; --dry-run only lists.
"""

import argparse
import json
import subprocess
import sys
import urllib.parse

# GitHub's statuses of a run that has not ended
GOING = {'queued', 'in_progress', 'waiting', 'requested', 'pending'}


def plan(runs, number, head_repo):
    """The runs to cancel, from GitHub's listing of the head branch's pull_request runs: those not done, of the head
    repository (any, when the pull request's head repository is gone), that list no other pull request"""
    doomed = []
    for run in runs:
        if run.get('event') != 'pull_request' or run.get('status') not in GOING:
            continue
        repo = (run.get('head_repository') or {}).get('full_name')
        if head_repo and repo != head_repo:
            continue
        listed = [p.get('number') for p in run.get('pull_requests') or []]
        if listed and number not in listed:
            continue
        doomed.append(run)
    return doomed


def gh(*args):
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('number', type=int, help='the pull request that closed')
    parser.add_argument('head_repo', help='its head repository (owner/name), empty when it is gone')
    parser.add_argument('head_branch', help='its head branch')
    parser.add_argument('--dry-run', action='store_true', help='list what would be cancelled, cancel nothing')
    args = parser.parse_args()
    query = urllib.parse.urlencode({'branch': args.head_branch, 'event': 'pull_request', 'per_page': 100})
    pages = gh(
        'api', '--paginate', f'repos/{{owner}}/{{repo}}/actions/runs?{query}', '--jq', '.workflow_runs[] | tojson'
    )
    runs = [json.loads(line) for line in pages.splitlines() if line.strip()]
    doomed = plan(runs, args.number, args.head_repo)
    print(
        f'#{args.number}: {len(runs)} run(s) of {args.head_repo or "a removed repository"}:{args.head_branch}, '
        f'{len(doomed)} still going'
    )
    failed = 0
    for run in doomed:
        what = f'run {run["id"]} ({run.get("name")}, {run["status"]}, {run.get("head_sha", "")[:9]})'
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
