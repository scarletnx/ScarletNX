#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The Vita driver's command line, without a game: the recompile's options every driver shares
(common/utilities/cli.h) in its help, each value they do not take stopping the run with it, and none of them with -x,
which compiles no code. Given the import's test (snx-test-vita-decrypt), an encrypted dump it makes: -x decrypts it
into the game's folder with --keys, the add-on without a license said; without --keys the keys' file is
~/.vita/vita_keys.txt, and with none there the import stops naming it."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    # absolute, not resolved: scarletnx-vita may be a link to scarletnx, which runs the driver its name says
    driver = pathlib.Path(sys.argv[1]).absolute()
    failures = []

    def run(args, status, contains, env=None):
        p = subprocess.run([str(driver), *args], capture_output=True, text=True, timeout=60, env=env)
        output = p.stdout + p.stderr
        if p.returncode != status or contains not in output:
            failures.append(f'{args}: exit {p.returncode}, not {status}, or no "{contains}"\n{output}')
        return output

    help_text = run(['--help'], 0, 'Common options')
    for line in (
        '\n  -O, --opt LEVEL        LLVM',
        '\n  --jobs N',
        '\n  --chunk N',
        '\n  --jit-cache PATH',
        '\n  --jit-only',
        '\n  --keep-ir',
        '\n  -q, --quiet',
        'On the Vita, -O is the level',
        'KEY is the key with a - for each _; the key as written, the older spelling, is still\n'
        '                         read, and said once',
    ):
        if line not in help_text:
            failures.append(f'the help lacks "{line.strip()}"')
    run(['-O9', 'game.vpk'], 2, 'scarletnx-vita: -O 9: not 0, 1, 2, 3, s or z')
    run(['--jobs=1025', 'game.vpk'], 2, 'scarletnx-vita: --jobs 1025: not a whole number from 0 (Auto) to 1024')
    run(['--chunk', 'x', 'game.vpk'], 2, 'scarletnx-vita: --chunk x: not a whole number of instructions from 1')
    run(['-x', '--keep-ir', 'game.vpk'], 2, "scarletnx-vita: --keep-ir is the recompile's, and -x compiles no code")
    run(['-x', '--opt=1', 'game.vpk'], 2, "scarletnx-vita: -O is the recompile's, and -x compiles no code")
    # a setting's key with a - for each _ is the same setting, said by its key, --press-len= the Vita's press length
    # as --press-len= is; some _ left is no setting
    run(['--aot-threads=many', 'game.vpk'], 2, 'scarletnx-vita: aot_threads=many is not a whole number from 0 to 1024')
    run(
        ['--press-len=-5', '--help'],
        0,
        'settings: vita_press_len=-5 is not a whole number from 0 to 2147483647: it keeps 0',
    )
    run(['--vita-hle_trace=1', 'game.vpk'], 2, 'scarletnx-vita: unknown setting: --vita-hle_trace=1')
    # the key as the .ini writes it, the older spelling: still read, and said once a run with the name now
    out = run(
        ['--vita_hle_trace=1', '--vita_hle_trace=2', '--help'],
        0,
        'settings: --vita_hle_trace is the old spelling of --vita-hle-trace: it is read as that; write --vita-hle-trace',
    )
    if out.count('old spelling') != 1:
        failures.append(f'the old spelling not said once:\n{out}')
    if '\n  --keys FILE' not in help_text or 'A dumped game still encrypted (its sce_pfs/ folder)' not in help_text:
        failures.append("the help lacks --keys or the encrypted dump's paragraph")
    if len(sys.argv) > 2:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([sys.argv[2]], env={**os.environ, 'VITA_DECRYPT_MAKE': tmp}, check=True, timeout=60)
            app = os.path.join(tmp, 'dump', 'app', 'SNXT00001')
            games = os.path.join(tmp, 'games')
            keys = os.path.join(tmp, 'vita_keys.txt')
            out = run(
                ['-x', '--outdir', games, '--keys', keys, app],
                0,
                'SELFs decrypted: eboot.bin, sce_module/libtest.suprx',
            )
            for line in (
                'the add-on ',
                'SCARLETNXADDON02 is not imported: it has no license in',
                'readme_extra.txt is not in its files.db, and the import leaves it out',
            ):
                if line not in out:
                    failures.append(f'the import does not say "{line}"\n{out}')
            for rel in ('app/data/big.bin', 'app/data/mixed.bin', 'app/sce_sys/icon0.png'):
                got = pathlib.Path(games, 'SNXT00001', rel).read_bytes()
                plain = pathlib.Path(tmp, 'plain', 'SNXT00001', rel[4:]).read_bytes()
                if got != plain:
                    failures.append(f'{rel}: {len(got)} bytes, not its {len(plain)} plain ones')
            addon = pathlib.Path(games, 'SNXT00001', 'ux0', 'addcont', 'SNXT00001', 'SCARLETNXADDON01', 'addon.dat')
            if addon.read_bytes() != pathlib.Path(tmp, 'plain', 'SCARLETNXADDON01', 'addon.dat').read_bytes():
                failures.append("the add-on's addon.dat is not its plaintext")
            home = os.path.join(tmp, 'home')
            os.mkdir(home)
            # the home folder the driver reads ~/.vita from: HOME, or USERPROFILE first on Windows
            env = {**os.environ, 'HOME': home, 'USERPROFILE': home}
            out = run(
                ['-x', '--outdir', os.path.join(tmp, 'other'), app],
                1,
                'is encrypted, and there are no Vita keys to decrypt it with at ',
                env,
            )
            if '.vita' not in out or 'vita_keys.txt (' not in out:
                failures.append(f'without keys, the import does not name ~/.vita/vita_keys.txt:\n{out}')
            os.mkdir(os.path.join(home, '.vita'))
            os.link(keys, os.path.join(home, '.vita', 'vita_keys.txt'))
            run(['-x', '--outdir', os.path.join(tmp, 'other'), app], 0, 'files decrypted', env)
    if failures:
        sys.exit('vita_driver:\n' + '\n'.join(failures))
    print('vita_driver: ok')


if __name__ == '__main__':
    main()
