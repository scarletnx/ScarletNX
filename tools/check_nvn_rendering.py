#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Run synthetic NVN state and real Vulkan output checks. No game compatibility claim."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

NATIVE = ('nvn-descriptors', 'nvn-tex-storage', 'nvn-commands', 'nvn-draw-banks', 'nvn-copy-formats', 'nvn-names',
          'nvn-advanced-blend', 'vk-coverage', 'vk-tess-passthrough', 'vk-sample-locations', 'nvn-queue-lifetime', 'vk-advanced-blend')
SHADER_NATIVE = ('shader-direct-regressions', 'shader-float-rounding', 'shader-indexed-ms',
                 'shader-integer-half', 'shader-tcs-dead-outputs', 'shader-blend-images',
                 'shader-half-native', 'shader-txd-uniform', 'shader-half-packing',
                 'shader-texture-limits', 'shader-mixed-sample-safe', 'shader-mixed-coverage', 'shader-fragment-mask')
GPU_MARKER = 'PASS NVN array copies: image texels, mirrored layers, view offsets and output padding exact=1'
GPU_FEATURES = (
    ('array_copies', 'PASS NVN array copies:', None),
    ('shader_windows', 'PASS shader read windows:', None),
    ('shader_windows_owned', 'PASS shader read windows GPU-owned BRX:', 'SKIP shader read windows GPU-owned BRX:'),
    ('texture_half', 'PASS half texture rounding:', 'SKIP half texture rounding:'),
    ('cross_stage_blend', 'PASS cross-stage blend snapshots:', 'SKIP cross-stage blend snapshots:'),
    ('copy_flags', 'PASS NVN copy flags:', None),
    ('stencil_masks', 'PASS NVN stencil masks:', None),
    ('transform_feedback', 'PASS NVN transform feedback:', 'SKIP NVN transform feedback:'),
    ('fixed_state', 'PASS NVN fixed state:', None),
    ('evaluation_only_tessellation', 'PASS NVN evaluation-only tessellation:', 'SKIP NVN evaluation-only tessellation:'),
    ('advanced_blend_subset', 'PASS NVN advanced blend:', None),
    ('mixed_sample_coverage', 'PASS NVN coverage modulation:', 'SKIP NVN coverage modulation:'),
    ('ratio_one_coverage', 'PASS NVN ratio-one coverage:', 'SKIP NVN ratio-one coverage:'),
    ('depth_mixed_coverage', 'PASS NVN depth mixed coverage:', 'SKIP NVN depth mixed coverage:'),
    ('portable_mixed_coverage', 'PASS portable mixed coverage:', 'SKIP portable mixed coverage:'),
    ('native_advanced_blend', 'PASS NVN native advanced blend:', 'SKIP NVN native advanced blend:'),
    ('event_waits', 'PASS NVN event waits:', None),
    ('subroutine_calls', 'PASS NVN subroutine calls:', None),
    ('copy_conversions', 'PASS NVN copy conversions:', None),
    ('programmable_blend', 'PASS NVN shader advanced blend:', 'SKIP NVN shader advanced blend:'),
    ('blend_equations', 'PASS NVN programmable blend equations:', None),
    ('draw_texture', 'PASS NVN DrawTexture:', None),
    ('draw_texture_multisample', 'PASS NVN DrawTexture multisample:', 'SKIP NVN DrawTexture multisample:'),
    ('sample_locations', 'PASS NVN custom raster samples:', 'SKIP NVN custom raster samples:'),
    ('coverage_to_color', 'PASS NVN coverage-to-color:', 'SKIP NVN coverage-to-color:'),
    ('float_rounding', 'PASS shader float narrowing:', 'SKIP shader float narrowing:'),
    ('integer_half', 'PASS shader integer-to-half:', None),
    ('indexed_multisample', 'PASS indexed multisample fetch:', 'SKIP indexed multisample fetch:'),
    ('stage_images', 'PASS graphics image stages:', 'SKIP graphics image stages:'),
    ('cross_stage_image_alias', 'PASS graphics cross-stage image alias:', 'SKIP graphics cross-stage image alias:'),
    ('texture_gradients', 'PASS NVN texture gradients:', 'SKIP NVN texture gradients:'),
    ('cube_array_texture_gradients', 'PASS NVN cube-array texture gradients:', 'SKIP NVN cube-array texture gradients:'),
    ('cube_minified_edge_probe', 'PASS NVN cube minified edge probe:', 'SKIP NVN cube minified edge probe:'),
    ('txd_uniform_offsets', 'PASS TXD uniform offsets:', 'SKIP TXD uniform offsets:'),
    ('txd_varying_offsets', 'PASS TEX/TXD varying offsets:', 'SKIP TXD varying offsets:'),
    ('texture_min_lod', 'PASS TEX minimum LOD:', 'SKIP TEX minimum LOD:'),
    ('alpha_coverage', 'PASS undithered alpha coverage:', 'SKIP undithered alpha coverage:'),
    ('late_blend_capture', 'PASS NVN late blend capture:', 'SKIP NVN late blend capture:'),
    ('late_blend_resolve', 'PASS NVN late blend resolve:', 'SKIP NVN late blend resolve:'),
    ('blend_images', 'PASS advanced blend with guest image:', 'SKIP advanced blend with guest image:'),
    ('advanced_targets', 'PASS shader advanced targets:', 'SKIP shader advanced targets:'),
    ('indexed_feedback', 'PASS NVN indexed texture feedback:', 'SKIP NVN indexed texture feedback:'),
    ('indexed_programmable_blend', 'PASS NVN indexed programmable blend:', 'SKIP NVN indexed programmable blend:'),
)
# The alpha fixture can run its base cases while one ordering variant is unavailable. Keep that explicit gap
# inside the feature result without treating a backend-specific heap fixture as a requirement for other modes.
GPU_PARTIAL_SKIPS = (('alpha_coverage', 'SKIP alpha-to-one coverage ordering:'),)


