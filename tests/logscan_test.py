#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The log summary (src/common/utilities/logscan.c, as snx-logscan or scarletnx --scan-log, the program given) on
made-up logs in the runtime's formats: the header lines, each kind of failure and the
function it is pinned on (past the SDK's abort for svcBreak), the place a crash is put (the game, the runtime, a
library), the older nightlies' svcBreak line and header, service commands counted, signatures that agree across runs,
paths shortened and the dumps left out, invalid UTF-8, long lines, the markdown's escaping. Offline.

    tests/logscan_test.py PROGRAM [ARGS...]   (build/snx-logscan, or build/scarletnx --scan-log)"""

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PROGRAM = sys.argv[1:]
SCHEMA = 1
LINE = 300


def run(args, text=None):
    out = subprocess.run(PROGRAM + args, input=text, capture_output=True, check=True)
    # Windows' stdout is in text mode: its \r\n as \n, for the checks that span lines
    return out.stdout.decode('utf-8').replace('\r\n', '\n')


def scan(text):
    return json.loads(run(['-'], text.encode('utf-8') if isinstance(text, str) else text))


HEADER = """[   0.00] ScarletNX - Some Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   0.00] system: Some Linux 1.0 (Linux 6.1.0), Some CPU 9000, 12 threads, 32 GB RAM
[   0.05] module main     at 0008005000 (71.8 MB)
[   0.27] vulkan: Some GPU 3000, Vulkan 1.4.351, driver 99d38200, graphics queue family 0, compute 2 (separate family)
[   0.30] game: 0100aaaabbbb0000, version 1.2.3
[   1.00]   [undeclared] nn::audio::detail::IAudioDevice cmd 7 -> ok
[   1.00]   [undeclared] nn::audio::detail::IAudioDevice cmd 7 -> ok
[   1.01]   [unknown] nn::hid::IHidServer cmd 92 -> UnknownCommandId
[   1.02] ipc: nn::foo::IBar: control request 4, UnknownCommandId
[   1.03] vulkan: unsupported texture format 3f, header word 0 0000003f (not drawn)
[   1.04] [game] loading /home/someone/save.bin
"""

SVC_BREAK = (
    HEADER
    + """[   2.25]
*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***
[   2.25]   info+00: 00000000ca8c0500
[   2.25]   x0 = 0000000000000000  x1 = 0000001300100df8  x2 = 0000000000000004  x3 = 0000000000000000
[   2.25]   pc=164ab3c4 lr=163d4828 sp=1300100c30 fp=1300100c30 tls=0
[   2.25]   pc <_ZNSt3__1somethingunrelated+364>  lr <_ZN2nn4diag6detail5AbortEPKNS_6ResultE+18>
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 163d1a68 <_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE+28>
[   2.25]    #2 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.25]    #3 9a1e2c4
[   2.25]    #4 927fd38 <nnMain+1938>
[   2.25]   code 163d4768: f90007ff 94171a7d a9417bfd 910083ff
[   2.25]   mem x1 1300100df8: 58cca 1300100e10 9a1e2c4 1300100f60
[   2.25]     vtable: [1] <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.26] threads (1):
[   2.26]   thread 1 'main' prio 44 (base 44) core 0: running
[   2.26]    #1 deadbeef <_ZN5other6threadEv+4>
[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.
"""
)

# a nightly before the build: and game: lines, with the folder in its header and svcBreak's older line
OLD_SVC_BREAK = """[   0.00] ScarletNX - Some Game (/home/someone/games/Some Game/Some Game)
[   2.25]
*** The game called svcBreak (reason=0, info=1300100df8 size=4) ***
[   2.25]   data: ca8c0500
[   2.25]   pc <_ZNSt3__1somethingunrelated+364>  lr <_ZN2nn4diag6detail5AbortEPKNS_6ResultE+18>  x16-> <x+0>
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+48>
[   2.32] FATAL: The game stopped (svcBreak). See the .log for details.
"""

CRASH_IN_GAME = (
    HEADER
    + """[   3.00]
*** Crash (signal 11) at host pc=0x7f0000001234, access to 0x7e0000000010 (game address 10) ***
[   3.00]   invalid memory access
[   3.00]   inside the recompiled code of game function 8123450 (+40 on the host)
[   3.00]   a null pointer plus an offset: a game short of memory does this, memory_gb=6 in its .ini may help
[   3.00]   thread 7 (Render)
[   3.00]   x0 = 0000000000000000  x1 = 0000000000000010  x2 = 0000000000000004  x3 = 0000000000000000
[   3.00]   pc <_ZN4Game6Render4DrawEv+24>  lr <_ZN4Game6Render5FrameEv+80>
[   3.00]    #0 8123500 <_ZN4Game6Render5FrameEv+80>
[   3.00]   code 8123460: f90007ff 94171a7d a9417bfd 910083ff
[   3.00]   memory: 1.2 GB of 4.0 GB used, by /home/someone/x
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""
)

