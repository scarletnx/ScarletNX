#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A 3DS game's update and DLC kept as a Switch game's are (scarletnx-3ds): each recorded beside the game in its folder
(<title ID>_<its entry>.ini naming its file, and its data folder) and checked in the game's addons=, given with the
game (--update) or alone once the game is made, then read when the game plays: the update's code runs. Another game's
is refused, a second update takes the first one's place, one whose file is gone is said and left out, and a game
played from its extraction says it plays without them."""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from ctr_fixtures import DLC_TITLE, TITLE, UPDATE_TITLE, cia, dlc_cia, dlc_content, ncch, put, titled


def exiting(value):
    """the game's code: MOV r0,#value; SVC #3 (ExitProcess), as ncch()'s, laid out as its ExHeader says"""
    code = bytearray(0x3000)
    put(code, 0, '<II', 0xE3A00000 | value, 0xEF000003)
    code[0x1000:0x1010] = b'READONLY-DATA!!!'
    code[0x2000:0x2004] = b'DATA'
    return bytes(code)


def update_cia(title=UPDATE_TITLE, version=1040, value=43):
    header, exheader = titled(title)
    return cia(
        application=ncch(patch_header=header, patch_exheader=exheader, code_blob=exiting(value)),
        title=title,
        patch_body=lambda body: put(body, 0x9C, '>H', version),
    )


def main():
    # absolute, not resolved: scarletnx-3ds may be a link to scarletnx, which runs the driver its name says
    driver = Path(sys.argv[1]).absolute()
    with tempfile.TemporaryDirectory(prefix='snx-3ds-addons-') as temp:
        root = Path(temp)
        env = {**os.environ, 'HOME': str(root), 'USERPROFILE': str(root)}
        data, games = root / 'data', root / 'recompiled'
        folder = games / f'{TITLE:016X}'
        ini = folder / f'{TITLE:016X}.ini'

        def run(*args, status=0):
            p = subprocess.run(
                [str(driver), '--data', str(data), *args],
                capture_output=True,
                text=True,
                cwd=root,
                env=env,
                timeout=120,
            )
            text = p.stdout + p.stderr
            assert p.returncode == status, (args, p.returncode, text)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
            return text

        def checked():
            return re.findall(r'^addons=(.*)$', ini.read_text(), re.M)[-1].split(',')

        def write(name, blob):
            (root / name).write_bytes(blob)
            return str(root / name)

        game = write('game.cia', cia())
        update = write('update.cia', update_cia())
        dlc = write('dlc.cia', dlc_cia([(0, dlc_content(file_name='first.txt'))]))
        text = run('--outdir', str(games), game)
        assert 'Guest ExitProcess: r0=42' in text and 'Playing with' not in text, text

        # an update alone: recorded beside its game, which plays with its code
        entry = f'{UPDATE_TITLE:016X}v1040'
        text = run('--outdir', str(games), update)
        assert f'Recorded the update {entry} beside its game in {folder}, checked in its addons=' in text, text
        assert f'Playing with the update {entry}' in text and 'Guest ExitProcess: r0=43' in text, text
        record = folder / f'{TITLE:016X}_{entry}.ini'
        assert f'source={update}' in record.read_text() and (folder / f'{TITLE:016X}_{entry}_data').is_dir()
        assert checked() == [entry]

        # a DLC alone: recorded and checked beside the update
        text = run('--outdir', str(games), dlc)
        assert f'Recorded the DLC {DLC_TITLE:016X} beside its game' in text, text
        assert f'Playing with the DLC {DLC_TITLE:016X}' in text and 'r0=43' in text, text
        assert checked() == [entry, f'{DLC_TITLE:016X}']

        # a newer update takes the older one's place, the DLC kept
        newer = write('newer.cia', update_cia(version=2064, value=44))
        text = run('--outdir', str(games), newer)
        assert 'r0=44' in text and checked() == [f'{UPDATE_TITLE:016X}v2064', f'{DLC_TITLE:016X}'], text

        # another game's update with this game, and another game's DLC alone: refused, nothing recorded
        stranger = 0x0004000E00999900
        other = write('other-update.cia', update_cia(stranger))
        text = run('--outdir', str(games), '--update', other, game, status=1)
        assert f'is not an update or a DLC of {game} ({TITLE:016X}): its title is {stranger:016X}' in text, text
        other_dlc = 0x0004008C00999900
        orphan = write('other-dlc.cia', dlc_cia([(0, dlc_content(other_dlc))], title=other_dlc))
        text = run('--outdir', str(games), orphan, status=1)
        assert f'is the DLC {other_dlc:016X} of the game 0004000000999900, which is not made in' in text, text
        assert checked() == [f'{UPDATE_TITLE:016X}v2064', f'{DLC_TITLE:016X}']
        assert not list(games.glob('0004000000999900*')) and not list(folder.glob('*00999900*'))

        # the game's folder played as its copy of the runtime plays it: a DLC whose file is gone is said, with the path
        os.remove(dlc)
        text = run(f'--game={folder}')
        assert (
            f'the DLC {DLC_TITLE:016X} checked in addons= is not at {dlc}: this run plays without it' in text
            and 'r0=44' in text
        ), text

        # both given with the game, in a folder of their own
        again = root / 'again'
        os.makedirs(again)
        dlc = write('dlc.cia', dlc_cia([(0, dlc_content(file_name='first.txt'))]))
        text = run('--outdir', str(again), '--update', update, '--update', dlc, game)
        assert 'Recorded the update' in text and 'Recorded the DLC' in text and 'r0=43' in text, text
        # an update takes no --update of its own
        text = run('--outdir', str(again), '--update', dlc, update, status=2)
        assert 'it takes no --update, --output, -x or --extract-container' in text, text

        # a game played from its extraction reads none of them, and says so
        text = run('--outdir', str(games), '--extract-container', game)
        assert f'which reads no update or DLC: this run plays without {UPDATE_TITLE:016X}v2064' in text, text
        assert 'r0=42' in text, text
    print('3DS updates and DLC recorded beside their game, checked in addons= and played with it')


if __name__ == '__main__':
    main()
