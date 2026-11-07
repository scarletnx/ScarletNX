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
    assert '--ctr_separate_windows=1' in help_text and '--ctr_top_resolution=2' in help_text
    assert '--vk_memory=N' in help_text
    assert '--async_pipelines=1' in help_text and '--pipeline_threads=N' in help_text
    assert '--gamepad_swap_ab=1' in help_text and '--gamepad_deadzone=N' in help_text
    assert '--gamepad_map=MAP' in help_text and '--touch=N' in help_text
    assert '--vsync=0' in help_text and '--fullscreen=1' in help_text and '--anisotropy=N' in help_text
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
    run(['--board-info', '--ctr_separate_windows=1', '--ctr_top_resolution=2', '--ctr_bottom_resolution=2'], 0,
        'Old 3DS\nARM11 cores: 2')
    run(['--model', 'new', '--board-info'], 0, 'New 3DS\nARM11 cores: 4')
    run(['--board-info', 'game.cia'], 2, 'Usage:')
    run(['--backend=gcc'], 2, 'unknown setting: --backend=gcc')  # --key=value is the 3DS tables'
    # and the Switch's the 3DS takes too (vk_memory), never another of the Switch's Debug table
    run(['--board-info', '--vk_memory=1'], 0, 'Old 3DS')
    run(['--board-info', '--async_pipelines=1', '--pipeline_threads=2'], 0, 'Old 3DS')
    run(['--board-info', '--vk_budget_mb=64'], 2, 'unknown setting: --vk_budget_mb=64')
    # the Controllers table's A and B and dead zone, read as the Switch's (a dead zone past 90 is 90, said), never
    # its vibration, which the 3DS has none of
    run(['--board-info', '--gamepad_swap_ab=1', '--gamepad_deadzone=30'], 0, 'Old 3DS')
    run(
        ['--board-info', '--gamepad_deadzone=95'],
        0,
        'settings: gamepad_deadzone=95 is not a whole number from 0 to 90: it keeps 90',
    )
    run(['--board-info', '--gamepad_rumble=50'], 2, 'unknown setting: --gamepad_rumble=50')
    # and its button map, which the 3DS input reads itself: a map it cannot read keeps the one before, a control sent
    # to a button the 3DS lacks sends nothing, each said with the value
    out = run(['--board-info', '--gamepad_map=south:A east:B'], 0, 'Old 3DS')
    assert 'gamepad_map' not in out, out
    run(
        ['--board-info', '--gamepad_map=south:A', '--gamepad_map=south:Q'],
        0,
        'settings: gamepad_map\'s "south:Q" is not a control and the button it sends (south:B): it keeps "south:A"',
    )
    run(
        ['--board-info', '--gamepad_map=east:Q'],
        0,
        'it keeps "", each control sending the button in its place',
    )
    run(['--board-info', '--gamepad_map=ls:L3'], 0, "settings: gamepad_map's ls:L3 sends nothing: the 3DS has no L3")
    out = run(['--board-info', '--gamepad_map=lb:ZL rb:ZR'], 0, 'Old 3DS')  # ir:rst's, which the runtime has
    assert 'sends nothing' not in out, out
    # the On-screen controls table's touch screen, as the Switch's (0 to 2), never its on-screen controls
    run(['--board-info', '--touch=1'], 0, 'Old 3DS')
    run(['--board-info', '--touch=5'], 0, 'settings: touch=5 is not a whole number from 0 to 2: it keeps 2')
    run(['--board-info', '--controls_haptics=0'], 2, 'unknown setting: --controls_haptics=0')
    # the Graphics table's vsync, never another of its rows
    run(['--board-info', '--vsync=0', '--anisotropy=16'], 0, 'Old 3DS')
    run(['--board-info', '--shadows=0'], 2, 'unknown setting: --shadows=0')
    # and the Display table's fullscreen, never its others
    run(['--board-info', '--fullscreen=1'], 0, 'Old 3DS')
    run(['--board-info', '--docked=0'], 2, 'unknown setting: --docked=0')
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
