# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A nightly's title and notes: the downloads by system, with their sizes and how to run them, what changed since the
previous nightly, and where the source is, which the GPL asks for beside the binaries. scripts/publish-builds.py takes
them for the nightlies of the nightly repository and a version's release, and the downloads and the source for a pull
request's builds.

The changes are the checkout's history from the commit the previous nightly was built from to the one built, left out
without it or when it is not an ancestor: a line for each pull request those commits came in by, with how many they
are and who opened it, and one for each commit no pull request merged, grouped by the [area] their titles open with.
Which pull request merged a commit is GitHub's to say (scripts/publish-builds.py asks it); main merges by rebase, so
its history holds no merge commit to read it from. The notes link the built commit first, which is where the next
nightly finds it. Each line links its pull request or commit in the repository: a bare #N in another repository's
release would name that repository's own.
"""

import collections
import re
import subprocess

SERVER = 'https://github.com'
# The release's files in the order the tables list them, named ScarletNX-<system>-<arch>: the system, its
# architectures and what the file is. A file a release gets that is missing here stops the notes: add its line
SYSTEMS = {
    'ScarletNX-linux-x86_64.AppImage': ('Linux', 'x86-64', 'AppImage'),
    'ScarletNX-linux-aarch64.AppImage': ('Linux', 'ARM64', 'AppImage'),
    'ScarletNX-linux-ppc64le.AppImage': ('Linux', 'PowerPC64 LE', 'AppImage'),
    'ScarletNX-linux-ppc64.AppImage': ('Linux', 'PowerPC64 BE', 'AppImage (Linux programs)'),
    'ScarletNX-linux-riscv64.AppImage': ('Linux', 'RISC-V 64', 'AppImage'),
    'ScarletNX-linux-loongarch64.AppImage': ('Linux', 'LoongArch64', 'AppImage'),
    'ScarletNX-windows-x86_64.zip': ('Windows', 'x86-64', 'zip'),
    'ScarletNX-windows-aarch64.zip': ('Windows', 'ARM64', 'zip'),
    'ScarletNX-android-universal.apk': ('Android', 'ARM64, x86-64', 'APK'),
    'ScarletNX-macos-arm64.zip': ('macOS', 'ARM64', 'zip'),
    'ScarletNX-ios-arm64.ipa': ('iOS', 'ARM64', 'IPA'),
}
# how each system's file runs, under the downloads; {updates}: the release AppImageUpdate takes an AppImage to, the
# latest of the repository its update information names (scripts/make-appimage.sh)
RUNNING = {
    'Linux': 'make the AppImage executable and run it. AppImageUpdate takes it to {updates}.',
    'Windows': 'unzip it anywhere and run `bin\\scarletnx.exe`.',
    'Android': 'Android 5.0 and later. The APK is signed with a debug key.',
    'macOS': "unzip it and open `ScarletNX.app`; signed ad hoc, so a download may need *Open* from the Finder's menu the first time.",
    'iOS': 'unsigned: AltStore, SideStore or a developer account signs it for the device.',
}
# what text from a commit or a pull request could make of the markdown around it: a link, a tag, a table cell
MARKDOWN = re.compile(r'([\\`*_\[\]<>|~])')
# a title's area, the [vk] or [driver, fs] it opens with
AREA = re.compile(r'\[([^\]]+)\]\s*(.*)')
# a pull request's commits listed, at most, before "and N more"
MOST_CHANGES = 30
# the lines a nightly's changes list, at most, before "and N more": a day of main is about 40 pull requests
MOST_LINES = 200

# A commit of main: its subject, the number of the pull request that merged it (None for one no pull request merged),
# and the commit
Change = collections.namedtuple('Change', 'text pr sha')
# A pull request as the changes name it: its number, title and author's login
Pull = collections.namedtuple('Pull', 'number title author')


def git(checkout, *args):
    return subprocess.run(['git', '-C', checkout, *args], check=True, capture_output=True, text=True).stdout


def escape(text):
    """text shown as it is, in GitHub's markdown and Discord's"""
    return MARKDOWN.sub(r'\\\1', text)


def changes(checkout, previous, sha, pulls=None):
    """What main got from PREVIOUS to SHA, newest first: each commit of its first parents, with the number of the pull
    request PULLS (commit to Pull) says merged it. None when PREVIOUS is missing or not an ancestor of SHA"""
    if not previous:
        return None
    ancestor = subprocess.run(
        ['git', '-C', checkout, 'merge-base', '--is-ancestor', previous, sha], capture_output=True, check=False
    )
    if ancestor.returncode:
        return None
    items = []
    log = git(checkout, 'log', '--first-parent', '--format=%H%x1f%s%x1e', f'{previous}..{sha}')
    for record in log.split('\x1e'):
        record = record.strip('\n')
        if not record:
            continue
        commit, subject = record.split('\x1f', 1)
        pull = (pulls or {}).get(commit)
        items.append(Change(subject, pull.number if pull else None, commit))
    return items


def change_line(change, url):
    """a commit as a line, its pull request or its commit linked in the repository at url"""
    if change.pr:
        return f'{escape(change.text)} ([#{change.pr}]({url}/pull/{change.pr}))'
    return f'{escape(change.text)} ([`{change.sha[:7]}`]({url}/commit/{change.sha}))'


def area(text):
    """a title's area and the rest of it: ('vk', 'Keep ...') for '[vk] Keep ...', ('', text) without one"""
    found = AREA.fullmatch(text.strip())
    return (found.group(1).strip(), found.group(2)) if found else ('', text.strip())


