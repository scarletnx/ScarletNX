# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The user's data folder's files in the XDG Base Directory folders, a build tree's driver's as an installed one's:
the game's settings in the config folder, its log and threads.txt in the state folder, the pipeline cache and the
extractions in the cache folder, each game in a folder of its own there. What an older build left beside the game
stays there, each run saying where it is and where this one keeps it. A game recompiled elsewhere (--outdir) keeps
everything in its folder. A game started with another environment than its driver's (an AppImage's copy, without
its portable config or home) finds its files where the driver put them, which <name>.folders beside it names. No
GPU: the game stops before it, its ExeFS taken away."""

import argparse
import errno
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from nx_runtime_smoke import make_nro


def older_ini_kept(games):
    text = (games / 'Smoke.ini').read_text()
    return 'kept from the older build' in text and 'Game language' not in text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    driver = str(args.driver.resolve())
    failures = []

    with tempfile.TemporaryDirectory(prefix='scarletnx-xdg-') as temporary:
        root = Path(temporary)
        env = {k: v for k, v in os.environ.items() if k not in ('SNX_GAME', 'APPIMAGE', 'APPDIR')}
        homes = {kind: root / kind.lower() for kind in ('DATA', 'CONFIG', 'STATE', 'CACHE')}
        for kind, path in homes.items():
            env[f'XDG_{kind}_HOME'] = str(path)
        env['HOME'] = str(root / 'home')
        data, config, state, cache = (homes[kind] / 'scarletnx' for kind in ('DATA', 'CONFIG', 'STATE', 'CACHE'))
        games = data / 'recompiled'
        folder = games / 'Smoke'  # the game's own
        # code units, which the unit cache's checks count: an NCE recompile (nce=1, the default where the runtime runs
        # NCE) makes none
        aot = ['--nce=0']

        def run(command, cwd=root, environment=None):
            p = subprocess.run(
                command,
                check=False,
                cwd=cwd,
                env=environment or env,
                capture_output=True,
                text=True,
                timeout=args.timeout,
            )
            return p.returncode, p.stdout + p.stderr

        def check(condition, what, output=''):
            if not condition:
                failures.append(f'{what}\n' + ''.join(f'    {line}\n' for line in output.splitlines()[-30:]))

        # what an older build left beside the game and in work/
        (folder / 'shader_cache').mkdir(parents=True)
        (folder / 'Smoke.ini').write_text('[Display]\nlanguage=1\n; kept from the older build\n')
        (folder / 'Smoke.log').write_text('older log\n')
        (folder / 'threads.txt').write_text('older threads\n')
        (folder / 'shader_cache/vk_pipelines.bin').write_bytes(b'pipelines')
        (data / 'work/Old/exefs').mkdir(parents=True)
        (data / 'work/Old/exefs/main').write_bytes(b'code')
        nro = root / 'smoke.nro'
        nro.write_bytes(make_nro())

        # recompiled into the data folder (--repo names it, as the menu does): the settings in the config folder
        status, out = run(
            [driver, '--repo', str(data), '-r', str(nro), 'Smoke', '--runtime', driver, '--jobs', '2', *aot]
        )
        check(status == 0 and (folder / 'Smoke').is_file(), 'the recompile did not put Smoke in recompiled/Smoke/', out)
        ini = config / 'recompiled/Smoke/Smoke.ini'
        older = 'kept from the older build'
        check(ini.is_file() and older not in ini.read_text(), f'the recompile did not write {ini}', out)
        check(older in (folder / 'Smoke.ini').read_text(), 'the older Smoke.ini was touched', out)
        told = f'{folder / "Smoke.ini"} was left by an older version: this one keeps it in {ini}'
        check(told in out, 'the recompile did not say where the older Smoke.ini is', out)
        # the code units' cache, units/ of the cache folder, which every game's recompile shares
        units = cache / 'units'
        kept = len(list(units.glob('*.o')))
        check(
            kept and f'unit cache: 0 of {kept} units reused (0.0 MB) from {units}' in out,
            f'no units kept in {units}',
            out,
        )

        # an older version's game of that name, its program a file where the folder goes: said, nothing touched
        (games / 'InTheWay').write_bytes(b'older program')
        status, out = run(
            [driver, '--repo', str(data), '-r', str(nro), 'InTheWay', '--runtime', driver, '--jobs', '2', *aot]
        )
        check(
            status != 0 and "is a file where this game's folder goes" in out,
            'the recompile did not say a file is where the folder goes',
            out,
        )
        check((games / 'InTheWay').read_bytes() == b'older program', "the older game's program was touched", out)

        # the build tree's own driver with no --repo works there too (#318), never in the checkout it was built from
        checkout = Path(__file__).resolve().parents[1]
        status, out = run([driver, '-r', str(nro), 'Tree', '--runtime', driver, '--jobs', '2', *aot])
        check(status == 0 and (games / 'Tree/Tree').is_file(), 'the driver without --repo did not use recompiled/', out)
        check(
            f'unit cache: {kept} of {kept} units reused' in out,
            f'another game of the same code did not reuse {units}',
            out,
        )
        check(not (checkout / 'games/Tree').exists(), 'the driver without --repo wrote in its checkout', out)

        # the game, started in its folder as ./Smoke, logs in the state folder and stops before the GPU without its
        # ExeFS. Each start says where the older files are, as long as they are there
        shutil.rmtree(folder / 'Smoke_data/exefs')
        log = state / 'recompiled/Smoke/Smoke.log'
        threads = state / 'recompiled/Smoke/threads.txt'
        pipelines = cache / 'recompiled/Smoke/shader_cache/vk_pipelines.bin'
        for start in ('first', 'second'):
            status, out = run(['./Smoke', '-H'], cwd=folder)
            text = log.read_text(errors='replace') if log.is_file() else ''
            check(
                status != 0 and 'ScarletNX - Smoke' in text and 'Could not load the game executables' in text,
                f'the game did not log to {log} and stop before the GPU (exit {status}, {start} start)',
                out,
            )
            check(
                'was left by an older version' not in text, f'the {start} start of a game in its folder said so', text
            )
        check((folder / 'Smoke.log').read_text() == 'older log\n', 'the game wrote in the older Smoke.log', out)
        # a .ini the game cannot read: said with its path and the reason, the defaults taken, the file not written over.
        # Root reads it all the same
        if hasattr(os, 'geteuid') and os.geteuid() != 0 and ini.is_file():  # its absence is said above
            before = ini.read_bytes()
            ini.chmod(0)
            try:
                status, out = run(['./Smoke', '-H'], cwd=folder)
            finally:
                ini.chmod(0o644)
            text = log.read_text(errors='replace') if log.is_file() else ''
            said = f'settings: cannot read {ini} ({os.strerror(errno.EACCES)}): every setting takes its default'
            check(said in text, f'the game did not say "{said}"', text)
            check(ini.read_bytes() == before, 'the game wrote over the .ini it could not read', out)
        check((folder / 'threads.txt').read_text() == 'older threads\n', 'the older threads.txt was touched', out)
        check(
            (folder / 'shader_cache/vk_pipelines.bin').read_bytes() == b'pipelines',
            'the older pipeline cache was touched',
        )
        check(older_ini_kept(folder), 'the game wrote its settings in its folder', text)
        check(
            all((folder / name).exists() for name in ('Smoke.abi', 'Smoke_code.so', 'Smoke_data')),
            'the game itself left its folder',
        )
        check(
            log.is_file() and threads.parent.is_dir() and not pipelines.exists(), 'the state folder was not used', text
        )

        # a game an older version laid out beside the others: it runs where it is, each start saying where its folder
        # goes (an NRO has no title ID: by its name)
        for name in ('Smoke', 'Smoke_code.so', 'Smoke.abi'):
            shutil.copy2(folder / name, games / name.replace('Smoke', 'Loose'))
        shutil.copytree(folder / 'Smoke_data', games / 'Loose_data')
        status, out = run(['./Loose', '-H'], cwd=games)
        told = f'{games}/Loose and its files were left by an older version: this one keeps them in {games}/Loose/'
        check(told in out, 'the loose game did not say where its folder goes', out)
        check((games / 'Loose').is_file() and not (games / 'Loose').is_dir(), 'the loose game was moved', out)

        # an extraction: the older build's work/ stays, and the driver says the cache folder's is used. hactool finds
        # no program in an empty PFS0
        (root / 'empty.nsp').write_bytes(b'PFS0' + bytes(12))
        status, out = run([driver, '--repo', str(data), '-x', str(root / 'empty.nsp')])
        check((data / 'work/Old/exefs/main').read_bytes() == b'code', 'the older work/ was touched', out)
        told = f'{data / "work"} was left by an older version: this one keeps it in {cache / "work"}'
        check(told in out, 'the extraction did not say where the older work/ is', out)

        # a folder of its own (a checkout's or a portable copy's games/, --outdir): everything beside the game
        beside = root / 'portable/games/Beside'
        status, out = run(
            [driver, '--repo', str(data), '-r', str(nro), 'Beside', '--runtime', driver, '--jobs', '2', *aot]
            + ['--outdir', str(beside.parent)]
        )
        check(status == 0 and (beside / 'Beside.ini').is_file(), 'the --outdir recompile did not keep Beside.ini', out)
        shutil.rmtree(beside / 'Beside_data/exefs')
        status, out = run(['./Beside', '-H'], cwd=beside)
        check((beside / 'Beside.log').is_file(), 'the game in a folder of its own did not log beside it', out)
        check(
            not (config / 'games/Beside').exists() and not (state / 'games/Beside').exists(),
            'the game in a folder of its own wrote in the XDG folders',
        )
        check(not (beside / 'Beside.folders').exists(), 'the game in a folder of its own got Beside.folders', out)
        # one a record names other folders for (its folder moved out of a data folder): the driver takes it away
        (beside / 'Beside.folders').write_text(f'config={config / "games/Beside"}\n')
        status, out = run([driver, '--repo', str(data), str(beside), '-H'])
        check(not (beside / 'Beside.folders').exists(), 'the driver kept a Beside.folders naming other folders', out)
        # a portable copy's .scarletnx: its config/, state/ and cache/, as off Linux
        portable = root / 'copy/.scarletnx'
        port = portable / 'games/Port'
        status, out = run(
            [driver, '--repo', str(portable), '-r', str(nro), 'Port', '--runtime', driver, '--jobs', '2', *aot]
            + ['--outdir', str(port.parent)]
        )
        check(
            status == 0 and (portable / 'config/games/Port/Port.ini').is_file() and not (port / 'Port.ini').exists(),
            "the portable copy's recompile did not keep Port.ini in its config/",
            out,
        )
        check(
            f'unit cache: 0 of {kept} units reused (0.0 MB) from {portable / "cache/units"}' in out
            and len(list((portable / 'cache/units').glob('*.o'))) == kept,
            "the portable copy's recompile did not keep its units in its cache/units",
            out,
        )
        shutil.rmtree(port / 'Port_data/exefs')
        status, out = run(['./Port', '-H'], cwd=port)
        check(
            (portable / 'state/games/Port/Port.log').is_file() and not (state / 'games/Port').exists(),
            "the portable copy's game did not log in its state/",
            out,
        )
        # its folders are in it, wherever it is moved: nothing to record
        check(not (port / 'Port.folders').exists(), "the portable copy's game got Port.folders", out)

        # a game made by an AppImage's driver with --appimage-portable-config (XDG_CONFIG_HOME its .config) or
        # --appimage-portable-home (HOME its .home, the XDG variables unset), and started outside it with the
        # environment the AppImage was given: its settings, the dump they name, its log and caches where the driver
        # put them, as <name>.folders beside it records
        appimage = root / 'ScarletNX.AppImage'
        bare = {k: v for k, v in env.items() if not k.startswith('XDG_')}
        made_home = Path(f'{appimage}.home')
        made = {  # the driver's environment, then its data, config, state and cache folders
            'PortableConfig': (
                dict(env, XDG_CONFIG_HOME=f'{appimage}.config'),
                data,
                Path(f'{appimage}.config/scarletnx'),
                state,
                cache,
            ),
            'PortableHome': (
                dict(bare, HOME=str(made_home)),
                made_home / '.local/share/scarletnx',
                made_home / '.config/scarletnx',
                made_home / '.local/state/scarletnx',
                made_home / '.cache/scarletnx',
            ),
        }
        for name, (environment, made_data, made_config, made_state, made_cache) in made.items():
            game = made_data / 'recompiled' / name
            status, out = run(
                [driver, '--repo', str(made_data), '-r', str(nro), name, '--runtime', driver, '--jobs', '2', *aot],
                environment=environment,
            )
            record = game / f'{name}.folders'
            expected = ''.join(
                f'{kind}={where / "recompiled" / name}\n'
                for kind, where in (('config', made_config), ('state', made_state), ('cache', made_cache))
            )
            check(
                status == 0 and record.is_file() and record.read_text() == expected,
                f'the recompile did not record its folders in {record}: {expected!r}',
                out,
            )
            ini = made_config / 'recompiled' / name / f'{name}.ini'
            ini.write_text(ini.read_text() + '[Game]\nsource=/nonexistent/Game.nsp\n')  # a dump read in place
            shutil.rmtree(game / f'{name}_data/exefs')
            status, out = run([f'./{name}', '-H'], cwd=game)
            log = made_state / 'recompiled' / name / f'{name}.log'
            text = log.read_text(errors='replace') if log.is_file() else ''
            said = (
                f"fs: the game's dump /nonexistent/Game.nsp, its source= setting, is not there; its settings are {ini}"
            )
            check(said in text, f'the game started outside the AppImage did not read {ini} and log in {log}', out)
            outside = ((config, made_config), (state, made_state), (cache, made_cache))
            check(
                not any((there / 'recompiled' / name).exists() for there, own in outside if there != own),
                f'the game {name} started outside the AppImage wrote in the folders of that environment',
                out,
            )
            check(
                not any((game / f'{name}{file}').exists() for file in ('.ini', '.log')),
                f'the game {name} started outside the AppImage wrote its settings or log beside it',
                out,
            )

        # an older version's game has no record: its settings where the environment it starts in puts them, the one
        # that made it reading them as before, another saying which .ini names no dump
        name = 'PortableConfig'
        game = data / 'recompiled' / name
        (game / f'{name}.folders').unlink(missing_ok=True)
        status, out = run([f'./{name}', '-H'], cwd=game, environment=made[name][0])
        log = state / 'recompiled' / name / f'{name}.log'
        ini = Path(f'{appimage}.config/scarletnx/recompiled/{name}/{name}.ini')
        said = f"fs: the game's dump /nonexistent/Game.nsp, its source= setting, is not there; its settings are {ini}"
        check(said in log.read_text(errors='replace'), f'the game without a record did not read {ini}', out)
        status, out = run([f'./{name}', '-H'], cwd=game)
        said = (
            f'fs: the game has no ExeFS in {game}/{name}_data/exefs/ and no dump to read it from: '
            f'{config}/recompiled/{name}/{name}.ini sets no source=, and {game}/{name}.source is not there'
        )
        check(said in log.read_text(errors='replace'), 'the game without a dump did not say where it looked', out)
        # a record naming a folder that is not there: said, with the one the game takes instead
        (game / f'{name}.folders').write_text(f'config={root / "gone"}\n')
        status, out = run([f'./{name}', '-H'], cwd=game)
        said = (
            f"{game}/{name}.folders names {root / 'gone'} for the game's settings, and it is not there: they are in "
            f'{config}/recompiled/{name}/'
        )
        check(said in out, 'the game did not say its recorded folder is not there', out)
        # the driver records where its own run keeps them before it starts the game, its environment the launcher's
        other = dict(env, XDG_CONFIG_HOME=str(root / 'other'))
        status, out = run([driver, '--repo', str(data), str(game), '-H'], environment=other)
        check(
            (game / f'{name}.folders').read_text().startswith(f'config={root}/other/scarletnx/recompiled/{name}\n'),
            'the driver that started the game did not record its own folders',
            out,
        )
        check(not (root / 'home').exists(), 'something was written in HOME')

    if failures:
        print('XDG layout:\n' + ''.join(failures), file=sys.stderr)
        return 1
    print(
        'XDG layout: settings in config, log and threads.txt in state, pipeline cache and work/ in cache, older '
        'files told on each run and left alone; a folder of its own keeps them beside the game'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
