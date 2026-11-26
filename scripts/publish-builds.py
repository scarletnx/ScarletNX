#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Publish a Build run's packages as releases, and say so on Discord.

    scripts/publish-builds.py plan --repo OWNER/REPO --to OWNER/NIGHTLY
    scripts/publish-builds.py publish --repo OWNER/REPO --to OWNER/NIGHTLY --kind nightly|pr|release --run ID
        --sha SHA [--pr N] [--tag TAG] [--day YYYY-MM-DD] --artifacts DIR [--checkout DIR]

plan follows the event GitHub Actions ran it for (GITHUB_EVENT_NAME, GITHUB_EVENT_PATH), and writes to GITHUB_OUTPUT
the run whose artifacts publish takes (kind, run, sha, short, pr, tag), when there is one to publish:
- a Build run that ended (workflow_run): a push of a version tag (v...) that built everywhere becomes REPO's release of
  that tag, the latest (a prerelease for a tag with a '-', v1.0.0-rc1); a scheduled run of main, or one by hand, that
  built everywhere becomes the day's nightly, NIGHTLY's release nightly-<day>-<commit> (the day the run began, UTC),
  the latest there, every nightly kept; one that failed says so at the top of the newest nightly, until the next comes
  out; a push to main becomes REPO's one prerelease of main, tagged main-<commit>, a tag that never moves: each package
  joins it as its job ends (build.yml starts this workflow by hand with the run, and the run's end does once more),
  the build before and its tag go when the first package of a newer one comes, a build that ends with no package
  leaves no prerelease, and an older build changes nothing; a pull request's run becomes NIGHTLY's prerelease
  pr-<number>, its files and notes replaced at each push;
- a pull request closed (pull_request_target) without a merge: its prerelease and tag are deleted; merged: they stay
  until a nightly has its merge commit, so its change can be had in between, and each nightly published deletes the
  ones it now has, with those of pull requests closed since, listed first (the sweep);
- a fork's pull request is published only once someone of the team (write, maintain or admin) labels it LABEL after
  its last push, which a new push takes off (synchronize). The label put on a finished run publishes it at once.
Each nightly also writes NIGHTLY's README (scripts/nightly-notes.py's readme), so that page says what it holds.
Nothing is built: the files come from the run's artifacts, which the workflow downloads into DIR between the two (one
folder each, as actions/download-artifact puts them), and no code of the pull request runs. Each file is named
ScarletNX-<system>-<arch> (scripts/nightly-notes.py), an AppImage with the .zsync AppImageUpdate reads beside it.

