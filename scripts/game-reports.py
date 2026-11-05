#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Game reports: the issues a player attached a ScarletNX log to, each summarized by the runtime's log summary
(src/common/utilities/logscan.c, as snx-logscan) in a comment of its own and grouped with the reports that failed the
same way.

    scripts/game-reports.py triage (--event FILE | --issue N) [--repo OWNER/NAME] [--dry-run] [--logscan PROGRAM]
    scripts/game-reports.py index [--repo OWNER/NAME] [--title-id ID] [--signature TEXT] [--outcome KIND]

triage (.github/workflows/game-reports.yml, FILE its event's JSON) reads the issue and its comments, and does nothing
when none of them has a log, attached (a .log, .txt, .gz or .zip on GitHub's attachment host) or pasted (a fenced
block the summary finds a log's header or failure marker in). Otherwise it scans the newest MAX_LOGS and writes their summary in
a comment of its own, edited rather than repeated as logs are added. The issue gets the `game report` label and a
`log: <how it ended>` label for each log's outcome, and the comment names the other game reports with the same
signature. An attachment that isn't a ScarletNX log is named as such. A maintainer's `status: confirmed` label on a
report gets a comment of its own, once: ready for an edenprobe case, and the MCP tools that write one.

PROGRAM is snx-logscan, or scarletnx with --scan-log after it (SNX_LOGSCAN, else snx-logscan on PATH): the workflow
builds it from its sources alone. --issue N triages that issue as an event for it would, and fails on a number that
isn't an issue's, a pull request or an issue without a log: the workflow's run by hand for one issue. --dry-run reads
GitHub as triage does but writes nothing: the comment and the label changes are printed instead.

index writes each game report as a JSON line: its number, title, state and URL, and each log's name, URL, signature,
outcome, title ID and build, as the summary comments record them (a JSON comment at their end). The filters keep the
reports with a log that matches.

GitHub is reached through gh (GH_TOKEN). Nothing of an issue reaches a shell: the event is read from its file, and
what is posted is the summary's escaped markdown. Downloads are limited to GitHub's attachment URLs and MAX_BYTES.
"""

import argparse
import gzip
import io
import json
import os
import re
import shlex
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile

LABEL = 'game report'
OUTCOME_PREFIX = 'log: '
LABEL_COLOR = 'c5def5'
OUTCOME_COLOR = 'ededed'
MARK = '<!-- game-reports:summary -->'
# a maintainer's mark that a report's failure is understood, and the comment it gets once: ready for an edenprobe
# case
CONFIRMED = 'status: confirmed'
CONFIRMED_COLOR = '0e8a16'
CONFIRMED_MARK = '<!-- game-reports:confirmed -->'
DATA = re.compile(r'<!-- game-reports (\{.*?\}) -->', re.DOTALL)
BOTS = ('github-actions[bot]', 'github-actions')
MAX_LOGS = 3
# triage's answer for an issue none of whose places holds a log
NO_LOG = 'no log attached'
MAX_BYTES = 64 << 20
MAX_SAME = 10
COMMENT_LIMIT = 65000  # GitHub refuses a comment over 65536 characters

# GitHub's attachment URLs: user-attachments/files/<id>/<name>, and the older <owner>/<repo>/files/<id>/<name>
ATTACHMENT = re.compile(
    r'https://github\.com/(?:user-attachments/files|[\w.-]+/[\w.-]+/files)/\d+/[^\s)\]>"\'<]+\.(?:log|txt|gz|zip)\b',
    re.IGNORECASE,
)


class GitHub:
    """The REST and GraphQL calls triage and index make, through gh"""

    def __init__(self, repo):
        self.repo = repo

    def api(self, path, method='GET', data=None, paginate=False):
        args = ['gh', 'api', '-H', 'Accept: application/vnd.github+json', '-X', method, path]
        if paginate:
            args += ['--paginate', '--slurp']
        if data is not None:
            args += ['--input', '-']
        result = subprocess.run(
            args, input=json.dumps(data) if data is not None else None, capture_output=True, text=True, check=False
        )
        if result.returncode:
            raise RuntimeError(f'gh api {method} {path}: {result.stderr.strip() or result.stdout.strip()}')
        value = json.loads(result.stdout) if result.stdout.strip() else None
        return [item for page in value for item in page] if paginate else value

    def issue(self, number):
        return self.api(f'repos/{self.repo}/issues/{number}')

    def comments(self, number):
        return self.api(f'repos/{self.repo}/issues/{number}/comments?per_page=100', paginate=True)

    def comment(self, number, body, existing=None):
        if existing:
            self.api(f'repos/{self.repo}/issues/comments/{existing}', 'PATCH', {'body': body})
        else:
            self.api(f'repos/{self.repo}/issues/{number}/comments', 'POST', {'body': body})

    def label(self, name, color, description):
        try:
            self.api(f'repos/{self.repo}/labels', 'POST', {'name': name, 'color': color, 'description': description})
        except RuntimeError as error:
            if 'already_exists' not in str(error):
                raise

    def set_labels(self, number, add, remove):
        if add:
            self.api(f'repos/{self.repo}/issues/{number}/labels', 'POST', {'labels': sorted(add)})
        for name in sorted(remove):
            self.api(f'repos/{self.repo}/issues/{number}/labels/{urllib.parse.quote(name, safe="")}', 'DELETE')

    def reports(self):
        """Every issue with the game report label, open or closed, with the bots' comments"""
        owner, name = self.repo.split('/', 1)
        query = (
            'query($owner: String!, $name: String!, $after: String) { repository(owner: $owner, name: $name) {'
            f' issues(first: 50, after: $after, labels: ["{LABEL}"], states: [OPEN, CLOSED]) {{'
            ' pageInfo { hasNextPage endCursor }'
            ' nodes { number title state url comments(first: 50) { nodes { author { login } body } } } } } }'
        )
        after, out = None, []
        while True:
            args = ['graphql', '-f', f'query={query}', '-f', f'owner={owner}', '-f', f'name={name}']
            if after:
                args += ['-f', f'after={after}']
            page = self.graphql(args)['data']['repository']['issues']
            out += page['nodes']
            if not page['pageInfo']['hasNextPage']:
                return out
            after = page['pageInfo']['endCursor']

    def graphql(self, args):
        result = subprocess.run(['gh', 'api', *args], capture_output=True, text=True, check=False)
        if result.returncode:
            raise RuntimeError(f'gh api graphql: {result.stderr.strip()}')
        return json.loads(result.stdout)


class DryRun(GitHub):
    """GitHub's reads, and its writes printed"""

    def comment(self, number, body, existing=None):
        print(f'--- {"edit comment " + str(existing) if existing else "new comment"} on #{number}\n{body}---')

    def label(self, name, color, description):
        pass

    def set_labels(self, number, add, remove):
        print(f'labels of #{number}: add {sorted(add)}, remove {sorted(remove)}')


def download(url):
    """The attachment's bytes, MAX_BYTES at most (GitHub's host redirects to its storage, which urllib follows)"""
    request = urllib.request.Request(url, headers={'User-Agent': 'scarletnx-game-reports'})
    with urllib.request.urlopen(request, timeout=60) as reply:
        data = reply.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f'over {MAX_BYTES >> 20} MB')
    return data


def unpack(name, data):
    """The log's text from the attachment: a .gz uncompressed, from a .zip its largest .log (else .txt), MAX_BYTES of
    either at most"""
    if data[:2] == b'\x1f\x8b':
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as f:
            data = f.read(MAX_BYTES + 1)
    elif data[:4] == b'PK\x03\x04':
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            members = [m for m in z.infolist() if not m.is_dir() and m.filename.lower().endswith(('.log', '.txt'))]
            members.sort(key=lambda m: (not m.filename.lower().endswith('.log'), -m.file_size))
            if not members:
                raise ValueError('no .log or .txt in the zip')
            with z.open(members[0]) as f:
                data = f.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f'over {MAX_BYTES >> 20} MB uncompressed')
    return data.decode('utf-8', 'replace')