def entries(items, pulls, url):
    """the changes' entries, newest first: a pull request once, at its newest commit, with how many of the commits it
    merged, and each commit no pull request merged, linked in the repository at url. Each is (area, title, line), line
    without its area"""
    by_number = {pull.number: pull for pull in (pulls or {}).values()}
    count = collections.Counter(item.pr for item in items if item.pr)
    shown, out = set(), []
    for item in items:
        if item.pr in shown:
            continue
        if item.pr:
            shown.add(item.pr)
            pull = by_number[item.pr]
            where, text = area(pull.title)
            many = f'{count[item.pr]} commit{"" if count[item.pr] == 1 else "s"}'
            link = f'[#{pull.number}]({url}/pull/{pull.number})'
            out.append((where, pull.title, f'{escape(text)} ({link}, {many}, by {escape(pull.author)})'))
        else:
            where, text = area(item.text)
            out.append((where, item.text, f'{escape(text)} ([`{item.sha[:7]}`]({url}/commit/{item.sha}))'))
    return out


def entry_lines(items, pulls, url):
    """the entries as one list, newest first, each with its area: a message's lines"""
    return [f'{escape(f"[{where}] ") if where else ""}{line}' for where, _, line in entries(items, pulls, url)]


def grouped(items, pulls, url, more_url):
    """the entries under a heading for each area, the areas in order and those of none last, newest first in each;
    past MOST_LINES the rest counted and linked"""
    listed = entries(items, pulls, url)
    kept, rest = listed[:MOST_LINES], len(listed) - MOST_LINES
    areas = sorted({where for where, _, _ in kept}, key=lambda where: (not where, where.lower()))
    lines = []
    for where in areas:
        lines += ['', f'**{escape(where) if where else "Other"}**', '']
        lines += [f'- {line}' for w, _, line in kept if w == where]
    if rest > 0:
        lines += ['', f'And [{rest} more]({more_url}).']
    return lines


def counted(items):
    """what the changes are, in numbers: 5 pull requests and 1 commit no pull request merged, 49 commits in all"""
    pulls = len({item.pr for item in items if item.pr})
    alone = sum(1 for item in items if not item.pr)
    parts = []
    if pulls:
        parts.append(f'{pulls} pull request{"" if pulls == 1 else "s"}')
    if alone:
        parts.append(f'{alone} commit{"" if alone == 1 else "s"} no pull request merged')
    said = ' and '.join(parts)
    return f'{said}, {len(items)} commit{"" if len(items) == 1 else "s"} in all' if pulls else said


def title(day, sha):
    """a nightly's title: the day (UTC) its run began, and its commit"""
    return f'Nightly {day} ({sha[:7]})'


def size(count):
    return f'{count / 1e6:.1f} MB'


def ordered(names):
    """names in the order of SYSTEMS; one that is not there stops the notes, saying where to add it"""
    unknown = [name for name in names if name not in SYSTEMS]
    if unknown:
        raise SystemExit(f'{", ".join(unknown)}: what system is it for? Add it to SYSTEMS in scripts/nightly-notes.py')
    return sorted(names, key=list(SYSTEMS).index)


