#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/game-reports.py's triage and index against a GitHub kept in memory: an issue without a log left alone, one
with a log summarized in one comment that later attachments edit, the labels for each outcome (a stale one removed),
the other reports with the same signature named, a file that isn't a log said so, a .gz and a .zip unpacked, the
summary's data read back by index, and pull requests skipped. Offline.

    tests/game_reports_test.py PROGRAM [ARGS...]   (the log summary: build/snx-logscan)"""

import argparse
import gzip
import importlib.util
import io
import json
import shlex
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('game_reports', ROOT / 'scripts' / 'game-reports.py')
reports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reports)

BOT = 'github-actions[bot]'
SCAN = reports.LogScan(sys.argv[1:])

SVC_BREAK = """[   0.00] ScarletNX - Some Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   0.30] game: 0100aaaabbbb0000, version 1.2.3
[   2.25]
*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.
"""

CRASH = """[   0.00] ScarletNX - Other Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   3.00]
*** Crash (signal 11) at host pc=0x1, access to 0x10 ***
[   3.00]   invalid memory access
[   3.00]   in the runtime itself (exe offset +1a2b3c)
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""


def url(n, name):
    return f'https://github.com/user-attachments/files/{n}/{name}'


class FakeGitHub:
    def __init__(self):
        self.issues = {}
        self.comment_list = {}
        self.labels = set()
        self.next = 100
        self.calls = []

    def add_issue(self, number, body, labels=(), pr=False, state='open'):
        self.issues[number] = {
            'number': number,
            'title': f'issue {number}',
            'body': body,
            'state': state,
            'labels': [{'name': n} for n in labels],
            'html_url': f'https://github.com/o/r/issues/{number}',
        }
        if pr:
            self.issues[number]['pull_request'] = {}
        self.comment_list[number] = []

    def add_comment(self, number, body, user='player'):
        self.next += 1
        self.comment_list[number].append({'id': self.next, 'body': body, 'user': {'login': user}})

    def issue(self, number):
        if number not in self.issues:
            raise RuntimeError(f'gh api GET repos/o/r/issues/{number}: Not Found (HTTP 404)')
        return json.loads(json.dumps(self.issues[number]))

    def comments(self, number):
        return json.loads(json.dumps(self.comment_list[number]))

    def comment(self, number, body, existing=None):
        self.calls.append(('comment', number, existing))
        if existing:
            next(c for c in self.comment_list[number] if c['id'] == existing)['body'] = body
        else:
            self.add_comment(number, body, BOT)

    def label(self, name, color, description):
        self.labels.add(name)

    def set_labels(self, number, add, remove):
        self.calls.append(('labels', number, sorted(add), sorted(remove)))
        names = {lb['name'] for lb in self.issues[number]['labels']}
        self.issues[number]['labels'] = [{'name': n} for n in sorted((names | set(add)) - set(remove))]

    def reports(self):
        return [
            {
                'number': i['number'],
                'title': i['title'],
                'state': i['state'].upper(),
                'url': i['html_url'],
                'comments': {
                    'nodes': [
                        {
                            'author': {'login': 'github-actions' if c['user']['login'] == BOT else c['user']['login']},
                            'body': c['body'],
                        }
                        for c in self.comment_list[i['number']]
                    ]
                },
            }
            for i in self.issues.values()
            if any(lb['name'] == reports.LABEL for lb in i['labels'])
        ]


def zipped(name, text):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as z:
        z.writestr('readme.md', 'not this')
        z.writestr(name, text)
    return out.getvalue()


FILES = {
    url(1, 'Some%20Game.log'): SVC_BREAK.encode(),
    url(2, 'other.log.gz'): gzip.compress(CRASH.encode()),
    url(3, 'notes.txt'): b'my computer specs: lots of RAM\n',
    url(4, 'logs.zip'): zipped('Some Game.log', SVC_BREAK),
}


def fetch(u):
    return FILES[u]


def check(cond, what):
    if not cond:
        sys.exit(f'game_reports_test: {what}')


def event(number):
    return {'issue': {'number': number}}


def refusal(call, *args):
    """The message call(*args) exits with, None when it returns"""
    try:
        call(*args)
    except SystemExit as error:
        return str(error)
    return None


