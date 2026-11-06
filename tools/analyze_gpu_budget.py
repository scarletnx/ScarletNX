#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Analyze measured GPU/CPU budgets without turning component savings into FPS.

Inputs are explicit measured profiles. Serial cost models, roofline lower bounds,
and measured frame comparisons remain separate evidence. No benchmark is run.
"""
import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import sys


LIMITATIONS = (
    'A component timing is not game FPS or a universal configuration recommendation.',
    'Serial estimates require exclusive costs in one timing domain; unknown overlap has no combined prediction.',
    'Roofline values are optimistic lower bounds, excluding latency, occupancy, synchronization and driver work.',
    'PASS evidence and workload manifests are supplied measurements, not independently certified by this tool.',
    'A synthetic pass, API inventory or supported feature count is not a game compatibility percentage.',
)


def number(value, name, positive=False):
    try:
        finite = not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value)
    except OverflowError:
        finite = False
    if not finite:
        raise ValueError(f'{name} must be a finite number')
    if value < 0 or (positive and value == 0):
        raise ValueError(f'{name} must be {"positive" if positive else "nonnegative"}')
    return value


def text(value, name):
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f'{name} must be a nonempty string')
    return value


def rational(value):
    return value if isinstance(value, Fraction) else Fraction(str(value))


def correctness(record):
    return (isinstance(record, dict) and record.get('status') == 'PASS'
            and isinstance(record.get('evidence'), str) and bool(record['evidence'].strip())
            and record.get('skipped', False) is False and record.get('validation_errors', 0) == 0)


def measured(record, name):
    if not isinstance(record, dict) or record.get('kind') != 'measured':
        raise ValueError(f'{name} requires kind=measured')
    text(record.get('source'), f'{name}.source')
    return record


def amdahl(fraction, target=7):
    """S=1/((1-f)+f/k); k=f/(1/S-(1-f)). None represents an infinite or impossible bound."""
    f, speed = rational(fraction), rational(target)
    if not 0 <= f <= 1 or speed <= 1:
        raise ValueError('Amdahl fraction must be in [0,1], target must exceed 1')
    denominator = 1 / speed - (1 - f)
    return {
        'selected_fraction': float(f),
        'infinite_component_speedup_ceiling': None if f == 1 else float(1 / (1 - f)),
        'ceiling_is_unbounded': f == 1,
        'required_component_speedup': float(f / denominator) if denominator > 0 else None,
        'attainability': 'finite' if denominator > 0 else 'asymptotic' if denominator == 0 else 'impossible',
    }


def roofline(component, hardware, calls):
    bounds = {}
    if 'bytes_per_call' in component:
        traffic = number(component['bytes_per_call'], 'bytes_per_call')
        if 'bandwidth_bytes_per_second' in hardware:
            bandwidth = number(hardware['bandwidth_bytes_per_second'], 'bandwidth_bytes_per_second', True)
            bounds['memory_ns_per_call'] = traffic / bandwidth * 1e9
    if 'operations_per_call' in component:
        operations = number(component['operations_per_call'], 'operations_per_call')
        if 'operations_per_second' in hardware:
            throughput = number(hardware['operations_per_second'], 'operations_per_second', True)
            operation_kind = text(component.get('operation_kind'), 'component.operation_kind')
            if operation_kind != text(hardware.get('operation_kind'), 'hardware.operation_kind'):
                raise ValueError('roofline operation kinds must match')
            bounds['compute_ns_per_call'] = operations / throughput * 1e9
    if not bounds:
        return {'status': 'unavailable', 'reason': 'matching traffic/work and hardware rates were not provided'}
    lower = max(bounds.values())
    return {'status': 'optimistic_lower_bound', **bounds, 'ns_per_call': lower,
            'ns_per_interval': lower * calls, 'combined_rule': 'max(memory,compute), not their sum'}


def workload_identity(workload, directory):
    fingerprint, artifact = workload.get('identity_sha256'), workload.get('identity_artifact')
    if not fingerprint or not artifact:
        return {'verified': False, 'reason': 'a matching workload manifest hash and artifact are required'}
    if not isinstance(fingerprint, str) or len(fingerprint) != 64 or any(c not in '0123456789abcdef' for c in fingerprint):
        raise ValueError('workload.identity_sha256 must be a lowercase SHA256 digest')
    path = directory / text(artifact, 'workload.identity_artifact')
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != fingerprint:
        return {'verified': False, 'reason': 'workload manifest hash mismatch', 'actual_sha256': actual}
    return {'verified': True, 'sha256': actual, 'artifact': str(path)}


def comparison(record, baseline, identity, require_same_hardware):
    reasons = []
    if not isinstance(record, dict) or record.get('kind') != 'measured':
        return {'status': 'unavailable', 'reason': 'an explicit measured record was not provided'}
    text(record.get('source'), 'comparison.source')
    elapsed = number(record.get('interval_ns'), 'comparison.interval_ns', True)
    if not identity['verified'] or record.get('workload_identity_sha256') != identity.get('sha256'):
        reasons.append('verified workload identity does not match')
    if record.get('scope') != baseline['scope'] or record.get('timing_domain') != baseline['timing_domain']:
        reasons.append('measurement scopes or timing domains differ')
    if not correctness(record.get('correctness')):
        reasons.append('correctness PASS evidence is missing, skipped or failed')
    if require_same_hardware and (not baseline.get('hardware_identity') or
                                  record.get('hardware_identity') != baseline['hardware_identity']):
        reasons.append('hardware, driver and settings identity does not match')
    if not isinstance(record.get('hardware_identity'), str) or not record['hardware_identity'].strip():
        reasons.append('measurement hardware identity is missing')
    return ({'status': 'rejected', 'reasons': reasons} if reasons else
            {'status': 'available', 'interval_ns': elapsed, 'source': record['source'],
             'hardware_identity': record['hardware_identity']})


def analyze(profile, target=7, selected=None, directory=Path('.'), metadata=()):
    number(target, 'target_speedup', True)
    if target <= 1 or not isinstance(profile, dict) or profile.get('schema_version') != 1:
        raise ValueError('schema_version=1 and target_speedup>1 are required')
    workload = profile.get('workload', {})
    if not isinstance(workload, dict) or not isinstance(profile.get('hardware', {}), dict):
        raise ValueError('workload and optional hardware must be objects')
    name = text(workload.get('id'), 'workload.id')
    baseline = measured(profile.get('measurement'), 'measurement')
    if 'hardware_identity' in baseline:
        text(baseline['hardware_identity'], 'measurement.hardware_identity')
    scope, domain = baseline.get('scope'), baseline.get('timing_domain')
    if scope not in ('frame', 'component') or domain not in ('wall', 'cpu_thread', 'gpu'):
        raise ValueError('scope must be frame/component; timing_domain must be wall/cpu_thread/gpu')
    serial = baseline.get('serial_components')
    if serial not in (True, False) or not isinstance(serial, bool):
        raise ValueError('measurement.serial_components must explicitly be true or false')
    interval = number(baseline.get('interval_ns'), 'measurement.interval_ns', True)
    hardware = profile.get('hardware', {})
    for rate in ('bandwidth_bytes_per_second', 'operations_per_second'):
        if rate in hardware:
            number(hardware[rate], 'hardware.' + rate, True)
    if 'operations_per_second' in hardware:
        text(hardware.get('operation_kind'), 'hardware.operation_kind')
    identity = workload_identity(workload, directory)
    components = profile.get('components')
    if not isinstance(components, list) or not components:
        raise ValueError('at least one measured component is required')
    rows, seen, total, replacement, accepted = [], set(), Fraction(0), Fraction(0), []
    for component in components:
        measured(component, 'component')
        component_name = text(component.get('name'), 'component.name')
        if component_name in seen:
            raise ValueError('duplicate component: ' + component_name)
        seen.add(component_name)
        if component.get('timing_domain', domain) != domain:
            raise ValueError('component and interval timing domains must match')
        cost = number(component.get('cost_ns'), 'component.cost_ns', True)
        calls = number(component.get('calls_per_interval'), 'component.calls_per_interval')
        text(component.get('frequency_source'), 'component.frequency_source')
        shape = text(component.get('workload_identity'), 'component.workload_identity')
        before = rational(cost) * rational(calls)
        total += before
        after = before
        row = {'name': component_name, 'cost_ns_per_call': cost, 'calls_per_interval': calls,
               'cost_ns_per_interval': float(before), 'source': component['source'],
               'roofline': roofline(component, hardware, calls)}
        if 'candidate' in component:
            candidate = component['candidate']
            measured(candidate, 'component.candidate')
            new_cost = number(candidate.get('cost_ns'), 'candidate.cost_ns', True)
            new_calls = number(candidate.get('calls_per_interval', calls), 'candidate.calls_per_interval')
            if 'calls_per_interval' in candidate:
                text(candidate.get('frequency_source'), 'candidate.frequency_source')
            reasons = []
            if not correctness(candidate.get('correctness')):
                reasons.append('correctness PASS evidence is missing, skipped or failed')
            if candidate.get('workload_identity') != shape:
                reasons.append('component workload identity does not match')
            if candidate.get('timing_domain', domain) != domain:
                reasons.append('candidate timing domain does not match')
            if reasons:
                row['candidate'] = {'status': 'rejected', 'reasons': reasons}
            else:
                after = rational(new_cost) * rational(new_calls)
                row['candidate'] = {'status': 'correctness_gated_component_measurement',
                                    'cost_ns_per_interval': float(after), 'saved_ns_per_interval': float(before - after),
                                    'time_reduction_percent': float(100 * (before - after) / before) if before else None,
                                    'source': candidate['source'], 'correctness': candidate['correctness']}
                accepted.append(component_name)
        replacement += after
        rows.append(row)
    if serial and total > rational(interval):
        raise ValueError('exclusive serial component costs exceed the measured interval; fix overlap or units')
    chosen = set(selected) if selected else seen
    if not chosen <= seen:
        raise ValueError('selected component is not in the profile')
    # Retain exact boundary cases such as 6/7 instead of converting the measured costs to a rounded fraction first.
    selection = sum((rational(c['cost_ns']) * rational(c['calls_per_interval'])
                     for c in components if c['name'] in chosen), Fraction(0))
    theory = amdahl(selection / rational(interval), target) if serial else {
        'status': 'unavailable', 'reason': 'selected costs may overlap'}
    for row in rows:
        if serial:
            row['fraction_of_interval'] = row['cost_ns_per_interval'] / interval
    model = {'status': 'unavailable', 'reason': 'overlap is unknown; costs cannot be added into a prediction'}
    if serial:
        predicted = rational(interval) - total + replacement
        model = {'status': 'conditional_serial_estimate', 'accepted_candidates': accepted,
                 'interval_ns': float(predicted), 'saved_ns': float(total - replacement),
                 'time_reduction_percent': float(100 * (total - replacement) / rational(interval)),
                 'speedup': float(rational(interval) / predicted) if predicted else None,
                 'assumption': 'exclusive same-domain costs and unchanged unmeasured remainder; not a measured frame result'}
    candidate = comparison(profile.get('candidate_measurement'), baseline, identity, True)
    verification = {'status': 'unverified', 'reason': 'a matching correctness-gated measured full-frame comparison is required'}
    if candidate['status'] == 'available':
        if scope == 'frame' and domain == 'wall' and correctness(baseline.get('correctness')):
            candidate['speedup'] = interval / candidate['interval_ns']
            verification = {'status': 'met' if rational(interval) >= rational(target) * rational(candidate['interval_ns'])
                            else 'not_met', 'measured_speedup': candidate['speedup'],
                            'baseline_interval_ns': interval, 'candidate_interval_ns': candidate['interval_ns']}
        else:
            candidate['frame_target_eligible'] = False
    reference = comparison(profile.get('reference_baseline'), baseline, identity, False)
    if reference['status'] == 'available':
        if not correctness(baseline.get('correctness')):
            reference = {'status': 'rejected', 'reasons': ['current baseline lacks correctness PASS evidence']}
        else:
            reference['engine'] = profile['reference_baseline'].get('engine', 'external')
            reference['relative_time_ratio_to_current'] = reference['interval_ns'] / interval
            reference['same_hardware'] = reference['hardware_identity'] == baseline.get('hardware_identity')
    return {'schema_version': 1, 'classification': 'measured_budget_analysis', 'workload': name,
            'scope': scope, 'timing_domain': domain, 'measurement_source': baseline['source'],
            'target_speedup': target, 'target_speed_increase_percent': 100 * (target - 1),
            'minimum_selected_fraction_at_infinite_speedup': float(1 - 1 / rational(target)),
            'selected_components': sorted(chosen), 'selected_amdahl_bound': theory,
            'components': rows, 'serial_model': model, 'workload_identity': identity,
            'measured_candidate': candidate, 'target_verification': verification,
            'reference_baseline': reference, 'benchmark_metadata': list(metadata), 'limitations': list(LIMITATIONS)}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def load_json(content):
    def invalid_constant(value):
        raise ValueError('non-finite JSON number: ' + value)
    return json.loads(content, object_pairs_hook=unique_object, parse_constant=invalid_constant)


def benchmark_metadata(path):
    raw = path.read_bytes()
    report = load_json(raw)
    if not isinstance(report, dict):
        raise ValueError('benchmark report must be an object')
    rows = report.get('measurements', [])
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise ValueError('benchmark measurements must be objects')
    summaries = report.get('summaries', [])
    if not isinstance(summaries, list) or any(not isinstance(row, dict) for row in summaries):
        raise ValueError('benchmark summaries must be objects')
    exact = report.get('status') == 'complete' and bool(rows) and all(row.get('exact') is True for row in rows)
    return {'path': str(path), 'sha256': hashlib.sha256(raw).hexdigest(), 'metadata_only': True,
            'status': report.get('status', 'unknown'), 'kind': report.get('kind'), 'rounds': report.get('rounds'),
            'exact_records': exact, 'measurement_count': len(rows),
            'workloads': [row.get('workload') for row in summaries],
            'scope': 'synthetic component report; no automatic frequency, frame or reference-baseline inference'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True, help='measured JSON profile; - reads standard input')
    parser.add_argument('--target-speedup', type=float, default=7, help='7 means a 600%% speed increase')
    parser.add_argument('--select', action='append', help='component selected for the Amdahl bound; repeatable')
    parser.add_argument('--bench-report', type=Path, action='append', default=[], help='paired benchmark JSON, metadata only')
    parser.add_argument('--out', type=Path, help='new JSON report; default standard output')
    args = parser.parse_args(argv)
    try:
        content = sys.stdin.read() if str(args.profile) == '-' else args.profile.read_text(encoding='utf-8')
        directory = Path.cwd() if str(args.profile) == '-' else args.profile.resolve().parent
        report = analyze(load_json(content), args.target_speedup, args.select, directory,
                         [benchmark_metadata(path) for path in args.bench_report])
        rendered = json.dumps(report, indent=2, allow_nan=False) + '\n'
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            with args.out.open('x', encoding='utf-8') as destination:
                destination.write(rendered)
        else:
            print(rendered, end='')
        return report
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
        parser.exit(2, f'GPU budget analysis failed: {error}\n')


if __name__ == '__main__':
    main()