class LogScan:
    """The log summary through its program: the JSON and the markdown of a log's text"""

    def __init__(self, program):
        self.program = program

    def run(self, text, *args):
        result = subprocess.run(
            [*self.program, '-', *args], input=text.encode('utf-8'), capture_output=True, timeout=60, check=False
        )
        if result.returncode:
            raise ValueError(f'the log summary failed: {result.stderr.decode("utf-8", "replace").strip()}')
        return result.stdout.decode('utf-8', 'replace')

    def scan(self, text):
        return json.loads(self.run(text))

    def markdown(self, text, name):
        return self.run(text, '--format', 'markdown', '--name', name)


def is_scarletnx_log(r):
    return bool(r['title'] or r['build'] or r['failure'] or r['launcher'])


# The comment's own words around the summaries: a name or an error is the issue's text, a code span as the summary's
# values are (GitHub links an issue reference or www. even escaped, nothing in a code span)
def code(value):
    return '`' + str(value).replace('`', "'").replace('\n', ' ') + '`'


# a fenced block in an issue or comment holding a log's lines, pasted rather than attached
FENCE = re.compile(r'^ {0,3}(`{3,}|~{3,})[^\n]*\n(.*?)^ {0,3}\1[`~]*[ \t]*$', re.MULTILINE | re.DOTALL)


