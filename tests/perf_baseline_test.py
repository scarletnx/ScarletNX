#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check that scripts/perf-baseline.py lowers a reference only on a confirmed improvement."""

import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('perf_baseline', ROOT / 'scripts' / 'perf-baseline.py')
perf_baseline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(perf_baseline)

BASELINE = """# SPDX-License-Identifier: GPL-3.0-or-later
# a comment the updater leaves alone
cpu_x86_64   1.50 30 60 3.00
cpu_aarch64  0.87 30 60 2.50
gpu_hardware 1.30 25 50 3.00
"""


def main():
    # upper below the reference: the reference follows the new median
    text, changes = perf_baseline.update_text(
        BASELINE,
        {'cpu_x86_64': (1.10, 1.05, 1.15, 31), 'cpu_aarch64': (1.40, 1.30, 1.55, 31),
         'gpu_hardware': (1.50, 1.40, 1.60, 8)},
    )
    assert 'cpu_x86_64 1.1000 30 60 3.00' in text, text
    assert 'cpu_aarch64  0.87 30 60 2.50' in text, text
    assert 'gpu_hardware 1.30 25 50 3.00' in text, text
    assert '# a comment the updater leaves alone' in text
    assert [(key, round(old, 2), round(new, 2)) for key, old, new, _ in changes] == [('cpu_x86_64', 1.5, 1.1)]

    # an interval that straddles the reference, or a run with no samples, changes nothing
    text, changes = perf_baseline.update_text(
        BASELINE, {'cpu_x86_64': (1.40, 1.35, 1.55, 31), 'cpu_aarch64': (1.0, 0.9, 1.1, 0)}
    )
    assert text == BASELINE and not changes

    # a locked line is a reviewed trade-off and never moves
    locked = BASELINE.replace('cpu_x86_64   1.50 30 60 3.00', 'cpu_x86_64   1.50 30 60 3.00 locked')
    text, changes = perf_baseline.update_text(locked, {'cpu_x86_64': (1.10, 1.05, 1.15, 31)})
    assert text == locked and not changes

    # a malformed baseline is an error, not a silent pass
    try:
        perf_baseline.update_text('cpu_x86_64 1.0 30\n', {})
    except ValueError:
        pass
    else:
        raise AssertionError('a short baseline line was accepted')

    # the whole tool, including one missing result file and the output file
    with tempfile.TemporaryDirectory(prefix='snx-perf-baseline-') as temporary:
        folder = Path(temporary)
        baseline, result, out = folder / 'baseline.txt', folder / 'result.txt', folder / 'out.txt'
        baseline.write_text(BASELINE, encoding='utf-8')
        result.write_text('cpu_x86_64 1.10 1.05 1.15 31\n', encoding='utf-8')
        status = perf_baseline.main(['--baseline', str(baseline), '--result', str(folder / 'missing.txt'),
                                     '--result', str(result), '--out', str(out)])
        assert status == 0, status
        assert 'cpu_x86_64 1.1000 30 60 3.00' in out.read_text(encoding='utf-8')

    print('perf_baseline_tool: references ratchet down on a confirmed improvement and never rise')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
