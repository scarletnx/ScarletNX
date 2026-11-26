#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/publish-builds.py against a fake gh (a small GitHub kept in a JSON file) and a fake Discord (a local HTTP
server): a version tag's release in the repository; the nightly made in the nightly repository, kept to the newest N,
never one older than the latest, and linked from the repository's rolling prerelease; a pull request's prerelease
replaced at each push and gone when it closes; a fork's only once the team labels it
after its last push; and the writes to the nightly repository made with its own token. Each release's notes link every
download with its size and say what changed: the changes since the last one, a pull request's commits and the main
commit it stands on; a version's message, each download. No card is posted, a nightly's or a pull request's: those are
ReiWimsy's (scarletnx/wimsy-core). The fake GitHub and Discord are tests/fake_github.py's. Offline."""

import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from fake_github import FAKE_GH, posted, serve

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'publish-builds.py'
REPO, TO = 'owner/repo', 'owner/nightly'
FILES = [
    'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-linux-ppc64le.AppImage',
    'ScarletNX-linux-ppc64.AppImage',
    'ScarletNX-linux-riscv64.AppImage',
    'ScarletNX-linux-loongarch64.AppImage',
    'ScarletNX-windows-x86_64.zip',
    'ScarletNX-windows-aarch64.zip',
    'ScarletNX-android-universal.apk',
    'ScarletNX-macos-arm64.zip',
    'ScarletNX-ios-arm64.ipa',
]
ZSYNC = [
    'ScarletNX-linux-x86_64.AppImage.zsync',
    'ScarletNX-linux-aarch64.AppImage.zsync',
    'ScarletNX-linux-ppc64le.AppImage.zsync',
    'ScarletNX-linux-ppc64.AppImage.zsync',
    'ScarletNX-linux-riscv64.AppImage.zsync',
    'ScarletNX-linux-loongarch64.AppImage.zsync',
]
# what a pull request's run makes: every package, the cross AppImages too, as it tests them
NIGHTLY_ONLY = ()
PR_FILES = [name for name in FILES + ZSYNC if not any(f'-{arch}.' in name for arch in NIGHTLY_ONLY)]
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


server, HOOK = serve()

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    bin_dir, checkout = root / 'bin', root / 'checkout'
    bin_dir.mkdir()
    (bin_dir / 'gh').write_text(FAKE_GH)
    (bin_dir / 'gh').chmod(0o755)
    state_file, log_file = root / 'state.json', root / 'gh.log'

    def git(*args):
        env = dict(os.environ, GIT_COMMITTER_DATE='2026-10-15T12:00:00Z', GIT_AUTHOR_DATE='2026-10-15T12:00:00Z')
        return subprocess.run(
            ['git', '-C', str(checkout), '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
            + ['-c', 'core.hooksPath=' + os.devnull, '-c', 'tag.gpgsign=false', *args],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()

    checkout.mkdir()
    git('init', '-q', '-b', 'main')
    git('commit', '-q', '--allow-empty', '-m', 'the older nightly')
    oldest = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', 'the previous nightly')
    previous = git('rev-parse', 'HEAD')
    git('tag', 'v0.1.0')
    git('switch', '-q', '-c', 'feature')
    git('commit', '-q', '--allow-empty', '-m', '[cpu] a commit of the pull request')
    git('switch', '-q', 'main')
    git('merge', '-q', '--no-ff', 'feature', '-m', 'Merge pull request #5 from someone/feature\n\n[ci] its title')
    sha = git('rev-parse', 'HEAD')
    git('tag', 'v0.2.0')

    def save_state(state):
        state_file.write_text(json.dumps(state))

    def state():
        return json.loads(state_file.read_text())

    def releases(repo=TO, s=None):
        return (s or state())['releases'].get(repo, [])

    def find(tag, repo=TO, s=None):
        return next((r for r in releases(repo, s) if r['tag_name'] == tag), None)

    def with_releases(repo, items):
        return dict(state(), releases=dict(state()['releases'], **{repo: items}))

    def calls():
        return [json.loads(line) for line in log_file.read_text().splitlines()] if log_file.exists() else []

    def artifacts(name, commit, skip=()):
        folder = root / name
        short = commit[:7]
        made = {
            f'ScarletNX-linux-x86_64-{short}.AppImage': {
                'ScarletNX-linux-x86_64.AppImage': 'elf x86',
                'ScarletNX-linux-x86_64.AppImage.zsync': 'zsync x86',
                'appinfo': 'i',
            },
            f'ScarletNX-linux-aarch64-{short}.AppImage': {
                'ScarletNX-linux-aarch64.AppImage': 'elf arm',
                'ScarletNX-linux-aarch64.AppImage.zsync': 'zsync arm',
            },
            f'ScarletNX-linux-ppc64le-{short}.AppImage': {
                'ScarletNX-linux-ppc64le.AppImage': 'elf power',
                'ScarletNX-linux-ppc64le.AppImage.zsync': 'zsync power',
            },
            f'ScarletNX-linux-ppc64-{short}.AppImage': {
                'ScarletNX-linux-ppc64.AppImage': 'elf power be',
                'ScarletNX-linux-ppc64.AppImage.zsync': 'zsync power be',
            },
            f'ScarletNX-linux-riscv64-{short}.AppImage': {
                'ScarletNX-linux-riscv64.AppImage': 'elf riscv',
                'ScarletNX-linux-riscv64.AppImage.zsync': 'zsync riscv',
            },
            f'ScarletNX-linux-loongarch64-{short}.AppImage': {
                'ScarletNX-linux-loongarch64.AppImage': 'elf loong',
                'ScarletNX-linux-loongarch64.AppImage.zsync': 'zsync loong',
            },
            f'ScarletNX-windows-x86_64-{short}': {'bin/scarletnx.exe': 'MZ', 'share/licenses/LICENSE': 'GPL'},
            f'ScarletNX-windows-aarch64-{short}': {'bin/scarletnx.exe': 'MZ arm64', 'share/licenses/LICENSE': 'GPL'},
            f'ScarletNX-android-universal-{short}.apk': {f'ScarletNX-android-universal-{short}.apk': 'PK apk'},
            # skip-decompress: the macOS zip lands at the artifact's own path, the IPA inside its folder
            f'ScarletNX-macos-arm64-{short}.zip': {'': 'PK app'},
            f'ScarletNX-ios-arm64-{short}.ipa': {f'ScarletNX-ios-arm64-{short}.ipa': 'PK ipa'},
        }
        for artifact, files in made.items():
            for file, text in files.items():
                if any(part in f'{artifact}/{file}' for part in skip):
                    continue
                (folder / artifact / file).parent.mkdir(parents=True, exist_ok=True)
                (folder / artifact / file).write_text(text)
        return folder

    def run(step, *extra, event=None, name='workflow_run', hooks=True, env_extra=None):
        event_file, output = root / 'event.json', root / 'output'
        event_file.write_text(json.dumps(event or {}))
        output.write_text('')
        env = dict(
            os.environ,
            PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            FAKE_GH_STATE=str(state_file),
            FAKE_GH_LOG=str(log_file),
            GH_TOKEN='repo-token',
            NIGHTLY_TOKEN='nightly-token',
            GITHUB_EVENT_NAME=name,
            GITHUB_EVENT_PATH=str(event_file),
            GITHUB_OUTPUT=str(output),
        )
        for kind in ('RELEASES', 'NIGHTLY', 'PRS'):
            env.pop(f'DISCORD_WEBHOOK_{kind}', None)
        if hooks:
            env.update(
                DISCORD_WEBHOOK_RELEASES=f'{HOOK}-releases',
                DISCORD_WEBHOOK_NIGHTLY=f'{HOOK}-nightly',
                DISCORD_WEBHOOK_PRS=f'{HOOK}-prs?thread_id=55',
            )
        env.update(env_extra or {})
        done = subprocess.run(
            [sys.executable, str(SCRIPT), step, '--repo', REPO, '--to', TO, *extra],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        planned = dict(line.split('=', 1) for line in output.read_text().splitlines())
        return done, planned

    def push_run(commit, conclusion='success', branch='main', number=1, event='push'):
        return {
            'workflow_run': {
                'id': number,
                'event': event,
                'head_branch': branch,
                'head_sha': commit,
                'conclusion': conclusion,
                'created_at': '2026-10-16T10:00:00Z',
                'head_repository': {'full_name': REPO},
            }
        }

    def nightly_release(commit, created, number):
        return {
            'id': number,
            'tag_name': f'nightly-2026-10-15-{commit[:7]}',
            'name': 'n',
            'created_at': '2026-10-01T00:00:00Z',  # GitHub's: the tag's commit's date, which every nightly shares
            'published_at': created,
            'body': f'Built from [`{commit[:7]}`](https://github.com/{REPO}/commit/{commit}).',
            'assets': [],
            'prerelease': False,
        }

    def everything(commit, run_id='41'):
        return ['--run', run_id, '--sha', commit, '--artifacts', str(artifacts('full', commit))]

    def publish(kind, *extra, **options):
        return run('publish', '--kind', kind, *extra, **options)[0]

    # ---- nightlies: in the nightly repository, linked from the rolling prerelease
    rolling = {
        'id': 9,
        'tag_name': 'nightly',
        'name': 'Nightly',
        'created_at': '2026-09-01',
        'body': 'old',
        'prerelease': True,
        'assets': [{'id': 7, 'name': 'ScarletNX-x86_64.AppImage', 'data': ''}],
    }
    save_state(
        {
            'next': 100,
            'clock': 0,
            'pulls': [],
            'events': {},
            'permissions': {},
            'runs': [],
            # the pull request each commit came in by: the merge commit #5's, merged into main; another, never merged
            'associated': {
                sha: [
                    {'number': 9, 'title': '[ci] closed', 'merged': False, 'baseRefName': 'main', 'author': None},
                    {
                        'number': 5,
                        'title': '[ci] its title',
                        'merged': True,
                        'baseRefName': 'main',
                        'author': {'login': 'someone'},
                    },
                ]
            },
            'tags': {
                TO: {f'nightly-2026-10-15-{c[:7]}': 'x' for c in (oldest, previous)},
                REPO: {'nightly': oldest, 'v0.1.0': previous, 'v0.2.0': sha},
            },
            'releases': {
                TO: [nightly_release(oldest, '2026-10-01', 1), nightly_release(previous, '2026-10-02', 2)],
                REPO: [rolling],
            },
        }
    )
    done, planned = run('plan', event=push_run(sha, number=41))
    check(
        'a green push to main is planned as a nightly',
        planned == {'kind': 'nightly', 'run': '41', 'sha': sha, 'short': sha[:7], 'pr': '', 'tag': ''},
        planned,
    )
    done, planned = run('plan', event=push_run(sha, number=44, event='schedule'))
    check(
        'a green scheduled run of main is planned as a nightly',
        planned == {'kind': 'nightly', 'run': '44', 'sha': sha, 'short': sha[:7], 'pr': '', 'tag': ''},
        planned,
    )
    done, planned = run('plan', event=push_run(sha, number=45, event='workflow_dispatch'))
    check(
        'a green run of main by hand is planned as a nightly',
        planned == {'kind': 'nightly', 'run': '45', 'sha': sha, 'short': sha[:7], 'pr': '', 'tag': ''},
        planned,
    )
    for event, why in (
        (push_run(sha, 'failure', event='schedule'), 'a failed scheduled run'),
        (push_run(sha, branch='dev', event='workflow_dispatch'), 'a run by hand of another branch'),
        (push_run(sha, 'failure'), 'a failed run'),
        (push_run(sha, branch='dev'), 'another branch'),
        (push_run(sha, branch='v9.9.9'), 'a branch named like a version, no tag'),
    ):
        done, planned = run('plan', event=event)
        check(f'{why} is not planned', not planned and done.returncode == 0, done.stdout + done.stderr)
    log_file.unlink(missing_ok=True)
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check('the nightly publishes', done.returncode == 0, done.stdout + done.stderr)
    s = state()
    tag = f'nightly-2026-10-15-{sha[:7]}'
    made = find(tag, s=s)
    check(
        'the release made, the latest, not a prerelease',
        made and made['make_latest'] == 'true' and not made['prerelease'],
        made,
    )
    check(
        "its files, and the AppImages' .zsync",
        made and sorted(a['name'] for a in made['assets']) == sorted(FILES + ZSYNC),
        made and made['assets'],
    )
    check(
        'its notes: the files linked from this release, the changes, the source',
        made
        and f'https://github.com/{TO}/releases/download/{tag}/ScarletNX-android-universal.apk' in made['body']
        and f'**ci**\n\n- its title ([#5](https://github.com/{REPO}/pull/5), 1 commit, by someone)' in made['body']
        and '#9' not in made['body']
        and f'https://github.com/{REPO}/tree/{sha}' in made['body']
        and 'newest 2 nightlies' in made['body']
        and f'[`{previous[:7]}...{sha[:7]}`]' in made['body']
        and '.AppImage.zsync' not in made['body'],
        made and made['body'],
    )
    data = {a['name']: a['data'] for a in made['assets']} if made else {}
    archive = data.get('ScarletNX-windows-x86_64.zip', '')
    check('the Windows build zipped from its folder', archive.startswith('PK') and 'bin/scarletnx.exe' in archive)
    check(
        'the macOS app and the iOS IPA as their jobs made them, not zipped again',
        data.get('ScarletNX-macos-arm64.zip') == 'PK app' and data.get('ScarletNX-ios-arm64.ipa') == 'PK ipa',
        {k: v[:20] for k, v in data.items()},
    )
    check(
        'only the newest 2 kept, the oldest and its tag gone',
        sorted(r['tag_name'] for r in releases(s=s)) == sorted([tag, f'nightly-2026-10-15-{previous[:7]}'])
        and f'nightly-2026-10-15-{oldest[:7]}' not in s['tags'][TO],
        s['tags'][TO],
    )
    check(
        'its downloads by system, each with its size (bytes as megabytes)',
        made
        and f'| **Windows** | x86-64 | [zip](https://github.com/{TO}/releases/download/{tag}/ScarletNX-windows-x86_64.zip) |'
        in made['body']
        and '| **Linux** | x86-64 | [AppImage](' in made['body']
        and ' | 0.0 MB |' in made['body'],
        made and made['body'],
    )
    linked = find('nightly', REPO, s)
    check(
        "the rolling prerelease: no files, a link to the latest, its tag moved there, the latest's downloads and changes",
        linked
        and not linked['assets']
        and linked['prerelease']
        and f'https://github.com/{TO}/releases/tag/{tag}' in linked['body']
        and f'[APK](https://github.com/{TO}/releases/download/{tag}/ScarletNX-android-universal.apk)' in linked['body']
        and f'([#5](https://github.com/{REPO}/pull/5), 1 commit, by someone)' in linked['body']
        and s['tags'][REPO]['nightly'] == sha,
        linked,
    )
    writes = [c for c in calls() if '-X' in c['args'] or c['args'][0] == 'release']
    to_writes = [c for c in writes if TO in ' '.join(c['args'])]
    repo_writes = [c for c in writes if REPO in ' '.join(c['args'])]
    check(
        'the nightly repository written with its token, the repository with its own',
        to_writes
        and repo_writes
        and all(c['token'] == 'nightly-token' for c in to_writes)
        and all(c['token'] == 'repo-token' for c in repo_writes),
        writes,
    )
    check(
        "no message on the nightly channel, its webhook set: its card is ReiWimsy's",
        posted('nightly') == [],
        posted('nightly'),
    )
    # the run again: the files replaced, no news
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check(
        'a run again replaces, posting nothing',
        done.returncode == 0 and posted('nightly') == [] and len(releases()) == 2,
        done.stdout + done.stderr,
    )
    # GitHub not answering which pull request merged a commit: the nightly stops, saying what gh said
    save_state(dict(state(), graphql_fail='gh: HTTP 502: Server Error'))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check(
        "the pull requests unread stop the nightly with GitHub's answer",
        done.returncode != 0 and 'HTTP 502' in done.stderr,
        done.stdout + done.stderr,
    )
    save_state({k: v for k, v in state().items() if k != 'graphql_fail'})
    check(
        "the commits' pull requests asked in one query",
        state().get('graphql_asked') and all(n == 1 for n in state()['graphql_asked']),
        state().get('graphql_asked'),
    )
    # a run of an older commit ending last: not published over the newest
    older_files = ['--run', '40', '--sha', oldest, '--artifacts', str(artifacts('old', oldest))]
    done = publish('nightly', *older_files, '--checkout', str(checkout))
    check(
        'an older commit is not published',
        done.returncode == 0
        and 'older than the newest nightly' in done.stdout
        and all(oldest[:7] not in r['tag_name'] for r in releases()),
        done.stdout + done.stderr,
    )
    # a nightly without one of its packages, or an AppImage without its .zsync, stops, saying which
    for skip, said in (('ScarletNX-windows', 'Windows, x86-64'), ('AppImage.zsync', 'has no ScarletNX-linux')):
        partial = ['--run', '42', '--sha', sha, '--artifacts', str(artifacts(f'part-{len(skip)}', sha, (skip,)))]
        done = publish('nightly', *partial, '--checkout', str(checkout))
        check(f'a nightly missing {skip} stops', done.returncode != 0 and said in done.stderr, done.stderr)
    # macOS and iOS may fail without failing the run (continue-on-error): the nightly goes out without them and says so
    apple = [
        '--run',
        '43',
        '--sha',
        sha,
        '--artifacts',
        str(artifacts('part-apple', sha, ('ScarletNX-macos', 'ScarletNX-ios'))),
    ]
    done = publish('nightly', *apple, '--checkout', str(checkout))
    made = find(tag)
    check(
        'a nightly without the macOS and iOS packages publishes the rest, saying those were not built',
        done.returncode == 0
        and made
        and not any(a['name'].startswith(('ScarletNX-macos', 'ScarletNX-ios')) for a in made['assets'])
        and '| **macOS** | ARM64 | not built, [see the run](https://github.com/owner/repo/actions/runs/43) |  |'
        in made['body']
        and '| **iOS** | ARM64 | not built' in made['body'],
        done.stderr + (made['body'] if made else ''),
    )
    # dry run: reads only
    log_file.unlink(missing_ok=True)
    save_state(with_releases(TO, [r for r in releases() if r['tag_name'] != tag]))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--dry-run')
    check(
        '--dry-run writes nothing',
        done.returncode == 0
        and not any('-X' in c['args'] or c['args'][0] == 'release' for c in calls())
        and 'would run: gh api -X POST' in done.stdout,
        done.stdout + done.stderr,
    )
    # no rolling prerelease yet: made, on the commit
    save_state(with_releases(REPO, []))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout))
    linked = find('nightly', REPO)
    check(
        'a rolling prerelease made when there is none',
        done.returncode == 0 and linked and linked['target_commitish'] == sha and linked['prerelease'],
        done.stdout + done.stderr,
    )

    # ---- a version tag: the repository's release
    first = {
        'id': 50,
        'tag_name': 'v0.1.0',
        'name': 'ScarletNX v0.1.0',
        'created_at': '2026-10-01',
        'published_at': '2026-10-01',
        'body': '',
        'prerelease': False,
        'assets': [],
    }
    save_state(with_releases(REPO, [*releases(REPO), first]))
    done, planned = run('plan', event=push_run(sha, branch='v0.2.0', number=43))
    check(
        'a version tag pushed is planned as a release',
        planned.get('kind') == 'release' and planned.get('tag') == 'v0.2.0',
        done.stdout + done.stderr,
    )
    done = publish('release', '--tag', 'v0.2.0', *everything(sha), '--checkout', str(checkout))
    version = find('v0.2.0', REPO)
    check(
        "the repository's release: the latest, on the commit, its files and their .zsync",
        done.returncode == 0
        and version
        and version['make_latest'] == 'true'
        and not version['prerelease']
        and version['target_commitish'] == sha
        and sorted(a['name'] for a in version['assets']) == sorted(FILES + ZSYNC),
        done.stdout + done.stderr,
    )
    check(
        'its notes: the changes since the previous version, its files',
        version
        and '## Changes since v0.1.0' in version['body']
        and version['body'].startswith('ScarletNX v0.2.0, built from [`')
        and 'AppImageUpdate takes it to the newest version.' in version['body']
        and f'- its title ([#5](https://github.com/{REPO}/pull/5), 1 commit, by someone)' in version['body']
        and f'https://github.com/{REPO}/releases/download/v0.2.0/ScarletNX-windows-x86_64.zip' in version['body'],
        version and version['body'],
    )
    sent = posted('releases')
    check(
        'one message on the releases channel, its downloads, the source, and since which version',
        len(sent) == 1
        and sent[0]['body']['embeds'][0]['title'] == 'ScarletNX v0.2.0'
        and [f['name'] for f in sent[0]['body']['embeds'][0]['fields']][len(FILES) :] == ['Source', 'Since']
        and '/compare/v0.1.0...v0.2.0' in sent[0]['body']['embeds'][0]['fields'][len(FILES) + 1]['value'],
        sent,
    )
    git('tag', 'v0.3.0-rc1')
    done = publish('release', '--tag', 'v0.3.0-rc1', *everything(sha), '--checkout', str(checkout))
    candidate = find('v0.3.0-rc1', REPO)
    check(
        'a tag with a "-" is a prerelease, never the latest',
        candidate and candidate['prerelease'] and candidate['make_latest'] == 'false',
        done.stdout + done.stderr,
    )
    done = publish('release', '--tag', 'main', *everything(sha))
    check('--kind release refuses a tag that is no version', done.returncode == 2, done.stderr)

    # ---- a pull request of the repository
    def pull(number, head, fork=False, labels=()):
        return {
            'number': number,
            'state': 'open',
            'title': 'Fix it @everyone',
            'html_url': f'https://x/pull/{number}',
            'user': {'login': 'alice', 'html_url': 'https://x/alice', 'avatar_url': 'https://x/alice.png'},
            'labels': [{'name': n} for n in labels],
            'merged': False,
            'base': {'sha': 'f' * 40},
            'head': {'sha': head, 'repo': {'full_name': 'alice/fork' if fork else REPO}},
        }

    def pr_run(head, number=50, created='2026-10-16T10:00:00Z', failed=('Windows (Clang + LLVM)',)):
        jobs = [
            'Linux AppImage (x86_64)',
            'Linux AppImage (aarch64)',
            'Linux AppImage (ppc64le)',
            'Linux AppImage (ppc64)',
            'Linux AppImage (riscv64)',
            'Linux AppImage (loongarch64)',
            'Windows (Clang + LLVM)',
            'Android APK',
        ]
        return {
            'id': number,
            'event': 'pull_request',
            'head_branch': 'x',
            'head_sha': head,
            'conclusion': 'failure' if failed else 'success',
            'status': 'completed',
            'created_at': created,
            'html_url': f'https://x/runs/{number}',
            'head_repository': {'full_name': REPO},
            'jobs': [
                {
                    'name': job,
                    'status': 'completed',
                    'conclusion': 'failure' if job in failed else 'success',
                    'html_url': f'https://x/runs/{number}/{job[:5]}',
                }
                for job in jobs
            ],
        }

    def pr_files(head, name, run_id, skip=()):
        made = artifacts(name, head, (*NIGHTLY_ONLY, *skip))
        return ['--run', run_id, '--sha', head, '--pr', '7', '--artifacts', str(made)]

    head1, head2 = 'a' * 40, 'b' * 40
    base_sha = 'e' * 40
    commits = [
        {'sha': f'{n:x}' * 40, 'commit': {'message': f'[gui] commit {n} [x](http://e)\n\nIts body.'}} for n in range(12)
    ]
    save_state(dict(state(), pulls=[pull(7, head1)], commits={'7': commits}, merge_base=base_sha, runs=[pr_run(head1)]))
    done, planned = run('plan', event={'workflow_run': pr_run(head1)})
    check('a pull request run is planned', planned.get('kind') == 'pr' and planned.get('pr') == '7', planned)
    done = publish('pr', *pr_files(head1, 'pr1-part', '50', ('ScarletNX-windows',)))
    pre = find('pr-7')
    check(
        'its prerelease, never the latest, titled PR #N',
        pre and pre['prerelease'] and pre['make_latest'] == 'false' and pre['name'] == 'PR #7: Fix it @everyone',
        pre,
    )
    check(
        'what was not built said in its row, linking the run',
        pre
        and '| **Windows** | x86-64 | not built, [see the run](https://github.com/owner/repo/actions/runs/50) |  |'
        in pre['body']
        and '| **Windows** | ARM64 | not built, [see the run](https://github.com/owner/repo/actions/runs/50) |  |'
        in pre['body']
        and '- **Windows**' not in pre['body']
        and len(pre['assets']) == len(PR_FILES) - 2,
        pre,
    )
    check(
        'a row for every cross AppImage, made by a pull request as by a nightly, the big-endian PowerPC64 one too',
        pre
        and all(f'| {arch} |' in pre['body'] for arch in ('PowerPC64 LE', 'PowerPC64 BE', 'RISC-V 64', 'LoongArch64'))
        and '| **Linux** | x86-64 |' in pre['body'],
        pre and pre['body'],
    )
    check(
        'its notes: the pull request, its author, its commit and the main commit it stands on, its commits by subject',
        pre
        and pre['body'].startswith('Test builds of [#7](https://x/pull/7) by [@alice](https://x/alice), at [`aaaaaaa`]')
        and f'on main at [`eeeeeee`](https://github.com/{REPO}/commit/{base_sha}).' in pre['body']
        and '12 commits, the newest first:' in pre['body']
        and "takes it to the newest nightly, not this pull request's next builds." in pre['body']
        and pre['body'].index('commit 11 ') < pre['body'].index('commit 0 ')
        and f'- \\[gui\\] commit 0 \\[x\\](http://e) ([`0000000`](https://github.com/{REPO}/pull/7/commits/{"0" * 40}))'
        in pre['body']
        and 'Its body' not in pre['body'],
        pre and pre['body'],
    )
    check(
        "no message on the pull requests channel, its webhook set: its card is ReiWimsy's",
        posted('prs') == [],
        posted('prs'),
    )
    # a push: the same prerelease, all four now, the message edited; it stands on a commit a nightly was built from
    save_state(dict(state(), pulls=[pull(7, head2)], merge_base=sha, runs=[pr_run(head2, 51, failed=())]))
    done = publish('pr', *pr_files(head2, 'pr2', '51'))
    pre = find('pr-7')
    check(
        'the main commit it stands on links its nightly, to set the two side by side',
        pre
        and f'on main at [`{sha[:7]}`](https://github.com/{REPO}/commit/{sha}) '
        f'([its nightly](https://github.com/{TO}/releases/tag/nightly-2026-10-15-{sha[:7]})).'
        in pre['body'],
        pre and pre['body'],
    )
    check(
        'the push replaces it',
        pre
        and sorted(a['name'] for a in pre['assets']) == sorted(PR_FILES)
        and head2[:7] in pre['body']
        and 'not built' not in pre['body'],
        pre,
    )
    # the run of the push before, ending last
    done = publish('pr', *pr_files(head1, 'pr1', '50'))
    check(
        "an older push's builds are not published",
        'are not published' in done.stdout and head2[:7] in find('pr-7')['body'],
        done.stdout,
    )
    # merged into a commit no nightly has yet: its builds stay, the change's only download until a nightly has it
    git('switch', '-q', '-c', 'after', sha)
    git('commit', '-q', '--allow-empty', '-m', 'merged after the newest nightly')
    later = git('rev-parse', 'HEAD')
    git('switch', '-q', 'main')
    early = dict(pull(7, head2), state='closed', merged=True, merge_commit_sha=later)
    done, planned = run(
        'plan',
        '--checkout',
        str(checkout),
        name='pull_request_target',
        event={'action': 'closed', 'pull_request': early},
    )
    check(
        'merged into a commit no nightly has: its prerelease stays, said',
        find('pr-7') and 'stays until a nightly has it' in done.stdout and not planned,
        done.stdout + done.stderr,
    )
    # merged into a commit the newest nightly has: gone
    closed = dict(pull(7, head2), state='closed', merged=True, merge_commit_sha=sha)
    before = len(posted('prs'))
    done, planned = run(
        'plan',
        '--checkout',
        str(checkout),
        name='pull_request_target',
        event={'action': 'closed', 'pull_request': closed},
    )
    s = state()
    check(
        "merged into the newest nightly's commit: its prerelease and tag gone, saying why",
        'pr-7' not in s['tags'][TO]
        and not find('pr-7', s=s)
        and not planned
        and f'#7 merged as {sha[:7]}, which the nightly of {sha[:7]} has' in done.stdout,
        done.stdout + done.stderr,
    )
    check('closing it posts nothing here', len(posted('prs')) == before, posted('prs')[before:])
    # closed without a merge: gone at once
    save_state(dict(state(), pulls=[pull(8, head1)]))
    publish('pr', '--run', '53', '--sha', head1, '--pr', '8', '--artifacts', str(artifacts('pr8', head1)))
    shut = dict(pull(8, head1), state='closed')
    done, planned = run(
        'plan',
        '--checkout',
        str(checkout),
        name='pull_request_target',
        event={'action': 'closed', 'pull_request': shut},
    )
    check(
        'closed without a merge: its prerelease gone at once',
        not find('pr-8') and '#8 closed without a merge' in done.stdout,
        done.stdout + done.stderr,
    )

    # ---- the sweep, as a nightly publishes: the builds a close's run left behind
    def pr_release(number):
        return {
            'id': 900 + number,
            'tag_name': f'pr-{number}',
            'name': f'PR #{number}',
            'created_at': '2026-10-01T00:00:00Z',
            'published_at': '2026-10-03',
            'body': '',
            'assets': [],
            'prerelease': True,
        }

    left = {
        20: dict(pull(20, head1)),
        21: dict(pull(21, head1), state='closed'),
        22: dict(pull(22, head1), state='closed', merged=True, merge_commit_sha=sha),
        23: dict(pull(23, head1), state='closed', merged=True, merge_commit_sha=later),
    }
    s = state()
    s['releases'][TO] += [pr_release(n) for n in left]
    s['tags'][TO].update({f'pr-{n}': 'x' for n in left})
    save_state(dict(s, pulls=list(left.values())))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '5')
    swept = done.stdout
    check(
        "the sweep lists the closed and the merged-and-built pull requests' builds, then deletes each, saying why",
        done.returncode == 0
        and "to delete, 2 pull requests' builds: pr-21 (#21 closed without a merge); pr-22 (#22 merged as" in swept
        and swept.index('to delete') < swept.index('deleted pr-21')
        and 'deleted pr-22: #22 merged as' in swept,
        swept + done.stderr,
    )
    check(
        'the open one and the one merged past the nightly stay, with their tags',
        find('pr-20')
        and find('pr-23')
        and not find('pr-21')
        and not find('pr-22')
        and {'pr-20', 'pr-23'} <= set(state()['tags'][TO])
        and 'pr-21' not in state()['tags'][TO],
        [r['tag_name'] for r in releases()],
    )
    save_state(dict(state(), pull_errors={'20': 'gh: HTTP 502: Server Error'}))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '5')
    check(
        'a pull request GitHub cannot answer for stops the sweep with its answer, deleting nothing',
        done.returncode != 0 and 'HTTP 502' in done.stderr and find('pr-20') and find('pr-23'),
        [r['tag_name'] for r in releases()] + [done.stdout + done.stderr],
    )
    save_state({k: v for k, v in state().items() if k != 'pull_errors'})

    # ---- a fork's pull request
    def labeled(login, at):
        return {'event': 'labeled', 'label': {'name': 'publish builds'}, 'actor': {'login': login}, 'created_at': at}

    fork_head = 'c' * 40
    base = dict(
        state(),
        pulls=[pull(9, fork_head, fork=True)],
        permissions={'bob': 'write', 'eve': 'read'},
        runs=[pr_run(fork_head, 60)],
    )
    save_state(base)
    done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
    check("a fork's run without the label is not planned", not planned and 'waits for' in done.stdout, done.stdout)
    for who, at, why in (
        ('eve', '2026-10-16T11:00:00Z', 'by someone not of the team'),
        ('bob', '2026-10-16T09:00:00Z', 'before its last push'),
    ):
        save_state(dict(base, pulls=[pull(9, fork_head, True, ['publish builds'])], events={'9': [labeled(who, at)]}))
        done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
        check(f'a label {why} lets nothing through', not planned, done.stdout)
    approved = [labeled('bob', '2026-10-16T11:00:00Z')]
    save_state(dict(base, pulls=[pull(9, fork_head, True, ['publish builds'])], events={'9': approved}))
    done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
    check("the team's label after the push lets it through", planned.get('pr') == '9', done.stdout + done.stderr)
    event = {
        'action': 'labeled',
        'label': {'name': 'publish builds'},
        'pull_request': pull(9, fork_head, True, ['publish builds']),
    }
    done, planned = run('plan', name='pull_request_target', event=event)
    check('the label put on a finished run publishes it', planned.get('run') == '60', done.stdout + done.stderr)
    save_state(dict(state(), runs=[]))
    done, planned = run('plan', name='pull_request_target', event=event)
    check(
        'the label put while it builds waits for the run',
        not planned and 'has not ended' in done.stdout,
        done.stdout,
    )
    log_file.unlink(missing_ok=True)
    done, planned = run('plan', name='pull_request_target', event=dict(event, action='synchronize'))
    took = [c for c in calls() if c['args'][:3] == ['api', '-X', 'DELETE']]
    check(
        "a fork's push takes the label off, with the repository's token",
        took
        and took[0]['token'] == 'repo-token'
        and took[0]['args'][3].endswith('/labels/publish%20builds')
        and not state()['pulls'][0]['labels'],
        took,
    )

    # ---- no token for the nightly repository: it fails, saying which
    done, _ = run(
        'plan',
        name='pull_request_target',
        event={'action': 'closed', 'pull_request': closed},
        env_extra={'NIGHTLY_TOKEN': ''},
    )
    check(
        'without NIGHTLY_TOKEN it stops, naming it',
        done.returncode != 0 and 'NIGHTLY_TOKEN is not set' in done.stderr,
        done.stdout + done.stderr,
    )

    # ---- no webhook: published, and said
    save_state(dict(state(), pulls=[pull(8, head1)]))
    unhooked = ['--run', '52', '--sha', head1, '--pr', '8', '--artifacts', str(artifacts('pr1', head1))]
    done = publish('pr', *unhooked, hooks=False)
    check(
        'without the webhooks it publishes, a pull request asking for none',
        done.returncode == 0 and 'DISCORD_WEBHOOK' not in done.stdout and find('pr-8'),
        done.stdout + done.stderr,
    )

server.shutdown()

# the packages a pull request's notes leave out are those build.yml's appimage-cross makes for a nightly's run alone:
# every leg where a pull request does not run the job, else the legs a pull request's run has not, and the others too
# where it skips their bundle
spec = importlib.util.spec_from_file_location('publish_builds', SCRIPT)
publish_builds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish_builds)


# whether a pushed name is a tag: GitHub's 404 says it is none, and a failure of another kind is raised, not read as
# "no tag" (which left a release's run unpublished without a word)
def tag_answer(outcome):
    hub = object.__new__(publish_builds.Publisher)
    hub.repo = 'scarletnx/ScarletNX'

    def read(path, repo=None):
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    hub.read = read
    try:
        return hub.tagged('v1.2.3')
    except RuntimeError as error:
        return str(error)


check('a ref GitHub has is a tag', tag_answer({'ref': 'refs/tags/v1.2.3'}) is True)
check('a 404 is no tag', tag_answer(RuntimeError('gh: Not Found (HTTP 404)')) is False)
check(
    'a failed lookup is raised, not taken as no tag',
    tag_answer(RuntimeError('gh: Bad credentials (HTTP 401)')) == 'gh: Bad credentials (HTTP 401)',
)
workflow = (SCRIPT.parent.parent / '.github' / 'workflows' / 'build.yml').read_text()
cross = workflow[workflow.index('\n  appimage-cross:\n') :]
head = cross[: cross.index('\n    steps:\n')]
job_if = re.search(r'\n    if: >-\n((?:      .*\n)+)', head)
skips_pulls = bool(job_if) and "github.event_name != 'pull_request'" in job_if[1]
legs = re.search(r"github\.event_name == 'pull_request' && '(\[.*?\])' \|\| '(\[.*?\])'", cross, re.S)
listed = re.findall(r'\n          - \{arch: ([\w-]+),', head)
check(
    "build.yml's appimage-cross has a pull request's legs and the others, or its legs listed for every event",
    legs or listed,
    cross[:400],
)
bundle = re.search(r'\n      - name: Bundle the AppImage\n        if: ([^\n]*)\n', cross)
if legs or listed:
    if legs:
        pull_arches, every_arch = ({leg['arch'] for leg in json.loads(text)} for text in legs.groups())
    else:
        pull_arches, every_arch = (set() if skips_pulls else set(listed)), set(listed)
    pull_made = set() if bundle and bundle[1] == "github.event_name != 'pull_request'" else pull_arches
    expected = {f'ScarletNX-linux-{arch}.AppImage' for arch in every_arch - pull_made}
    check(
        "NIGHTLY_ONLY is what build.yml's appimage-cross makes for a nightly's run and not a pull request's",
        getattr(publish_builds, 'NIGHTLY_ONLY', None) == expected and set(NIGHTLY_ONLY) == every_arch - pull_made,
        f'NIGHTLY_ONLY {sorted(getattr(publish_builds, "NIGHTLY_ONLY", ()))}, build.yml {sorted(expected)}',
    )

# what the script publishes is what publish.yml downloads from the run, each artifact by the name the script looks
# for and into the folder of that name
publish = (SCRIPT.parent.parent / '.github' / 'workflows' / 'publish.yml').read_text()
short = '${{ steps.plan.outputs.short }}'
downloads = dict(re.findall(r'\n          name: ([^\n]+)\n          path: artifacts/([^\n]+)\n', publish))
wanted = {name.format(short=short) for name in publish_builds.ARTIFACTS}
check(
    'publish.yml downloads every artifact publish-builds.py publishes, each into the folder of its name',
    wanted <= set(downloads) and all(downloads[name] == name for name in wanted & set(downloads)),
    f'not downloaded: {sorted(wanted - set(downloads))}; elsewhere: '
    f'{sorted(name for name in wanted & set(downloads) if downloads[name] != name)}',
)


def pull_skipped(text):
    """the artifacts publish.yml leaves out of a pull request's download, by an if: of the plan's kind"""
    out = set()
    for step in re.finditer(r'^      - name: Download [^\n]*\n((?:        .*\n)+)', text, re.M):
        cond = re.search(r'^        if: (.*)$', step[1], re.M)
        name = re.search(r'^          name: (.*)$', step[1], re.M)
        if cond and name and "kind != 'pr'" in cond[1]:
            out.add(name[1])
    return out


# a package a pull request's run makes and its download skips reads in the prerelease's notes as not built
nightly_only = {
    name.format(short=short)
    for name, package in publish_builds.ARTIFACTS.items()
    if package in publish_builds.NIGHTLY_ONLY
}
check(
    "publish.yml downloads for a pull request every package but NIGHTLY_ONLY's",
    pull_skipped(publish) == nightly_only,
    f'skipped for a pull request: {sorted(pull_skipped(publish))}, NIGHTLY_ONLY: {sorted(nightly_only)}',
)
check(
    'a download skipped for a pull request is seen',
    pull_skipped(
        publish.replace(
            '      - name: Download the RISC-V 64 AppImage\n        if: steps.plan.outputs.run\n',
            "      - name: Download the RISC-V 64 AppImage\n        if: steps.plan.outputs.run && steps.plan.outputs.kind != 'pr'\n",
        )
    )
    == {f'ScarletNX-linux-riscv64-{short}.AppImage'},
    pull_skipped(publish),
)


# the jobs whose package runs as a user would on every event: seconds each, on what main's push and the nightly
# publish. appimage-cross's runs through the qemu handlers its tests install, where SNX_TESTS
EVERY_EVENT = ('appimage', 'windows')


def installed_checks(text):
    """{job id: what is wrong with when it runs} of build.yml's checks of the packages as installed, the steps named
    "Run ... as a user would" at the end of the jobs that make them: EVERY_EVENT's with no if:, the others where
    SNX_TESTS, a pull request, a run by hand and a version's tag, whose release waits on them"""
    out = {}
    head, jobs = text[: text.index('\njobs:\n')], text[text.index('\njobs:\n') :]
    for block in re.split(r'\n(?=  [\w-]+:\n)', jobs)[1:]:
        for step in re.finditer(r'^      - name: Run [^\n]* as a user would\n((?:        .*\n)*)', block + '\n', re.M):
            cond = re.search(r'^        if: (.*)$', step[1], re.M)
            cond = cond[1] if cond else ''
            job = block.split(':', 1)[0].strip()
            wrong = []
            if job in EVERY_EVENT:
                if cond:
                    wrong.append(
                        f"runs if {cond}, not on every event: main's push and the nightly publish what it checks"
                    )
            else:
                if "env.SNX_TESTS == 'true'" not in cond:
                    wrong.append(f"runs on main's push and the nightly: {cond}")
                if 'event_name' in cond or 'refs/' in cond:
                    wrong.append(f'picks its events apart from SNX_TESTS: {cond}')
            out.setdefault(job, []).extend(wrong)
    if "tags: ['v*']" not in head:
        out['on'] = ["a version's tag starts no Build"]
    tests = re.search(r'\n  SNX_TESTS: >-\n((?:    .*\n)+)', head)
    if not tests or "startsWith(github.ref, 'refs/tags/')" not in tests[1]:
        out['env'] = ["a version's tag does not test"]
    return out


