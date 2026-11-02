#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Worker failures and timeouts must stop competing processes too."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location('probe', Path(__file__).resolve().parents[1] / 'tools/bench_cpu_scheduling.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Workers(unittest.TestCase):
    def test_valid_result(self):
        self.assertEqual(probe.run_worker([sys.executable, '-c', 'import json; print(json.dumps(dict(ok=1)))']), {'ok': 1})

    def test_failed_worker_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'worker exited 7'):
            probe.run_worker([sys.executable, '-c', 'raise SystemExit(7)'])

    def test_failed_worker_stops_its_competitor(self):
        with tempfile.TemporaryDirectory(prefix='snx-scheduling-') as folder:
            pid_file = Path(folder) / 'child.pid'
            code = ('import subprocess,sys; from pathlib import Path; '
                    'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"],'
                    'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); '
                    'Path(sys.argv[1]).write_text(str(child.pid)); raise SystemExit(7)')
            with self.assertRaisesRegex(RuntimeError, 'worker exited 7'):
                probe.run_worker([sys.executable, '-c', code, str(pid_file)])
            stat = Path('/proc') / pid_file.read_text() / 'stat'
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                try:
                    if stat.read_text().split(') ', 1)[1][0] == 'Z':
                        return
                except FileNotFoundError:
                    return
                time.sleep(.01)
            self.fail('the failed worker left its competitor running')

    def test_timeout_stops_descendants(self):
        with tempfile.TemporaryDirectory(prefix='snx-scheduling-') as folder:
            pid_file = Path(folder) / 'child.pid'
            code = ('import subprocess,sys,time; from pathlib import Path; '
                    'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                    'Path(sys.argv[1]).write_text(str(child.pid)); time.sleep(60)')
            with self.assertRaises(subprocess.TimeoutExpired):
                probe.run_worker([sys.executable, '-c', code, str(pid_file)], timeout=1)
            self.assertTrue(pid_file.exists(), 'descendant must start before testing cleanup')
            stat = Path('/proc') / pid_file.read_text() / 'stat'
            if stat.exists():
                try:
                    state = stat.read_text().split(') ', 1)[1][0]
                except FileNotFoundError:
                    return
                self.assertEqual(state, 'Z', 'timed-out descendant must not remain runnable')


if __name__ == '__main__':
    unittest.main()
