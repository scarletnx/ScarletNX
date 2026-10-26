#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Ratchet a performance baseline's references from measured ratios.

A result file is one line per key, written by snx-test-perf-cpu or snx-test-perf-gpu:

    key median lower upper count

The reference for a key moves down only when the whole 95% interval of a run is below it, so a noisy or slightly
worse run leaves the line alone and an improvement tightens it. No code path raises a reference; an intended
trade-off is a reviewed edit to the baseline.
"""

import argparse
import sys
from pathlib import Path


def read_results(paths):
    results = {}
    for path in paths:
        file = Path(path)
        if not file.is_file():
            continue
        for number, line in enumerate(file.read_text(encoding='utf-8').splitlines(), 1):
            parts = line.split()
            if not parts:
                continue
            if len(parts) != 5:
                raise ValueError(f'{path}:{number}: expected key median lower upper count')
            try:
                results[parts[0]] = (float(parts[1]), float(parts[2]), float(parts[3]), int(parts[4]))
            except ValueError as error:
                raise ValueError(f'{path}:{number}: {error}') from error
    return results


def update_text(text, results):
    """Return the baseline text with the references that this run improved, and the changes made."""
    lines, changes = [], []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            lines.append(line)
            continue
        parts = line.split()
        if len(parts) not in (5, 6):
            raise ValueError(f'baseline line has {len(parts)} fields, not 5 or 6: {line!r}')
        key = parts[0]
        reference = float(parts[1])
        if len(parts) == 6:  # `locked`: a reviewed trade-off that the ratchet leaves alone
            if parts[5] != 'locked':
                raise ValueError(f'baseline line has an unknown sixth field: {parts[5]!r}')
            lines.append(line)
            continue
        measured = results.get(key)
        if measured and measured[3] > 0 and measured[2] < reference:
            median = measured[0]
            if median > 0:
                lines.append(f'{key} {median:.4f} {parts[2]} {parts[3]} {parts[4]}')
                changes.append((key, reference, median, measured[2]))
                continue
        lines.append(line)
    return '\n'.join(lines) + '\n', changes


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--result', action='append', default=[], type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args(argv)
    try:
        results = read_results(args.result)
        text, changes = update_text(args.baseline.read_text(encoding='utf-8'), results)
    except (OSError, ValueError) as error:
        print(f'perf-baseline: {error}', file=sys.stderr)
        return 1
    for key, old, new, upper in changes:
        print(f'perf-baseline: {key}: {old:.4f} -> {new:.4f} (upper bound {upper:.4f})')
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding='utf-8')
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
