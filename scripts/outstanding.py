#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The branches that hold work main does not have and no open pull request carries.

A session that stops before it opens its pull request leaves a branch that main does not have and no pull request
holds. Nothing shows it again: it is not in main, and no page of GitHub lists it. This tool lists it.

    scripts/outstanding.py
    scripts/outstanding.py --base origin/main --json
    scripts/outstanding.py --prs open.json --no-fetch

It reads the refs of the repository (`refs/heads` and `refs/remotes/*`), keeps the ones that hold a commit whose
patch the base does not have (`git cherry`), and drops the ones that an open pull request names or holds. `gh pr list`
gives the open pull requests. `--prs FILE` reads them from a file instead, for a machine without `gh` or for a test.

The exit code is 0 when no branch is outstanding, and 1 when one is, so a script can wait on it.
"""

import argparse
import fnmatch
import json
import subprocess
import sys
from pathlib import Path

PR_FIELDS = 'number,headRefName,headRefOid,isDraft,url'


def git(repo, *args):
    """The output of one git command in the repository, or None when it fails."""
    done = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, check=False)
    return done.stdout if done.returncode == 0 else None


def open_prs(repo, prs_file):
    """The open pull requests: the number, the head branch, the head commit and the draft mark."""
    if prs_file:
        return json.loads(Path(prs_file).read_text())
    done = subprocess.run(['gh', 'pr', 'list', '--state', 'open', '--limit', '200', '--json', PR_FIELDS],
                          capture_output=True, text=True, check=False, cwd=str(repo))
    if done.returncode != 0:
        raise SystemExit(f'gh pr list failed: {done.stderr.strip()}')
    return json.loads(done.stdout)


def branch_refs(repo, base):
    """The branches of the repository, as (remote, name, refname); `remote` is 'local' for a branch of its own."""
    out = git(repo, 'for-each-ref', '--format=%(refname)', 'refs/heads', 'refs/remotes')
    if out is None:
        raise SystemExit(f'{repo} is not a git repository')
    for refname in out.split():
        if refname == base or refname.endswith('/HEAD'):
            continue
        if refname.startswith('refs/heads/'):
            yield 'local', refname[len('refs/heads/'):], refname
        elif refname.startswith('refs/remotes/'):
            remote, _, name = refname[len('refs/remotes/'):].partition('/')
            if name:
                yield remote, name, refname


def unmerged(repo, base, refname):
    """The commits of the branch whose patch the base does not have, oldest first."""
    out = git(repo, 'cherry', base, refname)
    if out is None:
        return []
    return [line[2:] for line in out.splitlines() if line.startswith('+')]


def subjects(repo, base, refname, commits):
    """The subjects of those commits, which a cherry-pick keeps though its patch differs (a conflict, a rebase)."""
    out = git(repo, 'log', '--no-merges', '--format=%H%x00%s', f'{base}..{refname}')
    if out is None:
        return set()
    have = set(commits)
    return {subject for sha, subject in (line.split('\0', 1) for line in out.splitlines() if '\0' in line)
            if sha in have}


def pr_work(repo, base, prs):
    """For each open pull request, the patches and the subjects of the commits its head has and the base lacks."""
    work = []
    for pr in prs:
        head = pr.get('headRefOid')
        if not head or git(repo, 'rev-parse', '--verify', '--quiet', head) is None:
            work.append((pr, set(), set()))
            continue
        commits = unmerged(repo, base, head)
        work.append((pr, set(commits), subjects(repo, base, head, commits)))
    return work


def carried(pr_work_list, name, commits, titles):
    """Whether an open pull request holds the work: it names the branch, or its head has the patches or their subjects."""
    patches = set(commits)
    for pr, pr_patches, pr_titles in pr_work_list:
        if pr.get('headRefName') == name:
            return True
        if patches and patches <= pr_patches:
            return True
        if titles and titles <= pr_titles:
            return True
    return False


def outstanding(repo, base, prs, skip=('backup/*',)):
    """The work that the base lacks and no open pull request holds, one entry for each set of commits."""
    work = pr_work(repo, base, prs)
    landed = set((git(repo, 'log', '--format=%s', base) or '').splitlines())
    found = {}
    for remote, name, refname in branch_refs(repo, base):
        if any(fnmatch.fnmatchcase(name, pattern) for pattern in skip):
            continue
        commits = unmerged(repo, base, refname)
        if not commits:
            continue
        titles = subjects(repo, base, refname, commits)
        if titles and titles <= landed:
            continue  # a squash kept the subject: the base has the work
        if carried(work, name, commits, titles):
            continue
        # a backup or a copy of a branch holds the same patches: one entry for the work, not one for each copy
        entry = found.setdefault(frozenset(commits), {
            'refs': [], 'commits': len(commits),
            'tip': (git(repo, 'log', '-1', '--format=%ad %s', '--date=short', refname) or '').strip()})
        entry['refs'].append(name if remote == 'local' else f'{remote}/{name}')
    for entry in found.values():
        entry['refs'].sort(key=lambda ref: ('/' not in ref, ref))  # a pushed branch first: another one can see it
    return sorted(found.values(), key=lambda b: (-b['commits'], b['refs'][0]))


def main(argv=None):
    parser = argparse.ArgumentParser(description='The branches with work main does not have and no open pull request.')
    parser.add_argument('--repo', default=str(Path(__file__).resolve().parents[1]),
                        help='the checkout to read (default: the one of this script)')
    parser.add_argument('--base', default='origin/main', help='the branch that the work reaches (default: origin/main)')
    parser.add_argument('--prs', help='a file of `gh pr list --json %s` output, in place of a live read' % PR_FIELDS)
    parser.add_argument('--no-fetch', action='store_true', help='do not fetch the base first')
    parser.add_argument('--skip', action='append', default=None, metavar='PATTERN',
                        help='a branch to leave out, as a shell pattern (default: backup/*)')
    parser.add_argument('--json', action='store_true', help='print JSON')
    args = parser.parse_args(argv)

    repo = Path(args.repo).resolve()
    if not args.no_fetch:
        git(repo, 'fetch', '--quiet', args.base.partition('/')[0])
    if git(repo, 'rev-parse', '--verify', '--quiet', args.base) is None:
        raise SystemExit(f'the base {args.base} is not in {repo}')

    found = outstanding(repo, args.base, open_prs(repo, args.prs), args.skip or ('backup/*',))
    if args.json:
        print(json.dumps(found, indent=2))
    elif not found:
        print(f'no branch holds work that {args.base} lacks without an open pull request')
    else:
        print(f'{len(found)} branch(es) hold work that {args.base} lacks, and no open pull request carries them:')
        for b in found:
            print(f"  {b['commits']:4d}  {', '.join(b['refs']):<48} {b['tip']}")
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