CRASH_IN_RUNTIME = (
    HEADER
    + """[   3.00]
*** Crash (signal 11) at host pc=0x55000001234, access to 0x10 ***
[   3.00]   invalid memory access
[   3.00]   in the runtime itself (exe offset +1a2b3c)
[   3.00]   on one of the runtime's threads, not the game's
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""
)

CRASH_IN_LIBRARY = (
    HEADER
    + """[   3.00]
*** Crash (signal 11) at host pc=0x7f00000099, access to 0x10 ***
[   3.00]   invalid memory access
[   3.00]   in libnvidia-glcore.so.580 +abc123 (?), on one of the runtime's threads
[   3.00]   host frames: libnvidia-glcore.so.580+abc123 scarletnx+1234
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""
)

UNDEFINED = (
    HEADER
    + """[   4.00]
*** The game ran instruction ce62c820 at 8040598, which the recompiler cannot run ***
[   4.00]   pc <main+40598>  lr <main+40568>
[   4.00]    #0 8040568 <main+40568>
[   4.01] FATAL: The game ran an instruction the recompiler cannot run (ce62c820 at 8040598). See the .log for details.
"""
)

# the launcher's Share Log file: its build, its message, the run's output (the game's warnings again, on stderr), then
# the newest game log, one without a build: line
LAUNCHER = (
    'ScarletNX main 9999fff (clang 23.1.3, Windows x86_64)\n'
    'Message: The game stopped (svcBreak). See C:\\Users\\someone\\AppData\\x.log\n'
    '\n== the last run: C:\\Users\\someone\\run.log ==\n'
    '  [undeclared] nn::audio::detail::IAudioDevice cmd 7 -> ok\n'
    '*** Crash (signal 11) at host pc=0x1 ***\n'
    '\n== C:\\Users\\someone\\state\\Some Game.log ==\n' + OLD_SVC_BREAK
)

# a Share Log whose newest game log is another game's, played before: the last run stands for itself
LAUNCHER_OTHER = (
    'ScarletNX main 9999fff (clang 23.1.3, Windows x86_64)\n'
    'Message: \n'
    '\n== the last run: C:\\Users\\someone\\run.log ==\n'
    + re.sub(r'(?m)^\[ *[0-9.]+\] ?', '', SVC_BREAK).replace('0100aaaabbbb0000', '0100cccc00000000')
    + '\n== C:\\Users\\someone\\state\\Other Game.log ==\n'
    + HEADER.replace('Some Game', 'Other Game')
)

# the other frontends' header on one line: the 3DS's with its title ID (3ds/aot/run.c), the Vita's without
THREEDS = (
    '$ /tmp/.mount_x/bin/scarletnx-3ds --game=/home/someone/Games/0004000000179400\n'
    "The title's folder is /home/someone/Games/0004000000179400\n"
    'Game: Some 3DS Game (0004000000179400) - ScarletNX main d22bbab (clang 23.1.4, Linux x86_64)\n'
    '3ds gpu: the driver cannot import the VRAM region\n'
    'No Vulkan device: no frames will be drawn, shown or saved\n'
)
# and how it stopped: its service calls the runtime has no model of (3ds/ipc/ipc.c, services/system.c), and the stop
# (3ds/aot/run.c), whose counts after the pc differ from run to run
THREEDS_STOP = THREEDS + (
    '3ds ipc: stub answered apt command 0x000e (header 0x000e0080)\n'
    '3ds ipc: stub answered apt command 0x000e (header 0x000e0080)\n'
    '3ds ipc: ndm:u command 0x0014 (header 0x00140040) answered success\n'
    '3ds ipc: gsp command 0x000c has a form the runtime does not take: 000c0000\n'
    'scarletnx-3ds: unsupported guest operation or missing static entry: pc=0x0013be3c exit=4 syndrome=0x00000032 '
    'blocks=351846 svcs=192 last_svc=0x32 ipc_service=7 ipc_header=0x000c0000 wait_service=\n'
    '\n[ended, status 3]\n'
)
VITA = 'Game: Some Vita Game - ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)\n'
# the GBA's: its title and console on the Switch's header line, its build, and its game code (agb/runtime/main.c)
GBA = (
    'ScarletNX - SOME GBA GAME (GBA)\n'
    'build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)\n'
    'cartridge: bpee\n'
)

