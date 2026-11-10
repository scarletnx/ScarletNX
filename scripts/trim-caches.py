#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Delete the GitHub Actions caches no run will read again, so the repository's 10 GB hold the ones builds use.

A pull request's caches are read by that pull request only, and a branch's by that branch and the pull requests into
it: once the pull request is closed or the branch gone, nothing reads them, yet they count until GitHub evicts them.
A key that ends in a run's number or a hash (ccache-linux-x86_64-<run>-<attempt>, cpm-linux-<hash of the
dependencies>) is one entry of a cache whose runs restore the entry used last: on each ref the older entries of that
family go too, among those of one version (GitHub's hash of the paths and of the system, so one key saved by two
systems is two caches). A key without such an end is a family of its own and stays. A compiler cache (ccache-...) is
saved by a push alone (build.yml): one on a pull request's ref, which that pull request reads in place of main's
newer one, goes too.

.github/workflows/cache-cleanup.yml runs it when a pull request closes and after each build. It needs gh, with GH_REPO
set and a token allowed actions: write; --dry-run only lists.
"""

import argparse
import json
import re
import subprocess
import sys
import time

# The end of a key that changes from one entry of a cache to the next: a run's number and attempt, a hashFiles and
# what follows it (android-llvm's NDK path), setup-msys2's hash of its package files
VOLATILE = re.compile(r'-\d+-\d+$|-[0-9a-f]{64}(-.*)?$|-files:[0-9a-f]+$')
PULL = re.compile(r'refs/pull/(\d+)/(merge|head)')
# How the key of a cache starts that build.yml saves from a push alone: a pull request's run reads main's
PUSH_ONLY = ('ccache-',)


def family(key):
    return VOLATILE.sub('', key, count=1)


def plan(caches, open_prs, branches):
    """The caches to delete, as (cache, why), from GitHub's listing, the open pull requests' numbers and the
    branches' names"""
    doomed, families = [], {}
    for cache in caches:
        ref = cache['ref']
        pull = PULL.fullmatch(ref)
        if pull and int(pull.group(1)) not in open_prs:
            doomed.append((cache, f'pull request #{pull.group(1)} is closed'))
        elif pull and cache['key'].startswith(PUSH_ONLY):
            doomed.append((cache, f"on pull request #{pull.group(1)}'s ref, where it is read in place of main's"))
        elif ref.startswith('refs/heads/') and ref[len('refs/heads/') :] not in branches:
            doomed.append((cache, 'its branch is gone'))
        elif ref.startswith('refs/tags/'):  # a version's build reads main's: what it saves no later run reads
            doomed.append((cache, "a tag's"))
        else:
            families.setdefault((ref, cache.get('version'), family(cache['key'])), []).append(cache)
    for entries in families.values():
        # the entry runs read or wrote last is the one the next run restores
        entries.sort(key=lambda c: (c['lastAccessedAt'], c['createdAt'], c['id']), reverse=True)
        doomed += [(cache, f'replaced by {entries[0]["key"]}') for cache in entries[1:]]
    return doomed


def gh(*args):
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--dry-run', action='store_true', help='list what would be deleted, delete nothing')
    args = parser.parse_args()
    caches = json.loads(
        gh('cache', 'list', '--limit', '100000', '--json', 'id,key,ref,version,sizeInBytes,createdAt,lastAccessedAt')
    )
    open_prs = {
        pr['number'] for pr in json.loads(gh('pr', 'list', '--state', 'open', '--limit', '100000', '--json', 'number'))
    }
    branches = set(gh('api', '--paginate', 'repos/{owner}/{repo}/branches', '--jq', '.[].name').split())
    # a listing that came back short would take every branch's caches for a gone branch's
    default = gh('api', 'repos/{owner}/{repo}', '--jq', '.default_branch').strip()
    if default not in branches:
        sys.exit(f'the branch listing has no {default}, the default branch: not deleting from it')
    doomed = plan(caches, open_prs, branches)
    failed = 0
    for cache, why in doomed:
        print(f'{cache["sizeInBytes"] / 1e6:8.1f} MB  {cache["ref"]}  {cache["key"]}: {why}')
        if args.dry_run:
            continue
        try:
            gh('cache', 'delete', str(cache['id']))
        except subprocess.CalledProcessError as error:
            failed += 1
            print(f'    not deleted: {error.stderr.strip()}')
        time.sleep(0.2)  # GitHub takes 400 deletions a minute
    total = sum(cache['sizeInBytes'] for cache in caches)
    freed = sum(cache['sizeInBytes'] for cache, _ in doomed)
    print(
        f'{"would free" if args.dry_run else "freed"} {freed / 1e9:.2f} of {total / 1e9:.2f} GB '
        f'({len(doomed)} of {len(caches)} caches)'
    )
    if failed:
        # the next run tries again; a red job on main for it would hide the builds' own state
        print(f'::warning::{failed} caches were not deleted')


if __name__ == '__main__':
    main()
