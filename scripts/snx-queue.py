#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""One lock and one queue for the heavy work of this machine.

A heavy command heats the machine and drains its battery: a build, a test suite, a game run, a recompile, an extract.
Two of them at the same time give wrong frame times and a slow build. This tool gives each one a place in one queue,
and it reads the machine before the job starts.

    scripts/snx-queue.py status
    scripts/snx-queue.py check --type gpu-exclusive
    scripts/snx-queue.py run --type cpu -- cmake --build build
    scripts/snx-queue.py run --type gpu-exclusive --priority 5 --reason "crash 12 of 20" -- tools/run_game.py GAME

The type of use says what the job needs:

| Type | Mode | The job |
| --- | --- | --- |
| `cpu` | exclusive | a build, a test suite, a recompile, an extract |
| `gpu-shared` | shared | a game run or a benchmark, with other shared work |
| `gpu-exclusive` | exclusive | a measured game run, or a test of a driver |

Two jobs run together only when both are shared. The queue serves the largest priority first, and jobs of one
priority in the order that they asked. `--priority` is an integer, and a larger value goes sooner (default 0). Give a
job that a person waits for a larger value. `--max-wait` is the time in seconds that a job waits for its place
(default 1800, and 0 waits with no limit).

Before a job takes its place, the lock reads the machine. On battery power, or while the user works on the machine,
the job stops and asks for the permission of the user. Put `SNX_POWER_OK=1` in front of the command, or give
`--power-ok`, after the user answers (the rule of CLAUDE.md).

The state is one folder: `~/Library/Caches/scarletnx/queue` on macOS, `~/.cache/scarletnx/queue` on Linux and
`%LOCALAPPDATA%\\scarletnx\\queue` on Windows. `--dir` or `SNX_QUEUE_DIR` names another one, for a test.

