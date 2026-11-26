#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/nightly-notes.py on a history of its own, merged by rebase as main is: a pull request's commits under one
line, a commit no pull request merged, a branch never merged, the lines grouped by area and cut past the most; the
downloads by system with their sizes, a file not built, text that would be markdown shown as it is"""

DOWNLOAD = 'https://github.com/owner/nightly/releases/download/t'


import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('nightly_notes', ROOT / 'scripts' / 'nightly-notes.py')
nightly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nightly)
failures = []


def check(what, ok):
    if not ok:
        failures.append(what)


with tempfile.TemporaryDirectory() as repo:
    # written on the 1st, landed on main on the 15th: the title dates the landing
    env = dict(os.environ, GIT_COMMITTER_DATE='2026-10-15T12:00:00Z', GIT_AUTHOR_DATE='2026-10-01T12:00:00Z')

    def git(*args):
        return subprocess.run(
            [
                'git',
                '-C',
                repo,
                '-c',
                'user.name=t',
                '-c',
                'user.email=t@t',
                '-c',
                'commit.gpgsign=false',
                '-c',
                'core.hooksPath=' + os.devnull,
                *args,
            ],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()

    git('init', '-q', '-b', 'main')
    git('commit', '-q', '--allow-empty', '-m', 'the previous nightly')
    previous = git('rev-parse', 'HEAD')
    # pull request #5's two commits, rebased onto main, then a commit pushed to main
    git('commit', '-q', '--allow-empty', '-m', '[cpu] a commit of the pull request')
    first = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', '[cpu] its second commit')
    merge = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', '[docs] a commit pushed to main')
    sha = git('rev-parse', 'HEAD')
    git('switch', '-q', '-c', 'elsewhere', previous)
    git('commit', '-q', '--allow-empty', '-m', 'never merged')
    unmerged = git('rev-parse', 'HEAD')
    git('switch', '-q', 'main')
    pull = nightly.Pull(5, '[ci] its title', 'someone')
    pulls = {first: pull, merge: pull}

    check(
        'changes, newest first, each commit with the pull request that merged it',
        nightly.changes(repo, previous, sha, pulls)
        == [
            ('[docs] a commit pushed to main', None, sha),
            ('[cpu] its second commit', 5, merge),
            ('[cpu] a commit of the pull request', 5, first),
        ],
    )
    check(
        'without the pull requests, each commit stands alone',
        [item.pr for item in nightly.changes(repo, previous, sha, {})] == [None, None, None],
    )
    url = 'https://github.com/owner/repo'
    check(
        "a change's line links its pull request, or its commit, in the repository: a bare #5 in the nightly "
        "repository's release would be that repository's",
        nightly.change_line(nightly.Change('[ci] its title', 5, merge), url)
        == f'\\[ci\\] its title ([#5]({url}/pull/5))'
        and nightly.change_line(nightly.Change('a fix', None, sha), url)
        == f'a fix ([`{sha[:7]}`]({url}/commit/{sha}))',
    )
    items = nightly.changes(repo, previous, sha, pulls)
    check(
        'a pull request once, with its commits counted and its author, and the commit no pull request merged',
        nightly.entry_lines(items, pulls, url)
        == [
            f'\\[docs\\] a commit pushed to main ([`{sha[:7]}`]({url}/commit/{sha}))',
            f'\\[ci\\] its title ([#5]({url}/pull/5), 2 commits, by someone)',
        ],
    )
    check(
        'an area taken from the title, none without one',
        nightly.area('[driver, fs] Say why') == ('driver, fs', 'Say why') and nightly.area('a fix') == ('', 'a fix'),
    )
    odd = [
        nightly.Change('{x} and {url}', None, sha),
        nightly.Change('[vk] b', 7, merge),
        nightly.Change('[gpu] a', 8, first),
    ]
    odd_pulls = {merge: nightly.Pull(7, '[vk] b', 'v'), first: nightly.Pull(8, '[gpu] a', 'g')}
    shown = nightly.grouped(odd, odd_pulls, url, 'https://compare')
    check(
        'the areas in order, those of none last as Other, braces in a title shown as they are',
        shown
        == [
            '',
            '**gpu**',
            '',
            f'- a ([#8]({url}/pull/8), 1 commit, by g)',
            '',
            '**vk**',
            '',
            f'- b ([#7]({url}/pull/7), 1 commit, by v)',
            '',
            '**Other**',
            '',
            f'- {{x}} and {{url}} ([`{sha[:7]}`]({url}/commit/{sha}))',
        ],
    )
    most = nightly.MOST_LINES
    nightly.MOST_LINES = 2
    cut = nightly.grouped(odd, odd_pulls, url, 'https://compare')
    nightly.MOST_LINES = most
    check(
        'past the most lines, the rest counted and linked',
        cut[-1] == 'And [1 more](https://compare).' and not any('#8' in line for line in cut),
    )
    check(
        'text that would be a link, a tag or a table cell is shown as it is',
        nightly.escape('[x](http://e) <b> a|b `c` *d* _e_ ~f~ \\')
        == '\\[x\\](http://e) \\<b\\> a\\|b \\`c\\` \\*d\\* \\_e\\_ \\~f\\~ \\\\',
    )
    check('no changes without a previous nightly', nightly.changes(repo, '', sha, pulls) is None)
    check('no changes from a commit that is not an ancestor', nightly.changes(repo, unmerged, sha, pulls) is None)
    check(
        'the title: the day its run began, and its commit',
        nightly.title('2026-10-16', sha) == f'Nightly 2026-10-16 ({sha[:7]})',
    )
    readme = nightly.readme('owner/repo', 'owner/nightly', {'ScarletNX-windows-aarch64.zip'})
    rows = [line for line in readme.splitlines() if line.startswith('| ') and '`ScarletNX-' in line]
    check(
        "the README's table: a row for every file of SYSTEMS, in its order, the optional ones said to be so",
        [re.search(r'`([^`]+)`', row).group(1) for row in rows] == list(nightly.SYSTEMS)
        and '| Windows, ARM64 | `ScarletNX-windows-aarch64.zip` | Unzip it and run `bin\\scarletnx.exe`. Present only when its job built |'
        in rows
        and not any('Present only' in row for row in rows if 'x86_64.zip' in row),
    )
    check(
        "the README says every nightly stays, how a pull request's builds go, and links the docs main has",
        'Every nightly stays here' in readme
        and 'once merged, when a nightly has its change' in readme
        and 'https://github.com/owner/repo/blob/main/docs/user/testing.md' in readme
        and 'https://github.com/owner/nightly/releases/latest' in readme
        and readme.startswith('<!-- publish.yml writes this file')
        and "[owner/repo](https://github.com/owner/repo/releases)'s one prerelease, `main-<commit>`" in readme,
    )
    for linked in re.findall(r'/blob/main/([^)#]+)', readme):
        check(f'the README links {linked}, which the checkout has', (ROOT / linked).exists())

    files = ['ScarletNX-android-universal.apk', 'ScarletNX-linux-aarch64.AppImage', 'ScarletNX-linux-x86_64.AppImage']
    sizes = {name: 94_563_085 for name in files}
    text = nightly.notes(repo, sha, previous, 'owner/repo', files, DOWNLOAD, 'Kept.', sizes=sizes, pulls=pulls)
    check(
        'the built commit is the first one the notes name, where the next nightly looks',
        re.search(r'[0-9a-f]{40}', text).group(0) == sha,
    )
    rows = [re.search(r'/([^/)]+)\)', line).group(1) for line in text.splitlines() if line.startswith('| **')]
    check(
        'the files in the table order',
        rows
        == ['ScarletNX-linux-x86_64.AppImage', 'ScarletNX-linux-aarch64.AppImage', 'ScarletNX-android-universal.apk'],
    )
    check(
        'a row each: the system, its architectures, the file by its kind, its size',
        f'| **Linux** | ARM64 | [AppImage]({DOWNLOAD}/ScarletNX-linux-aarch64.AppImage) | 94.6 MB |' in text
        and f'| **Android** | ARM64, x86-64 | [APK]({DOWNLOAD}/ScarletNX-android-universal.apk) | 94.6 MB |' in text,
    )
    check(
        'how to run each system built, once each, and nothing of one not built',
        text.count('- **Linux**: ') == 1 and '- **Android**: ' in text and '- **Windows**' not in text,
    )
    check(
        'where AppImageUpdate takes an AppImage: the newest nightly unless said otherwise',
        'AppImageUpdate takes it to the newest nightly.' in text
        and 'takes it to the newest version.'
        in nightly.notes(repo, sha, '', 'owner/repo', files, DOWNLOAD, '', updates='the newest version'),
    )
    missing = nightly.table(files, DOWNLOAD, sizes, ['ScarletNX-windows-x86_64.zip'], 'https://run')
    check(
        'a file not built: its row in its place, linking why',
        missing[4] == '| **Windows** | x86-64 | not built, [see the run](https://run) |  |',
    )
    check('a system as a run names it', nightly.system('ScarletNX-windows-x86_64.zip') == 'Windows, x86-64')
    try:
        nightly.notes(repo, sha, previous, 'owner/repo', files + ['ScarletNX-windows-arm64.zip'], DOWNLOAD, 'Kept.')
        check('a file with no system stops the notes', False)
    except SystemExit as stop:
        check(
            'a file with no system stops the notes, saying where to add it',
            'ScarletNX-windows-arm64.zip' in str(stop) and 'SYSTEMS in scripts/nightly-notes.py' in str(stop),
        )
    check(
        'a merged pull request listed once under its area, linked, and the commit pushed to main under its own',
        f'**ci**\n\n- its title ([#5]({url}/pull/5), 2 commits, by someone)' in text
        and f'**docs**\n\n- a commit pushed to main ([`{sha[:7]}`]({url}/commit/{sha}))' in text,
    )
    check(
        'the changes counted, from the compare of the two commits',
        f'[`{previous[:7]}...{sha[:7]}`]({url}/compare/{previous}...{sha}): 1 pull request and 1 commit no pull '
        'request merged, 3 commits in all, by area.' in text,
    )
    try:
        nightly.notes(repo, sha, previous, 'owner/repo', files, DOWNLOAD, 'Kept.')
        check('notes with changes and no pull requests stop', False)
    except ValueError as stop:
        check('notes with changes and no pull requests stop, saying what they need', 'pass pulls' in str(stop))
    check(
        'the source named, folded away at the end',
        f'https://github.com/owner/repo/tree/{sha}' in text
        and text.index('<details>') > text.index('## Changes')
        and text.rstrip().endswith('</details>'),
    )
    check(
        'no changes section without a previous nightly',
        '## Changes' not in nightly.notes(repo, sha, '', 'owner/repo', files, DOWNLOAD, 'Kept.'),
    )
    check(
        'the first line says what the build is',
        nightly.notes(repo, sha, '', 'owner/repo', files, DOWNLOAD, '', lead='ScarletNX v1, built from').startswith(
            f'ScarletNX v1, built from [`{sha[:7]}`]'
        ),
    )
    check(
        'the same commit built again',
        'None: the same commit' in nightly.notes(repo, sha, sha, 'owner/repo', files, DOWNLOAD, 'Kept.', pulls={}),
    )
    check(
        'the files linked from the release they are in, and what becomes of it',
        f'[APK]({DOWNLOAD}/ScarletNX-android-universal.apk)' in text and 'committed 2026-10-15. Kept.' in text,
    )


for failure in failures:
    print('FAIL:', failure)
print('nightly-notes:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
