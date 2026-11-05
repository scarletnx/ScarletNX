#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/ci-local.py: its commands, and the report of what this machine can run.

The commands that build and test are the developers' to run; this checks that the tool itself starts on every host,
names every command, and reports the tools rather than failing when one is missing. The run is quick and needs no
build.
"""

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'ci-local.py'


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, timeout=120)


def load_script():
    spec = importlib.util.spec_from_file_location('ci_local', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CiLocal(unittest.TestCase):
    def test_help_names_every_command(self) -> None:
        result = run('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in ('doctor', 'static', 'native', 'preflight', 'linux', 'android', 'ios', 'windows'):
            self.assertIn(command, result.stdout)

    def test_doctor_reports_this_machine(self) -> None:
        result = run('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('cmake', result.stdout)
        self.assertIn('ANDROID_NDK_ROOT', result.stdout)
        self.assertIn('The commands:', result.stdout)

    def test_a_docs_change_needs_no_build(self) -> None:
        module = load_script()
        self.assertTrue(module.static_only(['docs/DEVELOPMENT.md', 'CLAUDE.md', 'scripts/ci-local.py']))
        self.assertFalse(module.static_only(['docs/DEVELOPMENT.md', 'src/nx/runtime/main.c']))
        self.assertFalse(module.static_only([]))
        self.assertIn(module.power_state(), (None, 'ac', 'battery'))


if __name__ == '__main__':
    unittest.main()
