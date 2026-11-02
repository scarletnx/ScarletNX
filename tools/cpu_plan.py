#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Prepare CPU implementation work, run checks and compare exact candidates."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import itertools
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET

from tune_cpu_sequences import available_cpus, execute, validate

ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = {'I2': ['T5'], 'I3': ['I2', 'I4', 'T5'], 'I6': ['T5'],
                'I13': ['T5'], 'L2': ['A5'], 'D1': ['A3']}
INACTIVE = {'implemented', 'retired', 'deferred'}
# These are known tests, not commands extracted from a plan document. Additional proofs still need review.
CHECKS = {
    'I7': (['snx-test-llvm-fp-helper-effects', 'snx-test-llvm-qemu-differential'],
           ['llvm_fp_helper_effects', 'llvm_qemu_differential.*']),
    'I8': (['snx-test-llvm-recpe', 'snx-test-llvm-recpx', 'snx-test-llvm-rsqrte',
            'snx-test-llvm-estimates', 'snx-test-llvm-unsigned-estimates',
            'snx-test-llvm-qemu-differential'],
           ['llvm_recpe', 'llvm_recpx', 'llvm_rsqrte', 'llvm_estimates', 'llvm_unsigned_estimates',
            'llvm_qemu_differential.*']),
    'I9': (['snx-test-llvm-simd-shift', 'snx-test-llvm-qemu-differential'],
           ['llvm_simd_shift', 'llvm_qemu_differential.*']),
    'I11': (['snx-test-llvm-round', 'snx-test-llvm-half', 'snx-test-llvm-fma-residual',
             'snx-test-llvm-qemu-differential'],
            ['llvm_round', 'llvm_half', 'llvm_fma_residual', 'llvm_qemu_differential.*']),
    'I15': (['snx-test-llvm-vfp-residual', 'snx-test-llvm-recpe', 'snx-test-llvm-estimates',
             'snx-test-llvm-unsigned-estimates', 'snx-test-llvm-simd-shift', 'snx-test-llvm-round',
             'snx-test-llvm-half', 'snx-test-llvm-fma-residual', 'snx-test-llvm-qemu-differential'],
            ['llvm_vfp_residual', 'llvm_recpe', 'llvm_estimates', 'llvm_unsigned_estimates', 'llvm_simd_shift',
             'llvm_round', 'llvm_half', 'llvm_fma_residual', 'llvm_qemu_differential.*']),
    'T3': (['scarletnx', 'snx-test-llvm-fp-helper-effects', 'snx-test-llvm-module'],
           ['llvm_fp_helper_effects', 'llvm_module', 'llvm_module_stats']),
    'T5': (['snx-test-llvm-block-fuzz'], ['llvm_block_fuzz.*']),
}


def read_plan(path):
    text = path.read_text()
    headings = list(re.finditer(r'^(#{3,5}) ([A-Z]\d+) (.+)$', text, re.M))
    items = {}
    for heading in headings:
        # Stop at any peer/ancestor heading, including an unnumbered subsection.
        rest = text[heading.end():]
        end = re.search(r'^#{1,' + str(len(heading[1])) + r'} ', rest, re.M)
        body = rest[:end.start()] if end else rest
        item = heading[2]
        if item in items:
            raise ValueError(f'duplicate plan item {item}')
        items[item] = {'id': item, 'title': heading[3], 'body': body.strip(),
                       'dependencies': DEPENDENCIES.get(item, [])}
    if not items:
        raise ValueError('no numbered CPU plan items found')
    return items


def work_queue(items):
    """Order active work while retaining incomplete prerequisites as explicit blockers."""
    visiting, visited = set(), set()

    def visit(item):
        if item in visiting:
            raise ValueError(f'cyclic prerequisites at {item}')
        if item in visited:
            return
        visiting.add(item)
        for dependency in items[item]['dependencies']:
            if dependency not in items:
                raise ValueError(f'{item}: unknown prerequisite {dependency}')
            visit(dependency)
        visiting.remove(item)
        visited.add(item)

    for item in items:
        visit(item)
    rows = []
    for position, row in enumerate(items.values()):
        if row['status'] in INACTIVE:
            continue
        priority = re.search(r'\bP([0-3])\b', row['body'])
        blocked = [d for d in row['dependencies'] if items[d]['status'] != 'implemented']
        rows.append({**row, 'blocked_by': blocked, 'mode': 'research' if row['status'] == 'research' else 'implementation',
                     'priority': int(priority[1]) if priority else 9, 'position': position})
    return sorted(rows, key=lambda row: (bool(row['blocked_by']), row['mode'] == 'research', row['priority'],
                                         row['status'] != 'in_progress', row['position']))


