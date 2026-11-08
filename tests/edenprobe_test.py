#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/edenprobe.py against a fake runtime that recompiles into a fake game, whose launches write report lines as
each test asks: a clean run, a regression, a case not run, a new pass, a crash and a hang each followed by another
launch, a baseline line that expects a crash, launches that add nothing, no report, an exit after the summary, words
edenprobe ignored, a baseline of another build or mode or none, broken lines; then the baseline of several runs.
Offline, no GPU."""

import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'edenprobe.py'
MODE = 'quick notime cpu'
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


# The runtime: "-r NRO NAME --outdir DIR --jobs N" copies itself to DIR/NAME/NAME, the game; the game, run with -H,
# plays the next launch of $FAKE_SCENARIO: appends its lines to the report under sdmc:/edenprobe/runs/<build>, then
# exits with its status, kills itself with its signal, or hangs. Each call is logged to $FAKE_SCENARIO.log
FAKE = """
# the build line a runtime carries: ScarletNX main 1a2b3c4, committed 2026-11-08 (clang 22.1.8, Linux x86_64)
import json, os, shutil, signal, sys, time
from pathlib import Path
scenario = Path(os.environ['FAKE_SCENARIO'])
plan = json.loads(scenario.read_text())
with open(f'{scenario}.log', 'a') as log:
    xdg = {k: v for k, v in os.environ.items() if k.startswith('XDG_') and k.endswith('_HOME')}
    log.write(json.dumps({'argv': sys.argv[1:], 'cwd': os.getcwd(), 'xdg': xdg}) + '\\n')
if sys.argv[1] == '-r':
    folder = Path(sys.argv[sys.argv.index('--outdir') + 1]) / sys.argv[3]
    folder.mkdir(parents=True)
    if plan.get('game', True):
        shutil.copy(sys.argv[0], folder / sys.argv[3])
    sys.exit(plan.get('recompile', 0))
edenprobe = Path(os.getcwd()) / f'{Path(sys.argv[0]).name}_data' / 'sdmc' / 'edenprobe'
count = edenprobe / 'launches'
number = int(count.read_text()) if count.exists() else 0
count.write_text(str(number + 1))
(scenario.parent / 'mode.seen').write_text((edenprobe / 'mode').read_text())
if number >= len(plan['launches']):
    sys.exit(3)
step = plan['launches'][number]
if step.get('last_run_only'):
    (edenprobe / 'last-run').write_text('runs/abc1234\\n')
if step.get('lines') is not None:
    run = edenprobe / 'runs' / plan.get('folder', 'abc1234')
    (run / 'images').mkdir(parents=True, exist_ok=True)
    (run / 'images' / 'failure.png').write_bytes(b'png')
    (edenprobe / 'last-run').write_text(f'runs/{plan.get("folder", "abc1234")}\\n')
    with open(run / 'report.jsonl', 'a') as report:
        for line in step['lines']:
            report.write(line + '\\n')
            report.flush()
            time.sleep(step.get('slow', 0))
if step.get('hang'):
    (scenario.parent / 'hung.pid').write_text(str(os.getpid()))
    time.sleep(60)
if step.get('signal'):
    os.kill(os.getpid(), step['signal'])