got = installed_checks(workflow)
check(
    "the checks as installed, in the jobs that make the packages: the AppImage's and the zip's on every event, the cross"
    " AppImages' on a pull request, a run by hand and a version's tag",
    sorted(got) == ['appimage', 'appimage-cross', 'windows'] and not any(got.values()),
    got,
)
for what, copy in {
    'on the testing events alone': workflow.replace(
        '      - name: Run the AppImage as a user would\n        uses:',
        "      - name: Run the AppImage as a user would\n        if: env.SNX_TESTS == 'true'\n        uses:",
    ),
    "on main's pushes alone": workflow.replace(
        "      - name: Run the zip's scarletnx.exe as a user would\n",
        "      - name: Run the zip's scarletnx.exe as a user would\n"
        "        if: github.event_name != 'pull_request' && github.ref == 'refs/heads/main'\n",
    ),
    "the cross AppImages' on main's pushes too": workflow.replace(
        "      - name: Run the AppImage as a user would\n        if: env.SNX_TESTS == 'true'\n",
        '      - name: Run the AppImage as a user would\n',
    ),
    'without the tags': workflow.replace("tags: ['v*']", 'tags: []'),
    "on no version's tag": workflow.replace(" ||\n    startsWith(github.ref, 'refs/tags/') }}", ' }}', 1),
}.items():
    check(
        f'a build.yml whose checks as installed run {what} is named wrong',
        copy != workflow and any(installed_checks(copy).values()),
        what,
    )