# the renderer's frame lines (gpu/vk/renderer.c): each frame over 50 ms, and every 600 presents the counts with the
# frames over 50 ms since the last such line (two counted there, then one more after it)
FRAMES = HEADER + (
    '[   5.00] vulkan: a 180 ms frame: 40 shaders translated (120 ms), 30 pipelines made (50 ms waited for), 2 render '
    'targets made, 10 texture uploads (512 KB), 0 ms in finishes\n'
    '[   6.00] vulkan: a 70 ms frame: 1 shaders translated (3 ms), 2 pipelines made (6 ms waited for), 0 render targets '
    'made, 300 texture uploads (90000 KB), 4 ms in finishes\n'
    '[  10.00] vulkan: 900 shaders translated (4000 ms), 700 pipelines made (100 from the cache, 9000 ms making, 800 ms '
    'waited for), 40 render targets made, 5000 texture uploads (900 MB); 2 frames over 50 ms in the last 600, the '
    'slowest 180 ms\n'
    '[  10.00] vulkan: memory used and budgeted: heap 0 3000 of 7000 MB, heap 1 100 of 200 MB\n'
    + ''.join(
        f'[  {11 + i}.00] vulkan: a {51 + i} ms frame: 0 shaders translated (0 ms), 0 pipelines made (0 ms waited '
        'for), 0 render targets made, 0 texture uploads (0 KB), 0 ms in finishes\n'
        for i in range(6)
    )
)

FATAL_ONLY = (
    HEADER
    + """[   0.50] FATAL: Could not load the game executables. See the .log for details.
"""
)

# a Share Log of a game log alone, without the last run's output before it
LAUNCHER_GAME_ONLY = (
    'ScarletNX main 9999fff (clang 23.1.3, Windows x86_64)\n'
    'Message: The game reached code at 0 that the recompiler missed\n'
    '\n== D:\\games\\0100aaaabbbb0000\\0100aaaabbbb0000.log ==\n' + HEADER + FATAL_ONLY
)


