#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A 3DS run's unattended save states from its command line, the Switch's state_save and state_load (at:slot): one run
saves slot 1 at 100 ms, the next loads it, and an item of no at:slot is said."""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from ctr_fixtures import TITLE, cia, ncch


def main():
    # absolute, not resolved: scarletnx-3ds may be a link to scarletnx, which runs the driver its name says
    driver = Path(sys.argv[1]).absolute()
    with tempfile.TemporaryDirectory(prefix='snx-3ds-states-') as temp:
        root = Path(temp).resolve()
        env = {**os.environ, 'HOME': str(root), 'USERPROFILE': str(root)}
        games = root / 'recompiled'
        folder = games / f'{TITLE:016X}'

        def run(*args):
            p = subprocess.run(
                [str(driver), '--data', str(root / 'data'), '--headless', '--seconds=1', *args],
                capture_output=True,
                text=True,
                cwd=root,
                env=env,
                timeout=300,
            )
            text = p.stdout + p.stderr
            if p.returncode:
                sys.exit(f'3ds_states: {args} exited with {p.returncode}\n{text}')
            return text

        code = bytearray(0x3000)
        code[0:4] = (0xEAFFFFFE).to_bytes(4, 'little')  # B .: the game runs until the run's second ends
        game = root / 'game.cia'
        game.write_bytes(cia(application=ncch(code_blob=bytes(code))))
        text = run('--outdir', str(games), str(game), '--state-save=100:1')
        state = folder / 'states' / f'{TITLE:016X}.1.snxstate'
        if not state.is_file() or not re.search(r'^Saved the state .*\.1\.snxstate in ', text, re.M):
            sys.exit(f'3ds_states: --state-save=100:1 made no slot 1 in {state.parent}\n{text}')
        text = run(f'--game={folder}', '--state-load=0:1')
        if not re.search(r'^Loaded the state .*\.1\.snxstate in ', text, re.M):
            sys.exit(f'3ds_states: --state-load=0:1 did not load slot 1\n{text}')
        text = run(f'--game={folder}', '--state-save=junk')
        if 'state_save=junk: junk is not at:slot or at:path' not in text:
            sys.exit(f'3ds_states: an item of no at:slot was not said\n{text}')
    print('3ds_states: a run saves and loads its states from its command line, and says what it could not')


if __name__ == '__main__':
    main()