# the pull requests of many commits, asked 50 at a time, each commit by its own alias, only one merged into main kept
spec = importlib.util.spec_from_file_location('publish_builds', SCRIPT)
publish_builds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish_builds)
with tempfile.TemporaryDirectory() as temporary:
    history = Path(temporary)

    def commit_all(*args):
        return subprocess.run(
            ['git', '-C', str(history), '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
            + ['-c', 'core.hooksPath=' + os.devnull, *args],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    commit_all('init', '-q', '-b', 'main')
    commit_all('commit', '-q', '--allow-empty', '-m', 'base')
    base = commit_all('rev-parse', 'HEAD')
    for n in range(120):
        commit_all('commit', '-q', '--allow-empty', '-m', f'[cpu] commit {n}')
    head = commit_all('rev-parse', 'HEAD')
    asked = []

    def answer(query):
        aliases = re.findall(r'(c\d+): object\(oid: "([0-9a-f]+)"\)', query)
        asked.append(len(aliases))
        nodes = {
            alias: {
                'associatedPullRequests': {
                    'nodes': [
                        {'number': 3, 'title': 'into a stack', 'merged': True, 'baseRefName': 'other'},
                        {'number': 4, 'title': '[cpu] in', 'merged': True, 'baseRefName': 'main', 'author': None},
                    ]
                }
            }
            for alias, _ in aliases
        }
        return {'data': {'repository': nodes}}

    hub = object.__new__(publish_builds.Publisher)
    hub.repo, hub.checkout, hub.graphql = 'owner/repo', str(history), answer
    found = hub.pulls(base, head)
    check('120 commits asked in queries of 50', asked == [50, 50, 20], asked)
    check(
        "each commit's pull request merged into main, a deleted author as ghost",
        len(found) == 120 and set(found.values()) == {publish_builds.nightly_notes.Pull(4, '[cpu] in', 'ghost')},
        set(found.values()),
    )
    check('no pull requests asked without a previous nightly', hub.pulls('', head) == {} and len(asked) == 3)

for failure in failures:
    print('FAIL:', failure)
print('publish-builds:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