def sources(issue, comments):
    """What may be a log in the issue and its comments, in order, each once: (name, url, attachment URL or None, the
    pasted block or None). A pasted block is a log when the summary finds a header or a failure in it"""
    places = [('the issue', issue.get('html_url'), issue.get('body'))]
    places += [(f'a comment by {c["user"]["login"]}', c.get('html_url'), c.get('body')) for c in comments]
    out, seen = [], set()
    for where, link, body in places:
        for url in ATTACHMENT.findall(body or ''):
            if url not in seen:
                seen.add(url)
                out.append((attachment_name(url), url, url, None))
        for m in FENCE.finditer(body or ''):
            out.append((f'pasted in {where}', link, None, m.group(2)))
    return out


def attachments(texts):
    """The attachment URLs in the texts, in order, each once"""
    return [url for _, _, url, _ in sources({'body': '\n'.join(t or '' for t in texts)}, []) if url]


def attachment_name(url):
    return urllib.parse.unquote(url.rsplit('/', 1)[-1])


def summary_data(body):
    m = DATA.search(body or '')
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except json.JSONDecodeError:
        return None


def index(github):
    """Each game report and its logs as its summary comment records them"""
    out = []
    for issue in github.reports():
        logs = []
        for c in issue['comments']['nodes']:
            if (c.get('author') or {}).get('login') in BOTS and MARK in c['body']:
                data = summary_data(c['body'])
                logs = data['logs'] if data else []
        out.append(
            {
                'number': issue['number'],
                'title': issue['title'],
                'state': issue['state'].lower(),
                'url': issue['url'],
                'logs': logs,
            }
        )
    return out


def same_signature(reports, number, signature):
    return [
        r for r in reports if r['number'] != number and any(log.get('signature') == signature for log in r['logs'])
    ][:MAX_SAME]


