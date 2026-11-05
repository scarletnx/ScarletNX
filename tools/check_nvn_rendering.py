#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Run synthetic NVN state and real Vulkan output checks. No game compatibility claim."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

NATIVE = ('nvn-descriptors', 'nvn-tex-storage', 'nvn-commands', 'nvn-draw-banks', 'nvn-copy-formats', 'nvn-names',
          'nvn-advanced-blend', 'vk-coverage', 'vk-tess-passthrough')
GPU_MARKER = 'PASS NVN array copies: image texels, mirrored layers, view offsets and output padding exact=1'
GPU_FEATURES = (
    ('array_copies', 'PASS NVN array copies:', None),
    ('copy_flags', 'PASS NVN copy flags:', None),
    ('stencil_masks', 'PASS NVN stencil masks:', None),
    ('transform_feedback', 'PASS NVN transform feedback:', 'SKIP NVN transform feedback:'),
    ('fixed_state', 'PASS NVN fixed state:', None),
    ('evaluation_only_tessellation', 'PASS NVN evaluation-only tessellation:', 'SKIP NVN evaluation-only tessellation:'),
    ('advanced_blend_subset', 'PASS NVN advanced blend:', None),
    ('mixed_sample_coverage', 'PASS NVN coverage modulation:', 'SKIP NVN coverage modulation:'),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_snapshot(root):
    """Record only the checkout's GPU sources, with portable repository-relative paths."""
    return [{'path': path.relative_to(root).as_posix(), 'sha256': digest(path)}
            for path in sorted((root / 'src/nx/gpu').rglob('*')) if path.is_file()]


def changed_sources(root, before):
    """Catch additions and deletions as well as byte changes during a suite."""
    old = {row['path']: row['sha256'] for row in before}
    new = {row['path']: row['sha256'] for row in source_snapshot(root)}
    return sorted(path for path in old.keys() | new.keys() if old.get(path) != new.get(path))


def classify(output, code, stable=True, gpu=False):
    """A successful process must report every required output check explicitly."""
    lines = output.splitlines()
    has = lambda marker: any(line.startswith(marker) for line in lines)
    status = ('pass' if code == 0 and (not gpu or has(GPU_MARKER)) else
              'skip' if gpu and code == 77 else 'fail')
    features = []
    if gpu and code == 0:
        for feature, passed, skipped in GPU_FEATURES:
            did_pass, did_skip = has(passed), bool(skipped and has(skipped))
            result = 'pass' if did_pass and not did_skip else 'skip' if did_skip and not did_pass else 'fail'
            features.append({'name': feature, 'status': result})
        if any(row['status'] == 'fail' for row in features):
            status = 'fail'
    if not stable:
        status = 'fail'
    return {'status': status, 'features': features,
            'skip_messages': [line for line in lines if line.startswith('SKIP NVN ')]}


def all_features_passed(checks):
    return all(row['status'] == 'pass' and not row['skip_messages'] and
               all(feature['status'] == 'pass' for feature in row['features']) for row in checks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--out', type=Path, default=Path('.cache/nvn-render-check'))
    parser.add_argument('--icd', type=Path, action='append', default=[],
                        help='also test this Vulkan ICD manifest, in addition to the default driver; repeatable')
    args = parser.parse_args()
    root = args.root.resolve()
    build, out = (root / args.build).resolve(), (root / args.out).resolve()
    native = [build / 'tests' / ('snx-test-' + name) for name in NATIVE]
    vulkan = build / 'src/nx/snx-test-vulkan'
    missing = [str(path) for path in [*native, vulkan, root / 'src/nx/gpu'] if not path.exists()]
    if missing:
        parser.error('build the test targets in the checkout first; missing: ' + ', '.join(missing))
    if out.exists():
        parser.error('use a new --out directory to preserve earlier results')
    drivers = [('default', None)]
    for i, path in enumerate(args.icd):
        path = path.resolve()
        if not path.is_file():
            parser.error(f'ICD manifest does not exist: {path}')
        drivers.append((f'icd-{i}', path))
    sources = source_snapshot(root)
    out.mkdir(parents=True)
    (out / 'sources.json').write_text(json.dumps(sources, indent=2) + '\n', encoding='utf-8')
    report = {'format_version': 2, 'scope': 'Synthetic checks only; no game or native NVN image comparison.',
              'sources_sha256': digest(out / 'sources.json'), 'runtime_source_files': len(sources), 'checks': []}
    jobs = [(path.name, [str(path)], None, None) for path in native]
    for label, manifest in drivers:
        for descriptors in (0, 1):
            jobs.append((f'{label}-descriptors-{descriptors}',
                         [str(vulkan), '-H', '--vk_memory=1', '--vk_record=0', f'--vk_descriptors={descriptors}'],
                         manifest, GPU_MARKER))
    for name, command, manifest, marker in jobs:
        environment = os.environ.copy()
        if manifest:
            environment['VK_ICD_FILENAMES'] = str(manifest)
            environment['VK_DRIVER_FILES'] = str(manifest)
        before = digest(Path(command[0]))
        start = time.monotonic()
        try:
            result = subprocess.run(command, cwd=root, env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, errors='replace', timeout=180)
            output, code = result.stdout, result.returncode
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
            if isinstance(output, bytes):
                output = output.decode('utf-8', errors='replace')
            output += '\nHarness timeout after 180 seconds.\n'
            code = -1
        stable = digest(Path(command[0])) == before
        classified = classify(output, code, stable, gpu=marker is not None)
        (out / f'{name}.log').write_text(output, encoding='utf-8')
        report['checks'].append({'name': name, 'command': command, 'icd': str(manifest) if manifest else None,
                                 'executable_sha256': before, 'executable_unchanged': stable, 'returncode': code,
                                 'seconds': time.monotonic() - start, 'log': f'{name}.log', **classified})
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'{classified["status"].upper()} {name}', flush=True)
    changed = changed_sources(root, sources)
    report['sources_changed'] = changed
    report['all_selected_passed'] = not changed and all(row['status'] == 'pass' for row in report['checks'])
    report['all_gpu_features_passed'] = report['all_selected_passed'] and all_features_passed(report['checks'])
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'Report: {out / "results.json"}')
    return int(bool(changed) or any(row['status'] == 'fail' for row in report['checks']))


if __name__ == '__main__':
    raise SystemExit(main())