def native_paths(build):
    """Frontend and Vulkan helpers are in tests; translator checks are built beside the NX targets."""
    return ([build / 'tests' / ('snx-test-' + name) for name in NATIVE] +
            [build / 'src/nx' / ('snx-test-' + name) for name in SHADER_NATIVE])


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


def comparison_passed(line):
    """Require an exact byte result or a nonzero count of checked comparisons."""
    results = []
    for token in line.split():
        if token.startswith('exact='):
            results.append(token == 'exact=1')
        elif token.startswith('checked='):
            results.append(re.fullmatch(r'checked=[1-9][0-9]*', token) is not None)
    return bool(results) and all(results)


def classify(output, code, stable=True, gpu=False):
    """A successful process must report every required output check explicitly."""
    lines = output.splitlines()
    has = lambda marker: any(line.startswith(marker) for line in lines)
    status = ('pass' if code == 0 and (not gpu or has(GPU_MARKER)) else
              'skip' if gpu and code == 77 else 'fail')
    features = []
    if gpu and code == 0:
        for feature, passed, skipped in GPU_FEATURES:
            did_pass = any(line.startswith(passed) and comparison_passed(line) for line in lines)
            did_skip = bool(skipped and has(skipped))
            partial_skip = any(name == feature and has(marker) for name, marker in GPU_PARTIAL_SKIPS)
            result = 'pass' if did_pass and not did_skip else 'skip' if did_skip and not did_pass else 'fail'
            if result == 'pass' and partial_skip:
                result = 'skip'
            features.append({'name': feature, 'status': result})
        if any(row['status'] == 'fail' for row in features):
            status = 'fail'
    if not stable:
        status = 'fail'
    if any('vulkan validation:' in line.lower() and
           any(error in line.lower() for error in ('vuid-', 'threading', 'validation error', 'unassigned-'))
           for line in lines):
        status = 'fail'
    return {'status': status, 'features': features,
            'skip_messages': [line for line in lines if line.startswith('SKIP NVN ') or
                              any(skipped and line.startswith(skipped) for _, _, skipped in GPU_FEATURES) or
                              any(line.startswith(marker) for _, marker in GPU_PARTIAL_SKIPS)]}


def all_features_passed(checks):
    return all(row['status'] == 'pass' and not row['skip_messages'] and
               all(feature['status'] == 'pass' for feature in row['features']) for row in checks)


def jobs(native, vulkan, drivers, validation=False, record='0'):
    """Build the serial driver/descriptor/recording matrix without changing old default job names."""
    if record not in ('0', '1', 'both'):
        raise ValueError('record must be 0, 1, or both')
    modes = (0, 1) if record == 'both' else (int(record),)
    selected = [(path.name, [str(path)], None, None) for path in native]
    for label, manifest in drivers:
        for descriptors in (0, 1, 2):
            for mode in modes:
                suffix = f'-record-{mode}' if mode else ''
                selected.append((f'{label}-descriptors-{descriptors}{suffix}',
                                 [str(vulkan), '-H', '--vk_memory=1', f'--vk_record={mode}',
                                  f'--vk_descriptors={descriptors}', f'--vk_validation={int(validation)}'],
                                 manifest, GPU_MARKER))
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--out', type=Path, default=Path('.cache/nvn-render-check'))
    parser.add_argument('--icd', type=Path, action='append', default=[],
                        help='also test this Vulkan ICD manifest, in addition to the default driver; repeatable')
    parser.add_argument('--validation', action='store_true', help='enable Vulkan validation for every GPU run')
    parser.add_argument('--record', choices=('0', '1', 'both'), default='0',
                        help='Vulkan recording-thread mode; both tests each driver and descriptor mode twice')
    args = parser.parse_args()
    root = args.root.resolve()
    build, out = (root / args.build).resolve(), (root / args.out).resolve()
    native = native_paths(build)
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
              'sources_sha256': digest(out / 'sources.json'), 'runtime_source_files': len(sources),
              'record': args.record, 'checks': []}
    for name, command, manifest, marker in jobs(native, vulkan, drivers, args.validation, args.record):
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