def source_catalog(root):
    result = subprocess.run(['rg', '--files', 'src', 'tests', 'tools', 'docs'], cwd=root,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return [Path(line) for line in result.stdout.splitlines()]


def source_matches(row, catalog):
    """Filename matches are navigation hints, not evidence that an item is implemented."""
    tokens = re.findall(r'`([^`\n]+)`', row['body'])
    names = set()
    for token in tokens:
        if re.fullmatch(r'[\w./-]+\.(?:c|h|py|md|txt|cmake)', token):
            names.add(token)
    evidence = set(row.get('evidence', []))
    # Documentation and status entries use repository paths, including on Windows.
    paths = (path.as_posix() for path in catalog)
    return sorted(path for path in paths if path in evidence or
                  any(path == name or path.endswith('/' + name) for name in names))


def write_packets(items, out, root, selected=None):
    queue = work_queue(items)
    if selected:
        if selected not in items:
            raise ValueError(f'unknown item {selected}')
        if items[selected]['status'] in INACTIVE:
            raise ValueError(f'{selected} is {items[selected]["status"]}; no active packet to generate')
        queue = [row for row in queue if row['id'] == selected]
    catalog = source_catalog(root)
    out.mkdir(parents=True, exist_ok=False)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    hashes = {}
    rows = []
    for row in queue:
        folder = out / row['id']
        folder.mkdir()
        matches = source_matches(row, catalog)
        for path in matches:
            if path not in hashes:
                hashes[path] = hashlib.sha256((root / path).read_bytes()).hexdigest()
        paths = [{'path': str(root / path), 'sha256': hashes[path], 'role': 'filename match'} for path in matches]
        evidence = [{'path': path, 'exists': (root / path).is_file()} for path in row.get('evidence', [])]
        checks = CHECKS.get(row['id'])
        context = {**row, 'source_matches': paths, 'recorded_evidence': evidence,
                   'automated_targets': checks[0] if checks else [], 'automated_tests': checks[1] if checks else [],
                   'verification_limit': 'Known checks do not prove the whole plan item or game performance.'}
        (folder / 'context.json').write_text(json.dumps(context, indent=2) + '\n')
        lines = [f'# {row["id"]}: {row["title"]}', '', f'Status: {row["status"]}. Mode: {row["mode"]}.',
                 'Incomplete prerequisites: ' + (', '.join(row['blocked_by']) or 'none recorded'), '',
                 'Recorded progress: ' + row.get('note', 'Unverified. Inspect existing code before treating it as missing.'), '',
                 row['body'], '', '## Source navigation', '',
                 'These are filename matches. Read the implementations before drawing conclusions.', '']
        lines.extend(f'- `{root / path}`' for path in matches)
        if not matches:
            lines.append('No filename match. Locate the relevant implementation before editing.')
        if checks:
            command = [sys.executable, str(root / 'tools/cpu_plan.py'), 'verify', row['id'],
                       '--build-dir', str(root / 'build'), '--out', str(folder / 'checks')]
            lines.extend(['', '## Known checks', '', '```sh', shlex.join(command), '```',
                          'This runs only the mapped checks. Complete the other proofs stated above separately.'])
        lines.extend(['', '## Working record', '', '- Mechanism and expected behavior:',
                      '- Candidate changes and ABI review:', '- Independent correctness proof:',
                      '- Baseline and candidate settings:', '- Measurements, regressions and remaining limits:', '',
                      'Keep game files and profiles local. Test output never marks this item complete.', ''])
        (folder / 'TASK.md').write_text('\n'.join(lines))
        rows.append({'id': row['id'], 'title': row['title'], 'status': row['status'], 'mode': row['mode'],
                     'blocked_by': row['blocked_by'], 'packet': str(folder / 'TASK.md')})
    snapshot = {'schema': 1, 'revision': revision, 'created_utc': datetime.now(timezone.utc).isoformat(),
                'allowed_cpus': available_cpus(),
                'status_counts': dict(Counter(row['status'] for row in items.values())), 'queue': rows,
                'claim': 'Preparation and navigation only. No implementation, completion or performance is inferred.'}
    (out / 'queue.json').write_text(json.dumps(snapshot, indent=2) + '\n')
    (out / 'README.md').write_text('# CPU work packets\n\n' + '\n'.join(
        f'- {row["id"]}: {row["title"]} ({row["status"]}); blockers: ' +
        (', '.join(row['blocked_by']) or 'none recorded') for row in rows) +
        '\n\nThe status counts describe the tracker. Unverified does not mean absent.\n')
    return snapshot


def expand_matrix(spec, item):
    if not isinstance(spec, dict) or spec.get('item') != item:
        raise ValueError('matrix item must match the requested plan item')
    axes = spec.get('axes')
    if not isinstance(axes, dict) or not axes:
        raise ValueError('axes must be a nonempty object')
    count = 1
    for name, values in axes.items():
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', name) or not isinstance(values, list) or not values:
            raise ValueError('each axis needs an identifier and a nonempty list')
        if any(type(value) not in (str, int) for value in values) or len({str(v) for v in values}) != len(values):
            raise ValueError('axis values must be distinct strings or integers')
        count *= len(values)
        if count > 256:
            raise ValueError('matrix exceeds the 256-candidate limit')

    def substitute(value, fields):
        if isinstance(value, str):
            def replace(match):
                if match[1] not in fields:
                    raise ValueError(f'unknown matrix axis {match[1]}')
                return str(fields[match[1]])
            return re.sub(r'\$\{([A-Za-z][A-Za-z0-9_]*)\}', replace, value)
        if isinstance(value, list):
            return [substitute(v, fields) for v in value]
        if isinstance(value, dict):
            result = {}
            for key, part in value.items():
                key = substitute(key, fields)
                if key in result:
                    raise ValueError(f'duplicate expanded key {key}')
                result[key] = substitute(part, fields)
            return result
        return value

    candidates = [substitute(spec.get('candidate'), dict(zip(axes, values)))
                  for values in itertools.product(*axes.values())]
    manifest = {'item': item, 'axes': axes, 'candidates': candidates}
    validate(manifest)
    return manifest


def verify_item(item, build, out, jobs, timeout, root):
    if item not in CHECKS:
        raise ValueError(f'{item}: no audited automatic check mapping yet; use its work packet to add the proofs')
    if jobs < 1 or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('jobs and timeout must be positive')
    out.mkdir(parents=True, exist_ok=False)
    targets, patterns = CHECKS[item]
    regex = '^(' + '|'.join(patterns) + ')$'
    commands = [
        ['cmake', '--build', str(build), '-j', str(jobs), '--target', *targets],
        ['ctest', '--test-dir', str(build), '-R', regex, '--show-only=json-v1'],
        ['ctest', '--test-dir', str(build), '-R', regex, '--output-on-failure',
         '--output-junit', str(out / 'ctest.xml')],
    ]
    report = {'item': item, 'commands': commands, 'verdict': 'incomplete',
              'claim': 'Checks are evidence only. Source status and PR readiness are not changed.'}
    try:
        for phase, command in zip(('build', 'discover', 'tests'), commands):
            print(f'{item}: {phase}', flush=True)
            output = execute(command, root, out / f'{phase}.log', timeout)
            if phase == 'discover':
                tests = json.loads(output).get('tests', [])
                if not tests:
                    raise ValueError('no matching tests were configured')
                report['tests'] = [test['name'] for test in tests]
                for pattern in patterns:
                    if not any(re.fullmatch(pattern, name) for name in report['tests']):
                        raise ValueError(f'missing configured test group: {pattern}')
        xml = ET.parse(out / 'ctest.xml')
        cases = xml.findall('.//testcase')
        report['skipped'] = [case.get('name') for case in cases if case.find('skipped') is not None]
        report['reported_tests'] = [case.get('name') for case in cases]
        if set(report['tests']) - set(report['reported_tests']):
            raise ValueError('configured tests are missing from the CTest result')
        failed = [case.get('name') for case in cases if case.find('failure') is not None or case.find('error') is not None]
        if not cases or failed:
            raise ValueError('CTest report has no cases or contains failures')
        report['verdict'] = 'incomplete' if report['skipped'] else 'passed-checks'
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired, ET.ParseError) as error:
        report['error'] = str(error)
    (out / 'checks.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, default=ROOT / 'docs/CPU_TRANSLATOR_PLAN.md')
    parser.add_argument('--status', type=Path, default=ROOT / 'docs/cpu_plan_status.json')
    sub = parser.add_subparsers(dest='command', required=True)
    listing = sub.add_parser('list')
    listing.add_argument('--json', action='store_true')
    show = sub.add_parser('show')
    show.add_argument('item')
    run = sub.add_parser('run')
    run.add_argument('item')
    run.add_argument('manifest', type=Path)
    run.add_argument('arguments', nargs=argparse.REMAINDER)
    next_work = sub.add_parser('next', help='show active work and incomplete prerequisites')
    next_work.add_argument('--json', action='store_true')
    packet = sub.add_parser('packet', help='write source navigation, requirements and proof records')
    packet.add_argument('item', nargs='?', help='omit to prepare every active item')
    packet.add_argument('--out', type=Path, required=True)
    matrix = sub.add_parser('matrix', help='expand a parameter grid into a validated candidate manifest')
    matrix.add_argument('item')
    matrix.add_argument('spec', type=Path)
    matrix.add_argument('--out', type=Path, required=True)
    verify = sub.add_parser('verify', help='build and run known checks without changing item status')
    verify.add_argument('item')
    verify.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    verify.add_argument('--out', type=Path, required=True)
    verify.add_argument('--jobs', type=int, default=len(available_cpus()))
    verify.add_argument('--timeout', type=float, default=1800)
    args = parser.parse_args(argv)
    try:
        items = read_plan(args.plan)
        status = json.loads(args.status.read_text())
        if status.get('schema') != 1:
            raise ValueError('unsupported CPU plan status schema')
        missing = set(status['items']) - set(items)
        if missing:
            raise ValueError('status refers to absent items: ' + ', '.join(sorted(missing)))
        for item, row in items.items():
            row.update(status['items'].get(item, {'status': 'unverified', 'evidence': []}))
        if args.command == 'list':
            if args.json:
                print(json.dumps(list(items.values()), indent=2))
            else:
                for row in items.values():
                    print(f"{row['id']:3} {row['status']:12} {row['title']}")
            return 0
        if args.command == 'next':
            rows = work_queue(items)
            if args.json:
                print(json.dumps(rows, indent=2))
            else:
                for row in rows:
                    state = 'blocked by ' + ', '.join(row['blocked_by']) if row['blocked_by'] else row['mode']
                    print(f"{row['id']:3} P{row['priority']} {state:24} {row['title']}")
            return 0
        if args.command == 'packet':
            snapshot = write_packets(items, args.out.resolve(), ROOT, args.item)
            print(f'Prepared {len(snapshot["queue"])} work packets in {args.out.resolve()}')
            return 0
        if args.item not in items:
            raise ValueError(f'unknown item {args.item}')
        row = items[args.item]
        if args.command == 'show':
            print(f"{row['id']} {row['title']} [{row['status']}]\n\n{row['body']}")
            if row['dependencies']:
                print('\nPrerequisites: ' + ', '.join(row['dependencies']))
            return 0
        if args.command == 'matrix':
            if row['status'] in {'retired', 'deferred'}:
                raise ValueError(f'{args.item} is {row["status"]}')
            manifest = expand_matrix(json.loads(args.spec.read_text()), args.item)
            with args.out.open('x') as output:
                output.write(json.dumps(manifest, indent=2) + '\n')
            print(f'Prepared {len(manifest["candidates"])} candidates in {args.out.resolve()}')
            return 0
        if args.command == 'verify':
            report = verify_item(args.item, args.build_dir.resolve(), args.out.resolve(), args.jobs, args.timeout, ROOT)
            print(json.dumps({'verdict': report['verdict'], 'report': str(args.out.resolve() / 'checks.json')}))
            return 0 if report['verdict'] == 'passed-checks' else 1
        # Measurement never marks an architecture item complete or applies its code.
        manifest = json.loads(args.manifest.read_text())
        if manifest.get('item') != args.item:
            raise ValueError('the manifest item must match the requested plan item')
        command = [sys.executable, str(ROOT / 'tools/tune_cpu_sequences.py'), str(args.manifest), *args.arguments]
        return subprocess.run(command, check=False).returncode
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'cpu-plan: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
