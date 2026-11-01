#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""edenprobe (https://github.com/scarletnx/edenprobe), a homebrew program of hardware probes, recompiled and run
headless, its report held against a baseline: how each case ended before. docs/EDENPROBE.md has the how and why.

    scripts/edenprobe.py run --runtime SCARLETNX --nro edenprobe.nro --mode WORDS --out DIR
                             [--baseline FILE | --advisory REPORT] [--summary FILE] [--timeout SECONDS]
                             [--launches N] [--jobs N]
    scripts/edenprobe.py baseline BASELINE... -o FILE

run recompiles the NRO into DIR/work, writes WORDS to its sdmc:/edenprobe/mode, and runs it with -H and its XDG
folders in DIR/work. A launch that ends before the report's summary, crashed or killed after --timeout seconds without
a new report line, is followed by another, which edenprobe goes on from after the case the last one ended inside
(reported as crashed), until the summary, a launch that adds no case, or --launches. DIR then holds report.jsonl, the
failures' images/, the recompile's and each launch's log, baseline.txt (this run's passing cases, and those the
--baseline expects to end otherwise that did) and summary.md, which --summary FILE also appends to (CI:
$GITHUB_STEP_SUMMARY). With --baseline, a case of it that does not end as its line says (pass, or the RESULT of a
"[RESULT] suite: name" line) is a regression: run exits 1 and names it; a passing case it lacks is listed, to be added.
A run that never reached its summary exits 1 too.

With --advisory REPORT, the pinned release's report.jsonl, the NRO is another build (edenprobe's main) and the run never
fails: its cases REPORT lacks that do not pass, REPORT's passing cases that it does not pass, and what would have failed
the run are warnings (GitHub's ::warning:: lines).

baseline writes the cases that end the same way in every BASELINE given (baseline.txt of runs of one build and one
mode): a case that passes only now and then stays out.
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

NAME = 'edenprobe'
EXE = '.exe' if os.name == 'nt' else ''
# a list longer than this in the summary ends with how many more: the files have them all
MOST = 60


class Failed(Exception):
    """a run or a file that cannot be held against a baseline"""


def words(mode):
    """the mode words as edenprobe reads them: case and order do not matter"""
    return sorted(mode.lower().split())


class Report:
    """report.jsonl: its build, its summary, its cases by "suite: name" (one listed twice passes if every line does)"""

    def __init__(self, path):
        self.build = self.summary = None
        self.cases, self.unknown, self.broken = {}, [], []
        with open(path, encoding='utf-8', errors='replace') as lines:
            for number, line in enumerate(lines, 1):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line)
                    kind = item['type']
                except (ValueError, KeyError, TypeError):
                    self.broken.append(number)
                    continue
                if kind == 'case':
                    self.case(item, number)
                elif kind == 'summary':
                    self.summary = item
                elif kind == 'note':
                    self.note(str(item.get('text', '')))

    def case(self, item, number):
        try:
            key = f'{item["suite"]}: {item["name"]}'
            result = item['result']
        except KeyError:
            self.broken.append(number)
            return
        if '\n' in key or '\r' in key:
            self.broken.append(number)
            return
        if result == 'skip' and item.get('reason'):
            result = f'skip ({item["reason"]})'
        if result == 'fail' and item.get('detail'):
            result = f'fail: {item["detail"]}'
        if self.cases.get(key, 'pass') == 'pass':
            self.cases[key] = result

    def note(self, text):
        # each launch says which build it is: a report two builds wrote is not one run
        started = re.match(r'edenprobe \S+ \(([^)]+)\) starting', text)
        if started:
            if self.build and self.build != started[1]:
                raise Failed(f'the report was written by two builds of edenprobe, {self.build} and {started[1]}')
            self.build = started[1]
        ignored = re.match(r'mode: .*\b(\d+ unknown words? ignored: .*)', text)
        if ignored:
            self.unknown.append(ignored[1])

    def passed(self):
        return sorted(key for key, result in self.cases.items() if result == 'pass')


