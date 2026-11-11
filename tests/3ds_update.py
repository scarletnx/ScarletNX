#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A standalone update laid over its game: the update's ExHeader and code are read, the game's RomFS stays the base's
and the update's is served apart (SelfNCCH's path type 5, which an updated game opens at boot), a mismatched update,
an update given as the game and a game given as the update are refused, and a game with no update reads as before."""

import subprocess
import sys
import tempfile
from pathlib import Path
from ctr_fixtures import TITLE, UPDATE_TITLE, cia, ncch, put, titled


def update_ncch(tid=UPDATE_TITLE):
    header, exheader = titled(tid)

    def named(d):
        exheader(d)
        d[:8] = b'PATCHED\0'

    return ncch(patch_header=header, patch_exheader=named, romfs_options={'file_name': 'patched.txt'})


def main():
    probe = str(Path(sys.argv[1]).absolute())
    with tempfile.TemporaryDirectory(prefix='snx-ctr-update-') as temp:
        root = Path(temp)

        def run(game, update, name):
            g, u, state = root / f'{name}-game.cia', root / f'{name}-update', root / f'{name}-state'
            g.write_bytes(game)
            u.write_bytes(update)
            state.mkdir()
            p = subprocess.run(
                [probe, str(g), '--update', str(u), str(state)], capture_output=True, text=True, timeout=20
            )
            text = p.stdout + p.stderr
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
            return p.returncode, p.stdout

        game = cia()
        code, out = run(game, cia(application=update_ncch(), title=UPDATE_TITLE), 'cia')
        assert code == 0, out
        # the update's ExHeader, the game's title, the base's icon kept, each RomFS read apart
        assert (
            f'title {TITLE:016x} name PATCHED exheader {UPDATE_TITLE:016x} romfs base update-romfs update icon 1' in out
        ), out
        assert 'self romfs base update-romfs update' in out, out

        # a console's update: the NCCH under the update's ID, its ExHeader naming the game's program
        def console_update():
            header, _ = titled(UPDATE_TITLE)
            return ncch(patch_header=header, romfs_options={'file_name': 'patched.txt'})

        code, out = run(game, cia(application=console_update(), title=UPDATE_TITLE), 'console')
        assert code == 0 and f'exheader {TITLE:016x} romfs base update-romfs update' in out, out
        code, out = run(game, update_ncch(), 'bare')
        assert code == 0 and 'name PATCHED' in out, out
        wrong = 0x0004000E00099900
        code, out = run(game, cia(application=update_ncch(wrong), title=wrong), 'other-game')
        assert code != 0 and f"is the update {wrong:016X}, not this game's ({UPDATE_TITLE:016X})" in out, out
        code, out = run(game, cia(), 'not-update')
        assert code != 0 and 'not an update' in out, out
        code, out = run(cia(application=update_ncch(), title=UPDATE_TITLE), update_ncch(), 'update-as-game')
        assert code != 0 and 'is an update, not a game' in out, out
    print("update laid over its game: read, and refused when it is not the game's")


if __name__ == '__main__':
    main()
