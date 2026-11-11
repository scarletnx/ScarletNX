#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/fail-fast.py against a fake GitHub: the mark looked for by its ETag (a look that finds nothing new is a 304),
the run cancelled only once a job that may not fail has ended failed, a cancel refused or the run ending already said
with the answer, the settings that are wrong named with their values, and start's watch left running on its own. Then
build.yml held to it: every job but Nightly due marks its failure or may fail, the ones that may fail are the ones
SNX_OPTIONAL_JOBS matches, and none of those marks. Offline."""

import http.server
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'fail-fast.py'
WORKFLOW = ROOT / '.github' / 'workflows' / 'build.yml'
RUN = '/repos/o/r/actions/runs/7'
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


def job(name, status='completed', conclusion='success'):
    return {'name': name, 'status': status, 'conclusion': conclusion if status == 'completed' else None}


class GitHub(http.server.BaseHTTPRequestHandler):
    """the run's artifacts, jobs and cancel, from STATE; every request kept in STATE['asked']"""

    def log_message(self, *args):
        pass

    def answer(self, status, body=None, etag=None):
        data = json.dumps(body).encode() if body is not None else b''
        self.send_response(status)
        if etag:
            self.send_header('ETag', etag)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        s = STATE
        s['asked'].append(('GET', self.path, self.headers.get('If-None-Match'), self.headers.get('Authorization')))
        if self.path == f'{RUN}/artifacts?name=fail-fast-2':
            s['looks'] += 1
            if s['broken']:
                s['broken'] -= 1
                return self.answer(500, {'message': 'Server Error'})
            if s['looks'] > s['mark_after']:
                s['marked'] = True
            etag = '"marked"' if s['marked'] else '"empty"'
            if self.headers.get('If-None-Match') == etag:
                return self.answer(304, etag=etag)
            return self.answer(200, {'total_count': int(s['marked']), 'artifacts': []}, etag)
        if self.path == f'{RUN}/jobs?filter=latest&per_page=100':
            s['reads'] += 1
            jobs = s['jobs'][min(s['reads'], len(s['jobs'])) - 1]
            return self.answer(200, {'total_count': len(jobs), 'jobs': jobs})
        self.answer(404, {'message': 'Not Found'})

    def do_POST(self):
        STATE['asked'].append(('POST', self.path, None, self.headers.get('Authorization')))
        STATE['cancels'] += 1
        self.answer(STATE['cancel'], {'message': 'refused'} if STATE['cancel'] >= 400 else {})


STATE = {}


def reset(**given):
    STATE.clear()
    STATE.update(asked=[], looks=0, reads=0, cancels=0, marked=False, mark_after=0, broken=0, cancel=202, jobs=[[]])
    STATE.update(given)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), GitHub)
threading.Thread(target=server.serve_forever, daemon=True).start()
OPTIONAL = r'^(macOS and iOS \(arm64\)|Linux AppImage \((ppc64le|ppc64|riscv64|loongarch64)\)(, as installed)?)$'
ENV = dict(
    os.environ,
    GITHUB_API_URL=f'http://127.0.0.1:{server.server_address[1]}',
    GITHUB_REPOSITORY='o/r',
    GITHUB_RUN_ID='7',
    GITHUB_RUN_ATTEMPT='2',
    GH_TOKEN='run-token',
    SNX_OPTIONAL_JOBS=OPTIONAL,
    SNX_FAIL_FAST_POLL='0.05',
)
ENV.pop('SNX_FAIL_FAST_ETAG', None)


def fail_fast(step='watch', timeout=20, **env):
    given = dict(ENV, **env)
    for key, value in env.items():
        if value is None:
            given.pop(key)
    return subprocess.run(
        [sys.executable, str(SCRIPT), step], env=given, capture_output=True, text=True, timeout=timeout, check=False
    )


def gone(pid):
    """pid ended: no such process, or a zombie, which an orphan stays where nothing reaps it (a CI container's first
    process is tail -f /dev/null, no init)"""
    try:
        os.kill(pid, 0)
    except OSError:
        return True
    try:
        with open(f'/proc/{pid}/stat') as stat:
            return stat.read().rsplit(')', 1)[1].split()[0] == 'Z'
    except OSError:
        return False


def stop(pid):
    """a watch start left running, ended and gone before the next look of the fake GitHub"""
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return
    for _ in range(100):
        if gone(pid):
            return
        time.sleep(0.05)
    failures.append(f'the watch {pid} outlived SIGTERM by 5 s')


