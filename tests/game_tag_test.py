# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""tools/game_tag.py with keys and game folders of its own: the tag against an HMAC made here, the IDs that share a
tag, the key made on first use and the ones refused, which from the folders given, and scan as a check."""

import hashlib
import hmac
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/game_tag.py'
GAME = '0100F00D00042000'  # no game's ID
NEXT = '0400F00D00062000'  # nor this one, of the console's next generation
KEY1 = b'\x11' * 32
POSIX = os.name == 'posix'


def expected(key, norm):
    return 'game-' + hmac.new(key, norm.encode('ascii'), hashlib.sha256).hexdigest()[:8]


class GameTag(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='snx-game-tag-test-')
        self.t = Path(self.folder.name)
        self.key = self.write_key('key1', KEY1)
        self.said = []
        self.base = expected(KEY1, f'nx:{GAME}')

    def tearDown(self):
        self.folder.cleanup()

    def write_key(self, name, data, mode=0o600):
        path = self.t / name
        path.write_bytes(data)
        path.chmod(mode)
        return path

    def run_tool(self, *args, key='', stdin=None, env=None):
        """(exit, stdout, stderr) of the tool, with --key self.key after the command unless key names another or None"""
        argv = [str(a) for a in args]
        key = self.key if key == '' else key
        if key is not None:
            argv[1:1] = ['--key', str(key)]
        run_env = dict(os.environ, PYTHONUTF8='1')
        run_env.update(env or {})
        r = subprocess.run(
            [sys.executable, str(TOOL), *argv],
            check=False,
            capture_output=True,
            text=True,
            encoding='utf-8',
            input=stdin,
            env=run_env,
        )
        self.said += [r.stdout, r.stderr]
        return r.returncode, r.stdout.rstrip('\n'), r.stderr.rstrip('\n')

    def tags(self, *ids):
        rc, out, err = self.run_tool('tag', *ids)
        self.assertEqual((rc, err), (0, ''))
        return out.split('\n')

    # --- tag ---

    def test_tag_is_the_hmac_of_the_normal_form(self):
        self.assertEqual(self.run_tool('tag', GAME), (0, self.base, ''))
        self.assertEqual(self.tags(GAME), [self.base])  # the same ID, the same tag

    def test_forms_of_one_id(self):
        self.assertEqual(self.tags('0x0100f00d00042000', f'nx:{GAME}', f'  {GAME} '), [self.base] * 3)

    def test_update_dlc_and_other_programs_share_the_tag(self):
        ids = ['0100F00D00042800', '0100F00D00043001', '0100F00D00043FFF', '0100F00D00042001']
        self.assertEqual(self.tags(*ids), [self.base] * 4)
        self.assertEqual(self.tags('0100F00D00044000'), [expected(KEY1, 'nx:0100F00D00044000')])  # the next game

    def test_another_key_another_tag(self):
        key2 = self.write_key('key2', b'\x22' * 32)
        rc, out, _ = self.run_tool('tag', GAME, key=key2)
        self.assertEqual((rc, out), (0, expected(b'\x22' * 32, f'nx:{GAME}')))
        self.assertNotEqual(out, self.base)

    def test_3ds_update_and_dlc_fall_on_the_application(self):
        want = expected(KEY1, '3ds:0004000000ABCD00')
        self.assertEqual(self.tags('0004000000abcd00', '3ds:0004000E00ABCD00', '0004008C00ABCD00'), [want] * 3)
        self.assertEqual(self.tags('0004000200ABCD00'), [expected(KEY1, '3ds:0004000200ABCD00')])  # a demo is its own

    def test_gba_code_in_any_case(self):
        self.assertEqual(self.tags('gba:zzze', 'GBA:ZZZE'), [expected(KEY1, 'gba:ZZZE')] * 2)

    def test_next_generation_id_has_a_prefix_of_its_own(self):
        want = expected(KEY1, f'nx2:{NEXT}')
        self.assertEqual(self.run_tool('tag', NEXT), (0, want, ''))
        ids = ['0400F00D00062800', '0400F00D00063001', '0400F00D00063FFF', '0x0400f00d00062000', f'nx2:{NEXT}']
        self.assertEqual(self.tags(*ids), [want] * 5)  # the update and the DLCs by the Switch's rule
        # the digits of a Switch game under 04, and the next 04 game, are games of their own
        self.assertEqual(
            self.tags('0400F00D00042000', '0400F00D00064000'),
            [expected(KEY1, 'nx2:0400F00D00042000'), expected(KEY1, 'nx2:0400F00D00064000')],
        )
        self.assertEqual(len({want, self.base, *self.tags('0400F00D00042000', '0400F00D00064000')}), 4)

    def test_tags_from_before_the_next_generation_stand(self):
        self.assertEqual(
            self.tags(GAME, '0004000000ABCD00', 'gba:AFRE'), ['game-ffa1e7c1', 'game-df553f4c', 'game-6ad636c7']
        )

    def test_id_of_no_system_names_the_ones_known(self):
        rc, out, err = self.run_tool('tag', '0500000000010000')
        self.assertEqual((rc, out), (2, ''))
        for prefix in ("'0500000000010000'", '(01...)', '(04...)', '(0004...)'):
            self.assertIn(prefix, err)

    def test_bad_ids_are_refused_with_their_value(self):
        bad_ids = [
            '0100F00D0004200',  # 15 digits
            '0100XYZ0000000000',
            'gba:AB',
            '0200000000000000',  # no system's
            '0100000000000809',  # a system title
            'nx:0004000000ABCD00',  # a 3DS ID under the Switch's prefix
            '0400000000000809',  # a next-generation system title
            f'nx2:{GAME}',  # a Switch ID under the next generation's prefix
            f'nx:{NEXT}',  # and the other way
        ]
        for bad in bad_ids:
            with self.subTest(bad):
                rc, out, err = self.run_tool('tag', GAME, bad)
                self.assertEqual((rc, out), (2, ''))  # no tag printed for the good one beside it
                self.assertIn(f"'{bad}'", err)

    # --- the key ---

    def home(self):
        """(the environment of a user whose home is a folder of this test, the key's file there)"""
        home = self.t / 'home'
        env = {'HOME': str(home), 'XDG_CONFIG_HOME': str(home / 'xdg'), 'APPDATA': str(home / 'AppData')}
        if sys.platform == 'win32':
            folder = home / 'AppData'
        elif sys.platform == 'darwin':
            folder = home / 'Library/Application Support'
        else:
            folder = home / 'xdg'
        return env, folder / 'scarletnx/game-tag.key'

    def test_missing_key_is_made_in_the_config_folder(self):
        env, path = self.home()
        rc, first, err = self.run_tool('tag', GAME, key=None, env=env)
        self.assertEqual(rc, 0)
        self.assertRegex(first, r'^game-[0-9a-f]{8}$')
        self.assertIn(f'no key at {path}: made one', err)
        self.assertEqual(path.stat().st_size, 32)
        if POSIX:
            self.assertEqual((path.stat().st_mode & 0o777, path.parent.stat().st_mode & 0o777), (0o600, 0o700))
        self.assertEqual(os.listdir(path.parent), ['game-tag.key'])  # no temporary file left
        self.assertEqual(first, expected(path.read_bytes(), f'nx:{GAME}'))
        self.assertEqual(self.run_tool('tag', GAME, key=None, env=env), (0, first, ''))  # read back, no line
        self.assertEqual(self.run_tool('tag', GAME, key=path), (0, first, ''))  # --key names the same file

    def test_scan_makes_a_missing_key_only_for_a_tag(self):
        env, path = self.home()
        clean, dirty = self.t / 'clean.md', self.t / 'dirty.md'
        clean.write_text('Nothing here.\n', encoding='utf-8')
        dirty.write_text(f'{GAME}\n', encoding='utf-8')
        self.assertEqual(self.run_tool('scan', clean, key=None, env=env), (0, '', ''))
        self.assertFalse(path.exists())
        rc, out, err = self.run_tool('scan', dirty, key=None, env=env)
        self.assertEqual((rc, out), (1, f'{dirty}:1: {expected(path.read_bytes(), "nx:" + GAME)} {GAME}'))
        self.assertIn(f'no key at {path}: made one', err)

    def test_which_makes_no_key_and_says_where_it_goes(self):
        env, path = self.home()
        games = self.t / 'games'
        games.mkdir()
        rc, out, err = self.run_tool('which', '--games', games, self.base, key=None, env=env)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f'no key at {path}:', err)
        self.assertIn('--key', err)
        self.assertFalse(path.parent.exists())

    def test_relative_xdg_config_home_is_not_taken(self):
        if sys.platform in ('win32', 'darwin'):
            self.skipTest('no XDG folders there')
        env, _ = self.home()
        env['XDG_CONFIG_HOME'] = 'relative'
        path = self.t / 'home/.config/scarletnx/game-tag.key'
        rc, _, err = self.run_tool('tag', GAME, key=None, env=env)
        self.assertEqual(rc, 0)
        self.assertIn(f'no key at {path}: made one', err)

    def test_bad_keys_are_refused(self):
        short = self.write_key('short', KEY1[:31])
        rc, out, err = self.run_tool('tag', GAME, key=short)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f'the key {short} is 31 bytes', err)
        long = self.write_key('long', KEY1 + b'\x11')
        rc, out, err = self.run_tool('tag', GAME, key=long)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f'the key {long} is 33 bytes', err)
        folder = self.t / 'keydir'
        folder.mkdir()
        rc, out, err = self.run_tool('tag', GAME, key=folder)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(str(folder), err)
        rc, out, err = self.run_tool('tag', '--key', '', GAME, key=None)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn('--key names a file, and it is empty', err)

    @unittest.skipUnless(POSIX, 'file modes are POSIX')
    def test_key_others_may_read_is_refused(self):
        seen = self.write_key('open', KEY1, 0o644)
        rc, out, err = self.run_tool('tag', GAME, key=seen)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f'the key {seen} has mode 0644', err)

    # --- the games of a folder ---

    def nacp(self, path, name, entry):
        path.parent.mkdir(parents=True, exist_ok=True)
        data = bytearray(0x4000)
        data[0x300 * entry : 0x300 * entry + len(name)] = name.encode()
        path.write_bytes(data)

    def games(self):
        """a folder as recompiled/ (a game, a labelled copy of it, a system title, a program with no ID) and one of
        dumps (a game and its update, a file with no ID, a one-word name, a name with marks, three GBA ROMs)"""
        made, dumps = self.t / 'recompiled', self.t / 'dumps'
        self.nacp(made / GAME / f'{GAME}_data/control.nacp', 'Fake Quest Deluxe', 0)
        self.nacp(made / f'{GAME}-label' / f'{GAME}-label_data/control.nacp', 'Fake Quest Deluxe', 2)
        self.nacp(made / 'homebrew/homebrew_data/control.nacp', 'Homebrew Fake Probe', 0)
        (made / '0100000000001000').mkdir()
        (made / '0004000000ABCD00').mkdir()
        # a guest's own file with an ID in its name is no dump
        (made / GAME / f'{GAME}_data/Inner Fake Thing [0100AAAA00050000].nsp').write_bytes(b'')
        (dumps / 'Another Fake Saga').mkdir(parents=True)
        for name in (
            'Another Fake Saga [0100BEEF00010000][v0].nsp',
            'Another Fake Saga/Another Fake Saga v1.0.1[0100BEEF00010800][65536][UPD].nsp',
            'Nameless Fake Adventure.xci',
            'Fakeonia [0100CAFE00020000].nsp',
            'Caf\u00e9 Hero\u2019s Tale \u2013 Second Act [0100D00D00030000].nsp',
        ):
            (dumps / name).write_bytes(b'')
        for name, code in (
            ('Fake Racer - Turbo Edition (U).gba', b'AFRE'),
            ('Robots (U).gba', b'AROE'),
            ('Fake Film - The Game (E).gba', b'AFGP'),
            ('Headerless Fake Cart (J).gba', None),
        ):
            (dumps / name).write_bytes(b'\0' * 0xAC + code + b'\0' * 16 if code else b'short')
        return made, dumps

    def test_which(self):
        made, dumps = self.games()
        saga, fakeonia, racer, ctr = self.tags('0100BEEF00010000', '0100CAFE00020000', 'gba:AFRE', '0004000000ABCD00')
        rc, out, err = self.run_tool(
            'which', '--games', made, '--games', dumps, self.base, saga, racer, fakeonia, ctr, 'game-00000000'
        )
        self.assertEqual(
            out.split('\n'),
            [
                f'{self.base} nx:{GAME} Fake Quest Deluxe',  # a folder's ID and its control.nacp, once with its copy
                f'{saga} nx:0100BEEF00010000 Another Fake Saga',  # a dump and its update
                f'{racer} gba:AFRE Fake Racer - Turbo Edition',  # the code of a ROM's header
                f'{fakeonia} nx:0100CAFE00020000 Fakeonia',
                f'{ctr} 3ds:0004000000ABCD00 (no name there)',
                f'game-00000000 not found (the tags of 8 IDs under {made}, {dumps} were tried)',
            ],
        )
        self.assertEqual((rc, err), (1, ''))
        # one folder: what only the other holds is not found; a game's own folder is one too
        rc, out, _ = self.run_tool('which', '--games', made / GAME, self.base, saga)
        self.assertEqual(rc, 1)
        self.assertEqual(out.split('\n')[0], f'{self.base} nx:{GAME} Fake Quest Deluxe')
        self.assertTrue(out.split('\n')[1].startswith(f'{saga} not found (the tags of 1 IDs under '))

    def test_which_finds_next_generation_games(self):
        made, dumps = self.t / 'recompiled', self.t / 'dumps'
        self.nacp(made / NEXT / f'{NEXT}_data/control.nacp', 'Fake Kart Sequel', 0)
        (made / '0400000000001000').mkdir()  # a system title's folder
        dumps.mkdir()
        (dumps / 'Fake Sequel Saga [0400BEEF00050000][v0].nsp').write_bytes(b'')
        (dumps / 'Fake Sequel Saga 0400BEEF00050800 v1.0.1.nsp').write_bytes(b'')
        kart, saga = self.tags(NEXT, '0400BEEF00050000')
        rc, out, err = self.run_tool('which', '--games', made, '--games', dumps, kart, saga, 'game-00000000')
        self.assertEqual(
            out.split('\n'),
            [
                f'{kart} nx2:{NEXT} Fake Kart Sequel',
                f'{saga} nx2:0400BEEF00050000 Fake Sequel Saga',  # the name before a bare ID too
                f'game-00000000 not found (the tags of 2 IDs under {made}, {dumps} were tried)',
            ],
        )
        self.assertEqual((rc, err), (1, ''))

    def test_which_refusals(self):
        made, _ = self.games()
        rc, out, err = self.run_tool('which', '--games', made, 'game-XYZ')
        self.assertEqual((rc, out), (2, ''))
        self.assertIn("'game-XYZ'", err)
        rc, out, err = self.run_tool('which', self.base)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn('--games', err)
        nowhere = self.t / 'nowhere'
        rc, out, err = self.run_tool('which', '--games', made, '--games', nowhere, self.base)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f"--games '{nowhere}' is not a folder", err)

    # --- scan ---

    def text(self, name, text):
        path = self.t / name
        path.write_text(text, encoding='utf-8')
        return path

    def scan(self, path, *more):
        made, dumps = self.games()
        return self.run_tool('scan', '--games', made, '--games', dumps, path, *more)

    def test_scan_of_a_clean_text(self):
        # a tag, a system title, IDs inside words, a one-word GBA title, "the game", the name of a folder with no ID,
        # and a name that only a file inside a game's data has
        clean = self.text(
            'clean.md',
            'A clean report: game-0123abcd hangs at boot, 0x0100000000000809 is the system version archive.\n'
            f'No ID in x{GAME}, g{GAME} or {GAME}ab, nor in Robots, the game or Homebrew Fake Probe.\n'
            f'Nor in 0400000000000809, x{NEXT} or {NEXT}ab.\n'
            'Inner Fake Thing is no name here.\n',
        )
        self.assertEqual(self.scan(clean), (0, '', ''))

    def test_scan_finds_an_id(self):
        path = self.text('id.md', 'line one\nthe crash is in (0x0100f00d00042800), the update\n')
        rc, out, err = self.run_tool('scan', path)  # an ID needs no folder of games
        self.assertEqual((rc, out), (1, f'{path}:2: {self.base} 0x0100f00d00042800'))
        self.assertIn('1 title ID(s) or game name(s) in 1 file(s)', err)
        rc, out, _ = self.run_tool('scan', '-', stdin=path.read_text(encoding='utf-8'))
        self.assertEqual((rc, out), (1, f'-:2: {self.base} 0x0100f00d00042800'))

    def test_scan_finds_a_next_generation_id_and_name(self):
        made = self.t / 'next'
        self.nacp(made / NEXT / f'{NEXT}_data/control.nacp', 'Fake Kart Sequel', 0)
        path = self.text('next.md', 'boots\n\nthe DLC [0x0400f00d00063001] hangs, and so does fake kart sequel\n')
        kart = expected(KEY1, f'nx2:{NEXT}')
        rc, out, _ = self.run_tool('scan', '--games', made, path)
        self.assertEqual((rc, out), (1, f'{path}:3: {kart} 0x0400f00d00063001\n{path}:3: {kart} "fake kart sequel"'))

    def test_scan_finds_names(self):
        fakeonia, racer, ctr = self.tags('0100CAFE00020000', 'gba:AFRE', '0004000000ABCD00')
        path = self.text(
            'names.md',
            'In fake quest\ndeluxe the menu hangs; Fakeonia too.\n'
            'Turbo Edition and Nameless Fake Adventure; a 3DS one, 0004000E00ABCD00.\nHeaderless fake cart.\n',
        )
        rc, out, _ = self.scan(path)
        self.assertEqual(rc, 1)
        self.assertEqual(
            out.split('\n'),
            [
                f'{path}:1: {self.base} "fake quest deluxe"',  # across a line break, in any case
                f'{path}:2: {fakeonia} "fakeonia"',  # a one-word name of a dump
                f'{path}:3: {ctr} 0004000E00ABCD00',  # a 3DS update's ID
                f'{path}:3: {racer} "turbo edition"',  # a part of a name after " - "
                f'{path}:3: untagged (no ID there) "nameless fake adventure"',  # a dump's name with no ID
                f'{path}:4: untagged (no ID there) "headerless fake cart"',  # a ROM too short for a header
            ],
        )

    def test_scan_folds_accents_and_cuts_at_a_dash(self):
        (hero,) = self.tags('0100D00D00030000')
        path = self.text('fold.md', 'The Cafe Heros Tale menu, and its Second\nAct.\n')
        rc, out, _ = self.scan(path)
        self.assertEqual((rc, out), (1, f'{path}:1: {hero} "cafe heros tale"\n{path}:1: {hero} "second act"'))

    def test_scan_refusals(self):
        clean = self.text('clean.md', 'Nothing here.\n')
        missing = self.t / 'missing.md'
        rc, out, err = self.run_tool('scan', clean, missing)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f'cannot read {missing}', err)
        rc, out, err = self.run_tool('scan', '--ids', clean)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn('--ids', err)
        rc, out, err = self.run_tool('scan', '--games', missing, clean)
        self.assertEqual((rc, out), (2, ''))
        self.assertIn(f"--games '{missing}' is not a folder", err)

    def test_no_command(self):
        for argv in ([], ['frob', GAME], ['tag']):
            with self.subTest(argv):
                rc, out, err = self.run_tool(*argv, key=None)
                self.assertEqual((rc, out), (2, ''))
                self.assertTrue(err)

    def test_the_key_is_never_printed(self):
        made, dumps = self.games()
        path = self.text('id.md', f'{GAME} and Fakeonia\n')
        self.run_tool('tag', GAME)
        self.run_tool('which', '--games', made, '--games', dumps, self.base)
        self.run_tool('scan', '--games', dumps, path)
        self.run_tool('tag', GAME, key=self.write_key('short', KEY1[:31]))
        self.run_tool('--help', key=None)
        said = '\n'.join(self.said).lower()
        self.assertIn(self.base, said)
        self.assertNotIn('1111', said)  # the key's bytes in hex, or its hex cut anywhere
        self.assertNotIn('\x11', said)


if __name__ == '__main__':
    unittest.main()
