# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""3DS CLI contract; no firmware, keys or game assets required."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    # absolute, not resolved: scarletnx-3ds may be a link to scarletnx, which runs the driver its name says
    driver = pathlib.Path(sys.argv[1]).absolute()

    def run(args, status, contains, cwd=None, env=None):
        environment = {**os.environ, **env} if env else None
        p = subprocess.run([str(driver), *args], capture_output=True, text=True, cwd=cwd, env=environment, timeout=10)
        output = p.stdout + p.stderr
        assert p.returncode == status, (args, p.returncode, output)
        assert contains in output, (args, output)
        assert 'ScarletNX - ' not in output, output  # no false launch/completion banner
        return output

    help_text = run(['--help'], 0, 'recompiled if needed,\n                  then played')
    assert '--recomp' in help_text and '--extract' in help_text and 'scarletnx-3ds-aot' not in help_text
    assert '--extract-container' in help_text and 'read where it lies' in help_text
    run(['--board-info', '--extract-container'], 2, 'Usage:')
    run(['-x', '-r', 'game.cia'], 2, 'Usage:')
    run(['-r', '--max-blocks', '5', 'game.cia'], 2, 'Usage:')
    run(['--board-info', '-r'], 2, 'Usage:')
    run(['--output'], 2, 'requires a directory')
    run(['--repo'], 2, 'requires a directory')
    run(['--board-info', '--repo', '.'], 0, 'Old 3DS')  # --repo is a global data-directory option
    run(['--board-info', '-x'], 2, 'Usage:')
    run([], 2, 'Usage:')
    run(['--model'], 2, 'requires old or new')
    run(['--model', 'other', '--board-info'], 2, 'requires old or new')
    run(['--board-info'], 0, 'Old 3DS\nARM11 cores: 2\nBase clock: 268111856 Hz\nFCRAM: 128 MiB')
    run(['--model', 'new', '--board-info'], 0, 'New 3DS\nARM11 cores: 4')
    run(['--board-info', 'game.cia'], 2, 'Usage:')
    run(['--backend=gcc'], 2, 'unknown setting: --backend=gcc')  # --key=value is the 3DS debug table's
    # an older version's packet trace (docs/DEPRECATIONS.md): still ctr_uds_trace=1, under the command line's own
    old = 'SNX_3DS_UDS_PACKET_TRACE'
    run(
        ['--board-info'],
        0,
        f'{old}=1 is deprecated, and this run reads it as --ctr_uds_trace=1: ctr_uds_trace=1',
        env={old: '1'},
    )
    run(
        ['--board-info', '--ctr_uds_trace=0'],
        0,
        'the command line gives ctr_uds_trace first: ctr_uds_trace=0',
        env={old: '1'},
    )
    run(['--board-info'], 0, f'{old}=yes is deprecated, and only 1 did anything: ctr_uds_trace=0', env={old: 'yes'})
    with tempfile.TemporaryDirectory(prefix='snx-3ds-') as temp:
        root = pathlib.Path(temp)
        for name in ['game with spaces;literal.CIA', 'game.nsp', '--game.cia', 'game.cci']:
            (root / name).write_bytes(b'not a real CIA')
        run([str(root / 'game with spaces;literal.CIA')], 1, 'archive section')
        run(['--model', 'new', '--', '--game.cia'], 1, 'archive section', cwd=root)
        run([str(root / 'game.cci')], 1, 'truncated or out-of-range archive section')
        run([str(root / 'game.nsp')], 2, f'{root / "game.nsp"} is not a game of this system')
        run([str(root / 'missing.cia')], 2, f'{root / "missing.cia"} does not exist')
        run([str(root)], 2, f'{root} is not a game of this system')
        (root / 'folder.cia').mkdir()
        run([str(root / 'folder.cia')], 2, f'{root / "folder.cia"} is not a file')
        (root / 'folder.cia').rmdir()
        if sys.platform != 'win32':
            # a working folder that is gone: the extraction folder it cannot resolve, with the system's reason
            gone = root / 'gone'
            gone.mkdir()

            def leave():
                os.chdir(gone)
                os.rmdir(gone)

            p = subprocess.run(
                [str(driver), '--repo', 'data', str(root / 'game with spaces;literal.CIA')],
                capture_output=True,
                text=True,
                preexec_fn=leave,
                timeout=10,
            )
            assert p.returncode == 1 and 'scarletnx-3ds: cannot resolve data/work/3ds: ' in p.stderr, p.stderr
        run([str(root / 'game.nsp'), 'second.cia'], 2, 'unexpected argument')
        run(['-x', '--output', 'unsafe\noutput', str(root / '--game.cia')], 2, 'control characters')
        if sys.platform != 'win32':
            banner_path = root / 'line\nScarletNX - fake.CIA'
            banner_path.write_bytes(b'not a CIA')
            run([str(banner_path)], 2, 'control characters')
            banner_path.unlink()
        assert sorted(p.name for p in root.iterdir()) == ['--game.cia', 'game with spaces;literal.CIA', 'game.cci', 'game.nsp']
    print('3DS driver CLI, malformed-input diagnostics and no output side effects passed')


if __name__ == '__main__':
    main()