RUNNING = [job('Linux (aarch64)', 'in_progress'), job('macOS and iOS (arm64)', conclusion='failure')]
FAILED = [job('Linux (aarch64)', conclusion='failure'), job('macOS and iOS (arm64)', conclusion='failure')]

# ---- a job fails: the mark found on the fourth look, the job's end on the third read, then the cancel
reset(mark_after=3, jobs=[RUNNING, RUNNING, FAILED])
done = fail_fast()
looks = [a for a in STATE['asked'] if 'artifacts' in a[1]]
check('the run cancelled once its failed job ended', done.returncode == 0 and STATE['cancels'] == 1, done.stdout)
check(
    'the artifacts asked for with their ETag after the first look, each look with nothing new a 304',
    looks[0][2] is None and all(a[2] == '"empty"' for a in looks[1:]) and len(looks) == 4,
    looks,
)
check(
    'the jobs read only once the mark was there, the cancel after the read that has the failure ended',
    STATE['reads'] == 3 and STATE['asked'][-1][:2] == ('POST', f'{RUN}/cancel'),
    STATE['asked'],
)
check('every request with the token', all(a[3] == 'Bearer run-token' for a in STATE['asked']), STATE['asked'])
check(
    'the failed job named, the optional one not',
    'Linux (aarch64) failed: cancelling the rest of run 7' in done.stdout and 'macOS' not in done.stdout,
    done.stdout,
)

# ---- an optional job's failure alone cancels nothing
reset(mark_after=0, jobs=[RUNNING])
try:
    done = fail_fast(timeout=1.5)
    check('a watch with only an optional failure keeps watching', False, done.stdout)
except subprocess.TimeoutExpired:
    pass
check('no cancel for an optional job', STATE['cancels'] == 0 and STATE['reads'] >= 2, STATE)

# ---- the cancel refused (a fork's token), or the run ending already
reset(jobs=[FAILED], cancel=403)
done = fail_fast()
check(
    'a refused cancel fails the watch, saying the answer',
    done.returncode == 1 and 'run 7 answered 403 to its cancel, and goes on' in done.stdout,
    done.stdout,
)
reset(jobs=[FAILED], cancel=409)
done = fail_fast()
check('a run ending already', done.returncode == 0 and 'it is ending already' in done.stdout, done.stdout)

# ---- the artifacts not answering: said, and looked for again
reset(broken=2, mark_after=3, jobs=[FAILED])
done = fail_fast()
check(
    'an error answer said with its status, the watch going on',
    done.returncode == 0 and done.stdout.count('the artifacts of run 7 answered 500') == 2 and STATE['cancels'] == 1,
    done.stdout,
)

# ---- the settings
for env, said in [
    ({'SNX_FAIL_FAST_POLL': 'soon'}, 'SNX_FAIL_FAST_POLL=soon is not a number of seconds'),
    ({'SNX_FAIL_FAST_POLL': '0'}, 'SNX_FAIL_FAST_POLL=0 is not a number of seconds over 0'),
    ({'SNX_OPTIONAL_JOBS': '(macOS'}, 'SNX_OPTIONAL_JOBS=(macOS is no regex'),
    ({'SNX_OPTIONAL_JOBS': None}, 'SNX_OPTIONAL_JOBS is not set'),
    ({'GH_TOKEN': None}, 'GH_TOKEN is not set'),
]:
    reset()
    done = fail_fast(**env)
    check(f'{env} refused, named', done.returncode != 0 and said in done.stderr and not STATE['asked'], done.stderr)

# ---- start: the first look said, the watch on its own once start has returned
with tempfile.TemporaryDirectory() as temporary:
    reset(mark_after=10**9)
    done = fail_fast('start', RUNNER_TEMP=temporary)
    pid = re.search(r'process (\d+), its log (\S+)', done.stdout)
    check('start says what it watches', done.returncode == 0 and pid and 'fail-fast-2 every 0.05 s' in done.stdout)
    if pid:
        time.sleep(0.5)
        try:
            os.kill(int(pid.group(1)), 0)
            alive = True
        except OSError:
            alive = False
        check('the watch outlives start', alive)
        check(
            "the watch starts from start's ETag: no second full answer",
            [a[2] for a in STATE['asked'] if 'artifacts' in a[1]][1:2] == ['"empty"'],
            STATE['asked'][:3],
        )
        if alive:
            stop(int(pid.group(1)))
        check('its log where start said', Path(pid.group(2)) == Path(temporary) / 'fail-fast.log')
    reset(broken=1, mark_after=10**9)
    done = fail_fast('start', RUNNER_TEMP=temporary)
    check(
        'a first look that fails is a warning, and the watch starts',
        '::warning title=fail-fast::the artifacts of run 7 answered 500' in done.stdout and 'process ' in done.stdout,
        done.stdout,
    )
    pid = re.search(r'process (\d+)', done.stdout)
    if pid:
        stop(int(pid.group(1)))
