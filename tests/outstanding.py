# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The checker of work without a pull request: a branch that main lacks and no open pull request carries.

The tests run the tool as a program, in a repository of their own with no remote, and give it the open pull requests
from a file, so they need no network and no `gh`.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / 'scripts' / 'outstanding.py'


def run(repo, *args):
    return subprocess.run([sys.executable, str(TOOL), '--repo', str(repo), '--base', 'main', '--no-fetch', *args],
                          capture_output=True, text=True, check=False)


class Repo:
    """A repository with a `main`, and the branches that its test asks for."""

    def __init__(self, path):
        self.path = Path(path)
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'Test')
        self.commit('a.txt', 'a', 'the base')

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.path), *args], capture_output=True, text=True, check=True).stdout

    def commit(self, name, text, message):
        (self.path / name).write_text(text)
        self.git('add', name)
        self.git('commit', '-q', '-m', message)

    def branch(self, name, commits):
        """A branch off `main` with one commit for each (file, text, message)."""
        self.git('checkout', '-q', '-b', name, 'main')
        for file, text, message in commits:
            self.commit(file, text, message)
        self.git('checkout', '-q', 'main')

    def prs(self, file, *entries):
        Path(file).write_text(json.dumps(list(entries)))
        return str(file)


class OutstandingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Repo(self.tmp.name)
        self.prs = str(Path(self.tmp.name) / 'prs.json')

    def test_a_branch_without_a_pull_request_is_listed(self):
        self.repo.branch('topic', [('b.txt', 'b', 'the work')])
        self.repo.prs(self.prs)
        done = run(self.repo.path, '--prs', self.prs)
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn('topic', done.stdout)
        self.assertIn('1 branch(es)', done.stdout)

    def test_a_pull_request_that_names_the_branch_hides_it(self):
        self.repo.branch('topic', [('b.txt', 'b', 'the work')])
        sha = self.repo.git('rev-parse', 'topic').strip()
        self.repo.prs(self.prs, {'number': 1, 'headRefName': 'topic', 'headRefOid': sha, 'isDraft': True})
        done = run(self.repo.path, '--prs', self.prs)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('no branch holds work', done.stdout)

    def test_a_pull_request_of_another_name_holds_the_same_commits(self):
        self.repo.branch('topic', [('b.txt', 'b', 'the work')])
        sha = self.repo.git('rev-parse', 'topic').strip()
        self.repo.prs(self.prs, {'number': 2, 'headRefName': 'renamed', 'headRefOid': sha, 'isDraft': True})
        self.assertEqual(run(self.repo.path, '--prs', self.prs).returncode, 0)

    def test_a_commit_main_has_by_patch_is_not_outstanding(self):
        self.repo.branch('landed', [('c.txt', 'c', 'the work')])
        self.repo.git('checkout', '-q', 'main')
        self.repo.git('cherry-pick', 'landed')
        self.repo.prs(self.prs)
        done = run(self.repo.path, '--prs', self.prs)
        self.assertEqual(done.returncode, 0, done.stdout)

    def test_the_count_is_the_commits_main_lacks(self):
        self.repo.branch('topic', [('b.txt', 'b', 'one'), ('c.txt', 'c', 'two')])
        self.repo.prs(self.prs)
        done = run(self.repo.path, '--prs', self.prs, '--json')
        self.assertEqual(json.loads(done.stdout)[0]['commits'], 2)

    def test_a_remote_branch_names_its_remote(self):
        self.repo.branch('topic', [('b.txt', 'b', 'the work')])
        sha = self.repo.git('rev-parse', 'topic').strip()
        self.repo.git('update-ref', 'refs/remotes/fork/topic', sha)
        self.repo.prs(self.prs)
        done = run(self.repo.path, '--prs', self.prs)
        self.assertIn('fork/topic', done.stdout)

    def test_a_cherry_pick_with_another_patch_is_carried_by_its_subject(self):
        self.repo.branch('topic', [('b.txt', 'b', 'the work')])
        self.repo.branch('copy', [('b.txt', 'b, changed', 'the work')])
        sha = self.repo.git('rev-parse', 'copy').strip()
        self.repo.prs(self.prs, {'number': 3, 'headRefName': 'copy', 'headRefOid': sha, 'isDraft': True})
        done = run(self.repo.path, '--prs', self.prs)
        self.assertEqual(done.returncode, 0, done.stdout)

    def test_a_backup_branch_is_left_out(self):
        self.repo.branch('backup/topic', [('b.txt', 'b', 'the work')])
        self.repo.prs(self.prs)
        self.assertEqual(run(self.repo.path, '--prs', self.prs).returncode, 0)
        self.assertEqual(run(self.repo.path, '--prs', self.prs, '--skip', 'nothing/*').returncode, 1)


if __name__ == '__main__':
    unittest.main()