def system(name):
    """what name is for, as a run says it: Linux, x86-64"""
    return ', '.join(SYSTEMS[ordered([name])[0]][:2])


def table(files, download, sizes=None, missing=(), why='', building=()):
    """The files, a row each in the order of SYSTEMS, linked from the release they are in, with their sizes; a file
    the run did not build says so, linking why, and one its job is still making (building) says that"""
    sizes = sizes or {}
    rows = ['| System | Architecture | Download | Size |', '|---|---|---|---|']
    for name in ordered([*files, *missing, *building]):
        named, arch, kind = SYSTEMS[name]
        if name in files:
            got, weight = f'[{kind}]({download}/{name})', size(sizes[name]) if name in sizes else ''
        elif name in building:
            got, weight = f'building, [see the run]({why})' if why else 'building', ''
        else:
            got, weight = f'not built, [see the run]({why})' if why else 'not built', ''
        rows.append(f'| **{named}** | {arch} | {got} | {weight} |')
    return rows


def downloads(files, download, url, sha, sizes=None, missing=(), why='', updates='the newest nightly', building=()):
    """the downloads: the table, how each system's file runs, and the documents"""
    lines = ['## Downloads', '', *table(files, download, sizes, missing, why, building), '']
    shown = dict.fromkeys(SYSTEMS[name][0] for name in ordered(files))
    lines += [f'- **{name}**: {RUNNING[name].format(updates=updates)}' for name in shown]
    lines += [
        '',
        (
            f'More in [the usage guide]({url}/blob/{sha}/docs/user/usage.md) and, for the app, '
            f'[its page]({url}/blob/{sha}/docs/hosts/android-app.md).'
        ),
    ]
    return lines


def source(tree):
    """Where a build's source is, which the GPL asks for beside the binaries, and its libraries' licenses, folded"""
    return [
        '<details>',
        '<summary>Source and licenses</summary>',
        '',
        f'The source of this build is at {tree}.',
        '',
        (
            '- The third-party libraries built into the runtime come from the release archives that '
            'cmake/dependencies.cmake lists, each checked against the SHA-256 written beside it.'
        ),
        (
            "- The AppImages also bundle libraries from Arch Linux packages (Arch Linux ARM's for ARM64): their "
            'licenses are in share/licenses, and the packages in share/licenses/bundled-packages.txt.'
        ),
        (
            '- The Windows zip holds the runtime DLLs it needs with their licenses in share/licenses, and the MSYS2 '
            'packages it was built with in share/licenses/msys2/build-packages.txt.'
        ),
        '',
        '</details>',
    ]


def notes(
    checkout,
    sha,
    previous,
    repo,
    files,
    download,
    kept,
    since='the previous nightly',
    sizes=None,
    lead='Built from main at',
    updates='the newest nightly',
    missing=(),
    why='',
    pulls=None,
    building=(),
):
    """download: where the release's files are; kept: what becomes of the release; since: what previous was; sizes:
    each file's bytes; lead: what the first line calls the build; updates: the release AppImageUpdate takes it to;
    missing: the files the run did not build, a row each linking why; pulls: the pull request that merged each commit
    (commit to Pull), which the changes need: an empty one says no commit came in by a pull request; building: the
    files their jobs are still making"""
    url = f'{SERVER}/{repo}'
    date = git(checkout, 'log', '-1', '--format=%cs', sha).strip()
    lines = [f'{lead} [`{sha[:7]}`]({url}/commit/{sha}), committed {date}.' + (f' {kept}' if kept else ''), '']
    lines += downloads(files, download, url, sha, sizes, missing, why, updates=updates, building=building)
    if previous and pulls is None:
        raise ValueError('the changes need the pull request of each commit: pass pulls')
    items = changes(checkout, previous, sha, pulls)
    if items is not None:
        compare = f'{url}/compare/{previous}...{sha}'
        said = f'{counted(items)}, by area' if items else 'no change'
        lines += ['', f'## Changes since {since}', '', f'[`{previous[:7]}...{sha[:7]}`]({compare}): {said}.']
        lines += grouped(items, pulls, url, compare) if items else ['', 'None: the same commit, built again.']
    lines += ['', *source(f'{url}/tree/{sha}')]
    return '\n'.join(lines) + '\n'