def bot_comments(gh, number):
    return [c for c in gh.comment_list[number] if c['user']['login'] == BOT]


def main():
    gh = FakeGitHub()
    gh.add_issue(1, 'The launcher has a typo')
    check(
        reports.triage(gh, event(1), SCAN, fetch) == 'no log attached' and not gh.calls,
        'an issue without a log is left alone',
    )

    gh.add_issue(2, f'It stops at the logos.\n\n[Some Game.log]({url(1, "Some%20Game.log")})', labels=['log: crash'])
    result = reports.triage(gh, event(2), SCAN, fetch)
    check('svcBreak' in result, result)
    (summary,) = bot_comments(gh, 2)
    check(summary['body'].startswith(reports.MARK), 'the summary is marked')
    check('svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv' in summary['body'], 'the signature is in the summary')
    check(
        {lb['name'] for lb in gh.issues[2]['labels']} == {'game report', 'log: svcBreak'},
        f'labels: {gh.issues[2]["labels"]} (the stale log: crash removed)',
    )
    check({'game report', 'log: svcBreak'} <= gh.labels, 'the labels are made')

    # a later comment attaches the same log zipped and a file that isn't a log: one comment, edited
    gh.add_comment(2, f'More: {url(4, "logs.zip")} and {url(3, "notes.txt")}')
    reports.triage(gh, event(2), SCAN, fetch)
    (summary,) = bot_comments(gh, 2)
    check(('comment', 2, summary['id']) in gh.calls, 'the summary is edited, not repeated')
    check("This doesn't look like a ScarletNX log" in summary['body'], 'a file that is not a log is said so')
    check(summary['body'].count('svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv') >= 2, 'the zip is unpacked')

    # another report of the same failure, and one of another, from a .gz
    gh.add_issue(3, f'Crashes too {url(1, "Some%20Game.log")}', state='closed')
    reports.triage(gh, event(3), SCAN, fetch)
    gh.add_issue(4, f'{url(2, "other.log.gz")}')
    reports.triage(gh, event(4), SCAN, fetch)
    (summary,) = bot_comments(gh, 3)
    check('**The same signature:** #2' in summary['body'], 'the open report with the same signature is named')
    (other,) = bot_comments(gh, 4)
    check('The same signature' not in other['body'], 'another failure names no other report')
    check('crash (invalid memory access) in the runtime 1234abc +1a2b3c' in other['body'], 'the .gz is read')
    gh.add_comment(2, 'bump')
    reports.triage(gh, event(2), SCAN, fetch)
    check('#3 (closed)' in bot_comments(gh, 2)[0]['body'], 'a closed report is named as closed')

    # index reads the summaries back
    index = {r['number']: r for r in reports.index(gh)}
    check(sorted(index) == [2, 3, 4], f'index: {sorted(index)}')
    check(index[4]['logs'][0]['signature'].startswith('crash (invalid memory access)'), 'index signature')
    check(index[2]['logs'][0]['title_id'] == '0100AAAABBBB0000', 'index title id')
    check(len(index[2]['logs']) == 2, f"index skips what isn't a log: {index[2]['logs']}")

    # an HTML comment ends at "--": the data survives a signature holding one
    body = reports.comment_body(
        [
            {
                'name': 'x.log',
                'url': 'u',
                'report': dict(SCAN.scan(CRASH), signature='a --> b'),
                'markdown': SCAN.markdown(CRASH, 'x.log'),
            }
        ],
        [],
        9,
    )
    data = body[body.index('<!-- game-reports {') :]
    check(data.count('-->') == 1 and reports.summary_data(body)['logs'][0]['signature'] == 'a --> b', 'the -- escape')

    # a log's lines pasted in a fenced block count, a template's placeholder doesn't
    gh.add_issue(6, 'Template:\n```\n*** Crash ... ***\n```\nMine:\n~~~text\n' + CRASH + '~~~\n')
    reports.triage(gh, event(6), SCAN, fetch)
    (pasted,) = bot_comments(gh, 6)
    check(pasted['body'].count('### `pasted in the issue`') == 1, 'one pasted log, the placeholder left out')
    check('**The same signature:** #4' in pasted['body'], 'a pasted log is grouped like an attached one')

    gh.add_issue(5, url(1, 'Some%20Game.log'), pr=True)
    check(reports.triage(gh, {'issue': gh.issue(5)}, SCAN, fetch) == 'not an issue', 'a pull request is skipped')
    check(
        reports.attachments(['see https://example.com/files/1/a.log and ' + url(7, 'b.txt')]) == [url(7, 'b.txt')],
        'only GitHub attachment URLs are fetched',
    )
    # a maintainer's confirmed label: one comment, ready for an edenprobe case, with the report's signature
    calls = len(gh.calls)
    check(
        reports.triage(
            gh, {'action': 'labeled', 'label': {'name': 'priority: low'}, 'issue': {'number': 2}}, SCAN, fetch
        )
        == 'a label'
        and len(gh.calls) == calls,
        'another label does nothing',
    )
    labeled = {'action': 'labeled', 'label': {'name': reports.CONFIRMED}, 'issue': {'number': 2}}
    check('ready for an edenprobe case' in reports.triage(gh, labeled, SCAN, fetch), 'the confirmed label')
    (confirmed,) = [c for c in bot_comments(gh, 2) if reports.CONFIRMED_MARK in c['body']]
    check(
        'svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv' in confirmed['body'], "the comment names the report's signature"
    )
    check('`edenprobe_scaffold`' in confirmed['body'] and 'with `issue` 2' in confirmed['body'], 'the MCP steps')
    check('posted already' in reports.triage(gh, labeled, SCAN, fetch), 'labeled again, no second comment')
    check(len([c for c in bot_comments(gh, 2) if reports.CONFIRMED_MARK in c['body']]) == 1, 'one confirmed comment')
    check(reports.CONFIRMED in gh.labels, 'the confirmed label is made for maintainers')

    # one issue asked for by its number (the workflow's run by hand): its number checked, then refused unless it is an
    # issue with a log
    check(reports.issue_number('1238') == 1238, 'an issue number')
    for text in ('abc', '0', '-3', '1.5', '', ' 7', '07', '1e3'):
        try:
            reports.issue_number(text)
            check(False, f'{text!r} taken as an issue number')
        except argparse.ArgumentTypeError as error:
            check(repr(text) in str(error), f'the refusal of {text!r} names it: {error}')
    cli = subprocess.run(
        [sys.executable, str(ROOT / 'scripts' / 'game-reports.py'), 'triage', '--issue', 'abc', '--repo', 'o/r'],
        capture_output=True,
        text=True,
        check=False,
    )
    check(cli.returncode == 2 and "'abc' is not an issue number" in cli.stderr, f'--issue abc: {cli.stderr}')
    for number, why in ((1, 'issue #1 has no log'), (5, '#5 is a pull request'), (99, 'issue #99 cannot be read')):
        calls = len(gh.calls)
        message = refusal(reports.triage_issue, gh, number, SCAN, fetch)
        check(message and why in message and len(gh.calls) == calls, f'#{number} refused with: {message}')
    gh.add_issue(7, f'Stops at the logos {url(1, "Some%20Game.log")}')
    check('svcBreak' in reports.triage_issue(gh, 7, SCAN, fetch), 'an issue with a log asked for by its number')
    check(len(bot_comments(gh, 7)) == 1, 'its summary posted once')
    # main's --issue, as the workflow runs it: a pasted log, so nothing is downloaded
    gh.add_issue(8, '~~~\n' + CRASH + '~~~\n')
    github, argv = reports.GitHub, sys.argv
    reports.GitHub = lambda repo: gh
    try:
        for number in (1, 8):
            sys.argv = ['game-reports.py', 'triage', '--issue', str(number), '--repo', 'o/r']
            sys.argv += ['--logscan', shlex.join(SCAN.program)]
            message = refusal(reports.main)
            check((message is None) == (number == 8), f'triage --issue {number} ends with: {message}')
    finally:
        reports.GitHub, sys.argv = github, argv
    check(len(bot_comments(gh, 8)) == 1, 'triage --issue 8 summarizes issue 8')
    print('game_reports_test: ok')


if __name__ == '__main__':
    main()