# what the runtime missed, in its own formats: shaders (vk/pipeline.c, compute.c), the GPU's methods and operations
# (gpu.c, maxwell/), the macros (maxwell/mme.c, mme_jit.c), SVCs (svc.c, svc_mem.c) and the JIT (guest_jit.c), with
# the time stamp a comma locale writes
MISSED = HEADER.replace('[   0.', '[   0,') + (
    '[   0,40] jit: guest code the recompiler missed is compiled when a thread reaches it, on 4 threads. 32 pcs of '
    'earlier runs queued\n'
    '[   2,00] vulkan: shader 0123456789abcdef (stage 4) not translated: TEXS encoding 3, fp32 1\n'
    '[   2,10] vulkan: shader 1111456789abcdef (stage 4) not translated: TEXS encoding 3, fp32 1\n'
    '[   2,11] vulkan: vertex 3333456789abcdef not translated: a jump table in a constant buffer\n'
    '[   2,12] vulkan: paired vertex 4444456789abcdef not translated: a jump table in a constant buffer\n'
    '[   2,13] vulkan: fragment 5555456789abcdef not translated: TEXS encoding 3, fp32 1\n'
    '[   2,14] vulkan: unsupported render target format c4, drawn as RGBA8\n'
    '[   2,15] vulkan: unsupported depth format 11, using D32F\n'
    "[   2,16] vulkan: a 4-sample texture of format 52 can't be loaded from memory (not drawn)\n"
    '[   2,20] vulkan: compute 2222456789abcdef not translated: global memory base R4 not tracked at 1a0: no write '
    'before\n'
    '[   2,30] gpu: method 100 for class b1c0 on subchannel 2: no engine here runs that class\n'
    '[   2,31] gpu: method 104 for class b1c0 on subchannel 2: no engine here runs that class\n'
    '[   2,40] gpu: semaphore operation 3a400040 is unknown\n'
    "[   2,50] gpu: a 2D blit from format c0 to ca isn't done\n"
    '[   2,60] gpu: macro operation 7 is unknown\n'
    '[   2,70] mme: macro 5 at word 1a0, 30 words uploaded together, hash bcbf04909857a72f: interpreted\n'
    '[   2,71] mme: macro 6 at word 1c0, 12 words uploaded together, hash 00000000deadbeef: draw\n'
    '[   2,80] mme: macro 5 (hash bcbf04909857a72f) is interpreted: its code is not one the compiler can follow\n'
    '[   3,00] svc 7f (CallSecureMonitor): not implemented (x0=0 x1=0 x2=0), NotImplemented\n'
    '[   3,10] svc 7f (CallSecureMonitor): not implemented (x0=1 x1=0 x2=0), NotImplemented\n'
    '[   3,20] svcGetInfo 20 (sub-id 0): unknown, InvalidEnumValue\n'
    '[   4,00] mme: 1000000 macro calls, 900000 native, 0 compiled and 100000 interpreted\n'
    '[   4,00] mme: the macros interpreted most, by hash: bcbf04909857a72f 100000\n'
    '[   9,00] jit: 2 batches compiled 5 pcs (1 failed, 0 still queued, 0 batches dropped by memory changes), 3 '
    'functions, 80 instructions, 20 KiB of code, 0,10 s compiling, 32 units loaded from /home/someone/x.jitcache\n'
)


def check(cond, what):
    if not cond:
        sys.exit(f'logscan_test: {what}')