Discord: a version's release gets a message on DISCORD_WEBHOOK_RELEASES's channel, each download linked with its size
and the changes since the version before; without the webhook nothing is posted, and the run says so. The nightlies'
and the pull requests' cards are ReiWimsy's (scarletnx/wimsy-core), which sees their releases change. gh reads and
writes REPO with GH_TOKEN, and NIGHTLY with NIGHTLY_TOKEN. --dry-run reads and prints each write instead.
.github/workflows/publish.yml runs this; tests/publish_builds_test.py runs it against a fake gh and a fake Discord.
"""

import argparse
import base64
import datetime
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('nightly_notes', Path(__file__).with_name('nightly-notes.py'))
nightly_notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nightly_notes)

SERVER = nightly_notes.SERVER
LABEL = 'publish builds'  # what lets a fork's pull request be published
SCARLET = 0xB0102A  # a version's colour
# the mascot beside each message
WIMSY = 'https://raw.githubusercontent.com/{repo}/main/dist/icons/hicolor/128x128/apps/wimsy.png'
# Discord's limits: an embed's description, and its whole text
DESCRIPTION, EMBED = 4096, 6000
# a version's changes listed, at most, before "and N more"
MOST_LINES = 10
# the commits one GraphQL query asks the pull requests of
PULLS_ASKED = 50
TEAM = {'admin', 'maintain', 'write'}
VERSION = re.compile(r'v\d[0-9A-Za-z.+-]*')
DAY = re.compile(r'\d{4}-\d{2}-\d{2}')
# what this repository's prerelease that linked the newest nightly was tagged, a tag that moved at each one: deleted
ROLLING = 'nightly'
# main's prerelease in this repository: its title, and its tags' prefix, main-<commit>, a tag that never moves
MAIN = 'main'
# the job of build.yml that makes each release file, whose state main's prerelease says while the file is not there
JOBS = {
    'ScarletNX-linux-x86_64.AppImage': 'Linux AppImage (x86_64)',
    'ScarletNX-linux-aarch64.AppImage': 'Linux AppImage (aarch64)',
    'ScarletNX-linux-ppc64le.AppImage': 'Linux AppImage (ppc64le)',
    'ScarletNX-linux-ppc64.AppImage': 'Linux AppImage (ppc64)',
    'ScarletNX-linux-riscv64.AppImage': 'Linux AppImage (riscv64)',
    'ScarletNX-linux-loongarch64.AppImage': 'Linux AppImage (loongarch64)',
    'ScarletNX-windows-x86_64.zip': 'Windows (Clang + LLVM)',
    'ScarletNX-windows-aarch64.zip': 'Windows (Clang + LLVM, aarch64)',
    'ScarletNX-android-universal.apk': 'Android APK',
    'ScarletNX-macos-arm64.zip': 'macOS and iOS (arm64)',
    'ScarletNX-ios-arm64.ipa': 'macOS and iOS (arm64)',
}
# the line a nightly's notes open with while the day after it has none
MISSED = '<!-- missed -->'
# A run's artifacts, by the commit's first 7 digits: the release file each gives, ScarletNX-<system>-<arch>
ARTIFACTS = {
    'ScarletNX-linux-x86_64-{short}.AppImage': 'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64-{short}.AppImage': 'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-linux-ppc64le-{short}.AppImage': 'ScarletNX-linux-ppc64le.AppImage',
    'ScarletNX-linux-ppc64-{short}.AppImage': 'ScarletNX-linux-ppc64.AppImage',
    'ScarletNX-linux-riscv64-{short}.AppImage': 'ScarletNX-linux-riscv64.AppImage',
    'ScarletNX-linux-loongarch64-{short}.AppImage': 'ScarletNX-linux-loongarch64.AppImage',
    'ScarletNX-windows-x86_64-{short}': 'ScarletNX-windows-x86_64.zip',
    'ScarletNX-windows-aarch64-{short}': 'ScarletNX-windows-aarch64.zip',
    'ScarletNX-android-universal-{short}.apk': 'ScarletNX-android-universal.apk',
    'ScarletNX-macos-arm64-{short}.zip': 'ScarletNX-macos-arm64.zip',
    'ScarletNX-ios-arm64-{short}.ipa': 'ScarletNX-ios-arm64.ipa',
}
# The packages a run may lack and still have built everywhere: their jobs fail without failing the run
# (continue-on-error in build.yml), so a release without them says they were not built
OPTIONAL = {
    'ScarletNX-linux-ppc64le.AppImage',
    'ScarletNX-linux-ppc64.AppImage',
    'ScarletNX-linux-riscv64.AppImage',
    'ScarletNX-linux-loongarch64.AppImage',
    'ScarletNX-windows-aarch64.zip',
    'ScarletNX-macos-arm64.zip',
    'ScarletNX-ios-arm64.ipa',
}
# The packages only a run a nightly can come of makes: a pull request's notes leave them out, where a nightly's say
# they were not built. None: a pull request's run makes every package (build.yml), as it tests them.
# tests/publish_builds_test.py holds them to build.yml
NIGHTLY_ONLY = set()


def published(release):
    """a release's order: when it was published. Its created_at is its tag's commit's date, which every nightly
    shares (each is tagged on the nightly repository's one commit)"""
    return release.get('published_at') or ''


class Hub:
    """GitHub through gh (REPO with GH_TOKEN, the nightly repository with NIGHTLY_TOKEN), and Discord's webhooks"""

    def __init__(self, repo, to, dry):
        self.repo, self.to, self.dry = repo, to, dry
        self.tokens = {repo: os.environ.get('GH_TOKEN', ''), to: os.environ.get('NIGHTLY_TOKEN', '')}

    def token(self, repo):
        if repo == self.to and not self.tokens[repo] and not self.dry:
            raise RuntimeError(f'NIGHTLY_TOKEN is not set: nothing can be written in {self.to}')
        return self.tokens[repo]

    def gh(self, args, repo, data=None, write=False):
        if write and self.dry:
            print('would run: gh', ' '.join(args), json.dumps(data) if data is not None else '')
            return {}
        run = subprocess.run(
            ['gh', *args],
            input=json.dumps(data) if data is not None else None,
            capture_output=True,
            text=True,
            env=dict(os.environ, GH_TOKEN=self.token(repo)),
            check=False,
        )
        if run.returncode:
            raise RuntimeError(f'gh {" ".join(args)}: {run.stderr.strip() or run.stdout.strip()}')
        return json.loads(run.stdout) if run.stdout.strip() else {}

    def read(self, path, repo=None):
        return self.gh(['api', path], repo or self.repo)

    def graphql(self, query):
        return self.gh(['api', 'graphql', '-f', f'query={query}'], self.repo)

    def missing(self, path, repo=None):
        """path read, or None when GitHub has nothing there (404)"""
        try:
            return self.read(path, repo)
        except RuntimeError as error:
            if 'HTTP 404' in str(error) or 'Not Found' in str(error):
                return None
            raise

    def pages(self, path, repo=None):
        """every item of a listing, 100 a page"""
        items, page = [], 1
        while True:
            sep = '&' if '?' in path else '?'
            got = self.gh(['api', f'{path}{sep}per_page=100&page={page}'], repo or self.repo)
            if isinstance(got, dict):
                got = next((got[key] for key in ('artifacts', 'workflow_runs', 'jobs') if key in got), got)
            items += got
            if len(got) < 100:
                return items
            page += 1

    def write(self, repo, method, path, data=None):
        args = ['api', '-X', method, path, *(['--input', '-'] if data is not None else [])]
        return self.gh(args, repo, data, write=True)

    def releases(self, repo):
        return self.pages(f'repos/{repo}/releases', repo)

    def webhook(self, channel):
        return os.environ.get(f'DISCORD_WEBHOOK_{channel.upper()}', '')

    def post(self, channel, embed, message=None, create=True):
        """the message of channel (releases, nightly, prs or issues) edited when message names it, else posted when
        create: its id, or None when there is none"""
        hook = self.webhook(channel)
        if not hook:
            print(f'::notice::No DISCORD_WEBHOOK_{channel.upper()}: nothing posted on Discord')
            return message
        body = {'embeds': [embed], 'allowed_mentions': {'parse': []}}
        if self.dry:
            print(f'would {"edit" if message else "post"} on Discord ({channel}):', json.dumps(body))
            return message
        # a webhook of a forum channel or a thread names it in its query (thread_id=...), which both requests keep
        base, _, query = hook.partition('?')
        edit = f'{base}/messages/{message}' + (f'?{query}' if query else '')
        post = f'{base}?' + (f'{query}&' if query else '') + 'wait=true'
        for method, url in (('PATCH', edit), ('POST', post)):
            if (method == 'PATCH' and not message) or (method == 'POST' and not create):
                continue
            request = urllib.request.Request(
                url,
                data=json.dumps(body).encode(),
                method=method,
                # Discord refuses urllib's own agent
                headers={'Content-Type': 'application/json', 'User-Agent': f'DiscordBot ({SERVER}/{self.repo}, 1)'},
            )
            try:
                with urllib.request.urlopen(request, timeout=30) as reply:
                    return json.loads(reply.read() or b'{}').get('id', message)
            except urllib.error.HTTPError as error:
                if method == 'PATCH' and error.code == 404:  # deleted on Discord: a new one, when it may post
                    continue
                print(f'::warning::Discord answered {error.code} to the {channel} message: {error.read()[:200]!r}')
                return message
            except (urllib.error.URLError, OSError) as error:
                print(f'::warning::Discord could not be reached for the {channel} message: {error}')
                return message
        return None

    def embed(self, title, url, description, fields=(), color=SCARLET, footer='', author=None):
        """a message's embed, its description cut by whole lines, the last first, to what Discord takes: what it lists
        gives way, the fields stay"""
        title = title[:256]
        room = EMBED - len(title) - len(footer) - len((author or {}).get('name', ''))
        room = max(0, min(DESCRIPTION, room - sum(len(f['name']) + len(f['value']) for f in fields)))
        while len(description) > room:
            description = description.rsplit('\n', 1)[0] if '\n' in description else description[:room]
        made = {
            'title': title,
            'url': url,
            'description': description,
            'color': color,
            'thumbnail': {'url': WIMSY.format(repo=self.repo)},
            'timestamp': datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds'),
        }
        if fields:
            made['fields'] = list(fields)
        if footer:
            made['footer'] = {'text': footer}
        if author:
            made['author'] = author
        return made


def listed(lines, more_url, most):
    shown = [f'- {line}' for line in lines[:most]]
    if len(lines) > most:
        shown.append(f'- and [{len(lines) - most} more]({more_url})')
    return shown


class Publisher(Hub):
    def __init__(self, args):
        super().__init__(args.repo, args.to, args.dry_run)
        self.checkout = args.checkout
        self.artifacts = Path(args.artifacts) if args.artifacts else None
        self.work = Path(tempfile.mkdtemp(prefix='publish-builds-'))
        self.output = {}

    def release(self, repo, tag):
        return next((r for r in self.releases(repo) if r['tag_name'] == tag), None)

    def put_release(self, repo, tag, name, body, prerelease, files, target=None):
        """the release of tag with these notes and exactly these files (each name to its path), made or replaced; it
        and whether it is new"""
        existing = self.release(repo, tag)
        fields = {'name': name, 'body': body, 'prerelease': prerelease, 'make_latest': str(not prerelease).lower()}
        if existing:
            made = self.write(repo, 'PATCH', f'repos/{repo}/releases/{existing["id"]}', fields) or existing
        else:
            fields = dict(fields, tag_name=tag, **({'target_commitish': target} if target else {}))
            made = self.write(repo, 'POST', f'repos/{repo}/releases', fields)
        if files:
            self.gh(
                ['release', 'upload', tag, *map(str, files.values()), '--repo', repo, '--clobber'], repo, write=True
            )
        for asset in (existing or {}).get('assets', []):
            if asset['name'] not in files:  # a file this run did not build: the old one would pass for it
                self.write(repo, 'DELETE', f'repos/{repo}/releases/assets/{asset["id"]}')
        return made, existing is None

    def drop_release(self, repo, release):
        self.write(repo, 'DELETE', f'repos/{repo}/releases/{release["id"]}')
        try:
            self.write(repo, 'DELETE', f'repos/{repo}/git/refs/tags/{release["tag_name"]}')
        except RuntimeError as error:
            if 'Reference does not exist' not in str(error):
                raise
            print(f'{release["tag_name"]} had no tag left')

    # ---- a run's packages
    def packages(self, sha):
        """the release files the run's artifacts give, by name, the files to upload with them (an AppImage's .zsync),
        and the release files it gave none for"""
        files, uploads, missing = {}, {}, []
        for pattern, name in ARTIFACTS.items():
            folder = self.artifacts / pattern.format(short=sha[:7])
            path = self.work / name
            if folder.is_file():  # a single-file package downloaded as it is, at the artifact's own path
                shutil.copyfile(folder, path)
                files[name] = uploads[name] = path
                continue
            if not folder.is_dir() or not any(folder.iterdir()):
                missing.append(name)
                continue
            if not Path(pattern).suffix:  # the Windows build is a folder: zipped as it is
                with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for item in sorted(folder.rglob('*')):
                        if item.is_file():
                            archive.write(item, item.relative_to(folder).as_posix())
            else:
                found = sorted(folder.rglob(name if name.endswith('.AppImage') else '*' + Path(name).suffix))
                if len(found) != 1:
                    raise RuntimeError(f'{folder.name} holds {len(found)} of {name}, not one')
                shutil.copyfile(found[0], path)
            files[name] = uploads[name] = path
            if name.endswith('.AppImage'):
                zsync = folder / f'{name}.zsync'
                if not zsync.is_file():
                    raise RuntimeError(f'{folder.name} has no {zsync.name}, which AppImageUpdate reads')
                uploads[zsync.name] = self.work / zsync.name
                shutil.copyfile(zsync, uploads[zsync.name])
        return files, uploads, missing

    def complete(self, run, sha):
        """the run's packages, and the optional ones it has none for; a required one missing stops it"""
        files, uploads, missing = self.packages(sha)
        required = [name for name in missing if name not in OPTIONAL]
        if required:
            said = '; '.join(map(nightly_notes.system, required))
            raise RuntimeError(f'run {run} built everywhere, yet has no package for {said}')
        return files, uploads, missing

    @staticmethod
    def fields(files, sizes, download):
        """a field for each download, with its size, in the order of the notes' table"""
        out = []
        for name in nightly_notes.ordered(list(files)):
            system, arch, kind = nightly_notes.SYSTEMS[name]
            value = f'[{kind}]({download}/{name})\n{nightly_notes.size(sizes[name])}'
            out.append({'name': system if ',' in arch else f'{system} {arch}', 'value': value, 'inline': True})
        return out

    def pulls(self, previous, sha):
        """the pull request that merged each commit of main from previous to sha, as GitHub says (commit to Pull): main
        merges by rebase, so its history names none. A commit no merged pull request into main has is left out"""
        if not previous or nightly_notes.changes(self.checkout, previous, sha, {}) is None:
            return {}
        commits = nightly_notes.git(self.checkout, 'log', '--first-parent', '--format=%H', f'{previous}..{sha}').split()
        owner, name = self.repo.split('/', 1)
        found = {}
        for start in range(0, len(commits), PULLS_ASKED):
            batch = commits[start : start + PULLS_ASKED]
            asked = ' '.join(
                f'c{n}: object(oid: "{commit}") {{ ... on Commit {{ associatedPullRequests(first: 5) {{ nodes {{ '
                'number title merged baseRefName author { login } } } } }'
                for n, commit in enumerate(batch)
            )
            data = self.graphql(f'query {{ repository(owner: "{owner}", name: "{name}") {{ {asked} }} }}')
            repository = (data.get('data') or {}).get('repository')
            if repository is None:
                raise RuntimeError(f'GitHub gave no pull requests of the commits {batch[0][:7]}...: {json.dumps(data)}')
            for n, commit in enumerate(batch):
                nodes = ((repository.get(f'c{n}') or {}).get('associatedPullRequests') or {}).get('nodes') or []
                merged = [p for p in nodes if p.get('merged') and p.get('baseRefName') == 'main']
                if merged:
                    pull = merged[0]
                    author = (pull.get('author') or {}).get('login') or 'ghost'
                    found[commit] = nightly_notes.Pull(pull['number'], pull['title'], author)
        return found

    def older(self, sha, previous):
        """sha is an ancestor of previous, a newer commit already published"""
        if not previous or previous == sha:
            return False
        command = ['git', '-C', self.checkout, 'merge-base', '--is-ancestor', sha, previous]
        return subprocess.run(command, capture_output=True, check=False).returncode == 0

    # ---- what each event publishes
    def nightlies(self):
        """NIGHTLY's nightlies, the newest published first"""
        return sorted(
            (r for r in self.releases(self.to) if r['tag_name'].startswith('nightly-')), key=published, reverse=True
        )

    def nightly(self, run, sha, day):
        files, uploads, missing = self.complete(run, sha)
        why = f'{SERVER}/{self.repo}/actions/runs/{run}'
        tag = f'nightly-{day}-{sha[:7]}'
        older = [r for r in self.nightlies() if r['tag_name'] != tag]
        previous = next((m.group(0) for r in older[:1] for m in [re.search(r'[0-9a-f]{40}', r['body'] or '')] if m), '')
        if self.older(sha, previous):  # a run that ended after a newer commit's: that one stays the latest
            print(f'{sha[:7]} is older than the newest nightly, {previous[:7]}: not published')
            return
        latest = f'{SERVER}/{self.to}/releases/latest'
        kept = f'Every nightly stays in this repository; [the latest]({latest}) is always the newest one.'
        download = f'{SERVER}/{self.to}/releases/download/{tag}'
        sizes = {name: path.stat().st_size for name, path in files.items()}
        pulls = self.pulls(previous, sha)
        body = nightly_notes.notes(
            self.checkout,
            sha,
            previous,
            self.repo,
            list(files),
            download,
            kept,
            sizes=sizes,
            missing=missing,
            why=why,
            pulls=pulls,
        )
        title = nightly_notes.title(day, sha)
        new = self.put_release(self.to, tag, title, body, False, uploads)[1]
        print(f'{"published" if new else "replaced the files of"} {tag}')
        for old in older[:1]:  # the day it stood for has its nightly now
            self.unsay_missed(old)
        self.put_readme()
        self.sweep()

    # ---- a day whose nightly did not come out, said where the newest one is
    def missed(self, run):
        """run, a scheduled one of main or one by hand, did not build everywhere: unless its day has its nightly, the
        newest nightly says at its top that the day's is not out, which run failed and in what"""
        day = run['created_at'][:10]
        newest = next(iter(self.nightlies()), None)
        if not newest:
            print(f'run {run["id"]} ended {run["conclusion"]}, and there is no nightly to say it on')
            return
        if newest['tag_name'].startswith(f'nightly-{day}-'):
            print(f'run {run["id"]} ended {run["conclusion"]}: {day} has its nightly, {newest["tag_name"]}')
            return
        jobs = self.pages(f'repos/{self.repo}/actions/runs/{run["id"]}/jobs', self.repo)
        failed = sorted(j['name'] for j in jobs if j.get('conclusion') in ('failure', 'timed_out', 'cancelled'))
        where = ', '.join(failed) if failed else f'no job: the run ended {run["conclusion"]}'
        url = run.get('html_url') or f'{SERVER}/{self.repo}/actions/runs/{run["id"]}'
        line = (
            f'{MISSED} **The nightly of {day} is not out: [its newest try]({url}) failed in {nightly_notes.escape(where)}.'
            f'** This one is of {newest["tag_name"][8:18]}.'
        )
        body = '\n'.join([line, '', *self.without_missed(newest['body'] or '').splitlines()]) + '\n'
        self.write(self.to, 'PATCH', f'repos/{self.to}/releases/{newest["id"]}', {'body': body})
        print(f'{newest["tag_name"]} says the nightly of {day} is not out: run {run["id"]} failed in {where}')

    @staticmethod
    def without_missed(body):
        """body without the line missed put at its top, and the blank line after it"""
        lines = body.splitlines()
        if lines and lines[0].startswith(MISSED):
            lines = lines[2:] if len(lines) > 1 and not lines[1].strip() else lines[1:]
        return '\n'.join(lines) + ('\n' if lines else '')

    def unsay_missed(self, release):
        body = release.get('body') or ''
        if body.startswith(MISSED):
            self.write(
                self.to, 'PATCH', f'repos/{self.to}/releases/{release["id"]}', {'body': self.without_missed(body)}
            )
            print(f'{release["tag_name"]} no longer says a nightly is not out')

    def put_readme(self):
        """NIGHTLY's README as nightly-notes.py's readme has it, written when it differs"""
        text = nightly_notes.readme(self.repo, self.to, OPTIONAL)
        path = f'repos/{self.to}/contents/README.md'
        now = self.missing(path, self.to)
        if now and base64.b64decode(now.get('content', '')).decode() == text:
            return
        data = {
            'message': 'Describe the nightlies as publish.yml makes them',
            'content': base64.b64encode(text.encode()).decode(),
            'branch': 'main',
            **({'sha': now['sha']} if now else {}),
        }
        self.write(self.to, 'PUT', path, data)
        print(f'{self.to} README.md written')

    # ---- main's prerelease: the build of the newest push, its packages each as its job ends
    def main_releases(self):
        """REPO's main prereleases, the newest first: one, but for a run that stopped between making one and deleting
        the one before"""
        return sorted(
            (r for r in self.releases(self.repo) if r['tag_name'].startswith(f'{MAIN}-')), key=published, reverse=True
        )

    @staticmethod
    def commit_of(release):
        """the commit a release's notes were built from, the first they name; '' for none"""
        found = re.search(r'[0-9a-f]{40}', (release or {}).get('body') or '')
        return found.group(0) if found else ''

    def main_plan(self, run):
        """the packages of run, a push to main, to download for main's prerelease: those its artifacts have and the
        prerelease lacks. None for a run older than the build the prerelease is of"""
        sha = run['head_sha']
        current = next(iter(self.main_releases()), None)
        built = self.commit_of(current)
        if current and built != sha and self.contains(sha, built):
            print(f"{sha[:7]} is older than main's prerelease, {built[:7]}: it stays")
            return
        have = {a['name'] for a in current['assets']} if current and built == sha else set()
        made = {
            a['name']
            for a in self.pages(f'repos/{self.repo}/actions/runs/{run["id"]}/artifacts')
            if not a.get('expired')
        }
        need = [
            file for pattern, file in ARTIFACTS.items() if pattern.format(short=sha[:7]) in made and file not in have
        ]
        self.take('main', run, need=need)

    def main_build(self, run_id, sha):
        """main's prerelease brought to run, a push to main: a new build takes it at its first package, tagged
        main-<commit>, and the one before goes; each package joins as it comes; the notes say what is built, building
        and not built; a build that ended with no package leaves no prerelease. An older build changes nothing"""
        run = self.read(f'repos/{self.repo}/actions/runs/{run_id}')
        releases = self.main_releases()
        current = releases[0] if releases else None
        built = self.commit_of(current)
        if current and built != sha and self.contains(sha, built):
            print(f"{sha[:7]} is older than main's prerelease, {built[:7]}: it stays")
            return
        ours = current if current and built == sha else None
        files, uploads, _ = self.packages(sha)
        tag = f'{MAIN}-{sha[:7]}'
        if not ours and not files:
            if run['status'] != 'completed':
                print(f'no package of {sha[:7]} yet: {current["tag_name"] if current else "no prerelease"} stays')
                return
            for old in releases:
                self.drop_release(self.repo, old)
                print(f'deleted {old["tag_name"]}: the build of {sha[:7]}, newer, ended with no package')
            self.drop_rolling()
            return
        if not ours:
            fields = {'tag_name': tag, 'target_commitish': sha, 'name': f'{MAIN} ({sha[:7]})', 'body': '',
                      'prerelease': True, 'make_latest': 'false'}  # fmt: skip
            ours = dict(self.write(self.repo, 'POST', f'repos/{self.repo}/releases', fields) or fields, assets=[])
            print(f"made {tag}, main's prerelease, at its first package")
        have = {a['name']: a.get('size', 0) for a in ours.get('assets', [])}
        new = {name: path for name, path in uploads.items() if name not in have}
        if new:
            self.gh(
                ['release', 'upload', tag, *map(str, new.values()), '--repo', self.repo, '--clobber'],
                self.repo,
                write=True,
            )
            print(f'{tag}: {", ".join(sorted(n for n in new if n in JOBS))} added')
        present = [name for name in JOBS if name in have or name in files]
        sizes = {**{n: have[n] for n in present if n in have}, **{n: files[n].stat().st_size for n in files}}
        jobs = {j['name']: j for j in self.pages(f'repos/{self.repo}/actions/runs/{run_id}/jobs', self.repo)}
        building = [n for n in JOBS if n not in present and (jobs.get(JOBS[n]) or {}).get('status') != 'completed'
                    and run['status'] != 'completed']  # fmt: skip
        missing = [n for n in JOBS if n not in present and n not in building]
        why = run.get('html_url') or f'{SERVER}/{self.repo}/actions/runs/{run_id}'
        begun = run.get('run_started_at') or run.get('created_at') or ''
        said = (
            f'Its run, [{run_id}]({why}), began {begun.replace("T", " ").rstrip("Z")} UTC. The build of each green push '
            f"to main, which the next push's first package replaces; what changed is in the nightlies of "
            f'[{self.to}]({SERVER}/{self.to}/releases).'
        )
        body = nightly_notes.notes(
            self.checkout,
            sha,
            '',
            self.repo,
            present,
            f'{SERVER}/{self.repo}/releases/download/{tag}',
            said,
            sizes=sizes,
            lead='The build of `main` at',
            missing=missing,
            why=why,
            building=building,
        )
        self.write(self.repo, 'PATCH', f'repos/{self.repo}/releases/{ours["id"]}', {'body': body})
        print(f'{tag}: {len(present)} built, {len(building)} building, {len(missing)} not built')
        for old in releases:
            if old['tag_name'] != tag:
                self.drop_release(self.repo, old)
                print(f'deleted {old["tag_name"]}: {tag} is newer')
        self.drop_rolling()

    def drop_rolling(self):
        """the prerelease ROLLING, which linked the newest nightly with a tag that moved, deleted where it is still"""
        rolling = self.release(self.repo, ROLLING)
        if rolling:
            self.drop_release(self.repo, rolling)
            print(f'deleted {self.repo} "{ROLLING}" and its tag, which moved at each nightly')

    def version(self, run, sha, tag):
        files, uploads, missing = self.complete(run, sha)
        why = f'{SERVER}/{self.repo}/actions/runs/{run}'
        earlier = sorted(
            (
                r
                for r in self.releases(self.repo)
                if VERSION.fullmatch(r['tag_name']) and r['tag_name'] != tag and not r.get('draft')
            ),
            key=published,
            reverse=True,
        )
        since = earlier[0]['tag_name'] if earlier else ''
        previous = nightly_notes.git(self.checkout, 'rev-list', '-n', '1', since).strip() if since else ''
        download = f'{SERVER}/{self.repo}/releases/download/{tag}'
        sizes = {name: path.stat().st_size for name, path in files.items()}
        pulls = self.pulls(previous, sha)
        body = nightly_notes.notes(
            self.checkout,
            sha,
            previous,
            self.repo,
            list(files),
            download,
            '',
            since,
            sizes,
            f'ScarletNX {tag}, built from',
            'the newest version',
            missing,
            why,
            pulls,
        )
        title = f'ScarletNX {tag}'
        made, new = self.put_release(self.repo, tag, title, body, '-' in tag, uploads, sha)
        print(f'{"published" if new else "replaced the files of"} {self.repo} {tag}')
        if new:
            url = made.get('html_url', f'{SERVER}/{self.repo}/releases/tag/{tag}')
            commit = f'{SERVER}/{self.repo}/commit/{sha}'
            more, compare = [{'name': 'Source', 'value': f'[`{sha[:7]}`]({commit})', 'inline': True}], url
            if since:
                compare = f'{SERVER}/{self.repo}/compare/{since}...{tag}'
                more.append({'name': 'Since', 'value': f'[{since}]({compare})', 'inline': True})
            items = nightly_notes.changes(self.checkout, previous, sha, pulls) or []
            lines = nightly_notes.entry_lines(items, pulls, f'{SERVER}/{self.repo}')
            said = '\n'.join([f'Built from [`{sha[:7]}`]({commit}).', *listed(lines, compare, MOST_LINES)])
            self.post('releases', self.embed(title, url, said, self.fields(files, sizes, download) + more))

    def pull_facts(self, pr, sha, run):
        """what a pull request's notes and message say of it besides the files: its link, its commit's, the main commit
        it stands on and the nightly of that commit when there is one, to set the two side by side, its commits as lines
        (the newest first, as a nightly's changes), the run's link"""
        url = f'{SERVER}/{self.repo}'
        number = pr['number']
        base = self.read(f'repos/{self.repo}/compare/{pr["base"]["sha"]}...{sha}')['merge_base_commit']['sha']
        commits = self.pages(f'repos/{self.repo}/pulls/{number}/commits', self.repo)
        lines = [
            f'{nightly_notes.escape((c["commit"]["message"] or "").splitlines()[0] if c["commit"]["message"] else "")} '
            f'([`{c["sha"][:7]}`]({url}/pull/{number}/commits/{c["sha"]}))'
            for c in reversed(commits)
        ]
        nightly = next(
            (
                r['html_url']
                for r in self.releases(self.to)
                if r['tag_name'].startswith('nightly-') and r['tag_name'].endswith(f'-{base[:7]}')
            ),
            '',
        )
        return {
            'pr': pr['html_url'],
            'commit': f'{url}/pull/{number}/commits/{sha}',
            'base': base,
            'base_said': f'[`{base[:7]}`]({url}/commit/{base})' + (f' ([its nightly]({nightly}))' if nightly else ''),
            'commits': lines,
            'all_commits': f'{pr["html_url"]}/commits',
            'run': f'{url}/actions/runs/{run}',
            'author': pr['user']['login'],
            'author_url': pr['user'].get('html_url', f'{SERVER}/{pr["user"]["login"]}'),
        }

    def pull_notes(self, pr, sha, files, sizes, missing, facts):
        number = pr['number']
        download = f'{SERVER}/{self.to}/releases/download/pr-{number}'
        lines = [
            (
                f'Test builds of [#{number}]({facts["pr"]}) by [@{facts["author"]}]({facts["author_url"]}), at '
                f'[`{sha[:7]}`]({facts["commit"]}), on main at {facts["base_said"]}.'
            ),
            '',
            (
                "A pull request's builds, not a release: each push to it replaces them. They are deleted when it "
                'closes without a merge, or once merged, when a nightly has its change.'
            ),
            '',
            *nightly_notes.downloads(
                list(files),
                download,
                f'{SERVER}/{self.repo}',
                sha,
                sizes,
                missing,
                facts['run'],
                "the newest nightly, not this pull request's next builds",
            ),
        ]
        if facts['commits']:
            count = len(facts['commits'])
            lines += ['', '## Commits', '', f'{count} commit{"" if count == 1 else "s"}, the newest first:', '']
            lines += [f'- {line}' for line in facts['commits'][: nightly_notes.MOST_CHANGES]]
            if count > nightly_notes.MOST_CHANGES:
                lines.append(f'- and [{count - nightly_notes.MOST_CHANGES} more]({facts["all_commits"]})')
        lines += ['', *nightly_notes.source(facts['commit'])]
        return '\n'.join(lines) + '\n'

    def pull(self, pr, sha, run):
        number = pr['number']
        if pr['head']['sha'] != sha:
            print(f'#{number} is at {pr["head"]["sha"][:7]} now: the builds of {sha[:7]} are not published')
            return
        files, uploads, missing = self.packages(sha)
        if not files:
            print(f'#{number}: nothing of {sha[:7]} was built, so its builds stay as they were')
            return
        missing = [name for name in missing if name not in NIGHTLY_ONLY]
        tag = f'pr-{number}'
        title = f'PR #{number}: {pr["title"]}'
        sizes = {name: path.stat().st_size for name, path in files.items()}
        facts = self.pull_facts(pr, sha, run)
        body = self.pull_notes(pr, sha, files, sizes, missing, facts)
        new = self.put_release(self.to, tag, title, body, True, uploads)[1]
        print(f'{"published" if new else "replaced"} {tag} with the builds of {sha[:7]}')

    def closed(self, pr):
        tag = f'pr-{pr["number"]}'
        release = self.release(self.to, tag)
        if not release:
            print(f'#{pr["number"]} closed: it had no builds in {self.to}')
            return
        why = self.leaving(pr, self.built_main())
        if not why:
            print(f'#{pr["number"]} merged as {pr["merge_commit_sha"][:7]}: {tag} stays until a nightly has it')
            return
        self.drop_release(self.to, release)
        print(f'deleted {tag}: {why}')

    # ---- the pull requests' builds no longer needed
    def built_main(self):
        """the commit the newest nightly was built from, '' with none"""
        newest = sorted(
            (r for r in self.releases(self.to) if r['tag_name'].startswith('nightly-')), key=published, reverse=True
        )
        return next((m.group(0) for r in newest[:1] for m in [re.search(r'[0-9a-f]{40}', r['body'] or '')] if m), '')

    def contains(self, commit, newer):
        """newer, a commit of main, has commit"""
        if not commit or not newer:
            return False
        run = subprocess.run(
            ['git', '-C', self.checkout, 'merge-base', '--is-ancestor', commit, newer],
            capture_output=True,
            text=True,
            check=False,
        )
        if run.returncode not in (0, 1):
            raise RuntimeError(f'git merge-base --is-ancestor {commit[:7]} {newer[:7]}: {run.stderr.strip()}')
        return run.returncode == 0

    def leaving(self, pr, built):
        """why pr's builds go now, or '' while they are still the ones to test: open, or merged into a commit the newest
        nightly (built from built) does not have yet"""
        number = pr['number']
        if pr['state'] == 'open':
            return ''
        if not pr.get('merged'):
            return f'#{number} closed without a merge'
        merge = pr.get('merge_commit_sha') or ''
        if self.contains(merge, built):
            return f'#{number} merged as {merge[:7]}, which the nightly of {built[:7]} has'
        return ''

    def sweep(self):
        """the pull requests' builds whose pull request closed without a merge, or merged into a nightly: listed, then
        deleted, each with why. Their own close deletes them too; this catches the ones a close's run left behind"""
        built = self.built_main()
        gone = []
        for release in self.releases(self.to):
            number = re.fullmatch(r'pr-(\d+)', release['tag_name'])
            if number:
                why = self.leaving(self.read(f'repos/{self.repo}/pulls/{number.group(1)}'), built)
                if why:
                    gone.append((release, why))
        if not gone:
            print("no pull request's builds to delete")
            return
        print(f"to delete, {len(gone)} pull requests' builds: " + '; '.join(f'{r["tag_name"]} ({w})' for r, w in gone))
        for release, why in gone:
            self.drop_release(self.to, release)
            print(f'deleted {release["tag_name"]}: {why}')

    # ---- who may publish a fork's pull request
    def fork(self, pr):
        return pr['head']['repo'] is None or pr['head']['repo']['full_name'] != self.repo

    def approved(self, pr, since):
        """labeled LABEL by the team after since (when the run of its head began), and still labeled"""
        if LABEL not in {label['name'] for label in pr.get('labels', [])}:
            return False
        events = self.pages(f'repos/{self.repo}/issues/{pr["number"]}/events', self.repo)
        for event in reversed(events):
            if event.get('event') != 'labeled' or event.get('label', {}).get('name') != LABEL:
                continue
            if event['created_at'] < since:
                return False
            login = event['actor']['login']
            try:
                permission = self.read(f'repos/{self.repo}/collaborators/{login}/permission').get('permission')
            except RuntimeError as error:
                permission = f'unknown: {error}'
            if permission in TEAM:
                return True
            print(f'"{LABEL}" was put by {login}, who is not of the team ({permission})')
        return False

    def pull_of(self, sha):
        """the open pull request whose head is sha"""
        pulls = self.pages(f'repos/{self.repo}/pulls?state=open', self.repo)
        return next((pr for pr in pulls if pr['head']['sha'] == sha), None)

    def take(self, kind, run, pr=None, tag='', need=None):
        """the run publish takes the artifacts of, for the workflow to download them; a nightly's day, the one it began;
        for main's prerelease, the files it needs, each between spaces"""
        self.output = {
            'kind': kind,
            'run': str(run['id']),
            'sha': run['head_sha'],
            'short': run['head_sha'][:7],
            'pr': str(pr['number']) if pr else '',
            'tag': tag,
            'day': run['created_at'][:10] if kind == 'nightly' else '',
            'need': f' {" ".join(need)} ' if need else '',
        }
        print(f'to publish: the {kind} builds of run {run["id"]} ({run["head_sha"][:7]})')

    def tagged(self, name):
        """name is a tag of REPO (a run's head_branch is the tag a tag push pushed)"""
        # GitHub's 404 alone says it is no tag: any other failure (a token, the network) is not an answer, and the run
        # of a release is not left unpublished by it
        return bool(self.missing(f'repos/{self.repo}/git/ref/tags/{urllib.parse.quote(name)}'))

    def on_run(self, run):
        # a scheduled run builds the default branch, as a push to it; a run by hand builds the branch it was asked for
        if run['event'] in ('push', 'schedule', 'workflow_dispatch'):
            name = run['head_branch']
            if run['head_repository']['full_name'] != self.repo:
                print(f'a push to {run["head_repository"]["full_name"]}: nothing to publish')
            elif name != 'main' and not (VERSION.fullmatch(name) and self.tagged(name)):
                print(f'a push to {name}: neither main nor a version tag, nothing to publish')
            elif name == 'main' and run['event'] == 'push':  # whatever it ended as: main's prerelease says it
                self.main_plan(run)
            elif run['conclusion'] != 'success':
                print(f'run {run["id"]} of {name} ended {run["conclusion"]}: nothing published')
                if name == 'main':
                    self.missed(run)
            elif name == 'main':
                self.take('nightly', run)
            else:
                self.take('release', run, tag=name)
            return
        if run['event'] != 'pull_request':
            print(f'a run of {run["event"]}: nothing to publish')
            return
        if run['conclusion'] == 'cancelled':
            print(f'run {run["id"]} was cancelled: a newer push has its own')
            return
        pr = self.pull_of(run['head_sha'])
        if not pr:
            print(f"{run['head_sha'][:7]} is no open pull request's head: nothing to publish")
        elif self.fork(pr) and not self.approved(pr, run['created_at']):
            print(f'#{pr["number"]} is a fork\'s: it waits for the team\'s "{LABEL}" label on this push')
        else:
            self.take('pr', run, pr)

    def on_package(self, run_id):
        """a job of a Build run of main's push uploaded its package: main's prerelease takes it"""
        if not str(run_id).isdigit():
            raise RuntimeError(f'the run of a package is a run id, not {run_id!r}')
        run = self.read(f'repos/{self.repo}/actions/runs/{run_id}')
        if run['event'] != 'push' or run['head_branch'] != 'main' or run['head_repository']['full_name'] != self.repo:
            print(f'run {run_id} is a {run["event"]} of {run["head_branch"]}, not a push to main: nothing to publish')
            return
        self.main_plan(run)

    def on_pull(self, action, pr, label):
        if action == 'closed':
            self.closed(pr)
        elif action == 'synchronize' and self.fork(pr) and LABEL in {lb['name'] for lb in pr.get('labels', [])}:
            # what was let through was the push before: this one waits for the label again
            path = f'repos/{self.repo}/issues/{pr["number"]}/labels/{urllib.parse.quote(LABEL)}'
            self.gh(['api', '-X', 'DELETE', path], self.repo, write=True)
            print(f'took "{LABEL}" off #{pr["number"]}: a new push')
        elif action == 'labeled' and label == LABEL and self.fork(pr):
            path = f'repos/{self.repo}/actions/workflows/build.yml/runs?head_sha={pr["head"]["sha"]}'
            done = sorted(
                (r for r in self.pages(path, self.repo) if r['status'] == 'completed' and r['event'] == 'pull_request'),
                key=lambda r: r['created_at'],
            )
            if not done:
                print(f'#{pr["number"]}: its build has not ended; it is published when it does')
            elif self.approved(pr, done[-1]['created_at']):
                self.take('pr', done[-1], pr)
        else:
            print(f'pull request {action}: nothing to publish')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('step', choices=['plan', 'publish'])
    parser.add_argument('--repo', required=True)
    parser.add_argument('--to', required=True, help='the nightly repository')
    parser.add_argument('--kind', choices=['nightly', 'pr', 'release', 'main'])
    parser.add_argument('--run', help='the Build run the artifacts are of')
    parser.add_argument('--sha', help='the commit it built')
    parser.add_argument('--pr', type=int, help="the pull request's number")
    parser.add_argument('--tag', help="the release's version tag")
    parser.add_argument('--artifacts', help='the folder the workflow downloaded the artifacts into')
    parser.add_argument('--checkout', default='.', help="REPO's history and tags, for the notes")
    parser.add_argument('--day', help="a nightly's day, YYYY-MM-DD: the day (UTC) its run began")
    parser.add_argument('--dry-run', action='store_true', help='read, and print each write instead')
    args = parser.parse_args()
    if args.kind == 'nightly' and not (args.day and DAY.fullmatch(args.day)):
        parser.error(f'--kind nightly takes its --day as YYYY-MM-DD, not {args.day!r}')
    if args.step == 'publish' and not (args.kind and args.run and args.sha and args.artifacts):
        parser.error('publish takes --kind, --run, --sha and --artifacts')
    if args.kind == 'pr' and not args.pr:
        parser.error('--kind pr takes --pr')
    if args.kind == 'release' and not (args.tag and VERSION.fullmatch(args.tag)):
        parser.error('--kind release takes a version --tag (v...)')
    publisher = Publisher(args)
    try:
        if args.step == 'publish':
            if args.kind == 'nightly':
                publisher.nightly(args.run, args.sha, args.day)
            elif args.kind == 'release':
                publisher.version(args.run, args.sha, args.tag)
            elif args.kind == 'main':
                publisher.main_build(args.run, args.sha)
            else:
                publisher.pull(publisher.read(f'repos/{args.repo}/pulls/{args.pr}'), args.sha, args.run)
            return
        name = os.environ['GITHUB_EVENT_NAME']
        with open(os.environ['GITHUB_EVENT_PATH'], encoding='utf-8') as file:
            event = json.load(file)
        if name == 'workflow_run':
            publisher.on_run(event['workflow_run'])
        elif name == 'pull_request_target':
            publisher.on_pull(event['action'], event['pull_request'], event.get('label', {}).get('name'))
        elif name == 'workflow_dispatch':  # a package of main's push, just uploaded (build.yml)
            publisher.on_package((event.get('inputs') or {}).get('run', ''))
        else:
            print(f'{name}: nothing to publish')
        if publisher.output and os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as file:
                file.writelines(f'{key}={value}\n' for key, value in publisher.output.items())
    finally:
        shutil.rmtree(publisher.work, ignore_errors=True)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'publish-builds: {error}')
