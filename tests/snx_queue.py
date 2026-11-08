# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The lock and queue of the heavy work: the power check, the priority, the shared types, and the guard's use.

The tests of a waiting job run the tool as a program, in a folder of their own, and pass SNX_POWER_OK=1 because the
machine that runs them can be on battery. The tests of the power check patch the probe of the module, so they give
the same result on every machine.
"""

import importlib.util
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'scripts' / 'snx-queue.py'
GUARD = ROOT / '.claude' / 'hooks' / 'guard.py'


def tool():
    spec = importlib.util.spec_from_file_location('snx_queue', TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


queue = tool()


def env(folder):
    """the environment of a job of the test: its own queue folder, and the power permission that a person gives"""
    return {**os.environ, 'SNX_QUEUE_DIR': str(folder), 'SNX_POWER_OK': '1'}


def job(folder, log, name, seconds):
    """a command that writes its name and the mark + to the log, waits, then writes the mark -"""
    code = (
        f'import time\n'
        f'p = {str(log)!r}\n'
        f'open(p, "a").write({name!r} + "+\\n")\n'
        f'time.sleep({seconds})\n'
        f'open(p, "a").write({name!r} + "-\\n")\n'
    )
    return [sys.executable, '-c', code]


class PowerTests(unittest.TestCase):
    """the machine is read before a job starts"""

    def setUp(self):
        self.folder = tempfile.mkdtemp(prefix='snx-queue-')
        self.q = queue.Queue(self.folder)

    def test_free_machine_is_ready(self):
        with (
            mock.patch.object(queue, 'power_state', return_value='ac'),
            mock.patch.object(queue, 'active_use', return_value=False),
        ):
            self.assertEqual(self.q.check('gpu-exclusive'), (True, ''))
            self.assertEqual(self.q.check('cpu'), (True, ''))

    def test_battery_refuses_until_the_user_answers(self):
        with (
            mock.patch.object(queue, 'power_state', return_value='battery'),
            mock.patch.object(queue, 'active_use', return_value=False),
        ):
            ready, why = self.q.check('cpu')
            self.assertFalse(ready)
            self.assertIn('battery', why)
            self.assertIn('SNX_POWER_OK=1', why)
            with self.assertRaises(queue.Refused):
                self.q.acquire('cpu', max_wait=1)
            with self.q.acquire('cpu', max_wait=1, power_ok=True) as hold:
                self.assertEqual(hold.record['type'], 'cpu')
            self.assertEqual(self.q.holders(), [])

    def test_a_person_on_the_machine_refuses(self):
        with (
            mock.patch.object(queue, 'power_state', return_value='ac'),
            mock.patch.object(queue, 'active_use', return_value=True),
        ):
            ready, why = self.q.check('gpu-shared')
            self.assertFalse(ready)
            self.assertIn('works on the machine', why)

    def test_the_environment_carries_the_answer(self):
        with mock.patch.dict(os.environ, {'SNX_POWER_OK': '1'}, clear=False):
            self.assertTrue(queue.power_allowed(False))
        with mock.patch.dict(os.environ, {'SNX_POWER_OK': '0'}, clear=False):
            self.assertFalse(queue.power_allowed(False))
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertFalse(queue.power_allowed(False))


class QueueTests(unittest.TestCase):
    """the place of a job, the priority, and the shared types"""

    def setUp(self):
        self.folder = tempfile.mkdtemp(prefix='snx-queue-')
        self.log = Path(self.folder) / 'log'
        self.log.touch()
        self.q = queue.Queue(self.folder)

    def start(self, name, seconds, kind='gpu-exclusive', priority=0, pipes=True):
        return subprocess.Popen(
            [
                sys.executable,
                str(TOOL),
                'run',
                '--type',
                kind,
                '--priority',
                str(priority),
                '--reason',
                name,
                '--',
                *job(self.folder, self.log, name, seconds),
            ],
            env=env(self.folder),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE if pipes else subprocess.DEVNULL,
            text=True,
        )

    def finish(self, *jobs):
        for p in jobs:
            _, stderr = p.communicate(timeout=20)
            self.assertEqual(p.returncode, 0, stderr)

    def marks(self):
        return self.log.read_text().split()

    def test_priority_serves_the_largest_first(self):
        first = self.start('first', 0.8, priority=0)
        time.sleep(0.3)
        second = self.start('second', 0.1, priority=0)
        time.sleep(0.3)
        urgent = self.start('urgent', 0.1, priority=5)
        self.finish(first, second, urgent)
        starts = [m[:-1] for m in self.marks() if m.endswith('+')]
        self.assertEqual(starts, ['first', 'urgent', 'second'])

    def test_two_shared_jobs_run_together(self):
        a = self.start('a', 0.6, kind='gpu-shared')
        time.sleep(0.1)
        b = self.start('b', 0.6, kind='gpu-shared')
        self.finish(a, b)
        marks = self.marks()
        self.assertLess(marks.index('b+'), marks.index('a-'))  # b starts before a ends: they share the machine

    def test_an_exclusive_job_waits_for_the_shared_ones(self):
        a = self.start('a', 0.5, kind='gpu-shared')
        b = self.start('b', 0.5, kind='gpu-shared')
        time.sleep(0.2)
        alone = self.start('alone', 0.1, kind='gpu-exclusive')
        self.finish(a, b, alone)
        marks = self.marks()
        self.assertGreater(marks.index('alone+'), marks.index('a-'))
        self.assertGreater(marks.index('alone+'), marks.index('b-'))

    def test_the_status_names_the_holder_and_the_queue(self):
        holder = self.start('holder', 2)
        time.sleep(0.5)
        waiter = self.start('waiter', 0.1)
        time.sleep(0.5)
        state = self.q.status()
        self.assertEqual([h['reason'] for h in state['holders']], ['holder'])
        self.assertEqual([w['reason'] for w in state['queue']], ['waiter'])
        self.assertEqual(state['occupancy'], 'exclusive')
        self.assertFalse(self.q.check('cpu', power_ok=True)[0])
        self.finish(holder, waiter)
        self.assertEqual(self.q.status()['holders'], [])

    def test_a_waiter_gives_up_after_max_wait(self):
        holder = self.start('holder', 2)
        time.sleep(0.4)
        started = time.time()
        late = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                'run',
                '--type',
                'gpu-exclusive',
                '--max-wait',
                '1',
                '--reason',
                'late',
                '--',
                *job(self.folder, self.log, 'late', 0.1),
            ],
            env=env(self.folder),
            capture_output=True,
            text=True,
        )
        self.assertEqual(late.returncode, 1)
        self.assertIn('no place in the queue', late.stderr)
        self.assertIn('holder', late.stderr)
        self.assertLess(time.time() - started, 10)
        holder.communicate(timeout=20)

    @unittest.skipIf(os.name == 'nt', 'Windows has no SIGKILL and no shared flock')
    def test_the_kernel_frees_the_lock_when_the_holder_dies(self):
        holder = self.start('doomed', 30, pipes=False)
        time.sleep(0.5)
        self.assertEqual(self.q.occupancy(), 'exclusive')
        os.kill(holder.pid, signal.SIGKILL)
        holder.wait(timeout=20)
        time.sleep(0.2)
        self.assertEqual(self.q.occupancy(), 'none')
        started = time.time()
        with self.q.acquire('cpu', max_wait=5, power_ok=True):
            pass
        self.assertLess(time.time() - started, 3)

    def test_a_waiter_that_stopped_leaves_the_queue(self):
        stale = Path(self.folder) / 'queue' / '00000001-dead.json'
        stale.write_text(
            json.dumps(
                {
                    'id': 'dead',
                    'pid': 1,
                    'type': 'cpu',
                    'mode': 'exclusive',
                    'priority': 99,
                    'reason': 'gone',
                    'seq': 1,
                    'since': time.time(),
                }
            )
        )
        old = time.time() - (queue.TICKET_TTL + 10)
        os.utime(stale, (old, old))
        self.assertEqual(self.q._tickets(), [])
        self.assertFalse(stale.exists())


class GuardTests(unittest.TestCase):
    """a heavy command of another session waits for the lock"""

    def setUp(self):
        self.folder = tempfile.mkdtemp(prefix='snx-queue-')
        self.holder = subprocess.Popen(
            [
                sys.executable,
                str(TOOL),
                'run',
                '--type',
                'cpu',
                '--reason',
                'a build',
                '--',
                sys.executable,
                '-c',
                'import time; time.sleep(30)',
            ],
            env=env(self.folder),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.6)

    def tearDown(self):
        self.holder.kill()
        self.holder.wait(timeout=10)

    def guard(self, command):
        call = {'tool_name': 'Bash', 'tool_input': {'command': command}}
        return subprocess.run(
            [sys.executable, str(GUARD)],
            input=json.dumps(call),
            capture_output=True,
            text=True,
            env=env(self.folder),
        )

    def test_a_heavy_command_waits_for_the_holder(self):
        blocked = self.guard('SNX_POWER_OK=1 cmake --build build')
        self.assertEqual(blocked.returncode, 2)
        self.assertIn('snx-queue.py run --type cpu', blocked.stderr)
        self.assertIn('cpu pid %d' % self.holder.pid, blocked.stderr)

    def test_the_queue_itself_and_a_light_command_pass(self):
        self.assertEqual(
            self.guard('SNX_POWER_OK=1 scripts/snx-queue.py run --type cpu -- cmake --build build').returncode, 0
        )
        self.assertEqual(self.guard('grep -n add_test tests/CMakeLists.txt').returncode, 0)

    def test_a_heavy_command_passes_when_the_machine_is_free(self):
        self.holder.kill()
        self.holder.wait(timeout=10)
        time.sleep(0.3)
        self.assertEqual(self.guard('SNX_POWER_OK=1 cmake --build build').returncode, 0)


if __name__ == '__main__':
    unittest.main()