class Baseline:
    """a baseline file: "# build: B" and "# mode: WORDS", then one case a line: "suite: name" for one that passes,
    "[RESULT] suite: name" for one that ends as RESULT, the report's result as the summary lists it ("skip (crashed)",
    "fail: DETAIL"), with no "]" in it. cases: each case's expected result"""

    def __init__(self, path):
        self.path, self.build, self.mode, self.cases = path, None, None, {}
        for line in Path(path).read_text(encoding='utf-8').splitlines():
            if line.startswith('# build:'):
                self.build = line.split(':', 1)[1].strip()
            elif line.startswith('# mode:'):
                self.mode = words(line.split(':', 1)[1])
            elif line.strip() and not line.startswith('#'):
                expected = re.fullmatch(r'\[([^\]]+)\] (.+)', line)
                if expected:
                    self.cases[expected[2]] = expected[1]
                else:
                    self.cases[line] = 'pass'
        if not self.build or self.mode is None:
            raise Failed(f'{path} names no build or no mode: "# build: ..." and "# mode: ..." come first')

    @staticmethod
    def text(build, mode, cases):
        """cases: each case's expected result"""
        return (
            '# edenprobe cases, one a line: "suite: name" passes, "[RESULT] suite: name" ends as RESULT '
            '(docs/EDENPROBE.md)\n'
            f'# build: {build}\n# mode: {" ".join(words(mode))}\n'
            + ''.join(
                f'{case}\n' if expected == 'pass' else f'[{expected}] {case}\n'
                for case, expected in sorted(cases.items())
            )
        )


def listed(title, lines):
    if not lines:
        return []
    shown = [f'- `{line}`' for line in lines[:MOST]]
    if len(lines) > MOST:
        shown.append(f'- and {len(lines) - MOST} more')
    return ['', f'{title}:', *shown]


def compare(report, baseline, mode):
    """problems (each fails the run) and the summary's lines"""
    problems, lines = [], []
    if baseline.build != report.build or baseline.mode != words(mode):
        problems.append(
            f'{baseline.path} is of edenprobe {baseline.build} in mode "{" ".join(baseline.mode)}", this run of '
            f'{report.build} in mode "{" ".join(words(mode))}": write it again from this run\'s baseline.txt'
        )
        return problems, lines
    regressed = [
        f'{key}: {report.cases.get(key, "not run")}'
        for key, expected in sorted(baseline.cases.items())
        if expected == 'pass' and report.cases.get(key) != 'pass'
    ]
    # a case whose line expects another result (a crash, as on the console) and that ends any other way, passing
    # included
    changed = [
        f'{key}: {report.cases.get(key, "not run")}, expected {expected}'
        for key, expected in sorted(baseline.cases.items())
        if expected != 'pass' and report.cases.get(key) != expected
    ]
    new = [key for key in report.passed() if key not in baseline.cases]
    if regressed:
        problems.append(f"{len(regressed)} of the baseline's {len(baseline.cases)} cases no longer pass")
    if changed:
        problems.append(f"{len(changed)} of the baseline's {len(baseline.cases)} cases no longer end as it expects")
    lines += listed('No longer passing', regressed)
    lines += listed('No longer ending as the baseline expects', changed)
    lines += listed(f"Passing and not in {baseline.path} (add them: this run's baseline.txt has them)", new)
    if not regressed and not changed:
        others = sorted(expected for expected in baseline.cases.values() if expected != 'pass')
        if others:
            ends = ', '.join(f'{others.count(result)} {result}' for result in sorted(set(others)))
            lines.insert(0, f'All {len(baseline.cases)} cases of {baseline.path} end as it expects ({ends}).')
        else:
            lines.insert(0, f'All {len(baseline.cases)} cases of {baseline.path} pass.')
    return problems, lines


def failing(result):
    return result != 'pass' and not result.startswith('skip')


def beside(report, known):
    """the warnings of a run of another build beside the pinned release's report, and the summary's lines"""
    own = [key for key in sorted(report.cases) if key not in known.cases]
    new = [f'{key}: {report.cases[key]}' for key in own if failing(report.cases[key])]
    lost = [f'{key}: {report.cases[key]}' for key in known.passed() if failing(report.cases.get(key, 'pass'))]
    lines = [f'{len(own)} cases not in {known.build}, {len(own) - len(new)} of them passing or skipped.']
    lines += listed(f'Not in {known.build}, not passing', new)
    lines += listed(f'Passing in {known.build}, not here', lost)
    warnings = [f'not in {known.build}: {case}' for case in new]
    warnings += [f'passing in {known.build}: {case}' for case in lost]
    return warnings, lines


def warn(text):
    """a GitHub annotation: one line, its % and line ends escaped"""
    text = text.replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
    print(f'::warning title=edenprobe main::{text}')


def report_of(sdmc):
    """the report the latest launch writes, which sdmc:/edenprobe/last-run names, or None"""
    last = sdmc / 'last-run'
    folder = last.read_text(encoding='utf-8').strip() if last.is_file() else ''
    path = sdmc / folder / 'report.jsonl'
    return path if folder and path.is_file() else None


def stop(process):
    if os.name == 'nt':
        process.kill()
    else:
        os.killpg(process.pid, signal.SIGKILL)
    process.wait()


