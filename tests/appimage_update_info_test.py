#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/appimage-update-info.sh, the release an AppImage's update information names, for each build GitHub Actions
describes: a version's tag the repository's latest, a pull request its own prerelease pr-N in the nightly repository, a
push to main the repository's newest prerelease (main's, main-<commit>), a nightly and a build outside Actions the
nightly repository's latest; scripts/make-appimage.sh taking its UPINFO from it, with the pr-N tag
scripts/publish-builds.py gives a pull request's builds; and the cross AppImages' container given the nightly's name.
Offline."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'appimage-update-info.sh'
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


def info(*args, **env):
    clean = {k: v for k, v in os.environ.items() if not k.startswith('GITHUB_') and k != 'SNX_BUILD_NAME'}
    return subprocess.run(
        ['bash', str(SCRIPT), *args], capture_output=True, text=True, env=dict(clean, **env), check=False
    )


for env, want, why in (
    (
        {'GITHUB_REPOSITORY': 'scarletnx/ScarletNX', 'GITHUB_REF_TYPE': 'tag', 'GITHUB_REF': 'refs/tags/v1.0.0'},
        'gh-releases-zsync|scarletnx|ScarletNX|latest|*x86_64.AppImage.zsync',
        "a version's tag updates to the repository's latest release",
    ),
    (
        {'GITHUB_REPOSITORY': 'scarletnx/ScarletNX', 'GITHUB_REF_TYPE': 'branch', 'GITHUB_REF': 'refs/pull/2919/merge'},
        'gh-releases-zsync|scarletnx|nightly|pr-2919|*x86_64.AppImage.zsync',
        'a pull request updates to its own prerelease, not to the nightly',
    ),
    (
        {'GITHUB_REPOSITORY': 'scarletnx/ScarletNX', 'GITHUB_REF_TYPE': 'branch', 'GITHUB_REF': 'refs/heads/main'},
        'gh-releases-zsync|scarletnx|ScarletNX|latest-pre|*x86_64.AppImage.zsync',
        "a push to main updates to main's prerelease, its next push, never to an older nightly",
    ),
    (
        {'GITHUB_REPOSITORY': 'someone/ScarletNX', 'GITHUB_REF': 'refs/heads/main'},
        'gh-releases-zsync|someone|ScarletNX|latest-pre|*x86_64.AppImage.zsync',
        "a fork's push to main names its own repository's prerelease",
    ),
    (
        {
            'GITHUB_REPOSITORY': 'scarletnx/ScarletNX',
            'GITHUB_REF_TYPE': 'branch',
            'GITHUB_REF': 'refs/heads/main',
            'SNX_BUILD_NAME': 'nightly-2026-11-26',
        },
        'gh-releases-zsync|scarletnx|nightly|latest|*x86_64.AppImage.zsync',
        "a nightly, built from main, updates to the nightly repository's latest",
    ),
    (
        {
            'GITHUB_REPOSITORY': 'someone/ScarletNX',
            'GITHUB_REF': 'refs/heads/main',
            'SNX_BUILD_NAME': 'nightly-2026-11-26',
        },
        'gh-releases-zsync|someone|nightly|latest|*x86_64.AppImage.zsync',
        "a fork's nightly names its own owner's nightly repository",
    ),
    (
        {'GITHUB_REPOSITORY': 'scarletnx/ScarletNX', 'GITHUB_REF': 'refs/heads/dravee/x'},
        'gh-releases-zsync|scarletnx|nightly|latest|*x86_64.AppImage.zsync',
        "another branch's run updates to the nightly repository's latest",
    ),
    ({}, 'gh-releases-zsync|scarletnx|nightly|latest|*x86_64.AppImage.zsync', 'a build outside Actions'),
    (
        {'GITHUB_REF': 'refs/pull/12/mergeX'},
        'gh-releases-zsync|scarletnx|nightly|latest|*x86_64.AppImage.zsync',
        'a ref that only starts like a pull request is not one',
    ),
):
    done = info('x86_64', **env)
    check(why, done.returncode == 0 and done.stdout == want + '\n', (done.returncode, done.stdout, done.stderr))

done = info('riscv64', GITHUB_REF='refs/pull/7/merge')
check("the architecture in the .zsync's pattern", '|pr-7|*riscv64.AppImage.zsync' in done.stdout, done.stdout)
for args in ((), ('',), ('x86_64', 'more')):
    done = info(*args)
    check(f'{args or "no ARCH"} is refused with its usage', done.returncode == 2 and 'usage:' in done.stderr, done)

make = (ROOT / 'scripts' / 'make-appimage.sh').read_text()
check(
    "make-appimage.sh's UPINFO is this script's line",
    'UPINFO=$("$(dirname "$0")/appimage-update-info.sh" "$ARCH")' in make and 'gh-releases-zsync' not in make,
)
cross = (ROOT / 'scripts' / 'ci' / 'cross.sh').read_text()
appimage = cross.split('\nappimage() {', 1)[1].split('\n}', 1)[0]
check(
    "the cross AppImages' container gets the ref and the nightly's name the update follows",
    all(f'-e {name} ' in appimage for name in ('GITHUB_REPOSITORY', 'GITHUB_REF', 'SNX_BUILD_NAME')),
    appimage,
)
check(
    "publish-builds.py tags main's prerelease main-<commit>, the newest prerelease a push to main updates to",
    "MAIN = 'main'" in (ROOT / 'scripts' / 'publish-builds.py').read_text(),
)
check(
    "publish-builds.py tags a pull request's builds pr-N, the tag the update names",
    "tag = f'pr-{number}'" in (ROOT / 'scripts' / 'publish-builds.py').read_text(),
)

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('appimage update info: ok')