def comment_body(entries, reports, number):
    out = [
        MARK,
        '## Log summary',
        '',
        (
            "Written by ScarletNX's log summary from the logs attached here, and updated when another is attached. "
            "Paths are shortened to `~` and the crash report's code and memory dumps are left out."
        ),
    ]
    for e in entries:
        out += ['', f'### {code(e["name"])}', '']
        if e.get('error'):
            out.append(f'Could not be read: {code(e["error"])}')
            continue
        if not e['report']:
            out.append(
                "This doesn't look like a ScarletNX log. The game's log is `<title>.log` in the game's folder, "
                "and the launcher's Share log writes one with the last run's output."
            )
            continue
        out.append(re.sub(r'^###', '####', e['markdown'], count=1).rstrip())
        if e['report']['signature']:
            same = same_signature(reports, number, e['report']['signature'])
            if same:
                refs = ', '.join(f'#{r["number"]}' + (' (closed)' if r['state'] == 'closed' else '') for r in same)
                out += ['', f'**The same signature:** {refs}']
    data = {
        'schema': 1,
        'logs': [
            {
                'name': e['name'],
                'url': e['url'],
                'signature': e['report']['signature'],
                'outcome': e['report']['outcome'],
                'title': e['report']['title'],
                'title_id': e['report']['title_id'],
                'game_version': e['report']['game_version'],
                'commit': e['report']['commit'],
            }
            for e in entries
            if e.get('report')
        ],
    }
    # "--" ends an HTML comment: inside the JSON's strings it is written escaped
    out += ['', '<!-- game-reports ' + json.dumps(data, ensure_ascii=False).replace('--', '-\\u002d') + ' -->']
    body = '\n'.join(out) + '\n'
    if len(body) > COMMENT_LIMIT:
        # the folded parts of each summary go first: the header lines and the signatures stay
        body = re.sub(r'\n<details>.*?</details>', '', body, flags=re.DOTALL)
    return body


def confirmed_body(issue, summary):
    signatures = sorted({log['signature'] for log in (summary or {}).get('logs', []) if log.get('signature')})
    out = [
        CONFIRMED_MARK,
        '## Confirmed: ready for an edenprobe case',
        '',
        (
            'A maintainer confirmed this report. The next step is a case in '
            '[edenprobe](https://github.com/scarletnx/edenprobe) that asks the console the one question this failure '
            'comes down to, so that every ScarletNX build is checked against it.'
        ),
    ]
    if signatures:
        out += ['', 'Signature: ' + ', '.join(code(sig) for sig in signatures)]
    out += [
        '',
        "With an agent and ScarletNX's MCP server (`scarletnx --mcp`, `.mcp.json` in a checkout):",
        '',
        f'1. `edenprobe_scaffold` writes the case into a local edenprobe checkout, with `issue` {issue}.',
        (
            "2. The agent writes the service call, the judge and where the expected answer comes from (Eden's source, "
            'or a console).'
        ),
        (
            '3. `edenprobe_check` runs the judges on the host. `edenprobe_run` runs the built NRO on ScarletNX, where '
            'the case should fail.'
        ),
        "4. `edenprobe_finding` writes the edenprobe pull request's text.",
        '',
        'Nothing of the game goes into the case: no code, disassembly, assets or values only that game uses.',
    ]
    return '\n'.join(out) + '\n'


def confirm(github, number):
    """The confirmed label's comment, once"""
    comments = github.comments(number)
    bots = [c for c in comments if (c.get('user') or {}).get('login') in BOTS]
    if any(CONFIRMED_MARK in (c.get('body') or '') for c in bots):
        return f'#{number}: confirmed, its comment posted already'
    summary = next((summary_data(c['body']) for c in bots if MARK in (c.get('body') or '')), None)
    github.comment(number, confirmed_body(number, summary))
    return f'#{number}: confirmed, ready for an edenprobe case'