# what a file of SYSTEMS asks after the download, in the README's table: the system's step, and for the packages a run
# may lack (scripts/publish-builds.py's OPTIONAL) that it is there only when its job built
AFTER = {
    'Linux': '`chmod +x` it and run it',
    'Windows': 'Unzip it and run `bin\\scarletnx.exe`',
    'Android': 'Install it. [The Android app]({docs}/hosts/android-app.md) has the first run',
    'macOS': "Unzip it, then *Open* from the Finder's menu, as the app is signed ad hoc",
    'iOS': 'Sideload it',
}


def readme(repo, to, optional=()):
    """the nightly repository's README: what it holds, which file is whose, the first run, a pull request's build, where
    a problem goes; optional: the files a release may lack"""
    url, docs = f'{SERVER}/{repo}', f'{SERVER}/{repo}/blob/main/docs'
    rows = ['| System | File | After downloading |', '| --- | --- | --- |']
    for name, (system_, arch, _kind) in SYSTEMS.items():
        step = AFTER[system_].format(docs=docs)
        if name in optional:
            step += '. Present only when its job built'
        rows.append(f'| {system_}, {arch} | `{name}` | {step} |')
    lines = [
        '<!-- publish.yml writes this file at each nightly (scripts/nightly-notes.py, readme): change it there -->',
        '# ScarletNX nightly builds',
        '',
        f'Builds of [ScarletNX]({url}), published by its [publish workflow]({url}/blob/main/.github/workflows/publish.yml):',
        '',
        (
            '- **Nightlies**, `nightly-<day>-<commit>`: one a day, of `main` as the first scheduled build of the day '
            '(UTC) that built everywhere. Every nightly stays here, and '
            f'[the latest]({SERVER}/{to}/releases/latest) is always the newest. When a day has none, the newest says '
            'at its top which build failed.'
        ),
        (
            "- **Pull requests**, `pr-<number>`: an open pull request's builds, replaced at each push to it. They are "
            'deleted when it closes without a merge, or once merged, when a nightly has its change. They are '
            'prereleases, not reviewed releases.'
        ),
        '',
        (
            f"The build of the newest push to `main` is not here: it is [{repo}]({url}/releases)'s one prerelease, "
            "`main-<commit>`, each package added as its job ends and the whole replaced by the next push's."
        ),
        '',
        "Each release's notes say which file is for which system, what changed and where its source is. This repository",
        f'holds only the releases: the code is in [{repo}]({url}).',
        '',
        '## Which file is mine',
        '',
        "Every release lists the same files. The newest nightly's links never change, so a bookmark stays current.",
        '',
        *rows,
        '',
        (
            'Games hardly run on macOS or iOS yet, so those two are for people testing the builds themselves '
            f"([macOS and iOS]({docs}/hosts/apple.md)). Each AppImage has a `.zsync` file beside it, which AppImage's update "
            'tools use to download only what changed. Games need a Vulkan 1.3 driver.'
        ),
        '',
        '## First run',
        '',
        (
            'Started with no arguments, the program opens the launcher. **Install Keys** installs your `prod.keys`, and '
            '**Load Switch NSP/XCI/NRO** takes a dump you made from your own console. Keys and games are never part of a '
            f'release. [Usage]({docs}/user/usage.md) covers the command line and the folders.'
        ),
        '',
        "## Trying a pull request's build",
        '',
        (
            "Open the pull request's release, `pr-<number>`, and take the file for your system from the table above. A pull "
            "request's build is made from its latest push and is replaced at the next one, so check the commit named in the "
            'notes against the one you mean to test. A pull request from a fork has no build until someone on the team '
            'labels it `publish builds`.'
        ),
        '',
        (
            "When you report what you saw, say which build it was (the nightly's tag, or the pull request's number and its "
            'commit) and add your system, processor, graphics card and driver version. The '
            f'[testing guide]({docs}/user/testing.md) has the form.'
        ),
        '',
        '## Problems',
        '',
        (
            f'A game that does not run as it should goes to the [Game report]({url}/issues/new?template=game-report.yml) '
            "form, with the log attached. A fault in a build's packaging, or a file missing from a release, goes to "
            f'[{repo}]({url}/issues) as an issue starting with `[ci]`, since the workflow that builds and publishes the '
            'files lives there.'
        ),
    ]
    return '\n'.join(lines) + '\n'