def main():
    r = scan(SVC_BREAK)
    check(r['schema'] == SCHEMA, 'schema')
    check(
        r['title'] == 'Some Game' and r['title_id'] == '0100AAAABBBB0000' and r['game_version'] == '1.2.3',
        f'header: {r["title"]!r} {r["title_id"]!r} {r["game_version"]!r}',
    )
    check(r['commit'] == '1234abc' and r['branch'] == 'main', 'build line')
    check(r['system'] == 'Some Linux 1.0 (Linux 6.1.0), Some CPU 9000, 12 threads, 32 GB RAM', 'system line')
    check(r['gpu'] == 'Some GPU 3000' and r['driver'] == '99d38200', 'gpu line')
    check(r['outcome'] == 'svcBreak' and r['failure']['reason'] == '0', 'svcBreak kind')
    check(
        r['failure']['function']['name'] == '_ZN2nn3hid18GetNpadJoyHoldTypeEv',
        f'svcBreak is pinned past the abort and not on the pc: {r["failure"]["function"]}',
    )
    check(len(r['failure']['frames']) == 5, f"only the failing thread's backtrace: {len(r['failure']['frames'])}")
    check(r['signature'] == 'svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv', r['signature'])
    check(r['failure']['fatal'].startswith('The game stopped'), 'the fatal message')
    ipc = {(c['kind'], c['interface'], c['command']): c['count'] for c in r['ipc']}
    check(
        ipc
        == {
            ('undeclared', 'nn::audio::detail::IAudioDevice', 7): 2,
            ('unknown', 'nn::hid::IHidServer', 92): 1,
            ('unknown control', 'nn::foo::IBar', 4): 1,
        },
        f'ipc: {ipc}',
    )
    check(
        [(g['area'], g['kind'], g['what']) for g in r['missed']] == [('gpu', 'texture format', '3f')],
        f"a texture format the runtime doesn't map is missed: {r['missed']}",
    )
    check(not any('texture format' in w['message'] for w in r['warnings']), 'and is not a warning')
    check(r['game_output'] == ['loading ~/save.bin'], f'game output, its path shortened: {r["game_output"]}')

    old = scan(OLD_SVC_BREAK)
    check(old['title'] == 'Some Game', f"the older header's folder is dropped: {old['title']!r}")
    check(
        old['build'] is None and old['system'] is None and old['title_id'] is None,
        'an older log has no build, system or title id',
    )
    check(old['signature'] == r['signature'], f'the same failure, the same signature: {old["signature"]}')

    c = scan(CRASH_IN_GAME)
    check(
        c['outcome'] == 'crash' and c['failure']['signal'] == '11' and c['failure']['game_address'] == '10',
        'crash fields',
    )
    check(c['failure']['place'] == {'kind': 'game', 'detail': ['8123450']}, f'crash place: {c["failure"]["place"]}')
    check(c['failure']['function']['name'] == '_ZN4Game6Render4DrawEv', 'a crash is pinned on its pc')
    check(c['signature'] == 'crash (invalid memory access) in _ZN4Game6Render4DrawEv', c['signature'])
    check(any('memory_gb=6' in d for d in c['failure']['details']), 'the crash report hint is kept')

    rt = scan(CRASH_IN_RUNTIME)
    check(rt['signature'] == 'crash (invalid memory access) in the runtime 1234abc +1a2b3c', rt['signature'])
    lib = scan(CRASH_IN_LIBRARY)
    check(lib['signature'] == 'crash (invalid memory access) in libnvidia-glcore.so.580 ?', lib['signature'])

    u = scan(UNDEFINED)
    check(u['outcome'] == 'unsupported instruction' and u['failure']['set'] == 'A64', 'A64 undefined instruction')
    check(u['signature'] == 'unsupported A64 instruction ce62c820', u['signature'])

    f = scan(FATAL_ONLY)
    check(
        f['outcome'] == 'fatal'
        and f['signature'] == 'fatal Could not load the game executables. See the .log for details.',
        f'fatal: {f["signature"]}',
    )

    fr = scan(FRAMES)['frames']
    check(fr['slow'] == 8 and fr['slowest_ms'] == 180, f'slow frames: the stats line counted 2, 6 after it: {fr}')
    check(
        [f['ms'] for f in fr['slowest']] == [180, 70, 56, 55, 54], f'the slowest kept, slowest first: {fr["slowest"]}'
    )
    check(
        fr['slowest'][0]['line'].startswith('a 180 ms frame: 40 shaders translated (120 ms), 30 pipelines'),
        'what the slowest frame did',
    )
    check(
        'frames over 50 ms in the last 600' in fr['stats'] and fr['memory'].startswith('memory used and budgeted:'),
        'the last stats and memory lines',
    )
    check(scan(SVC_BREAK)['frames'] is None, 'no frame lines, no frames')
    md = run(['-', '--format', 'markdown'], FRAMES.encode())
    check('**Frames over 50 ms:** 8, the slowest 180 ms' in md, 'markdown frames')

    m = scan(MISSED)
    check(m['title'] == 'Some Game' and m['seconds'] == 9.0, f"a comma locale's time stamps: {m['title']!r}")
    missed = {(g['area'], g['kind'], g['what']): g['count'] for g in m['missed']}
    check(
        missed
        == {
            ('gpu', 'shader', 'TEXS encoding 3, fp32 1'): 3,
            ('gpu', 'shader', 'a jump table in a constant buffer'): 2,
            ('gpu', 'compute shader', 'global memory base R4 not tracked at 1a0: no write before'): 1,
            ('gpu', 'class', 'b1c0'): 2,
            ('gpu', 'texture format', '3f'): 1,
            ('gpu', 'target format', 'c4'): 1,
            ('gpu', 'depth format', '11'): 1,
            ('gpu', 'gpu', "a 4-sample texture of format 52 can't be loaded from memory"): 1,
            ('gpu', 'gpu', 'semaphore operation 3a400040 is unknown'): 1,
            ('gpu', 'gpu', "a 2D blit from format c0 to ca isn't done"): 1,
            ('mme', 'macro', 'bcbf04909857a72f'): 1,
            ('mme', 'macro compile', 'bcbf04909857a72f: its code is not one the compiler can follow'): 1,
            ('mme', 'mme', 'macro operation 7 is unknown'): 1,
            ('cpu', 'svc', 'svc 7f (CallSecureMonitor)'): 2,
            ('cpu', 'cpu', 'svcGetInfo 20 (sub-id 0)'): 1,
        },
        f'missed: {missed}',
    )
    check(
        m['mme'] == {
            'calls': '1000000 macro calls, 900000 native, 0 compiled and 100000 interpreted',
            'interpreted_most': 'bcbf04909857a72f 100000',
        },
        f'mme: {m["mme"]}',
    )
    check(m['jit'] == {'earlier': 32, 'compiled': 5, 'failed': 1, 'queued': 0}, f'jit: {m["jit"]}')
    check(m['warnings'] == scan(HEADER)['warnings'], f'what was missed is not a warning again: {m["warnings"]}')
    md = run(['-', '--format', 'markdown'], MISSED.encode())
    for want in (
        '**What else the runtime missed:**',
        '- **GPU:**\n  - shader not translated: `TEXS encoding 3, fp32 1` (3 shaders)',
        '  - methods of class `b1c0`, which no engine runs (2x)',
        "  - texture format `3f`, which the runtime doesn't map (not drawn)",
        "  - render target format `c4`, which the runtime doesn't map (drawn as RGBA8)",
        "  - depth format `11`, which the runtime doesn't map (drawn as D32F)",
        '  - macro with no native form: `bcbf04909857a72f`',
        '  - interpreted most, by hash: `bcbf04909857a72f 100000`',
        '  - `svc 7f (CallSecureMonitor)` not implemented (2x)',
        '  - guest code the recompiler missed, compiled at run time: 5 pcs (1 failed, 0 still queued)',
    ):
        check(want in md, f'markdown missed: {want!r} not in\n{md}')
    check('someone' not in md, 'the JIT cache path is not quoted')
    # a table of what was missed full of shaders' reasons still takes a texture format that comes after them
    crowded = scan(
        HEADER
        + ''.join(f'[ 2.00] vulkan: vertex {i:016x} not translated: unknown opcode {i:016x}\n' for i in range(80))
        + '[ 3.00] vulkan: unsupported texture format 72, header word 0 00000072 (not drawn)\n'
    )
    kinds = [g['kind'] for g in crowded['missed']]
    check(
        len(kinds) == 64 and kinds.count('texture format') == 2 and kinds.count('shader') == 62,
        f'a full table of shaders keeps room for a texture format: {kinds.count("texture format")}',
    )
    native = scan(HEADER + '[ 4.00] mme: 1000000 macro calls, 1000000 native, 0 compiled and 0 interpreted\n')
    check(
        native['mme'] is None and not [g for g in native['missed'] if g['area'] == 'mme'],
        f'every macro native: {native["mme"]}',
    )

    la = scan(LAUNCHER)
    check(
        la['build'] == 'ScarletNX main 9999fff (clang 23.1.3, Windows x86_64)' and la['commit'] == '9999fff',
        f"the launcher's build: {la['build']}",
    )
    check(la['signature'] == r['signature'] and la['ipc'] == [], "only the game log's section is scanned")
    t3 = scan(THREEDS)
    check(
        (t3['title'], t3['title_id'], t3['console'], t3['commit'], t3['branch'])
        == ('Some 3DS Game', '0004000000179400', '3DS', 'd22bbab', 'main'),
        f"the 3DS's header: {t3['title']} {t3['title_id']} {t3['console']} {t3['commit']}",
    )
    check('- **Console:** 3DS' in run(['-', '--format', 'markdown'], THREEDS.encode()), "the 3DS's console in markdown")
    gb = scan(GBA)
    check(
        (gb['title'], gb['title_id'], gb['console'], gb['commit']) == ('SOME GBA GAME', 'BPEE', 'GBA', '1234abc'),
        f"the GBA's header: {gb['title']} {gb['title_id']} {gb['console']} {gb['commit']}",
    )
    check(scan(SVC_BREAK)['console'] == 'Switch' and scan(LAUNCHER)['console'] == 'Switch', "the Switch's console")
    st = scan(THREEDS_STOP)
    check(
        st['outcome'] == 'fatal'
        and st['failure']['fatal'] == 'unsupported guest operation or missing static entry: pc=0x0013be3c'
        and st['signature'] == 'fatal unsupported guest operation or missing static entry: pc=N',
        f"the 3DS's stop: {st['outcome']} {st['signature']}",
    )
    calls = {(i['kind'], i['interface'], i['command']): i['count'] for i in st['ipc']}
    check(
        calls == {('undeclared', 'apt', 0xE): 2, ('undeclared', 'ndm:u', 0x14): 1, ('bad request', 'gsp', 0xC): 1},
        f"the 3DS's service calls: {calls}",
    )
    vstop = scan(VITA + 'scarletnx-vita: cannot resolve the game\'s folder for X\n')
    check(vstop['outcome'] == 'fatal' and vstop['failure']['fatal'] == "cannot resolve the game's folder for X", 'the Vita')
    check(scan(HEADER + 'scarletnx: Game failed (exit status 1).\n')['outcome'] == 'no failure logged', "not a frontend's")
    vi = scan(VITA)
    check(
        (vi['title'], vi['title_id'], vi['console'], vi['commit']) == ('Some Vita Game', None, 'Vita', '1234abc'),
        f"the Vita's header: {vi['title']} {vi['title_id']} {vi['commit']}",
    )
    go = scan(LAUNCHER_GAME_ONLY)
    check(
        go['title'] == 'Some Game' and go['outcome'] == scan(HEADER + FATAL_ONLY)['outcome'] and go['tail'],
        f"a game log alone in a Share Log: {go['title']} {go['outcome']} {len(go['tail'])} last lines",
    )
    lo = scan(LAUNCHER_OTHER)
    check(
        lo['title'] == 'Some Game' and lo['title_id'] == '0100CCCC00000000' and lo['outcome'] == 'svcBreak',
        f"another game's log does not stand for the run: {lo['title']} {lo['title_id']} {lo['outcome']}",
    )
    check(lo['signature'] == r['signature'], f"the run's own failure: {lo['signature']}")
    # a game log long enough that the pool grows while it is read, the run's scan kept from before
    long_other = LAUNCHER_OTHER + ''.join(
        f'[ 3.00] vulkan: vertex {i:016x} not translated: reason {i} ' + 'x' * 200 + '\n' for i in range(60)
    )
    lo = scan(long_other)
    check(lo['title_id'] == '0100CCCC00000000', f"the run's title ID after the pool grew: {lo['title_id']}")
    check(
        la['launcher']['message'] == 'The game stopped (svcBreak). See ~\\AppData\\x.log',
        f"the launcher's message, shortened: {la['launcher']['message']}",
    )
    check(
        all(len(t) <= LINE + 3 for t in scan(HEADER + '[game] ' + 'x' * 5000)['game_output']),
        'long lines are cut',
    )

    # nothing of the player's folders, the game's code or its memory in what is written
    for text in (SVC_BREAK, OLD_SVC_BREAK, CRASH_IN_GAME, LAUNCHER):
        out = json.dumps(scan(text))
        for leak in ('someone', 'f90007ff', '58cca', '00000000ca8c0500', 'ca8c0500'):
            check(leak not in out, f'{leak} copied into the summary')

    # invalid UTF-8 as U+FFFD: the JSON stays readable
    bad = scan(HEADER.encode() + b'[game] caf\xe9 \xff\n')
    check(bad['game_output'][-1] == 'caf\ufffd \ufffd', f'invalid UTF-8: {bad["game_output"]}')

    # the markdown quotes the log as text: no mention, link, image or fence of its own
    # GitHub links an issue reference (#N, GH-N, owner/repo#N), a URL and www. even escaped, but nothing in a code span:
    # every value the log gave is one, its backticks made quotes
    title = 'Hi @team [x](https://e.example) ![i](j) www.example.com #1 GH-2 scarletnx/ScarletNX#3 `tick`'
    sneaky = HEADER.replace('Some Game', title) + '[game] ```\n' + FATAL_ONLY
    md = run(['-', '--format', 'markdown'], sneaky.encode())
    heading = md.splitlines()[0]
    check(heading.startswith('### `' + title.replace('`', "'") + '` ('), f'the title is a code span:\n{heading}')
    for line in md.splitlines():
        if line.startswith('- **') and not line.endswith(':**'):  # an area of what the runtime missed has no value
            value = line.split(':** ', 1)[1]
            check(value.startswith('`') or value.split(' ')[0].isalpha(), f'a value outside a code span: {line}')
    check('\n```\n```' not in md and "'" in md, 'a fence in a line is broken')

    # no signature where the place says nothing: it would group reports that only share their kind
    nowhere = scan(HEADER + '*** Crash (signal 11) at host pc=0x1 ***\n[ 1.0]   invalid memory access\n')
    check(nowhere['outcome'] == 'crash' and nowhere['signature'] is None, f'unknown place: {nowhere["signature"]}')
    aborted = scan(
        HEADER + '*** The game called svcBreak (reason 0, info 1, 4 bytes) ***\n'
        '   #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>\n'
    )
    check(aborted['signature'] is None, f"only the SDK's abort: {aborted['signature']}")
    headless = scan(
        '*** The game ran instruction ce62c820 at 8040598, which the recompiler cannot run ***\n'
        '  pc <main+40598>  lr <main+40568>\n'
    )
    check(headless['signature'] == 'unsupported A64 instruction ce62c820', 'an instruction needs no place')
    unnamed = scan('*** Crash (signal 11) at host pc=0x1 ***\n  invalid memory access\n  pc <main+40>  lr <main+44>\n')
    check(unnamed['signature'] is None, f"a module's offset in no named game: {unnamed['signature']}")

    # the account's folder on the drives a desktop mounts, on Silverblue, and on Windows in any case
    homes = scan(
        HEADER
        + '[game] a /run/media/someone/Games/x b /media/someone/y c /var/home/someone/z d C:\\users\\someone\\w\n'
    )
    check(homes['game_output'][-1] == 'a ~/Games/x b ~/y c ~/z d ~\\w', f'home folders: {homes["game_output"][-1]}')

    # an account's name whole on each system, spaces, accents and brackets in it: up to the separator after it, and
    # with no separator after it, to the line's end or a byte no account name holds
    for given, shown in [
        ('/home/John Smith/games/x.nsp', '~/games/x.nsp'),
        ('/usr/home/John Smith/x', '~/x'),
        ('/Users/Ana Maria/Library/x', '~/Library/x'),
        ('/Volumes/Data/Users/Ana Maria/x', '/Volumes/Data~/x'),
        ('C:\\Users\\John Smith\\AppData\\x', '~\\AppData\\x'),
        ('C:/Users/John Smith/AppData/x', '~/AppData/x'),
        ('d:\\USERS/José (Trabalho) Ñandú\\x', '~\\x'),
        ('\\\\pc\\C$\\Users\\John Smith\\x', '\\\\pc\\C$~\\x'),
        ('/mnt/c/Users/John Smith/x', '~/x'),
        ('/data/user/10/org.scarletnx/files/x', '~/files/x'),
        ('saved in C:\\Users\\John Smith', 'saved in ~'),
        ('"C:\\Users\\John Smith", then /home/Jane Doe: done', '"~", then ~: done'),
        ('C:\\Users\\John Smith D:\\x and /home/Jane Doe /tmp/y', '~ D:\\x and ~ /tmp/y'),
        # names nobody: the shared storage (where it is tells it from the app's own folder), the data folder's users/
        ('/storage/emulated/0/Games/x', '/storage/emulated/0/Games/x'),
        ('/home/John Smith/.local/share/scarletnx/users/online.ini', '~/.local/share/scarletnx/users/online.ini'),
    ]:
        out = scan(HEADER + '[game] ' + given + '\n')['game_output'][-1]
        check(out == shown, f'home folder {given!r}: {out!r}, not {shown!r}')

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'game.log'
        path.write_text(SVC_BREAK)
        md = run([str(path), '--format', 'markdown'])
        check('**Signature:** `svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv`' in md, 'markdown from a file')
        check('IAudioDevice` cmd 7 (undeclared, 2x)' in md, 'markdown service commands')
        check(len(run([str(path), str(path)]).splitlines()) == 2, 'json: a line a log')
    print('logscan_test: ok')


if __name__ == '__main__':
    main()