def launch(game, log, env, sdmc, quiet, settings):
    """the game's exit status, or None when it was killed after quiet seconds without a new line in its report: a
    slow machine goes on as long as cases end"""
    with open(log, 'wb') as out:
        process = subprocess.Popen(
            [str(game), '-H', *(f'--{s}' for s in settings)],
            cwd=game.parent,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=out,
            stderr=subprocess.STDOUT,
            start_new_session=os.name != 'nt',
        )
        heard, size = time.monotonic(), None
        try:
            while True:
                try:
                    return process.wait(timeout=0.25)
                except subprocess.TimeoutExpired:
                    pass
                path = report_of(sdmc)
                now = path.stat().st_size if path else None
                if now != size:
                    heard, size = time.monotonic(), now
                elif time.monotonic() - heard > quiet:
                    stop(process)
                    return None
        except BaseException:  # Ctrl-C included: the game has a session of its own and would outlive this
            stop(process)
            raise


def play(args, out):
    """recompile and launch until the summary: what went wrong, what each step took, the report and its file"""
    work = out / 'work'
    xdg = {f'XDG_{kind.upper()}_HOME': str(work / 'xdg' / kind) for kind in ('data', 'config', 'state', 'cache')}
    env = dict(os.environ, **xdg)
    env.pop('SNX_GAME', None)
    games = work / 'games'
    games.mkdir(parents=True)
    began = time.monotonic()
    command = [str(args.runtime.resolve()), '-r', str(args.nro.resolve()), NAME, '--outdir', str(games)]
    with open(out / 'recompile.log', 'wb') as log:
        recompiled = subprocess.run(
            command + ['--jobs', str(args.jobs)], check=False, env=env, stdout=log, stderr=subprocess.STDOUT
        )
    steps = [f'recompiled in {time.monotonic() - began:.0f} s']
    game = games / NAME / f'{NAME}{EXE}'
    if recompiled.returncode or not game.is_file():
        made = '' if game.is_file() else f' without making {game.name}'
        return [f'the recompile exited {recompiled.returncode}{made}: recompile.log'], steps, None, None
    sdmc = games / NAME / f'{NAME}_data' / 'sdmc' / 'edenprobe'
    sdmc.mkdir(parents=True, exist_ok=True)
    (sdmc / 'mode').write_text(' '.join(args.mode.split()) + '\n', encoding='utf-8')

    report, cases = None, -1
    for number in range(1, args.launches + 1):
        began = time.monotonic()
        status = launch(game, out / f'launch-{number}.log', env, sdmc, args.timeout, args.setting)
        ended = f'killed after {args.timeout:g} s with no new line' if status is None else f'exit {status}'
        steps.append(f'launch {number}: {ended}, {time.monotonic() - began:.0f} s')
        print(steps[-1], flush=True)
        path = report_of(sdmc)
        if not path:
            return [f'launch {number} ({ended}) left no report: launch-{number}.log says why'], steps, None, None
        try:
            report = Report(path)
        except Failed as error:
            return [str(error)], steps, None, path
        if report.summary:
            return [f'the program ended with {ended} after its summary'] if status else [], steps, report, path
        if len(report.cases) <= cases:
            problem = f'launch {number} ({ended}) added no case: edenprobe would only end the same way again'
            return [problem], steps, report, path
        cases = len(report.cases)
    return [f'{args.launches} launches and still no summary'], steps, report, path