server.shutdown()

# ---- build.yml: who marks a failure, who may fail
text = WORKFLOW.read_text()
optional = re.search(r"^  SNX_OPTIONAL_JOBS: '(.*)'$", text, re.M)
check("build.yml's env has SNX_OPTIONAL_JOBS", optional)
optional = re.compile(optional.group(1) if optional else OPTIONAL)
check("build.yml's SNX_OPTIONAL_JOBS is this test's", optional.pattern == OPTIONAL, optional.pattern)
body = text[text.index('\njobs:\n') :]
blocks = re.split(r'\n  (?=[a-z][\w-]*:\n)', body)[1:]
ARCHES = ('x86_64', 'aarch64', 'ppc64le', 'ppc64', 'riscv64', 'loongarch64')
seen = 0
for block in blocks:
    name_id = block.split(':', 1)[0]
    name = re.search(r'^    name: (.+)$', block, re.M).group(1)
    arches = [a for a in ARCHES if re.search(rf'["\[ ]{a}["\],]', block)] if '${{ matrix.arch }}' in name else ['']
    names = [name.replace('${{ matrix.arch }}', a) for a in arches]
    may_fail = re.search(r'^    continue-on-error: true$', block, re.M) is not None
    marks = '*fail-fast-mark' in block or '&fail-fast-mark' in block
    seen += 1
    if name_id == 'nightly-due':
        check('Nightly due marks nothing: it cancels its run by design', not marks)
        continue
    if may_fail:
        check(f'{name_id}, which may fail, marks nothing', not marks)
        check(f'{name_id}: SNX_OPTIONAL_JOBS matches {names}', all(optional.search(n) for n in names), names)
    else:
        check(f'{name_id}, which may not fail, marks its failure', marks)
        file_at = max(block.find('*fail-fast-file'), block.find('&fail-fast-file'))
        mark_at = max(block.find('*fail-fast-mark'), block.find('&fail-fast-mark'))
        check(f"{name_id} writes the mark's file before it uploads it", 0 <= file_at < mark_at, (file_at, mark_at))
        check(f'{name_id}: SNX_OPTIONAL_JOBS leaves out {names}', not any(optional.search(n) for n in names), names)
check("build.yml's jobs read: fewer means this test no longer reads the file as it is", seen >= 14, seen)
check('no job dispatches a workflow to cancel its run', 'cancel-run.yml' not in text)
# upload-artifact refuses a path with a . or .. in it ("Relative pathing '.' and '..' is not allowed"): the mark of
# 2c12f69 named ../../_temp/_github_workflow/event.json, and no failed job marked its run
lines = text.splitlines()
uploads = 0
for at, line in enumerate(lines):
    if 'uses: actions/upload-artifact@' not in line:
        continue
    uploads += 1
    # the step's keys sit where uses: is, past a "- " that opens the step on its line
    key = len(line) - len(line.lstrip()) + (2 if line.lstrip().startswith('- ') else 0)
    paths, inside = [], False
    for following in lines[at + 1 :]:
        indent = len(following) - len(following.lstrip())
        if following.strip() and indent < key:
            break
        value = following.strip()
        if value.startswith('path:'):
            inside = value == 'path: |'
            if not inside:
                paths.append(value[len('path:') :].strip())
        elif inside and value and not re.match(r'[\w-]+:', value):
            paths.append(value)
        elif inside:
            inside = False
    for path in paths:
        parts = path.lstrip('!').replace('\\', '/').split('/')
        check(f"upload-artifact's path {path} (line {at + 1}) has no . or .. in it", not {'.', '..'} & set(parts))
check("build.yml's uploads read", uploads >= 10, uploads)
mark_file = re.search(r'run: echo "\$GITHUB_JOB" > (\S+)', text)
check(
    'the mark uploads the file its step before writes',
    mark_file and f'          path: {mark_file.group(1)}\n          overwrite: true' in text,
    mark_file and mark_file.group(1),
)

for failure in failures:
    print('FAIL:', failure)
print('fail-fast:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
