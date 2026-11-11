# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A fake gh (a small GitHub kept in a JSON file: releases and tags, pull requests, issues and their comments, the
users' permissions, Build runs, their jobs and their cancel, files of a branch) and a fake Discord (a local HTTP server
keeping each webhook's messages), for tests/publish_builds_test.py and tests/cancel_pr_runs_test.py. Offline."""

import http.server
import json
import threading

FAKE_GH = r"""#!/usr/bin/env python3
import json, os, re, sys
path = os.environ['FAKE_GH_STATE']
with open(path) as f:
    s = json.load(f)
args = sys.argv[1:]
with open(os.environ['FAKE_GH_LOG'], 'a') as log:
    log.write(json.dumps({'args': args, 'token': os.environ.get('GH_TOKEN')}) + '\n')
def save():
    with open(path, 'w') as f:
        json.dump(s, f)
def out(value):
    print(json.dumps(value))
def stop(text):
    sys.stderr.write(text + '\n')
    sys.exit(1)
if args[:2] == ['release', 'upload']:
    end = args.index('--repo')
    repo = args[end + 1]
    release = next((r for r in s['releases'][repo] if r['tag_name'] == args[2]), None) or stop('release not found')
    for file in args[3:end]:
        name = os.path.basename(file)
        release['assets'] = [a for a in release['assets'] if a['name'] != name]
        s['next'] += 1
        with open(file, 'rb') as f:
            data = f.read()
        release['assets'].append({'id': s['next'], 'name': name, 'data': data[:64].decode('latin-1'), 'size': len(data),
                                  'browser_download_url': f'https://github.com/{repo}/releases/download/{args[2]}/{name}'})
    save()
    sys.exit(0)
assert args[0] == 'api', args
method = args[args.index('-X') + 1] if '-X' in args else 'GET'
target = [a for a in args[1:] if not a.startswith('-') and a not in ('GET', 'POST', 'PATCH', 'PUT', 'DELETE')][0]
if target == 'graphql':  # the links of a pull request or an issue, as GitHub's GraphQL gives them
    asked = next(a for a in args if a.startswith('query='))
    number = re.search(r'number:(\d+)', asked).group(1)
    field, refs = (('pullRequest', 'closingIssuesReferences') if 'pullRequest(' in asked
                   else ('issue', 'closedByPullRequestsReferences'))
    out({'data': {'repository': {field: {refs: {'nodes': s.get('links', {}).get(number, [])}}}}})
    sys.exit(0)
route, _, query = target.partition('?')
query = dict(p.split('=', 1) for p in query.split('&') if p)
data = json.load(sys.stdin) if '--input' in args else None
page, per = int(query.get('page', 1)), int(query.get('per_page', 30))
def listing(items):
    out(items[(page - 1) * per:page * per])
m = re.fullmatch(r'repos/([^/]+/[^/]+)(?:/(.*))?', route)
repo, rest = m.group(1), m.group(2) or ''
releases, tags = s['releases'].setdefault(repo, []), s['tags'].setdefault(repo, {})
if rest == 'releases' and method == 'GET':
    listing(sorted(releases, key=lambda r: r['created_at'], reverse=True))
elif rest == 'releases' and method == 'POST':
    s['next'] += 1
    s['clock'] += 1
    # created_at is the tag's commit's date, as GitHub has it: the nightly repository's releases all share one
    release = dict(data, id=s['next'], created_at='2026-10-01T00:00:00Z', published_at=f'2026-11-01T{s["clock"]:08d}', assets=[],
                   html_url=f'https://github.com/{repo}/releases/tag/{data["tag_name"]}')
    releases.append(release)
    tags.setdefault(data['tag_name'], data.get('target_commitish', 'default-branch'))
    save()
    out(release)
elif rest.startswith('releases/assets/') and method == 'DELETE':
    number = int(rest.split('/')[-1])
    for release in releases:
        release['assets'] = [a for a in release['assets'] if a['id'] != number]
    save()
elif rest.startswith('releases/') and method in ('PATCH', 'DELETE'):
    number = int(rest.split('/')[-1])
    release = next((r for r in releases if r['id'] == number), None) or stop('no such release')
    if method == 'DELETE':
        releases.remove(release)
    else:
        release.update(data)
        out(release)
    save()
elif rest.startswith('git/refs/tags/') and method in ('DELETE', 'PATCH'):
    tag = rest[len('git/refs/tags/'):]
    if tag not in tags:
        stop('gh: Reference does not exist (HTTP 422)')
    if method == 'DELETE':
        del tags[tag]
    else:
        tags[tag] = data['sha']
    save()
elif rest.startswith('git/ref/tags/'):
    tag = rest[len('git/ref/tags/'):]
    out({'ref': f'refs/tags/{tag}', 'object': {'sha': tags[tag]}}) if tag in tags else stop('gh: Not Found (HTTP 404)')
elif rest == 'pulls' and method == 'GET':
    listing([p for p in s['pulls'] if p['state'] == 'open'])
elif re.fullmatch(r'pulls/\d+/commits', rest):
    listing(s.get('commits', {}).get(rest.split('/')[1], []))
elif re.fullmatch(r'compare/[0-9a-f]+\.\.\.[0-9a-f]+', rest):
    out({'merge_base_commit': {'sha': s['merge_base']}})
elif re.fullmatch(r'pulls/\d+', rest):
    if rest.split('/')[1] in s.get('pull_errors', {}):  # what gh says when GitHub can't answer for it
        stop(s['pull_errors'][rest.split('/')[1]])
    pull = next(p for p in s['pulls'] if p['number'] == int(rest.split('/')[1]))
    out(pull)
    if s.get('reopen_after_read'):  # reopened right after this read, which starts that run
        pull['state'] = 'open'
        s['runs'].append(s.pop('reopen_after_read'))
        save()
elif re.fullmatch(r'issues/\d+/events', rest):
    listing(s['events'].get(rest.split('/')[1], []))
elif re.fullmatch(r'issues/\d+/labels/.+', rest) and method == 'DELETE':
    number, label = int(rest.split('/')[1]), rest.split('/', 3)[3].replace('%20', ' ')
    pull = next(p for p in s['pulls'] if p['number'] == number)
    pull['labels'] = [l for l in pull['labels'] if l['name'] != label]
    save()
elif re.fullmatch(r'collaborators/[^/]+/permission', rest):
    login = rest.split('/')[1]
    if login in s.get('permission_errors', {}):  # what gh says when GitHub can't answer for that login
        stop(s['permission_errors'][login])
    out({'permission': s['permissions'].get(login, 'read')})
elif rest == 'actions/workflows/build.yml/runs':
    out({'workflow_runs': [r for r in s['runs'] if r['head_sha'] == query.get('head_sha')]})
elif rest == '' and method == 'GET':
    out({'full_name': repo, 'default_branch': 'main'})
elif rest.startswith('git/ref/heads/'):
    branch = rest[len('git/ref/heads/'):]
    heads = s.setdefault('heads', {}).setdefault(repo, {'main': 'c0ffee' * 6 + 'c0ff'})
    if s.pop('hide_branch_once', None) == branch:  # another run makes it right after this look
        save()
        stop('gh: Not Found (HTTP 404)')
    out({'ref': f'refs/heads/{branch}', 'object': {'sha': heads[branch]}}) if branch in heads else stop('gh: Not Found (HTTP 404)')
elif rest == 'git/refs' and method == 'POST':
    heads = s.setdefault('heads', {}).setdefault(repo, {'main': 'c0ffee' * 6 + 'c0ff'})
    if data['ref'][len('refs/heads/'):] in heads:
        stop('gh: Reference already exists (HTTP 422)')
    heads[data['ref'][len('refs/heads/'):]] = data['sha']
    save()
    out({'ref': data['ref']})
elif rest.startswith('contents/'):
    files = s.setdefault('files', {}).setdefault(repo, {})
    name = rest[len('contents/'):]
    branch = query.get('ref') or (data or {}).get('branch') or 'main'
    if branch not in s.setdefault('heads', {}).setdefault(repo, {'main': 'c0ffee' * 6 + 'c0ff'}):
        stop('gh: No commit found for the ref (HTTP 404)')
    key = f'{branch}:{name}'
    if method == 'GET':
        out(files[key]) if key in files else stop('gh: Not Found (HTTP 404)')
    elif method == 'DELETE':
        if key not in files or data.get('sha') != files[key]['sha']:
            stop('gh: sha does not match (HTTP 409)')
        del files[key]
        save()
    else:
        if s.get('pass_puts'):  # the writes before the one that fails
            s['pass_puts'] -= 1
        elif s.get('fail_puts'):  # another run's write came first, or one that cannot be done
            s['fail_puts'] -= 1
            save()
            stop(s.get('put_error', 'gh: is at 1a2b3c but expected 4d5e6f (HTTP 409)'))
        if key in files and data.get('sha') != files[key]['sha']:
            stop('gh: Invalid request.\n\n"sha" wasn\'t supplied. (HTTP 422)' if 'sha' not in data else 'gh: is at x but expected y (HTTP 409)')
        s['next'] += 1
        files[key] = {'content': data['content'], 'sha': f'blob{s["next"]}'}
        save()
        out({'content': files[key]})
elif re.fullmatch(r'issues/\d+', rest):
    out(s['issues'][rest.split('/')[1]])
elif re.fullmatch(r'issues/\d+/comments', rest) and method == 'POST':  # made with the job's token
    s['next'] += 1
    comment = {'id': s['next'], 'user': {'login': 'github-actions[bot]'}, 'body': data['body'],
               'html_url': f'https://github.com/{repo}/pull/{rest.split("/")[1]}#issuecomment-{s["next"]}'}
    s.setdefault('comments', {}).setdefault(rest.split('/')[1], []).append(comment)
    save()
    out(comment)
elif re.fullmatch(r'issues/\d+/comments', rest):
    listing(s.get('comments', {}).get(rest.split('/')[1], []))
elif re.fullmatch(r'pulls/\d+/reviews', rest):
    listing(s.get('reviews', {}).get(rest.split('/')[1], []))
elif re.fullmatch(r'pulls/\d+/comments', rest):
    listing(s.get('review_comments', {}).get(rest.split('/')[1], []))
elif re.fullmatch(r'actions/runs/\d+', rest):
    run = next(r for r in s['runs'] if r['id'] == int(rest.split('/')[-1]))
    if run.get('steps'):  # a build going on: each look at it finds it a step further
        step = run['steps'].pop(0)
        run.update(status=step['status'], conclusion=step.get('conclusion'), jobs=step['jobs'])
        save()
    out(run)
elif re.fullmatch(r'actions/runs/\d+/jobs', rest):
    run = next(r for r in s['runs'] if r['id'] == int(rest.split('/')[2]))
    out({'jobs': run.get('jobs', [])})
elif rest == 'actions/runs' and method == 'GET':  # a branch's runs of one event, newest first
    from urllib.parse import unquote
    picked = [r for r in s['runs'] if r.get('head_branch') == unquote(query.get('branch', ''))
              and r.get('event') == query.get('event')]
    out({'total_count': len(picked), 'workflow_runs': picked[(page - 1) * per:page * per]})
elif re.fullmatch(r'actions/runs/\d+/cancel', rest) and method == 'POST':
    run = next(r for r in s['runs'] if r['id'] == int(rest.split('/')[2]))
    if run['status'] == 'completed':
        stop('gh: Cannot cancel a workflow run that is completed. (HTTP 409)')
    run.update(status='completed', conclusion='cancelled')
    save()
    out({})
else:
    stop(f'fake gh: {method} {target}')
"""


class Discord(http.server.BaseHTTPRequestHandler):
    requests, messages, next_id = [], {}, [1000]

    def log_message(self, *args):
        pass

    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_one(self, method):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        Discord.requests.append(
            {'method': method, 'path': self.path, 'body': body, 'agent': self.headers['User-Agent']}
        )
        path = self.path.split('?')[0]
        if method == 'POST':
            Discord.next_id[0] += 1
            Discord.messages[str(Discord.next_id[0])] = body
            Discord.requests[-1]['reply'] = str(Discord.next_id[0])
            self.reply(200, {'id': str(Discord.next_id[0])})
        elif path.split('/')[-1] in Discord.messages:
            Discord.messages[path.split('/')[-1]] = body
            self.reply(200, {'id': path.split('/')[-1]})
        else:
            self.reply(404, {'message': 'Unknown Message'})

    def do_POST(self):
        self.handle_one('POST')

    def do_PATCH(self):
        self.handle_one('PATCH')


def posted(channel):
    return [r for r in Discord.requests if f'token-{channel}' in r['path']]


def serve():
    """the fake Discord started: its server, and the webhook URL every channel's hook starts with"""
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Discord)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f'http://127.0.0.1:{server.server_address[1]}/api/webhooks/1/token'
