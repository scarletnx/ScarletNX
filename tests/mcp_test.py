#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scarletnx --mcp (src/nx/runtime/driver/mcp.c) driven over stdio as an MCP client does: initialize, the tool list,
scan_log, read_log by text, by range and its last lines, and its refusal of a file outside the data folder, find_game
on a made-up extracted game (its NPDM's program ID, its NACP's version), symbolize and disassemble on a made-up NRO
(nx_runtime_smoke.py's), the errors a tool answers with, an unknown method, a notification left unanswered and a line
that isn't JSON, and on UNIX the GDB tools against a made-up GDB (FAKE_GDB). Offline.

    tests/mcp_test.py SCARLETNX"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nx_runtime_smoke import make_nro

LOG = """[   0.00] ScarletNX - Some Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   0.30] game: 0100aaaabbbb0000, version 1.2.3
[   1.00] sm: GetServiceHandle(hid)
[   2.25]
*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.
"""


# the parts of an edenprobe checkout the scaffold writes into, as edenprobe lays them out
CHECKOUT = {
    'source/serviceref.h': (
        'typedef enum {\n    SERVICE_RANDOM, ///< csrng: two asks fill their buffers, differently\n'
        '    SERVICE_CASES\n} ServiceCaseId;\n\nbool ServiceJudgeRandom(uint32_t rc, char *out, size_t out_size);\n\n'
        '#ifdef __cplusplus\n}\n#endif\n'
    ),
    'source/serviceref.c': (
        'static const struct {\n    const char *suite, *name;\n} CASES[] = {\n'
        '    [SERVICE_RANDOM] = { "csrng", "GenerateRandomBytes fills the buffer" },\n};\n'
    ),
    'source/tests/t_service.c': 'static void RunCsrng(void) {}\n\nvoid RunService(void) {\n    RunCsrng();\n}\n',
    'source/tests/t_serviceref.c': 'static void CheckRandom(void) {}\n\nvoid RunReferenceService(void) {\n    CheckRandom();\n}\n',
    'source/ui.c': '    { "csrng", UI_FAMILY_SERVICE },\n    { "nifm", UI_FAMILY_NET },\n',
}
NAME = 'GetNpadJoyHoldType answers the hold type set before it'


def report(result, detail):
    lines = [
        {'type': 'note', 'text': 'edenprobe 0.5.0 (abc1234) starting'},
        {'type': 'case', 'suite': 'hid-style', 'name': NAME, 'tier': 'exact', 'result': result, 'detail': detail},
        {'type': 'summary', 'passed': 1, 'failed': 0, 'skipped': 0, 'crashed': 0},
    ]
    return ''.join(json.dumps(line) + '\n' for line in lines)


# A GDB that answers the MI commands the GDB tools send (mcp_gdb.c) as GDB 14 does, over a made-up game: thread 1 the
# runtime's, thread 2 a guest thread whose KThread (at 0x5000, the layout below) names its SDK thread and frame
# records in guest memory. It logs each command it reads to the file FAKE_GDB_LOG names
FAKE_GDB = r'''
import os, re, signal, sys, threading

LOG = open(os.environ['FAKE_GDB_LOG'], 'a')
MEM, BASE, END = 0x7F0000000000, 0x8000000, 0x10000000
LAYOUT = {'sizeof(KThread)': 416, 'id': 0, 'name': 8, 'state': 40, 'in_svc': 44, 'tls': 48, 'arg': 56, 'cpu.x': 64,
          'cpu.pc': 312, 'cpu.n': 320, 'cpu.tpidrro': 336, 'arm.r': 344, 'arm.cpsr': 408}
TLS, TYPE, NAME, FP = 0x9000000, 0x9001000, 0x9002000, 0x9003000


def q64(v):
    return v.to_bytes(8, 'little')


kthread = bytearray(416)
kthread[0:8] = q64(72)
kthread[8:13] = b'kname'
kthread[44:48] = (1).to_bytes(4, 'little')
kthread[48:56], kthread[56:64] = q64(TLS), q64(TYPE)
kthread[64 + 29 * 8:64 + 31 * 8] = q64(FP) + q64(0x8000088)
kthread[312:320] = q64(0x8000080)
memory = {
    0x5000: bytes(kthread),
    MEM + TLS + 0x1F8: q64(TYPE),
    MEM + TYPE + 0x46: (1).to_bytes(2, 'little'),
    MEM + TYPE + 0x1A0: q64(NAME),
    MEM + NAME: b'MainThread\0',
    MEM + FP: q64(FP + 16) + q64(0x8000084),
    MEM + FP + 16: q64(0) + q64(0x8000090),
    MEM + 0x8000080: b'ABCD',
}
values = {'(unsigned long)g_mem': MEM, 'g_layout.base': BASE, 'g_layout.end': END, '(unsigned long)g_arm_mod': 0,
          '$_thread': 2}
lock = threading.Lock()
continues = 0


def say(*lines):
    with lock:
        sys.stdout.write(''.join(line + '\n' for line in lines))
        sys.stdout.flush()


def cstring(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def unquote(s):
    return re.sub(r'\\(.)', lambda m: '\n' if m.group(1) == 'n' else m.group(1), s[1:-1])


def read(address, size):
    for at, data in memory.items():
        if at <= address and address + size <= at + len(data):
            return data[address - at:address - at + size]
    return None


def later(seconds, *lines):
    threading.Timer(seconds, say, lines).start()


STOP_BREAK = ('*stopped,reason="breakpoint-hit",disp="keep",bkptno="7",frame={addr="0x1",func="crash_report",args=[],'
              'file="/home/u/src/x/src/nx/runtime/crash.c",line="9"},thread-id="2",stopped-threads="all"')
STOP_INT = ('*stopped,reason="signal-received",signal-name="SIGINT",signal-meaning="Interrupt",frame={addr="0x7f",'
            'func="poll",args=[],from="/usr/lib/libc.so.6"},thread-id="1",stopped-threads="all"')
# an interrupt is a SIGINT to the game, which gdbserver reports as a stop: this process plays the game
signal.signal(signal.SIGINT, lambda *_: later(0.05, STOP_INT))
say('=thread-group-added,id="i1"', '(gdb)')
for raw in sys.stdin:
    token, command = re.match(r'(\d*)(.*)', raw.rstrip('\n')).groups()
    LOG.write(command + '\n')
    LOG.flush()
    done = token + '^done'
    if command.startswith('-gdb-set debuginfod'):
        say(token + '^error,msg="No symbol \\"debuginfod\\" in current context."', '(gdb)')
    elif command.startswith('-gdb-set') or command.startswith('-break-insert') and 'crash_report' not in command:
        say(done, '(gdb)')
    elif command.startswith('-target-select'):
        say('~"Remote debugging using 127.0.0.1\\n"', token + '^connected',
            '*stopped,frame={addr="0x7f",func="poll",args=[],from="/usr/lib/libc.so.6"},thread-id="1"', '(gdb)')
    elif command == '-list-thread-groups':
        say(done + ',groups=[{id="i1",type="process",pid="%d",executable="/x/scarletnx"}]' % os.getpid(), '(gdb)')
    elif command.startswith('-break-insert'):
        say(done + ',bkpt={number="7",type="breakpoint",func="crash_report"}', '(gdb)')
    elif command.startswith('-data-evaluate-expression'):
        m = re.match(r'-data-evaluate-expression (?:--thread (\d+) )?(".*")$', command)
        expr = unquote(m.group(2))
        member = re.match(r'\(long\)\(&\(\(KThread\*\)0\)->(.*)\)$', expr)
        size = re.match(r'\(long\)\((sizeof\(KThread\))\)$', expr)
        if member or size:
            value = LAYOUT[(member or size).group(1)]
        elif expr == '(unsigned long)tls_thread':
            value = 0x5000 if m.group(1) == '2' else 0
        else:
            value = values.get(expr)
        if value is None:
            say(token + '^error,msg=' + cstring('No symbol in current context.'), '(gdb)')
        else:
            say(done + ',value="%d"' % value, '(gdb)')
    elif command.startswith('-data-read-memory-bytes'):
        address, size = command.split()[1:3]
        data = read(int(address, 16), int(size))
        if data is None:
            say(token + '^error,msg=' + cstring('Unable to read memory.'), '(gdb)')
        else:
            say(done + ',memory=[{begin="%s",offset="0x0000000000000000",end="0x0",contents="%s"}]'
                % (address, data.hex()), '(gdb)')
    elif command == '-thread-info':
        frame = 'frame={level="0",addr="0x7f",func="poll",args=[],from="/usr/lib/libc.so.6"}'
        say(done + ',threads=[{id="1",target-id="Thread 0x7f01 (LWP 100)",name="scarletnx",' + frame
            + ',state="stopped"},{id="2",target-id="Thread 0x7f02 (LWP 101)",name="Guest 0",' + frame
            + ',state="stopped"}],current-thread-id="1"', '(gdb)')
    elif command.startswith('-stack-list-frames'):
        say(done + ',stack=[frame={level="0",addr="0x7f",func="poll",from="/usr/lib/libc.so.6"},'
            'frame={level="1",addr="0x2",func="kwait",file="/home/u/src/x/src/nx/hle/kernel/kernel.c",line="42"},'
            'frame={level="2",addr="0x3",func="svc_WaitSynchronization",file="/home/u/src/x/src/nx/hle/svc/svc.c",'
            'line="7"}]', '(gdb)')
    elif command.startswith('-interpreter-exec console '):
        cli = unquote(command[len('-interpreter-exec console '):])
        if cli == 'print esc':
            say(r'~"\101\tB\"q\"\\\n"', done, '(gdb)')
        elif cli == 'backtrace':
            say('~' + cstring('#0  F_8000080 () at /home/u/src/x/src/nx/a.c:1\n'), done, '(gdb)')
        else:
            say('~' + cstring('ran: ' + cli + '\n'), done, '(gdb)')
    elif command.startswith('-exec-continue'):
        continues += 1
        say(token + '^running', '*running,thread-id="all"', '(gdb)')
        if continues == 1:
            later(0.3, STOP_BREAK)
        elif continues == 2:
            say('~"tick\\n"')
        elif continues == 4:
            later(0.3, '*stopped,reason="exited",exit-code="03"')
    elif command.startswith('-exec-next'):
        say(token + '^running', '*running,thread-id="all"', '(gdb)',
            '*stopped,reason="end-stepping-range",frame={addr="0x8000084",func="F_8000080",args=[]},thread-id="2"')
    elif command == '-target-detach':
        say(done, '(gdb)')
    elif command == '-gdb-exit':
        say(token + '^exit')
        break
    else:
        say(token + '^error,msg="unknown"', '(gdb)')
'''


def gdb_session(runtime, data, nro, tmp):
    """The GDB tools against FAKE_GDB, a request at a time: attach, the commands gdb_command runs and those it refuses,
    a resume that stops, one left running and interrupted, one interrupted when its wait ends, the game's end"""
    bin_dir, log = tmp / 'bin', tmp / 'gdb.log'
    bin_dir.mkdir()
    (bin_dir / 'gdb').write_text(f'#!{sys.executable}\n' + FAKE_GDB)
    (bin_dir / 'gdb').chmod(0o755)
    env = dict(os.environ, PATH=f'{bin_dir}{os.pathsep}{os.environ.get("PATH", "")}', FAKE_GDB_LOG=str(log))
    server = subprocess.Popen(
        [runtime, '--mcp', '--repo', str(data)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env
    )

    def ask(tool, **arguments):
        server.stdin.write(json.dumps(call(tool, **arguments)) + '\n')
        server.stdin.flush()
        reply = json.loads(server.stdout.readline())['result']
        return reply.get('isError', False), reply['content'][0]['text']

    def gdb(command, **arguments):
        return ask('gdb_command', command=command, **arguments)

    try:
        bad, said = ask('gdb_attach', port=4321, game=str(nro))
        check(not bad and '127.0.0.1:4321' in said and 'Breakpoint 7 is on crash_report' in said, f'attach: {said}')
        check('threads can' not in said, f'the KThread layout read: {said}')
        bad, said = ask('gdb_attach', port=4321)
        check(bad and 'already attached' in said, f'a second attach: {said}')
        for refused in ('she echo x', 'shell ls', 'commands 1', 'thread apply all bt', 'frame apply 2 p 1',
                        'set logging file x', 'maint set x', 'p $_shell("id")', 'p 1\nshell ls', 'dis', 'c 3',
                        'n x', 'advance', 'interrupt'):
            bad, said = gdb(refused)
            check(bad, f'{refused!r} refused: {said}')
        bad, said = gdb('p 1+1')
        check(not bad and 'ran: print 1+1' in said, f'an alias run by its word: {said}')
        check(gdb('inf threads')[1].startswith('ran: info threads'), 'a prefix run as the full word')
        check(gdb('set var x = 1')[1].startswith('ran: set var x = 1'), 'set var')
        bad, said = gdb('bt')
        check(not bad and said.startswith('#0  F_8000080 [main') and ' at src/nx/a.c:1' in said, f'named, cut: {said}')
        check(gdb('print esc')[1] == 'A\tB"q"\\\n', f'MI escapes: {gdb("print esc")[1]!r}')

        bad, said = ask('gdb_threads')
        check(not bad and "2 Thread 0x7f02 (LWP 101) 'Guest 0': guest thread 72 'MainThread', in an SVC" in said, said)
        check('pc=8000080 [main' in said and 'lr=8000088 [main' in said, f'guest pc and lr, named: {said}')
        check('kwait (src/nx/hle/kernel/kernel.c:42) < svc_WaitSynchronization (src/nx/hle/svc/svc.c:7)' in said, said)
        check("1 Thread 0x7f01 (LWP 100) 'scarletnx': the runtime's, at kwait (src/nx/" in said, said)
        check('1 guest threads, 1 of the runtime' in said, said)
        bad, said = ask('gdb_guest')
        check(not bad and said.startswith("guest thread 72 'MainThread' (GDB thread 2), in an SVC"), said)
        check('x29=0000000009003000' in said and '  #1 8000084 [main' in said and '  #2 8000090 [main' in said, said)
        bad, said = ask('gdb_guest', thread=1)
        check(bad and 'not a guest thread' in said, f'the runtime thread: {said}')
        bad, said = ask('gdb_memory', address='8000080', size=4)
        check(not bad and said == '0008000080  41 42 43 44' + ' ' * 38 + 'ABCD\n', f'memory: {said!r}')
        check('outside the guest' in ask('gdb_memory', address='7000000', size=4)[1], 'below the guest')
        check('Unable to read' in ask('gdb_memory', address='9100000', size=4)[1], 'unreadable')

        bad, said = gdb('continue', wait=5)
        check(not bad and 'stopped (breakpoint-hit 7) in thread 2 at crash_report (src/nx/runtime/crash.c:9)' in said,
              f'a resume that stops: {said}')
        bad, said = gdb('continue', wait=0)
        check(not bad and 'The game is running' in said, f'left running: {said}')
        time.sleep(0.5)
        check('running' in gdb('p 1')[1] and 'running' in ask('gdb_threads')[1], 'nothing runs while it runs')
        bad, said = gdb('interrupt')
        check(not bad and said.startswith('tick\n') and 'stopped (signal-received SIGINT) in thread 1' in said,
              f'interrupted, with what GDB said meanwhile: {said}')
        bad, said = gdb('continue', wait=1)
        check(not bad and 'Still running after 1 s: interrupted.' in said and 'SIGINT' in said, f'the wait: {said}')
        bad, said = gdb('n 2')
        check(not bad and 'stopped (end-stepping-range) in thread 2 at F_8000080 [main' in said, f'next: {said}')
        bad, said = gdb('continue', wait=5)
        check(not bad and 'the game exited with code 03' in said, f'the end: {said}')
        bad, said = gdb('bt')
        check(bad and 'exited' in said, f'after the end: {said}')
        bad, said = ask('gdb_detach')
        check(not bad and 'Detached' in said, said)
        check('gdb_attach' in ask('gdb_threads')[1], 'no GDB after the detach')
    finally:
        server.stdin.close()
        server.wait(timeout=30)
    sent = log.read_text().splitlines()
    check('-interpreter-exec console "print 1+1"' in sent and '-exec-next 2' in sent, f'what GDB was sent: {sent}')
    check('-target-select remote 127.0.0.1:4321' in sent and sent[-1] == '-gdb-exit', 'connected, ended')
    check('-target-detach' not in sent, 'no detach from a game that exited')
    check('-exec-interrupt' not in sent, 'interrupts signal the game')
    check(not any('shell' in s or 'apply' in s or 'logging' in s for s in sent), 'nothing refused reached GDB')


def check(cond, what):
    if not cond:
        sys.exit(f'mcp_test: {what}')


def npdm(program_id):
    """A main.npdm with what the loader reads: META, 64-bit, the ACI0's offset and size, its program ID"""
    data = bytearray(0x100)
    data[0:4] = b'META'
    data[0x0C] = 1
    struct.pack_into('<II', data, 0x70, 0x80, 0x40)
    data[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', data, 0x90, program_id)
    return bytes(data)


def call(tool, id_=None, **arguments):
    return {
        'jsonrpc': '2.0',
        'id': id_ or tool,
        'method': 'tools/call',
        'params': {'name': tool, 'arguments': arguments},
    }


def main():
    runtime = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        data, outside = Path(tmp) / 'data', Path(tmp) / 'outside'
        game = data / 'work' / 'Some Game'
        (game / 'exefs').mkdir(parents=True)
        (game / 'exefs' / 'main').write_bytes(b'\0' * 16)
        (game / 'exefs' / 'main.npdm').write_bytes(npdm(0x0100AAAABBBB0000))
        nacp = bytearray(0x4000)
        nacp[0x3060:0x3065] = b'1.2.3'
        (game / 'control.nacp').write_bytes(bytes(nacp))
        (data / 'games').mkdir()
        log = data / 'games' / 'game.log'
        log.write_text(LOG)
        outside.mkdir()
        (outside / 'secret.log').write_text(LOG)
        (outside / 'secret.nro').write_bytes(make_nro())
        nro = data / 'smoke.nro'
        nro.write_bytes(make_nro())
        # a clone on Windows has CRLF lines (git's autocrlf): the scaffold keeps each file's ending. Written as bytes,
        # so every system writes the same
        checkout, crlf, broken = Path(tmp) / 'edenprobe', Path(tmp) / 'crlf', Path(tmp) / 'broken'
        for root, ending in ((checkout, '\n'), (crlf, '\r\n'), (broken, '\n')):
            for rel, text in CHECKOUT.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(text.replace('\n', ending).encode())
        (broken / 'source/serviceref.h').write_bytes(b'typedef enum {\n} ServiceCaseId;\n')
        ours, theirs, passing = Path(tmp) / 'ours.jsonl', Path(tmp) / 'theirs.jsonl', Path(tmp) / 'passing.jsonl'
        ours.write_text(report('fail', 'GetNpadJoyHoldType: rc 0xca8c0500'))
        theirs.write_text(report('pass', 'hold type 1, rc 0x0'))
        passing.write_text(report('pass', 'hold type 1, rc 0x0'))
        hold = {
            'family': 'service',
            'suite': 'hid-style',
            'id': 'HID_HOLD_TYPE',
            'name': NAME,
            'tier': 'exact',
            'issue': 345,
        }

        requests = [
            {'jsonrpc': '2.0', 'id': 'init', 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 'list', 'method': 'tools/list'},
            call('scan_log', path=str(log)),
            call('read_log', 'grep', path=str(log), pattern='SVCBREAK|nothing', ignore_case=True, context=1),
            call('read_log', 'range', path=str(log), start=2, count=2),
            call('read_log', 'tail', path=str(log), count=3),
            call('read_log', 'outside', path=str(outside / 'secret.log')),
            call('scan_log', 'scan_outside', path=str(outside / 'secret.log')),
            call('symbolize', 'game_outside', game=str(outside / 'secret.nro'), addresses=['8000000']),
            call('find_game', title_id='0100aaaabbbb0000'),
            call('find_game', 'find_none', title_id='0100000000000001'),
            call('state_info', title_id='0100AAAABBBB0000'),
            call('state_info', 'states_none', title_id='0100000000000001'),
            call('symbolize', game=str(nro), addresses=['8000080', 'zz']),
            call('disassemble', game=str(nro), start='8000000', end='8000094'),
            call('disassemble', 'no_range', game=str(nro)),
            call('symbolize', 'no_game', game='0100000000000001', addresses=['1']),
            call('edenprobe_scaffold', 'scaffold', checkout=str(checkout), **hold),
            call('edenprobe_scaffold', 'again', checkout=str(checkout), **hold),
            call('edenprobe_scaffold', 'crlf', checkout=str(crlf), **hold),
            call('edenprobe_scaffold', 'broken', checkout=str(broken), **hold),
            call('edenprobe_scaffold', 'bad_id', checkout=str(checkout), **dict(hold, id='hold')),
            call('edenprobe_scaffold', 'cpu', checkout=str(checkout), **dict(hold, family='cpu')),
            call('edenprobe_check', 'no_host', checkout=str(data)),
            call('edenprobe_run', 'no_nro', nro=str(data / 'none.nro'), mode='quick notime service', filter='hid'),
            call(
                'edenprobe_finding',
                'finding',
                issue=345,
                suite='hid-style',
                name=NAME,
                scarletnx_report=str(ours),
                reference_report=str(theirs),
                source='reden answers what SetNpadJoyHoldType set.',
            ),
            call(
                'edenprobe_finding', 'passing', issue=345, suite='hid-style', name=NAME, scarletnx_report=str(passing)
            ),
            call('gdb_threads', 'gdb_none'),
            call('gdb_command', 'gdb_command_none', command='bt'),
            call('gdb_attach', 'gdb_bad_port', port=0),
            call('gdb_guest', 'gdb_guest_none', thread=2),
            call('gdb_memory', 'gdb_memory_bad', address='zz'),
            call('gdb_detach', 'gdb_detach_none'),
            call('nope'),
            {'jsonrpc': '2.0', 'id': 'method', 'method': 'resources/list'},
        ]
        # a save state of the game in the data folder's games/, from a runtime of another SNX_ABI
        # (common/utilities/state_header.c's header)
        states = data / 'games' / '0100AAAABBBB0000' / 'states'
        states.mkdir(parents=True)
        header = bytearray(512)
        header[0:8] = b'SNXSTATE'
        struct.pack_into('<IIQQQII', header, 8, 1, 0, 0x0100AAAABBBB0000, 1700000000, 90 * 10**9, 0, 0)
        header[64:69] = b'older'
        (states / '0100AAAABBBB0000.3.snxstate').write_bytes(bytes(header))
        stdin = ''.join(json.dumps(r) + '\n' for r in requests) + 'not json\n'
        run = subprocess.run(
            [runtime, '--mcp', '--repo', str(data)],
            input=stdin,
            capture_output=True,
            text=True,
            timeout=120,
            check=True,
        )
        written = {rel: (checkout / rel).read_bytes().decode() for rel in CHECKOUT}
        crlf_written = {rel: (crlf / rel).read_bytes().decode() for rel in CHECKOUT}
        untouched = all(
            (broken / rel).read_bytes() == text.encode() for rel, text in CHECKOUT.items() if 'serviceref.h' not in rel
        )
        if os.name != 'nt':  # the GDB tools are UNIX only
            gdb_session(runtime, data, nro, Path(tmp))
    replies = [json.loads(line) for line in run.stdout.splitlines()]
    by_id = {r['id']: r for r in replies}
    check(
        len(replies) == len(requests),
        f'a reply a request but the notification, and one for the bad line: {len(replies)}',
    )
    check(any(r['id'] is None and r['error']['code'] == -32700 for r in replies), 'a line that is not JSON')

    def text(name):
        return by_id[name]['result']['content'][0]['text']

    def failed(name):
        return by_id[name]['result'].get('isError', False)

    init = by_id['init']['result']
    check(init['protocolVersion'] == '2025-06-18' and 'tools' in init['capabilities'], 'initialize')
    check('same game version' in init['instructions'], 'the instructions')
    tools = {t['name']: t for t in by_id['list']['result']['tools']}
    check(
        set(tools)
        == {
            'scan_log',
            'read_log',
            'find_game',
            'state_info',
            'symbolize',
            'disassemble',
            'edenprobe_scaffold',
            'edenprobe_check',
            'edenprobe_run',
            'edenprobe_finding',
            'gdb_attach',
            'gdb_command',
            'gdb_threads',
            'gdb_guest',
            'gdb_memory',
            'gdb_detach',
        },
        f'tools: {sorted(tools)}',
    )
    check(all(t['inputSchema']['type'] == 'object' and len(t['description']) > 40 for t in tools.values()), 'schemas')
    check(tools['symbolize']['inputSchema']['required'] == ['game', 'addresses'], 'required arguments')

    scan = json.loads(text('scan_log'))
    check(scan['signature'] == 'svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv', f'scan_log: {scan["signature"]}')
    check(scan['failure']['function']['demangled'] == 'nn::hid::GetNpadJoyHoldType()', 'demangled in the binary')

    grep = text('grep').splitlines()
    check(grep[0].endswith('has 9 lines'), grep[0])
    check(any(line.startswith('6: *** The game called svcBreak') for line in grep), f'read_log by text:\n{grep}')
    # two matches (the marker and FATAL's line), a line of context each side, joined
    check([line.split(':')[0] for line in grep[1:]] == ['5', '6', '7', '8', '9'], f'context each side: {grep}')
    check([line.split(':')[0] for line in text('range').splitlines()[1:]] == ['2', '3'], 'read_log by range')
    check([line.split(':')[0] for line in text('tail').splitlines()[1:]] == ['7', '8', '9'], 'read_log, the last lines')
    check(failed('outside') and 'outside the folders' in text('outside'), 'read_log refuses a file outside')
    check(failed('scan_outside') and 'outside the folders' in text('scan_outside'), 'so does scan_log')
    check(failed('game_outside') and 'outside the folders' in text('game_outside'), 'and symbolize, for a game')

    games = json.loads(text('find_game'))
    check(
        len(games) == 1 and games[0]['title_id'] == '0100AAAABBBB0000' and games[0]['version'] == '1.2.3',
        f'find_game: {games}',
    )
    check(json.loads(text('find_none')) == [], 'find_game with another title ID')
    states = json.loads(text('state_info'))
    check(
        len(states) == 1
        and states[0]['slot'] == '3'
        and states[0]['program'] == '0100AAAABBBB0000'
        and states[0]['play_time_s'] == 90
        and states[0]['runtime'] == 'older'
        and 'SNX_ABI' in (states[0]['cannot_load'] or ''),
        f'state_info: {states}',
    )
    check(json.loads(text('states_none')) == [], 'state_info with another title ID')

    sym = text('symbolize').splitlines()
    check(sym[0].startswith('8000080 main') and 'not a hexadecimal address' in sym[1], f'symbolize: {sym}')
    dis = text('disassemble')
    if 'no A64 disassembler' not in dis:
        check('b\t0x80' in dis.replace(' ', '\t') or '8000080' in dis.splitlines()[0], f'the first branch:\n{dis}')
        check('svc' in dis and 'adr' in dis, f'the code disassembled:\n{dis}')
    check(failed('no_range') and 'name a function, or a range' in text('no_range'), 'disassemble without a target')
    check(failed('no_game') and 'title ID' in text('no_game'), 'a title ID with no game')
    # without a game under the stub (and on Windows, where they are UNIX only) the GDB tools fail, saying why
    def gdb_fails(name, why):
        return failed(name) and (why if os.name != 'nt' else 'UNIX only') in text(name)

    check(gdb_fails('gdb_none', 'gdb_attach'), 'gdb_threads without GDB')
    check(gdb_fails('gdb_command_none', 'gdb_attach'), 'gdb_command too')
    check(gdb_fails('gdb_guest_none', 'gdb_attach'), 'gdb_guest too')
    check(failed('gdb_memory_bad') and 'hexadecimal' in text('gdb_memory_bad'), 'gdb_memory checks the address')
    check(gdb_fails('gdb_bad_port', 'port'), 'gdb_attach checks the port')
    check(gdb_fails('gdb_detach_none', 'gdb_attach'), 'gdb_detach without GDB')
    check(failed('nope'), 'an unknown tool')
    check(by_id['method']['error']['code'] == -32601, 'an unknown method')
    # the scaffold: each part where edenprobe keeps it, the call at its runner's end, the new suite's ui.c row
    check(not failed('scaffold') and 'Left to write' in text('scaffold'), text('scaffold'))
    h, c = written['source/serviceref.h'], written['source/serviceref.c']
    check(h.index('    SERVICE_HID_HOLD_TYPE, ///< hid-style: ') < h.index('    SERVICE_CASES'), 'the enum entry')
    check(h.index('bool ServiceJudgeHidHoldType(') < h.index('#ifdef __cplusplus'), 'the judge declared')
    check(
        '    [SERVICE_HID_HOLD_TYPE] = { "hid-style",' in c and c.index('[SERVICE_HID_HOLD_TYPE]') < c.index('\n};'),
        f'the CASES row:\n{c}',
    )
    check('bool ServiceJudgeHidHoldType(uint32_t rc' in c and 'TODO(ScarletNX#345)' in c, 'the judge')
    t = written['source/tests/t_service.c']
    check(t.index('static void RunHidHoldType(void)') < t.index('void RunService(void)'), 'the device side')
    check(t.rstrip().endswith('RunCsrng();\n    RunHidHoldType();\n}'), f'called at RunService end:\n{t}')
    check('Report(SERVICE_HID_HOLD_TYPE, CHECK_EXACT, passed, detail);' in t, 'the tier')
    r = written['source/tests/t_serviceref.c']
    check(r.rstrip().endswith('CheckRandom();\n    CheckHidHoldType();\n}'), f'the reference check:\n{r}')
    check(
        written['source/ui.c'].startswith(
            '    { "csrng", UI_FAMILY_SERVICE },\n    { "hid-style", UI_FAMILY_SERVICE },'
        ),
        'the new suite row after the service family',
    )
    check(failed('again') and 'already' in text('again'), 'a case written twice')
    check(not failed('crlf'), f'a clone with CRLF lines: {text("crlf")}')
    check(
        all(
            t.replace('\r\n', '\n') == written[rel] and '\n' not in t.replace('\r\n', '')
            for rel, t in crlf_written.items()
        ),
        'the CRLF clone gets the same parts, every line still ending in CRLF',
    )
    check(
        failed('broken') and 'nothing was written' in text('broken') and untouched, 'an unknown layout, no file changed'
    )
    check(failed('bad_id') and failed('cpu'), 'a bad id, a family the scaffold does not write')
    check(failed('no_host') and 'host.mk' in text('no_host'), 'edenprobe_check outside a checkout')
    check(failed('no_nro') and 'cannot be read' in text('no_nro'), 'edenprobe_run without its NRO')

    # the finding: ScarletNX's line and the reference's, the subject's length said, warnings when they don't differ
    finding = text('finding')
    check(
        '| ScarletNX (`edenprobe 0.5.0 (abc1234) starting`) | fail | `GetNpadJoyHoldType: rc 0xca8c0500` |' in finding,
        finding,
    )
    check('| reference (' in finding and 'scarletnx/ScarletNX#345' in finding and 'WARNING' not in finding, finding)
    check('this one is' in finding, 'a subject over 72 characters is said so')
    check('WARNING: the case passes on ScarletNX' in text('passing'), 'a case that shows nothing')
    print('mcp_test: ok')


if __name__ == '__main__':
    main()