def triage(github, event, logscan, fetch=download):
    issue = event.get('issue') or {}
    if 'pull_request' in issue or not issue.get('number'):
        return 'not an issue'
    number = issue['number']
    if event.get('action') in ('labeled', 'unlabeled'):
        # a label changes no log: only the confirmed one does something
        if event.get('action') == 'labeled' and (event.get('label') or {}).get('name') == CONFIRMED:
            return confirm(github, number)
        return 'a label'
    issue = github.issue(number)
    comments = github.comments(number)
    users = [c for c in comments if (c.get('user') or {}).get('login') not in BOTS]
    found = []
    for name, link, url, pasted in sources(issue, users):
        if pasted is not None:
            r = logscan.scan(pasted)
            if not is_scarletnx_log(r):
                continue
        found.append((name, link, url, pasted))
    if not found:
        return NO_LOG
    entries = []
    for name, link, url, pasted in found[-MAX_LOGS:]:
        entry = {'name': name, 'url': link, 'report': None}
        try:
            log = pasted if pasted is not None else unpack(name, fetch(url))
            r = logscan.scan(log)
            if is_scarletnx_log(r):
                entry['report'], entry['markdown'] = r, logscan.markdown(log, name)
        except (OSError, ValueError, zipfile.BadZipFile, EOFError) as error:
            entry['error'] = str(error)
        entries.append(entry)
    body = comment_body(entries, index(github), number)
    existing = next(
        (c['id'] for c in comments if (c.get('user') or {}).get('login') in BOTS and MARK in (c.get('body') or '')),
        None,
    )
    github.comment(number, body, existing)
    have = {label['name'] for label in issue.get('labels', [])}
    outcomes = {OUTCOME_PREFIX + e['report']['outcome'] for e in entries if e['report']}
    if not outcomes:
        return f'#{number}: no ScarletNX log in {len(entries)} attachment(s)'
    want = {LABEL} | outcomes
    github.label(LABEL, LABEL_COLOR, 'An issue with a ScarletNX log attached (scripts/game-reports.py)')
    github.label(CONFIRMED, CONFIRMED_COLOR, 'A maintainer understands the failure: ready for an edenprobe case')
    for name in outcomes - have:
        github.label(name, OUTCOME_COLOR, 'How the attached log says the run ended')
    stale = {name for name in have if name.startswith(OUTCOME_PREFIX)} - outcomes
    github.set_labels(number, want - have, stale)
    return f'#{number}: {len(entries)} log(s), {", ".join(sorted(outcomes))}'


def issue_number(text):
    """--issue's value: an issue's number, a whole number from 1 as GitHub writes it"""
    if not re.fullmatch(r'[1-9][0-9]*', text):
        raise argparse.ArgumentTypeError(f'{text!r} is not an issue number (a whole number from 1)')
    return int(text)


def triage_issue(github, number, logscan, fetch=download):
    """--issue N: the issue as an event for it would triage it. Asked for by its number, an issue that can't be read, a
    pull request or an issue without a log is refused rather than left alone"""
    try:
        issue = github.issue(number)
    except RuntimeError as error:
        raise SystemExit(f'game-reports: issue #{number} cannot be read: {error}') from None
    if 'pull_request' in issue:
        raise SystemExit(f'game-reports: #{number} is a pull request, and its comments are no game reports')
    result = triage(github, {'issue': issue}, logscan, fetch)
    if result == NO_LOG:
        raise SystemExit(f'game-reports: issue #{number} has no log attached or pasted, so nothing to summarize')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='command', required=True)
    t = sub.add_parser('triage')
    which = t.add_mutually_exclusive_group(required=True)
    which.add_argument('--event')
    which.add_argument('--issue', type=issue_number)
    t.add_argument('--dry-run', action='store_true')
    t.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    t.add_argument('--logscan', default=os.environ.get('SNX_LOGSCAN', 'snx-logscan'))
    i = sub.add_parser('index')
    i.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', 'scarletnx/ScarletNX'))
    i.add_argument('--title-id')
    i.add_argument('--signature')
    i.add_argument('--outcome')
    args = ap.parse_args()
    if not args.repo:
        sys.exit('game-reports: no --repo and no GITHUB_REPOSITORY')
    github = GitHub(args.repo)
    if args.command == 'triage':
        if args.dry_run:
            github = DryRun(args.repo)
        logscan = LogScan(shlex.split(args.logscan))
        if args.issue:
            print(triage_issue(github, args.issue, logscan))
            return
        with open(args.event, encoding='utf-8') as f:
            event = json.load(f)
        print(triage(github, event, logscan))
        return
    for report in index(github):
        logs = [
            log
            for log in report['logs']
            if (not args.title_id or (log.get('title_id') or '').upper() == args.title_id.upper())
            and (not args.signature or log.get('signature') == args.signature)
            and (not args.outcome or log.get('outcome') == args.outcome)
        ]
        if logs or not (args.title_id or args.signature or args.outcome):
            print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