sys.exit(step.get('exit', 0))
"""


def note(text):
    return json.dumps({'type': 'note', 'text': text})


def case(suite, name, result='pass', **more):
    return json.dumps({'type': 'case', 'suite': suite, 'name': name, 'tier': 'exact', 'result': result, **more})


def summary(passed, failed=0, skipped=0, crashed=0):
    return json.dumps({'type': 'summary', 'passed': passed, 'failed': failed, 'skipped': skipped, 'crashed': crashed})


START = [note('edenprobe 0.4.0 (abc1234) starting'), note('folder: sdmc:/edenprobe/runs/abc1234'), note('mode: quick')]
CASES = [
    case('cpu', 'adds'),
    case('cpu', 'multiplies'),
    case('cpu', 'divides', 'fail', detail='got 3, wanted 4'),
    case('cpu', 'reads the counter', 'skip', tier='none', reason='may crash', detail='needs fatal'),
]
WHOLE = START + CASES + [summary(2, 1, 1)]
BASELINE = '# build: abc1234\n# mode: quick notime cpu\ncpu: adds\n'
MISSING = 'missing'


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary).resolve()  # the script resolves its paths: /tmp is a link on macOS
    runtime = root / 'bin' / 'scarletnx'
    runtime.parent.mkdir()
    runtime.write_text(f'#!{sys.executable}\n{FAKE}')
    runtime.chmod(0o755)
    nro = root / 'edenprobe.nro'
    nro.write_bytes(b'NRO0')
    number = 0

    def run(launches, baseline=BASELINE, mode=MODE, extra=(), **plan):
        """the script's exit status, its output, its folder, and the fake's log; the plan's launches are lists of
        lines, or dicts with lines and exit, signal or hang. baseline None: no --baseline; MISSING: one not there"""
        global number
        number += 1
        test = root / f'test-{number}'
        test.mkdir()
        steps = [step if isinstance(step, dict) else {'lines': step} for step in launches]
        (test / 'scenario.json').write_text(json.dumps({'launches': steps, **plan}))
        command = [sys.executable, str(SCRIPT), 'run', '--runtime', str(runtime), '--nro', str(nro), '--mode', mode]
        command += ['--out', str(test / 'out'), '--summary', str(test / 'summary.md'), '--timeout', '10', *extra]
        if baseline is not None:
            if baseline != MISSING:
                (test / 'baseline.txt').write_text(baseline)
            command += ['--baseline', str(test / 'baseline.txt')]
        env = dict(os.environ, FAKE_SCENARIO=str(test / 'scenario.json'))
        done = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60, check=False)
        log = test / 'scenario.json.log'
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return done.returncode, done.stdout + done.stderr, test / 'out', calls

    status, text, out, calls = run([WHOLE])
    check('a clean run passes', status == 0, text)
    check(
        'its summary names the build and the counts',
        (out / 'summary.md').read_text().startswith('### edenprobe abc1234: 2 passed, 1 failed, 1 skipped, 0 crashed'),
    )
    check('every case of the baseline passing is said', 'All 1 cases of' in text, text)
    check(
        'the summary kept, and appended to --summary',
        (out / 'summary.md').read_text() == (out.parent / 'summary.md').read_text()
        and text.startswith('launch 1: exit 0'),
    )
    check(
        "the run's baseline.txt: build, mode in order, its passing cases",
        (out / 'baseline.txt')
        .read_text()
        .endswith('# build: abc1234\n# mode: cpu notime quick\ncpu: adds\ncpu: multiplies\n'),
        (out / 'baseline.txt').read_text() if (out / 'baseline.txt').exists() else 'none',
    )
    check('the report and its images are kept', (out / 'report.jsonl').is_file() and (out / 'images').is_dir())
    check('the mode file holds the words', (out.parent / 'mode.seen').read_text() == 'quick notime cpu\n')

    recompile, game = (calls + [{'argv': [], 'cwd': '', 'xdg': {}}] * 2)[:2]
    check(
        'the NRO recompiled into the work folder',
        recompile['argv'][:3] == ['-r', str(nro), 'edenprobe'] and str(out / 'work' / 'games') in recompile['argv'],
        recompile['argv'],
    )
    check('the game run headless from its folder', game['argv'] == ['-H'] and game['cwd'].endswith('games/edenprobe'))
    check(
        'its XDG folders in the work folder',
        len(game['xdg']) == 4 and all(v.startswith(str(out / 'work' / 'xdg')) for v in game['xdg'].values()),
        game['xdg'],
    )
    check(
        'a new pass listed, and not failing the run',
        'Passing and not in' in text and '- `cpu: multiplies`' in text and status == 0,
    )

    # edenprobe's ban list: a probe that knows machine= gets the runtime's commit beside the words; words that name a
    # machine keep theirs; the probe above, which predates the word, got none
    old_nro = nro.read_bytes()
    nro.write_bytes(b'NRO0 a mode parser with machine=%s')
    status, text, out, calls = run([WHOLE])
    check(
        "a probe that knows machine= gets machine=scarletnx@<the runtime's commit>",
        (out.parent / 'mode.seen').read_text() == 'quick notime cpu machine=scarletnx@1a2b3c4\n',
        text,
    )
    status, text, out, calls = run([WHOLE], mode='quick notime cpu machine=console', baseline=None)
    check(
        'words that name a machine keep theirs',
        (out.parent / 'mode.seen').read_text() == 'quick notime cpu machine=console\n',
        text,
    )
    nro.write_bytes(old_nro)

    status, text, out, calls = run([WHOLE], baseline=BASELINE + 'cpu: divides\ncpu: reads the counter\ncpu: gone\n')
    check('a case of the baseline that fails is a regression', status == 1, text)
    check('the count of them', "3 of the baseline's 4 cases no longer pass" in text, text)
    check('a failing one with its detail', '- `cpu: divides: fail: got 3, wanted 4`' in text, text)
    check('a skipped one with its reason', '- `cpu: reads the counter: skip (may crash)`' in text, text)
    check('one the report lacks', '- `cpu: gone: not run`' in text, text)

    crash = START + CASES[:2]
    resumed = [case('cpu', 'divides', 'skip', tier='none', reason='crashed', detail='ended inside')] + CASES[3:]
    status, text, out, calls = run(
        [{'lines': crash, 'signal': 11}, resumed + [summary(2, 0, 1, 1)]], baseline=BASELINE + 'cpu: multiplies\n'
    )
    check('a crash, then the launch that goes on, reaching the summary', status == 0, text)
    check('each launch said', 'launch 1: exit -11' in text and 'launch 2: exit 0' in text, text)
    check('both launches logged', (out / 'launch-1.log').is_file() and (out / 'launch-2.log').is_file())
    check('the run one report', 'cpu: multiplies' in (out / 'baseline.txt').read_text())

    # a line that expects another result: divides crashes, as the console ends it too
    expects = BASELINE + 'cpu: multiplies\n[skip (crashed)] cpu: divides\n'
    status, text, out, calls = run([{'lines': crash, 'signal': 11}, resumed + [summary(2, 0, 1, 1)]], baseline=expects)
    check('an expected crash that crashes passes', status == 0, text)
    check(
        'the summary says every case ends as the baseline expects',
        'All 3 cases of' in text and 'end as it expects (1 skip (crashed))' in text,
        text,
    )
    check(
        "this run's baseline.txt keeps the expected crash it had",
        (out / 'baseline.txt').read_text().endswith('cpu: adds\n[skip (crashed)] cpu: divides\ncpu: multiplies\n'),
        (out / 'baseline.txt').read_text(),
    )
    status, text, out, calls = run([WHOLE], baseline=expects)
    check(
        'an expected crash that fails instead is a regression',
        status == 1
        and "1 of the baseline's 3 cases no longer end as it expects" in text
        and '- `cpu: divides: fail: got 3, wanted 4, expected skip (crashed)`' in text,
        text,
    )
    check(
        "one that did not crash is not in this run's baseline.txt", 'divides' not in (out / 'baseline.txt').read_text()
    )
    passing = START + CASES[:2] + [case('cpu', 'divides')] + CASES[3:] + [summary(3, 0, 1)]
    status, text, out, calls = run([passing], baseline=expects)
    check(
        'an expected crash that passes now is a regression, not a new pass',
        status == 1 and '- `cpu: divides: pass, expected skip (crashed)`' in text and 'Passing and not in' not in text,
        text,
    )
    status, text, out, calls = run(
        [{'lines': crash, 'signal': 11}, resumed + [summary(2, 0, 1, 1)]], baseline=expects + 'cpu: gone\n'
    )
    check(
        'a plain line beside it fails as before, alone',
        status == 1
        and "1 of the baseline's 4 cases no longer pass" in text
        and '- `cpu: gone: not run`' in text
        and 'no longer end as it expects' not in text,
        text,
    )

    status, text, out, calls = run(
        [{'lines': START + CASES[:1], 'hang': True}, CASES[1:] + [summary(2, 1, 1)]], extra=['--timeout', '3']
    )
    check('a launch killed after 3 s without a line, then the next', status == 0 and 'killed after 3 s' in text, text)
    hung = int((out.parent / 'hung.pid').read_text())
    try:
        os.kill(hung, 0)
        check('the hung game was killed', False)
    except ProcessLookupError:
        pass
    # Ctrl-C while a launch runs stops its game too, which has a session of its own
    stopped = root / 'stopped'
    stopped.mkdir()
    (stopped / 'scenario.json').write_text(json.dumps({'launches': [{'lines': START, 'hang': True}]}))
    command = [sys.executable, str(SCRIPT), 'run', '--runtime', str(runtime), '--nro', str(nro), '--mode', MODE]
    script = subprocess.Popen(
        command + ['--out', str(stopped / 'out')],
        env=dict(os.environ, FAKE_SCENARIO=str(stopped / 'scenario.json')),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for _ in range(200):
        if (stopped / 'hung.pid').is_file():
            break
        time.sleep(0.05)
    script.send_signal(signal.SIGINT)
    script.wait(timeout=30)
    game = int((stopped / 'hung.pid').read_text())
    try:
        os.kill(game, 0)
        check('Ctrl-C stops the game too', False)
    except ProcessLookupError:
        pass

    status, text, out, calls = run([{'lines': WHOLE, 'slow': 0.4}], extra=['--timeout', '1.5'])
    # past the 1.5 s of --timeout in all; the seconds themselves are the runner's (3 here, 4 on macOS)
    took = re.search(r'launch 1: exit 0, (\d+) s', text)
    check('a slow launch that keeps adding lines is not killed', status == 0 and took and int(took[1]) >= 2, text)

    # --advisory: edenprobe's main beside the pinned release's report, warnings only, never a failure
    known = root / 'release.jsonl'
    known.write_text(
        '\n'.join(
            [
                note('edenprobe 0.4.0 (v0.4.0) starting'),
                case('cpu', 'adds'),
                case('cpu', 'multiplies'),
                case('cpu', 'divides', 'fail'),
                summary(2, 1),
            ]
        )
        + '\n'
    )
    main = START + [
        case('cpu', 'adds'),
        case('cpu', 'multiplies', 'fail', detail='got 5'),
        case('cpu', 'divides', 'fail'),
        case('cpu', 'shifts', 'fail', detail='got 0%\nwanted 1'),
        case('cpu', 'rotates'),
        case('cpu', 'reads the counter', 'skip', reason='may crash'),
        summary(2, 3, 1),
    ]
    status, text, out, calls = run([main], baseline=None, extra=['--advisory', str(known)])
    warned = [line for line in text.splitlines() if line.startswith('::warning title=edenprobe main::')]
    check('a run beside the release never fails', status == 0, text)
    check(
        'a case the release lacks that fails is a warning, on one line',
        '::warning title=edenprobe main::not in v0.4.0: cpu: shifts: fail: got 0%25%0Awanted 1' in warned,
        text,
    )
    check(
        'a case the release passes that fails here is a warning',
        '::warning title=edenprobe main::passing in v0.4.0: cpu: multiplies: fail: got 5' in warned,
        text,
    )
    check('a case the release fails too, one passing or skipped, warns nothing', len(warned) == 2, warned)
    check(
        'the summary counts the cases the release lacks, and says it only warns',
        "(edenprobe's main: warnings only)" in text and '3 cases not in v0.4.0, 2 of them passing or skipped.' in text,
        text,
    )
    status, text, out, calls = run([START + CASES[:1]], baseline=None, extra=['--advisory', str(known)])
    check(
        'beside the release, a run with no summary is a warning too',
        status == 0 and '::warning title=edenprobe main::launch 2 (exit 3) added no case' in text,
        text,
    )
    status, text, out, calls = run([WHOLE], baseline=None, extra=['--advisory', str(root / MISSING)])
    check('no release report to compare with is a warning', status == 0 and '::warning title=' in text, text)
    status, text, out, calls = run([WHOLE], extra=['--advisory', str(known)])
    check('--baseline and --advisory are one or the other', status == 2, text)

    status, text, out, calls = run([{'lines': crash, 'exit': 1}, {'lines': [], 'exit': 1}])
    check('a launch that adds no case ends the run', status == 1 and 'launch 2 (exit 1) added no case' in text, text)
    check('no baseline.txt from a run with no summary', not (out / 'baseline.txt').exists())
    check('the report of a run with no summary is still kept', (out / 'report.jsonl').is_file())
    check('the summary says it had none', '### edenprobe abc1234, no summary' in text, text)

    status, text, out, calls = run(
        [{'lines': START, 'exit': 1}, {'lines': CASES[:1], 'exit': 1}], extra=['--launches', '2']
    )
    check('launches run out before the summary', status == 1 and '2 launches and still no summary' in text, text)

    status, text, out, calls = run([{'lines': None, 'exit': 2}])
    check('a launch that leaves no report', status == 1 and 'launch 1 (exit 2) left no report' in text, text)
    status, text, out, calls = run([{'lines': None, 'last_run_only': True, 'exit': 2}])
    check('one that names its folder, but no report there', status == 1 and 'left no report' in text, text)

    status, text, out, calls = run([{'lines': WHOLE, 'signal': 11}])
    check('a crash after the summary fails', status == 1 and 'ended with exit -11 after its summary' in text, text)

    ignored = [note('mode: quick, from the mode file; 1 unknown word ignored: grpahics')]
    status, text, out, calls = run([START[:2] + ignored + CASES + [summary(2, 1, 1)]])
    check('words edenprobe ignored fail the run', status == 1 and 'ignored words of the mode: 1 unknown' in text, text)

    status, text, out, calls = run([WHOLE], baseline=BASELINE.replace('abc1234', 'fed4321'))
    check('a baseline of another build', status == 1 and 'write it again' in text and 'fed4321' in text, text)
    status, text, out, calls = run([WHOLE], mode='quick cpu')
    check('a baseline of another mode', status == 1 and 'write it again' in text, text)
    status, text, out, calls = run([WHOLE], mode='CPU Quick  notime')
    check('the same words in another order and case', status == 0, text)
    status, text, out, calls = run([WHOLE], baseline='cpu: adds\n')
    check('a baseline without its build and mode', status == 1 and 'names no build or no mode' in text, text)

    status, text, out, calls = run([WHOLE], baseline=None)
    check('without --baseline, a whole run passes', status == 0 and 'no longer pass' not in text, text)
    status, text, out, calls = run([WHOLE], baseline=MISSING)
    check('a baseline named and missing', status == 1 and 'no baseline at' in text, text)

    status, text, out, calls = run([START + ['{"type":"case",'] + CASES + [summary(2, 1, 1)]])
    check('a line that is no JSON', status == 1 and 'no case, note or summary: [4]' in text, text)
    status, text, out, calls = run([WHOLE + [note('edenprobe 0.4.0 (fed4321) starting')]])
    check('a report two builds wrote', status == 1 and 'two builds of edenprobe' in text, text)
    for twice in ([CASES[0], case('cpu', 'adds', 'fail')], [case('cpu', 'adds', 'fail'), CASES[0]]):
        status, text, out, calls = run([START + twice + CASES[1:] + [summary(2, 2, 1)]])
        check('a case listed twice passes only if both pass', status == 1 and '- `cpu: adds: fail`' in text, text)
    status, text, out, calls = run([START[1:] + CASES + [summary(2, 1, 1)]])
    check('a report without its build', status == 1 and 'names no build' in text, text)

    kept = root / 'kept'
    kept.mkdir()
    (kept / 'mine.txt').write_text('mine')
    status, text, out, calls = run([WHOLE], extra=['--out', str(kept)])
    check('a folder of files that is no run is not emptied', status == 1 and (kept / 'mine.txt').exists(), text)
    earlier = root / 'test-1' / 'out'  # the clean run's
    (earlier / 'images' / 'gone').write_text('an earlier picture')
    status, text, out, calls = run([WHOLE], extra=['--out', str(earlier)])
    check(
        'an earlier run emptied for the next',
        status == 0 and not (earlier / 'images' / 'gone').exists() and (earlier / 'report.jsonl').is_file(),
        text,
    )
    status, text, out, calls = run([WHOLE], mode='notime cpu')
    check('a mode without quick refused before recompiling', status == 1 and 'needs "quick"' in text and not calls)
    status, text, out, calls = run([WHOLE], recompile=5)
    check('a failed recompile', status == 1 and 'the recompile exited 5' in text and len(calls) == 1, text)
    status, text, out, calls = run([WHOLE], game=False)
    check('a recompile that makes no game', status == 1 and 'exited 0 without making edenprobe' in text, text)

    # the baseline of several runs: what each passed
    first, second, other = root / 'one.txt', root / 'two.txt', root / 'other.txt'
    first.write_text('# build: abc1234\n# mode: quick cpu\ncpu: adds\ncpu: flaky\ncpu: multiplies\n')
    second.write_text('# build: abc1234\n# mode: cpu quick\ncpu: multiplies\ncpu: adds\n')
    other.write_text('# build: fed4321\n# mode: quick cpu\ncpu: adds\n')
    written = root / 'written.txt'

    def baseline(*files):
        command = [sys.executable, str(SCRIPT), 'baseline', *map(str, files), '-o', str(written)]
        done = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
        return done.returncode, done.stdout + done.stderr

    status, text = baseline(first, second)
    check('the cases every run passed', status == 0, text)
    check(
        'written sorted under the build and mode',
        written.read_text().endswith('# build: abc1234\n# mode: cpu quick\ncpu: adds\ncpu: multiplies\n'),
        written.read_text(),
    )
    check('the one left out said', 'left out: cpu: flaky' in text, text)
    written.unlink()
    status, text = baseline(first, other)
    check('runs of two builds refused', status == 1 and 'one build and one mode only' in text and not written.exists())
    crashed, again, passed = root / 'crashed.txt', root / 'again.txt', root / 'passed.txt'
    crashed.write_text('# build: abc1234\n# mode: quick cpu\ncpu: adds\n[skip (crashed)] cpu: reads\n')
    again.write_text('# build: abc1234\n# mode: quick cpu\n[skip (crashed)] cpu: reads\ncpu: adds\n')
    passed.write_text('# build: abc1234\n# mode: quick cpu\ncpu: adds\ncpu: reads\n')
    status, text = baseline(crashed, again)
    check(
        'a case every run ended the same other way is written with its result',
        status == 0 and written.read_text().endswith('# mode: cpu quick\ncpu: adds\n[skip (crashed)] cpu: reads\n'),
        written.read_text() if written.exists() else text,
    )
    status, text = baseline(crashed, passed)
    check(
        'one that ended two ways is left out, with each',
        status == 0
        and 'left out: cpu: reads (skip (crashed), pass)' in text
        and written.read_text().endswith('# mode: cpu quick\ncpu: adds\n'),
        text,
    )

for failure in failures:
    print('FAIL:', failure)
print('edenprobe:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
