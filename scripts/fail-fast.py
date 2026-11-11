#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Cancel a Build run once one of its jobs has failed, so its other jobs stop building what nothing will use.

A job of .github/workflows/build.yml that fails uploads the artifact fail-fast-<attempt> (its fail-fast-mark step), and
the jobs still running watch for it here: once a job that failed has ended, the watch cancels the run. The failed job
keeps its failure, where a job that cancels its own run ends cancelled and hides what failed. The watch runs inside the
jobs that already hold a runner, so the cancel waits for no runner of its own however long the queue is.

    scripts/fail-fast.py start   # in a step: the first look, said in the step's log, then the watch in the background
    scripts/fail-fast.py watch   # the watch itself, until it cancels the run or its job ends

The artifacts are asked for with their ETag: a look that finds nothing new is a 304, which GitHub does not count against
the token's 1000 requests an hour; the jobs are read only once the mark is there. A job whose name SNX_OPTIONAL_JOBS
matches (one that may fail without failing the run, continue-on-error) never has the run cancelled. It reads
GITHUB_API_URL, GITHUB_REPOSITORY, GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT, GH_TOKEN (a token allowed actions: write; a fork's
pull request has none, and its cancel is refused and said), SNX_OPTIONAL_JOBS, RUNNER_TEMP (where start writes the
watch's log) and SNX_FAIL_FAST_POLL (the seconds between looks, 30 when unset). tests/fail_fast_test.py runs it against
a fake GitHub.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

POLL = 30
# once the mark is there: how often the jobs are read, waiting for the job that failed to end, and for how long at most
POLL_JOBS = 10
WAIT_JOBS = 600


def need(name):
    value = os.environ.get(name, '')
    if not value:
        raise SystemExit(f'fail-fast: {name} is not set')
    return value


class Run:
    """the run this job is part of, as the environment names it"""

    def __init__(self):
        self.api = f'{need("GITHUB_API_URL")}/repos/{need("GITHUB_REPOSITORY")}/actions/runs/{need("GITHUB_RUN_ID")}'
        self.id = os.environ['GITHUB_RUN_ID']
        self.mark = f'fail-fast-{need("GITHUB_RUN_ATTEMPT")}'
        self.token = need('GH_TOKEN')
        try:
            self.optional = re.compile(need('SNX_OPTIONAL_JOBS'))
        except re.error as error:
            raise SystemExit(f'fail-fast: SNX_OPTIONAL_JOBS={os.environ["SNX_OPTIONAL_JOBS"]} is no regex: {error}')
        poll = os.environ.get('SNX_FAIL_FAST_POLL', str(POLL))
        try:
            self.poll = float(poll)
        except ValueError:
            raise SystemExit(f'fail-fast: SNX_FAIL_FAST_POLL={poll} is not a number of seconds') from None
        if self.poll <= 0:
            raise SystemExit(f'fail-fast: SNX_FAIL_FAST_POLL={poll} is not a number of seconds over 0')

    def ask(self, path, method='GET', etag=None):
        """(status, ETag, body) of the run's API at path; a 304 or an error status is returned, not raised"""
        request = urllib.request.Request(f'{self.api}{path}', method=method)
        request.add_header('Authorization', f'Bearer {self.token}')
        request.add_header('Accept', 'application/vnd.github+json')
        if etag:
            request.add_header('If-None-Match', etag)
        try:
            with urllib.request.urlopen(request, timeout=30) as answer:
                text = answer.read().decode()
                return answer.status, answer.headers.get('ETag'), json.loads(text) if text.strip() else None
        except urllib.error.HTTPError as error:
            return error.code, error.headers.get('ETag'), error.read().decode(errors='replace')

    def look(self, etag):
        """(status, ETag, how many marks): the artifacts named after this attempt's mark"""
        status, new, body = self.ask(f'/artifacts?name={self.mark}', etag=etag)
        count = body.get('total_count', 0) if status == 200 and isinstance(body, dict) else 0
        return status, new or etag, count

    def failed(self):
        """the jobs of the latest attempt that ended failed and may not fail; None when the jobs did not answer"""
        status, _, body = self.ask('/jobs?filter=latest&per_page=100')
        if status != 200 or not isinstance(body, dict):
            say(f'the jobs of run {self.id} answered {status}: reading them again in {POLL_JOBS} s')
            return None
        return [
            job['name']
            for job in body.get('jobs', [])
            if job.get('status') == 'completed'
            and job.get('conclusion') in ('failure', 'timed_out')
            and not self.optional.search(job.get('name', ''))
        ]


def say(text):
    print(f'{time.strftime("%H:%M:%S")} fail-fast: {text}', flush=True)


def watch(run, etag=None):
    """0 when the run was cancelled or was ending already; 1 when the cancel was refused, or no job ended failed within
    WAIT_JOBS seconds of the mark"""
    while True:
        try:
            status, etag, count = run.look(etag)
        except (urllib.error.URLError, OSError, ValueError) as error:
            say(f'the artifacts of run {run.id} did not answer ({error}): looking again in {run.poll:g} s')
            status, count = None, 0
        if status not in (None, 200, 304):
            say(f'the artifacts of run {run.id} answered {status}: looking again in {run.poll:g} s')
        if count:
            break
        time.sleep(run.poll)
    say(f'{run.mark} is there, a job of run {run.id} failed: waiting for it to end')
    deadline = time.monotonic() + WAIT_JOBS
    while True:
        try:
            failed = run.failed()
        except (urllib.error.URLError, OSError, ValueError) as error:
            say(f'the jobs of run {run.id} did not answer ({error}): reading them again in {POLL_JOBS} s')
            failed = None
        if failed:
            break
        if time.monotonic() > deadline:
            say(
                f'{run.mark} is there, yet no job of run {run.id} SNX_OPTIONAL_JOBS leaves in ended failed within'
                f' {WAIT_JOBS} s: the run goes on'
            )
            return 1
        time.sleep(min(run.poll, POLL_JOBS))
    say(f'{", ".join(failed)} failed: cancelling the rest of run {run.id}')
    status, _, body = run.ask('/cancel', method='POST')
    if status == 202:
        say(f'run {run.id} cancelled')
        return 0
    if status == 409:
        say(f'run {run.id} answered 409 to its cancel: it is ending already')
        return 0
    say(f'run {run.id} answered {status} to its cancel, and goes on: {body}')
    return 1


def start(run):
    """the first look, said here, then the watch in a process of its own, which outlives this step"""
    log = os.path.join(need('RUNNER_TEMP'), 'fail-fast.log')
    try:
        status, etag, count = run.look(None)
    except (urllib.error.URLError, OSError, ValueError) as error:
        status, etag, count = f'no answer ({error})', None, 0
    if status != 200:
        print(f'::warning title=fail-fast::the artifacts of run {run.id} answered {status}: the watch looks again')
    elif count:
        print(f'{run.mark} is there already: a job of run {run.id} has failed')
    env = dict(os.environ, SNX_FAIL_FAST_ETAG=etag or '')
    command = [sys.executable, os.path.abspath(__file__), 'watch']
    with open(log, 'ab') as out:
        if os.name == 'nt':
            flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
            try:  # out of the step's job object where it lets a process leave, so the step's end leaves it running
                child = subprocess.Popen(
                    command, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=out,
                    creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB,
                )  # fmt: skip
            except OSError:
                child = subprocess.Popen(
                    command, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=out, creationflags=flags
                )
        else:
            child = subprocess.Popen(
                command, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=out, start_new_session=True
            )
    print(
        f'fail-fast: watching run {run.id} for {run.mark} every {run.poll:g} s, the jobs SNX_OPTIONAL_JOBS matches'
        f' left out: process {child.pid}, its log {log}'
    )
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('step', choices=['start', 'watch'])
    args = parser.parse_args()
    run = Run()
    if args.step == 'start':
        return start(run)
    return watch(run, os.environ.get('SNX_FAIL_FAST_ETAG') or None)


if __name__ == '__main__':
    sys.exit(main())