The lock is an `flock` of `heavy.lock`, so the kernel releases it when the holder dies. The queue is one file for
each waiter, and a waiter that stops for 300 s leaves the queue. `tools/run_game.py` and the Bash guard
(`.claude/hooks/guard.py`) use this tool.
"""

import argparse
import contextlib
import importlib.util
import json
import os
import subprocess
import sys
import time
import uuid

TYPES = {
    'cpu': ('exclusive', 'the CPU and the memory: a build, a test suite, a recompile, an extract'),
    'gpu-shared': ('shared', 'the GPU, with other shared work: a game run, a benchmark'),
    'gpu-exclusive': ('exclusive', 'the GPU alone: a measured game run, a test of a driver'),
}
DEFAULT_WAIT = 1800  # seconds a job waits for its place in the queue, unless --max-wait says otherwise
POLL = 0.2  # seconds between two looks at the queue
TICKET_TTL = 300  # seconds without a heartbeat before a waiter leaves the queue
POWER_OK = 'SNX_POWER_OK=1'  # in front of a command: the user answered, as .claude/hooks/guard.py reads it


def power_allowed(flag):
    """whether the user allowed the command: --power-ok, or SNX_POWER_OK in the environment"""
    return flag or os.environ.get('SNX_POWER_OK', '') not in ('', '0')


class Refused(Exception):
    """the machine is not ready: battery power, or a person works on it"""


class Timeout(Exception):
    """the job did not get its place in the queue before --max-wait"""


def state_dir():
    """the folder of the queue, as the platform gives it, unless SNX_QUEUE_DIR names one"""
    named = os.environ.get('SNX_QUEUE_DIR')
    if named:
        return named
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    elif sys.platform == 'darwin':
        base = os.path.expanduser('~/Library/Caches')
    else:
        base = os.environ.get('XDG_CACHE_HOME') or ''
        if not os.path.isabs(base):
            base = os.path.expanduser('~/.cache')
    return os.path.join(base, 'scarletnx', 'queue')


def _lock(fd, shared):
    """take the flock of fd: True when the call got it, False when another process holds it"""
    if os.name == 'nt':
        import msvcrt

        if shared:  # Windows has no shared lock: a shared job takes the lock alone
            shared = False
        try:
            msvcrt.locking(fd.fileno(), msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False
    import fcntl

    try:
        fcntl.flock(fd.fileno(), (fcntl.LOCK_SH if shared else fcntl.LOCK_EX) | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def _block(fd):
    """the same flock, but it waits for the holder"""
    if os.name == 'nt':
        import msvcrt

        msvcrt.locking(fd.fileno(), msvcrt.LK_LOCK, 1)
        return
    import fcntl

    fcntl.flock(fd.fileno(), fcntl.LOCK_EX)


def power_state():
    """'ac', 'battery', or None when the machine has no battery"""
    if sys.platform == 'darwin':
        try:
            out = subprocess.run(['pmset', '-g', 'batt'], capture_output=True, text=True, timeout=3).stdout
        except (OSError, subprocess.SubprocessError):
            return None
        if 'Battery Power' in out:
            return 'battery'
        return 'ac' if 'AC Power' in out else None
    if sys.platform.startswith('linux'):
        folder = '/sys/class/power_supply'
        try:
            batteries = [n for n in os.listdir(folder) if n.startswith('BAT')]
        except OSError:
            return None
        if not batteries:
            return None
        for name in batteries:
            try:
                with open(os.path.join(folder, name, 'status')) as f:
                    status = f.read().strip().lower()
            except OSError:
                continue
            if status == 'discharging':
                return 'battery'
        return 'ac'
    return None


def active_use():
    """whether a person uses the machine now: a display that stays awake, or a game (as guard.py reads it)"""
    if sys.platform != 'darwin':
        return False
    try:
        out = subprocess.run(['pmset', '-g', 'assertions'], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    for line in out.splitlines():
        words = line.split()
        if len(words) == 2 and words[0] in ('NoDisplaySleepAssertion', 'PreventUserIdleDisplaySleep'):
            if words[1] == '1':
                return True
    return False


def pid_alive(pid):
    """whether the process is still there (the record of a dead holder goes on the next read)"""
    if os.name == 'nt':
        return True  # no cheap probe: a record of Windows goes when the lock goes
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def load():
    """this module, from a program that cannot import a file with a hyphen in its name"""
    spec = importlib.util.spec_from_file_location('snx_queue', os.path.abspath(__file__))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Hold:
    """the place of a job in the queue, held until release() or the end of the process"""

    def __init__(self, queue, fd, record):
        self.queue = queue
        self.fd = fd
        self.record = record

    def release(self):
        if self.fd is None:
            return
        self.queue._drop_holder(self.record['id'])
        self.fd.close()
        self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()
        return False


class Queue:
    def __init__(self, root=None):
        self.root = root or state_dir()
        self.lock_path = os.path.join(self.root, 'heavy.lock')
        self.state_path = os.path.join(self.root, 'state.json')
        self.state_lock = os.path.join(self.root, 'state.lock')
        self.queue_dir = os.path.join(self.root, 'queue')
        os.makedirs(self.queue_dir, exist_ok=True)

    # ---- the state file: the holders and the counter of the queue ----
    @contextlib.contextmanager
    def _state(self):
        fd = open(self.state_lock, 'a+')
        try:
            _block(fd)
            yield
        finally:
            fd.close()

    def _read(self):
        try:
            with open(self.state_path) as f:
                state = json.load(f)
        except (OSError, ValueError):
            return {'holders': [], 'seq': 0}
        if not isinstance(state, dict):
            return {'holders': [], 'seq': 0}
        state.setdefault('holders', [])
        state.setdefault('seq', 0)
        return state

    def _write(self, state):
        tmp = self.state_path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(state, f)
        os.replace(tmp, self.state_path)

    def holders(self):
        """the records of the holders, without the ones of a process that has gone"""
        holders = self._read()['holders']
        live = [h for h in holders if pid_alive(h.get('pid', 0))]
        if len(live) != len(holders):
            with self._state():
                state = self._read()
                state['holders'] = [h for h in state['holders'] if pid_alive(h.get('pid', 0))]
                self._write(state)
        return live

    def _add_holder(self, record):
        with self._state():
            state = self._read()
            state['holders'] = [h for h in state['holders'] if h.get('id') != record['id']]
            state['holders'].append(record)
            self._write(state)

    def _drop_holder(self, holder_id):
        with self._state():
            state = self._read()
            state['holders'] = [h for h in state['holders'] if h.get('id') != holder_id]
            self._write(state)

    # ---- who holds the machine ----
    def occupancy(self):
        """'none', 'shared' or 'exclusive': the lock of the machine, as the kernel has it now"""
        fd = open(self.lock_path, 'a+')
        try:
            exclusive_holder = not _lock(fd, True)
        finally:
            fd.close()
        if exclusive_holder:
            return 'exclusive'
        fd = open(self.lock_path, 'a+')
        try:
            return 'none' if _lock(fd, False) else 'shared'
        finally:
            fd.close()

    # ---- the queue: one file for each waiter ----
    def _tickets(self):
        now = time.time()
        live = []
        for name in sorted(os.listdir(self.queue_dir)):
            if not name.endswith('.json'):
                continue
            path = os.path.join(self.queue_dir, name)
            try:
                age = now - os.stat(path).st_mtime
                with open(path) as f:
                    record = json.load(f)
            except (OSError, ValueError):
                continue
            if age > TICKET_TTL:
                with contextlib.suppress(OSError):
                    os.remove(path)
                continue
            record['ticket'] = path
            record['waiting'] = round(age, 1)
            live.append(record)
        live.sort(key=lambda r: (-int(r.get('priority', 0)), int(r.get('seq', 0))))
        return live

    def _register(self, kind, priority, reason):
        with self._state():
            state = self._read()
            state['seq'] = int(state['seq']) + 1
            self._write(state)
            seq = state['seq']
        record = {
            'id': uuid.uuid4().hex[:12],
            'pid': os.getpid(),
            'type': kind,
            'mode': TYPES[kind][0],
            'priority': int(priority),
            'reason': reason,
            'seq': seq,
            'since': time.time(),
        }
        path = os.path.join(self.queue_dir, '%08d-%s.json' % (seq, record['id']))
        with open(path, 'w') as f:
            json.dump(record, f)
        record['ticket'] = path
        return record

    def _grantable(self, record):
        """whether the queue serves this waiter now: the head of the queue, and every shared waiter behind it"""
        granted = []
        for ticket in self._tickets():
            if all(ticket['mode'] == 'shared' and g['mode'] == 'shared' for g in granted):
                granted.append(ticket)
            else:
                break
        return any(g['id'] == record['id'] for g in granted)

    # ---- what a job asks ----
    def refuse(self, kind, power_ok=False):
        """why the machine is not ready for this type, or None"""
        if power_ok:
            return None
        state = power_state()
        if state == 'battery':
            return (
                f'the machine runs on battery power. Ask the user in the chat for this command, with its time and '
                f'its reason, then run it with {POWER_OK} in front of it.'
            )
        if active_use():
            return (
                'the user works on the machine now (a display that stays awake). Ask the user in the chat for this '
                f'command, then run it with {POWER_OK} in front of it.'
            )
        return None

    def check(self, kind, power_ok=False):
        """(ready, why): whether a job of this type starts now"""
        why = self.refuse(kind, power_ok)
        if why:
            return False, why
        mode = TYPES[kind][0]
        occupied = self.occupancy()
        if occupied == 'none':
            return True, ''
        if mode == 'exclusive':
            return False, f'another job holds the machine ({occupied})'
        return (occupied == 'shared'), ('' if occupied == 'shared' else 'a job holds the machine alone')

    def acquire(self, kind, priority=0, reason='', max_wait=DEFAULT_WAIT, power_ok=False, on_wait=None):
        """the place of this job: a Hold, after the queue serves it. Raises Refused or Timeout."""
        if kind not in TYPES:
            raise ValueError(f'unknown type {kind}: use one of {", ".join(TYPES)}')
        why = self.refuse(kind, power_ok)
        if why:
            raise Refused(why)
        record = self._register(kind, priority, reason)
        deadline = None if not max_wait else time.time() + max_wait
        announced = False
        try:
            while True:
                with contextlib.suppress(OSError):
                    os.utime(record['ticket'])  # the heartbeat: a waiter that stops leaves the queue
                if self._grantable(record):
                    fd = open(self.lock_path, 'a+')
                    if _lock(fd, record['mode'] == 'shared'):
                        self._add_holder(record)
                        return Hold(self, fd, record)
                    fd.close()
                if deadline is not None and time.time() > deadline:
                    raise Timeout(self._wait_message(kind, max_wait))
                if not announced and on_wait is not None:
                    on_wait(self.status())
                    announced = True
                time.sleep(POLL)
        finally:
            with contextlib.suppress(OSError):
                os.remove(record['ticket'])

    def _wait_message(self, kind, max_wait):
        parts = [f'no place in the queue for {kind} after {max_wait} s']
        holders = self.holders()
        if holders:
            parts.append('the holder is ' + '; '.join(_describe(h) for h in holders))
        else:
            parts.append('the machine is free now: run the command again')
        return ', '.join(parts)

    def status(self):
        """the machine and the queue, as a dict that --json prints"""
        holders = self.holders()
        return {
            'dir': self.root,
            'power': power_state(),
            'active_use': active_use(),
            'occupancy': self.occupancy(),
            'holders': holders,
            'queue': self._tickets(),
        }


def _describe(record):
    age = max(0.0, time.time() - float(record.get('since', time.time())))
    text = f'{record.get("type")} pid {record.get("pid")} for {age:.0f} s'
    if record.get('reason'):
        text += f' ({record["reason"]})'
    return text


def _describe_waiter(record):
    text = f'{record.get("type")} priority {record.get("priority")} pid {record.get("pid")}'
    if record.get('reason'):
        text += f' ({record["reason"]})'
    return text


def print_status(state, out=sys.stdout):
    power = state['power'] or 'no battery'
    print(f'machine: {power}, {state["occupancy"]} lock', file=out)
    if state['active_use']:
        print('the user works on the machine now', file=out)
    if not state['holders']:
        print('holders: none', file=out)
    for record in state['holders']:
        print(f'holder: {_describe(record)}', file=out)
    if not state['queue']:
        print('queue: empty', file=out)
    for place, record in enumerate(state['queue'], 1):
        print(f'queue {place}: {_describe_waiter(record)}, waiting {record.get("waiting", 0)} s', file=out)


def main(argv):
    if '--' in argv:
        cut = argv.index('--')
        head, command = argv[:cut], argv[cut + 1 :]
    else:
        head, command = argv, []
    parser = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0], allow_abbrev=False, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument('--dir', help='the folder of the queue (default: the cache folder of this user)')
    parser.add_argument('--json', action='store_true', help='print the state of the machine and the queue as JSON')
    sub = parser.add_subparsers(dest='action', required=True)

    sub.add_parser('status', help='print the machine, the holders and the queue')

    check = sub.add_parser('check', help='say whether a job of this type starts now: exit 0 when it does')
    check.add_argument('--type', required=True, choices=sorted(TYPES))
    check.add_argument('--power-ok', action='store_true', help='the user allowed this command on this machine now')

    run = sub.add_parser('run', help='take the place of this job, run the command, and release the place')
    run.add_argument('--type', required=True, choices=sorted(TYPES), help='the type of use of the job')
    run.add_argument('--priority', type=int, default=0, help='a larger value goes sooner (default 0)')
    run.add_argument('--reason', default='', help='what the job is, for the queue and for the other sessions')
    run.add_argument('--max-wait', type=int, default=DEFAULT_WAIT, help=f'seconds to wait (default {DEFAULT_WAIT})')
    run.add_argument('--power-ok', action='store_true', help='the user allowed this command on this machine now')
    run.add_argument('--quiet', action='store_true', help='no line while the job waits for its place')

    args = parser.parse_args(head)
    queue = Queue(args.dir)

    if args.action == 'status':
        state = queue.status()
        if args.json:
            print(json.dumps(state, indent=2))
        else:
            print_status(state)
        return 0

    if args.action == 'check':
        ready, why = queue.check(args.type, power_allowed(args.power_ok))
        if args.json:
            print(json.dumps({'type': args.type, 'ready': ready, 'why': why}, indent=2))
        else:
            print(f'{args.type}: ready' if ready else f'{args.type}: wait. {why}')
        return 0 if ready else 1

    if not command:
        parser.error('run needs the command after --, for example: run --type cpu -- cmake --build build')
    try:
        with queue.acquire(
            args.type,
            priority=args.priority,
            reason=args.reason,
            max_wait=args.max_wait,
            power_ok=power_allowed(args.power_ok),
            on_wait=None if args.quiet else _announce_wait,
        ):
            return _run(command)
    except Refused as e:
        print(f'snx-queue: {e}', file=sys.stderr)
        return 2
    except Timeout as e:
        print(f'snx-queue: {e}', file=sys.stderr)
        return 1


def _announce_wait(state):
    holders = '; '.join(_describe(h) for h in state['holders']) or 'no holder record'
    print(f'snx-queue: waiting for the machine ({holders})', file=sys.stderr, flush=True)


def _run(command):
    """the command, with the place of the job held until it ends"""
    try:
        return subprocess.run(command).returncode
    except KeyboardInterrupt:
        return 130
    except FileNotFoundError:
        print(f'snx-queue: {command[0]}: not found', file=sys.stderr)
        return 127


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
