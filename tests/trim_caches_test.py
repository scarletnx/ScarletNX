#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/trim-caches.py's choice of caches, on keys as the workflows write them"""

import importlib.util
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location(
    'trim_caches', Path(__file__).resolve().parent.parent / 'scripts' / 'trim-caches.py'
)
trim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trim)

H1, H2 = 'a' * 64, 'b' * 64
NDK = '/usr/local/lib/android/sdk/ndk/28.2.13676358'
failures = []


def check(what, got, want):
    if got != want:
        failures.append(f'{what}: got {got!r}, want {want!r}')


for key, want in [
    ('ccache-linux-x86_64-38789058950-1', 'ccache-linux-x86_64'),
    ('ccache-android-arm64-v8a-38786187285-2', 'ccache-android-arm64-v8a'),
    (f'cpm-android-arm64-v8a-{H1}', 'cpm-android-arm64-v8a'),
    (f'android-llvm-x86_64-{H1}-{NDK}', 'android-llvm-x86_64'),
    (f'msys2-pkgs-upd:true-conf:b326d718-files:{H1}', 'msys2-pkgs-upd:true-conf:b326d718'),
    # one entry per object (sccache): nothing to tell its family from, so it stays
    (f'sccache/e/a/d/{H1}', f'sccache/e/a/d/{H1}'),
]:
    check(f'family of {key}', trim.family(key), want)

ids = iter(range(1, 1000))


def cache(ref, key, accessed, created=None, version='v1'):
    return {
        'id': next(ids),
        'ref': ref,
        'key': key,
        'version': version,
        'sizeInBytes': 1,
        'lastAccessedAt': accessed,
        'createdAt': created or accessed,
    }


MAIN, PR = 'refs/heads/main', 'refs/pull/204/merge'
caches = {
    # a run's ccache replaces the one before; each architecture keeps its own
    'old ccache': cache(MAIN, 'ccache-linux-x86_64-100-1', '2026-10-14T10:00:00Z'),
    'new ccache': cache(MAIN, 'ccache-linux-x86_64-200-1', '2026-10-15T10:00:00Z'),
    'aarch64 ccache': cache(MAIN, 'ccache-linux-aarch64-100-1', '2026-10-14T10:00:00Z'),
    # the dependencies changed back: the older entry is the one runs read now
    'cpm read now': cache(MAIN, f'cpm-linux-{H1}', '2026-10-15T09:00:00Z', '2026-10-01T00:00:00Z'),
    'cpm left': cache(MAIN, f'cpm-linux-{H2}', '2026-10-10T00:00:00Z'),
    'llvm': cache(MAIN, f'android-llvm-x86_64-{H1}-{NDK}', '2026-10-15T08:00:00Z'),
    'msys2 old files': cache(MAIN, f'msys2-pkgs-upd:true-conf:b326d718-files:{H1}', '2026-10-13T00:00:00Z'),
    'msys2 new files': cache(MAIN, f'msys2-pkgs-upd:true-conf:b326d718-files:{H2}', '2026-10-15T00:00:00Z'),
    'msys2 other conf': cache(MAIN, f'msys2-pkgs-upd:true-conf:855f2908-files:{H1}', '2026-10-12T00:00:00Z'),
    # an open pull request's: the same rule, on its own ref; its newest is not main's
    'open pr old': cache(PR, f'deps-linux-x86_64-{H1}', '2026-10-14T00:00:00Z'),
    'open pr new': cache(PR, f'deps-linux-x86_64-{H2}', '2026-10-14T01:00:00Z'),
    # but no compiler cache, which a push alone saves: the pull request would read its own in place of main's
    'open pr ccache': cache(PR, 'ccache-linux-x86_64-160-1', '2026-10-14T01:00:00Z'),
    'open pr ccache head': cache('refs/pull/204/head', 'ccache-apple-161-2', '2026-10-14T02:00:00Z'),
    'live branch ccache': cache('refs/heads/dravee/ci-arm64', 'ccache-linux-x86_64-170-1', '2026-10-15T11:00:00Z'),
    'open pr cpm': cache(PR, f'cpm-linux-{H1}', '2026-10-14T03:00:00Z'),
    'closed pr ccache': cache('refs/pull/180/merge', 'ccache-linux-x86_64-140-1', '2026-10-15T11:00:00Z'),
    'closed pr': cache('refs/pull/180/merge', f'cpm-linux-{H1}', '2026-10-15T11:00:00Z'),
    'closed pr head': cache('refs/pull/181/head', f'cpm-linux-{H1}', '2026-10-15T11:00:00Z'),
    'gone branch': cache('refs/heads/dravee/merged', f'cpm-linux-{H1}', '2026-10-15T11:00:00Z'),
    'live branch': cache('refs/heads/dravee/ci-arm64', f'cpm-linux-{H1}', '2026-10-15T11:00:00Z'),
    'sccache 1': cache('refs/heads/dravee/ci-arm64', f'sccache/e/a/d/{H1}', '2026-10-15T11:00:00Z'),
    'sccache 2': cache('refs/heads/dravee/ci-arm64', f'sccache/5/2/5/{H2}', '2026-10-15T11:00:00Z'),
    'tag': cache('refs/tags/nightly', f'cpm-linux-{H1}', '2026-10-01T00:00:00Z'),
    # one key saved twice: by another system it is another cache, by the same one a copy
    'other system': cache(MAIN, 'ccache-linux-x86_64-100-1', '2026-10-14T10:00:00Z', version='v2'),
    'copy': cache('refs/heads/dravee/ci-arm64', f'sccache/e/a/d/{H1}', '2026-10-15T10:00:00Z'),
}
doomed = trim.plan(list(caches.values()), open_prs={204}, branches={'main', 'dravee/ci-arm64'})
names = {id(c): name for name, c in caches.items()}
got = {names[id(c)]: why for c, why in doomed}
check(
    'deleted',
    sorted(got),
    sorted(
        [
            'old ccache',
            'cpm left',
            'msys2 old files',
            'open pr old',
            'open pr ccache',
            'open pr ccache head',
            'closed pr',
            'closed pr ccache',
            'closed pr head',
            'gone branch',
            'copy',
            'tag',
        ]
    ),
)
check('why for a closed pull request', got.get('closed pr'), 'pull request #180 is closed')
check("why for a closed pull request's compiler cache", got.get('closed pr ccache'), 'pull request #180 is closed')
check('why for a gone branch', got.get('gone branch'), 'its branch is gone')
check("why for a tag's", got.get('tag'), "a tag's")
check('why for a replaced entry', got.get('old ccache'), 'replaced by ccache-linux-x86_64-200-1')
check(
    "why for a pull request's compiler cache",
    got.get('open pr ccache'),
    "on pull request #204's ref, where it is read in place of main's",
)
check('nothing deleted twice', len(doomed), len(got))
# a listing of nothing but main's newest entries leaves them all
newest = [caches[name] for name in ('new ccache', 'aarch64 ccache', 'cpm read now', 'llvm', 'msys2 new files')]
check('newest entries kept', trim.plan(newest, set(), {'main'}), [])

for failure in failures:
    print('FAIL:', failure)
print('trim-caches:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
