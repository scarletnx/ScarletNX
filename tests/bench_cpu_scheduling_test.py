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
    def assert_descendant_exited(self, pid_file):
        self.assertTrue(pid_file.exists(), 'descendant must start before testing cleanup')
        stat = Path('/proc') / pid_file.read_text() / 'stat'
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                if stat.read_text().split(') ', 1)[1][0] == 'Z':
                    return
            except FileNotFoundError:
                return
            time.sleep(.01)
        self.fail('the worker left its descendant running after cleanup')

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
            self.assert_descendant_exited(pid_file)

    def test_timeout_stops_descendants(self):
        with tempfile.TemporaryDirectory(prefix='snx-scheduling-') as folder:
            pid_file = Path(folder) / 'child.pid'
            ready_file = Path(folder) / 'child.ready'
            # Keep a terminating child alive briefly so the parent exiting cannot hide the signal/exit race.
            child_code = ('import signal,sys,time; from pathlib import Path\n'
                          'def finish(*_):\n    time.sleep(.2)\n    raise SystemExit(0)\n'
                          'signal.signal(signal.SIGTERM,finish)\n'
                          'Path(sys.argv[1]).write_text("ready")\n'
                          'time.sleep(60)\n')
            code = ('import subprocess,sys,time; from pathlib import Path\n'
                    f'child=subprocess.Popen([sys.executable,"-c",{child_code!r},sys.argv[2]],'
                    'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)\n'
                    'while not Path(sys.argv[2]).exists(): time.sleep(.01)\n'
                    'Path(sys.argv[1]).write_text(str(child.pid))\n'
                    'time.sleep(60)\n')
            with self.assertRaises(subprocess.TimeoutExpired):
                probe.run_worker([sys.executable, '-c', code, str(pid_file), str(ready_file)], timeout=1)
            self.assert_descendant_exited(pid_file)


if __name__ == '__main__':
    unittest.main()
