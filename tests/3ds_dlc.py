#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A game's DLC laid beside it: the NCCH archive and am:app serve it, another game's, a game given as a DLC and a DLC
given as the game are refused."""

import subprocess
import sys
import tempfile
from pathlib import Path
from ctr_fixtures import DLC_TITLE, cia, dlc_cia, dlc_content


def main():
    probe = str(Path(sys.argv[1]).absolute())
    with tempfile.TemporaryDirectory(prefix='snx-ctr-dlc-') as temp:
        root = Path(temp)
        game, dlc, other = root / 'game.cia', root / 'dlc.cia', root / 'other-dlc.cia'
        game.write_bytes(cia())
        dlc.write_bytes(dlc_cia([(0, dlc_content(file_name='first.txt')), (3, dlc_content(file_name='second.txt'))]))
        stranger = (DLC_TITLE & ~0xFFFFFF00) | 0x00999900
        other.write_bytes(dlc_cia([(0, dlc_content(stranger))], title=stranger))
        p = subprocess.run([probe, str(game), str(dlc), str(other)], capture_output=True, text=True, timeout=30)
        text = p.stdout + p.stderr
        assert p.returncode == 0 and 'am:app' in p.stdout, text
        assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
        # a DLC given as the game is refused, pointing to --dlc
        q = subprocess.run([probe, str(dlc), str(dlc), str(other)], capture_output=True, text=True, timeout=30)
        assert q.returncode != 0
    print('DLC served by the NCCH archive and am:app, and refused when it is not the game\'s')


if __name__ == '__main__':
    main()