def run(args):
    out = args.out.resolve()
    if not args.nro.is_file():
        sys.exit(f'no NRO at {args.nro}')
    if not os.access(args.runtime, os.X_OK):
        sys.exit(f'{args.runtime} is not an executable')
    if 'quick' not in words(args.mode):
        sys.exit('the mode needs "quick": without it edenprobe waits for a person on its start screen')
    if args.launches < 1:
        sys.exit('--launches is at least 1')
    if out.exists() and any(out.iterdir()) and not (out / 'work').is_dir():
        sys.exit(f"{out} holds files and is no earlier run's: --out is emptied, name another")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    problems, steps, report, path = play(args, out)

    lines = []
    if path:
        shutil.copy2(path, out / 'report.jsonl')
        if (path.parent / 'images').is_dir():
            shutil.copytree(path.parent / 'images', out / 'images')
    if report:
        if not report.build:
            problems.append('the report names no build: no "edenprobe ... (BUILD) starting" note')
        if report.broken:
            problems.append(f'report.jsonl lines that are no case, note or summary: {report.broken[:10]}')
        if report.unknown:
            problems.append(f'edenprobe ignored words of the mode: {"; ".join(report.unknown)}')
    if report and report.summary:
        ends = dict.fromkeys(report.passed(), 'pass')
        if args.baseline and not args.baseline.is_file():
            problems.append(f"no baseline at {args.baseline}: this run's baseline.txt is the one to put there")
        elif args.baseline:
            try:
                against = Baseline(args.baseline)
                found, lines = compare(report, against, args.mode)
                problems += found
                # the other results the baseline expects and this run had stay in its baseline.txt, so a baseline
                # written again from runs keeps them
                if (against.build, against.mode) == (report.build, words(args.mode)):
                    ends.update(
                        (key, expected)
                        for key, expected in against.cases.items()
                        if expected != 'pass' and report.cases.get(key) == expected
                    )
            except Failed as error:
                problems.append(str(error))
        text = Baseline.text(report.build, args.mode, ends)
        (out / 'baseline.txt').write_text(text, encoding='utf-8')
    warnings = []
    if report and report.summary and args.advisory:
        try:
            warnings, lines = beside(report, Report(args.advisory))
        except (Failed, OSError) as error:
            problems.append(f'{args.advisory}: {error}')

    title = f'edenprobe {report.build if report else "(no report)"}'
    if report and report.summary:
        title += ': ' + ', '.join(
            f'{report.summary.get(count, "?")} {count}' for count in ('passed', 'failed', 'skipped', 'crashed')
        )
    else:
        title += ', no summary'
    if args.advisory:
        title += " (edenprobe's main: warnings only)"
    text = [
        f'### {title}',
        '',
        f'Mode `{" ".join(args.mode.split())}`; ' + '; '.join(steps) + '.',
        *[f'\n**{problem}**' for problem in problems],
        *lines,
    ]
    summary = '\n'.join(text) + '\n'
    print(summary)
    (out / 'summary.md').write_text(summary, encoding='utf-8')
    if args.summary:
        with open(args.summary, 'a', encoding='utf-8') as file:
            file.write(summary)
    if args.advisory:
        for line in (problems + warnings)[:MOST]:
            warn(line)
        return 0
    return 1 if problems else 0


def baseline(args):
    files = [Baseline(path) for path in args.baselines]
    first = files[0]
    for other in files[1:]:
        if (other.build, other.mode) != (first.build, first.mode):
            sys.exit(
                f'{other.path} is of {other.build} in mode "{" ".join(other.mode)}", {first.path} of '
                f'{first.build} in mode "{" ".join(first.mode)}": one build and one mode only'
            )
    # a case that ends the same way in every file: the result each file gives it
    cases = {
        key: expected
        for key, expected in first.cases.items()
        if all(other.cases.get(key) == expected for other in files[1:])
    }
    args.output.write_text(Baseline.text(first.build, ' '.join(first.mode), cases), encoding='utf-8')
    dropped = set().union(*(file.cases for file in files)) - set(cases)
    print(
        f'{args.output}: {len(cases)} cases of edenprobe {first.build}; {len(dropped)} did not end the same way in every run'
    )
    for case in sorted(dropped):
        ends = ', '.join(file.cases.get(case, 'not there') for file in files)
        print(f'  left out: {case} ({ends})')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    runs = commands.add_parser('run', help='recompile, run and check edenprobe')
    runs.add_argument('--runtime', type=Path, required=True, help='the scarletnx driver')
    runs.add_argument('--nro', type=Path, required=True)
    runs.add_argument('--mode', required=True, help='the mode file\'s words, "quick" among them')
    runs.add_argument('--out', type=Path, required=True, help="emptied, then the run's files")
    against = runs.add_mutually_exclusive_group()
    against.add_argument('--baseline', type=Path)
    against.add_argument('--advisory', type=Path, metavar='REPORT', help="the pinned release's report.jsonl")
    runs.add_argument('--summary', type=Path)
    runs.add_argument(
        '--timeout', type=float, default=600, help='seconds a launch may go without a new report line (default 600)'
    )
    runs.add_argument('--launches', type=int, default=16, help='launches at most (default 16)')
    runs.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    runs.add_argument(
        '--setting',
        action='append',
        default=[],
        metavar='KEY=VALUE',
        help="a runtime setting for every launch, as --KEY=VALUE (vk_memory=2: the in-place path, which lavapipe takes)",
    )
    writes = commands.add_parser('baseline', help='the cases that end the same way in every baseline.txt given')
    writes.add_argument('baselines', type=Path, nargs='+')
    writes.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        return run(args) if args.command == 'run' else baseline(args)
    except Failed as error:
        sys.exit(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
