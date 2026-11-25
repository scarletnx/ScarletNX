#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The console a 3DS game runs on (scarletnx-3ds): --model over the command line's ctr_model, over the game's own
Settings (ctr_model in its picks, <title ID>.picks beside its .ini), over the launcher's for every game (ctr_model given
with --launcher-picks=1), over the one its .ini records. A game with no pick of its own, as every game made before there
were any, inherits; a value that is none is said with the one it keeps."""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from ctr_fixtures import TITLE, cia, ncch

# the memory the application has on each console, which its run says (run.c)
OLD, NEW = 'Memory: 64 MiB', 'Memory: 124 MiB'


def new3ds_mode(exheader):
    """the New 3DS's 124 MiB memory mode, which a title that runs on both asks for (ExHeader 0x20D)"""
    exheader[0x20D] = 1


def main():
    # absolute, not resolved: scarletnx-3ds may be a link to scarletnx, which runs the driver its name says
    driver = Path(sys.argv[1]).absolute()
    with tempfile.TemporaryDirectory(prefix='snx-3ds-console-') as temp:
        # resolved, as the driver names its paths: macOS's temporary folder is under /var, a link to /private/var
        root = Path(temp).resolve()
        env = {**os.environ, 'HOME': str(root), 'USERPROFILE': str(root)}
        data, games = root / 'data', root / 'recompiled'
        folder = games / f'{TITLE:016X}'
        ini, picks = folder / f'{TITLE:016X}.ini', folder / f'{TITLE:016X}.picks'

        def run(*args, status=0):
            p = subprocess.run(
                [str(driver), '--data', str(data), '--headless', *args],
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

        def played(console, *args, own=None, said=()):
            """the game's folder played: its console, whether the line says it is the game's own, its settings lines"""
            text = run(f'--game={folder}', *args)
            assert console in text, (args, text)
            mine = re.findall(
                r"^Played on (an Old|a New) 3DS: the game's own Settings \(ctr_model=(\w+) in (.*)\)$", text, re.M
            )
            assert mine == ([(own[0], own[1], str(picks))] if own else []), (args, mine, text)
            lines = [line for line in text.splitlines() if line.startswith('settings: ')]
            assert lines == list(said), (args, lines, text)

        def recorded():
            return re.findall(r'^model=(.*)$', ini.read_text(), re.M)

        def pick(text):
            picks.write_text(text)

        game = str(root / 'game.cia')
        Path(game).write_bytes(cia(application=ncch(patch_exheader=new3ds_mode)))
        text = run('--outdir', str(games), game)
        assert 'Guest ExitProcess: r0=42' in text and OLD in text and recorded() == ['old'], text

        # a game made before there were picks: its .ini alone, with the keys an older version wrote; it inherits the
        # launcher's, then its own record
        ini.write_text(f'source={game}\nmodel=new\n')
        assert not picks.exists()
        played(NEW)
        played(OLD, '--ctr-model=old', '--launcher-picks=1')

        # its own pick, the last line's, over the launcher's and its record; under the command line's, auto there too
        ini.write_text(f'source={game}\nmodel=old\nsystem=3ds\n')
        pick('; its own\n[3DS]\nctr_model=old\nctr_model=new\n')
        played(NEW, own=('a New', 'new'))
        played(NEW, '--ctr-model=old', '--launcher-picks=1', own=('a New', 'new'))
        played(OLD, '--model', 'old', '--ctr-model=new', '--launcher-picks=1')
        played(OLD, '--ctr-model=old')
        played(OLD, '--ctr-model=auto', '--launcher-picks=0')

        # its picks' other keys said; auto there is no pick, nor a picks file without the key
        ini.write_text(f'source={game}\nmodel=new\nsystem=3ds\n')
        pick('[3DS]\nctr_model=old\n[Other]\nunknown=1\n')
        other = (
            f"settings: unknown=1 in {picks} is no setting a 3DS game's picks take: it is kept there and does nothing"
        )
        played(OLD, '--ctr-model=new', '--launcher-picks=1', own=('an Old', 'old'), said=[other])
        pick('[3DS]\nctr_model=auto\n')
        played(NEW)
        played(OLD, '--ctr-model=old', '--launcher-picks=1')
        pick('[3DS]\n')
        played(OLD, '--ctr-model=old', '--launcher-picks=1')

        # a value that is none, an empty one too: said with the one it keeps, the launcher's or auto's, which is the record
        for value, kept, console, args in (
            ('newer', 'auto', NEW, ()),
            ('newer', 'old', OLD, ('--ctr-model=old', '--launcher-picks=1')),
            ('New', 'new', NEW, ('--ctr-model=new', '--launcher-picks=1')),
            ('', 'auto', NEW, ()),
        ):
            pick(f'[3DS]\nctr_model={value}\n')
            said = f'settings: ctr_model={value} in {picks} is not auto, old or new: it keeps {kept}'
            played(console, *args, said=[said])

        # picks that cannot be read (a folder there): said with what it keeps
        picks.unlink()
        picks.mkdir()
        text = run(f'--game={folder}', '--ctr-model=old', '--launcher-picks=1')
        assert OLD in text and re.search(
            rf'^settings: {re.escape(str(picks))} could not be read \(.+\): ctr_model keeps old$', text, re.M
        ), text
        picks.rmdir()

        # the game's file played again: its own pick, which its .ini's record is not
        ini.write_text(f'source={game}\nmodel=old\nsystem=3ds\n')
        pick('[3DS]\nctr_model=new\n')
        text = run('--outdir', str(games), game)
        assert NEW in text and "the game's own Settings" in text and recorded() == ['old'], text
        # a recompile plays it on none, and records the console it has without its own
        text = run('--outdir', str(games), '-r', game)
        assert 'Played on' not in text and 'Memory:' not in text and recorded() == ['old'], text
        picks.unlink()
        text = run('--outdir', str(games), game)
        assert OLD in text and "the game's own Settings" not in text and recorded() == ['old'], text
        text = run('--outdir', str(games), '--model', 'new', game)
        assert NEW in text and recorded() == ['new'], text

        # the launcher's mark is 0 or 1
        text = run('--launcher-picks=2', '--board-info', status=2)
        assert 'scarletnx-3ds: --launcher-picks=2 is not 0 or 1' in text, text
        assert 'New 3DS' in run('--ctr-model=new', '--launcher-picks=1', '--board-info')
    print("a 3DS game's console: the command line's, its own, the launcher's, its record's")


if __name__ == '__main__':
    main()
