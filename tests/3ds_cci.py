#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A cartridge image or a bare NCCH read where it lies against its extraction: the same images accepted and
refused, the same bytes read, and a cartridge's update partition played as the update."""

import io
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from ctr_fixtures import NONE, cia, ncsd, ncch, put, tar, titled, UPDATE_TITLE, z3ds


def main():
    # absolute, not resolved: scarletnx-3ds may be a link to scarletnx, which runs the driver its name says
    driver, probe = [str(Path(p).absolute()) for p in sys.argv[1:3]]
    checked = 0
    with tempfile.TemporaryDirectory(prefix='snx-ctr-cci-') as temp:
        root = Path(temp)

        def both(blob, name, extension='cci'):
            nonlocal checked
            checked += 1
            source, out = root / f'{name}.{extension}', root / name
            source.write_bytes(blob)
            # the home folder is the test's: no 3DS keys of the user's decrypt an encrypted case
            env = {**os.environ, 'HOME': str(root), 'USERPROFILE': str(root)}
            x = subprocess.run([driver, '-x', '--output', str(out), str(source)], capture_output=True, text=True,
                               timeout=20, env=env)
            args = [probe, str(source)] + ([str(out)] if x.returncode == 0 else [])
            p = subprocess.run(args, capture_output=True, text=True, timeout=20)
            text = x.stdout + x.stderr + p.stdout + p.stderr
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
            if x.returncode and not p.returncode:
                # the read in place takes no whole-data hash (an ExeFS file's), so it also accepts an image whose
                # only fault is one of those, as the CIA's test does
                assert 'SHA-256 mismatch' in text, (name, text)
                source.unlink()
                shutil.rmtree(out, ignore_errors=True)
                return x.returncode, text
            assert (x.returncode == 0) == (p.returncode == 0), (name, x.returncode, p.returncode, text)
            if x.returncode:
                assert x.returncode in (1, 3) and p.returncode in range(21, 26), (name, text)
                assert (x.returncode == 3) == (p.returncode == 23), (name, text)
                source.unlink()
                return x.returncode, text
            assert 'match its extraction' in p.stdout, (name, text)
            source.unlink()
            return 0, text

        def manifest(name):
            text = (root / name / 'manifest.ini').read_text()
            assert 'state=extracted' in text, text
            return text

        # the encrypted case needs 3ds_decrypt.py's helpers, and those the cryptography package, which CI has
        # none of: the module's import says so and exits, and the test's other cases run without it
        enc_image = None
        import contextlib
        import importlib.util
        spec = importlib.util.spec_from_file_location('ctr_decrypt_test', Path(__file__).parent / '3ds_decrypt.py')
        decrypt_helpers = importlib.util.module_from_spec(spec)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                spec.loader.exec_module(decrypt_helpers)
            enc_image = decrypt_helpers.encrypt_ncch(ncch())
        except (ImportError, SystemExit):
            pass
        update_header, update_exheader = titled(UPDATE_TITLE)
        update = ncch(patch_header=update_header, patch_exheader=update_exheader)
        other_header, other_exheader = titled(UPDATE_TITLE + 1)

        # the plain cartridge, its compressed code, and one without a RomFS
        assert both(ncsd(), 'plain')[0] == 0
        assert both(ncsd(ncch(compressed=True)), 'compressed')[0] == 0
        assert both(ncsd(ncch(with_romfs=False)), 'no-romfs')[0] == 0
        # the .3ds name of the same image, and a bare NCCH as a .cxi, an .app and homebrew's .hbb
        assert both(ncsd(), 'as-3ds', extension='3ds')[0] == 0
        assert both(ncch(), 'as-cxi', extension='cxi')[0] == 0
        assert both(ncch(), 'as-app', extension='app')[0] == 0
        assert both(ncch(), 'as-hbb', extension='hbb')[0] == 0
        # the update partition is the game: its ExHeader and code are extracted, and the manifest says so
        status, text = both(ncsd(update=update), 'update')
        assert status == 0, text
        assert 'overlay=1' in manifest('update'), text
        # an update partition of another title is not this game's update: the game's own runs
        status, text = both(ncsd(update=ncch(patch_header=other_header, patch_exheader=other_exheader)), 'no-overlay')
        assert status == 0 and 'overlay=0' in manifest('no-overlay'), text
        # a dump declaring a larger card than its file holds, as a trimmed one does
        assert both(ncsd(media_size=0x100000), 'declared-large')[0] == 0
        # refusals: no game in partition 0, a partition outside the image, a game that is not an application
        empty = bytearray(ncsd())
        put(empty, 0x120, '<II', 0, 0)  # partition 0 at nowhere
        status, text = both(bytes(empty), 'no-game')
        assert status == 3 and 'game partition (0)' in text, text
        beyond = bytearray(ncsd())
        put(beyond, 0x120, '<II', 0x400000, 8)  # 512 MiB into a 12 KiB image
        status, text = both(bytes(beyond), 'beyond')
        assert status == 1, text
        notapp = bytearray(ncsd())
        put(notapp, 0x200 + 0x18D, '<B', 0)  # the game NCCH is data, not an executable application
        status, text = both(bytes(notapp), 'not-app')
        assert status == 3 and 'holds no game' in text, text
        # a tool that decrypted an image but left its flag saying encrypted: the ExHeader is plain, which the header's
        # hash of it says, so the image is read as it is, with no keys, and the driver says what it found
        flagged = bytearray(ncsd())
        put(flagged, 0x200 + 0x18F, '<B', 0)  # the game NCCH's NoCrypto off
        status, text = both(bytes(flagged), 'plain-flagged')
        assert status == 0 and 'ExHeader is plain' in text, text
        # a seed bit on content that is already plain is no crypto at all: the image is read as the plain one it is
        # (an image that really is seed-encrypted is refused, as 3ds_decrypt.py's case shows)
        seed = bytearray(ncsd())
        put(seed, 0x200 + 0x18F, '<B', 0x20)
        status, text = both(bytes(seed), 'plain-seed')
        assert status == 0 and 'ExHeader is plain' in text, text
        short = bytearray(ncsd())
        put(short, 0x200 + 0x104, '<I', 3)  # a game NCCH claiming three media units, too few for its sections
        assert both(bytes(short), 'size')[0] == 1
        # the bare NCCH's own refusal: a size that is neither its media units nor its bytes
        wrong = bytearray(ncch())
        put(wrong, 0x104, '<I', 3)
        assert both(bytes(wrong), 'wrong-size', extension='cxi')[0] == 1
        # mutation smoke tests over the plain image and its header's table
        plain = ncsd()
        rng = random.Random(0x43434921)
        for i in range(40):
            mutant = bytearray(plain)
            for _ in range(1 + i % 4):
                at = rng.randrange(len(mutant))
                mutant[at] ^= 1 << rng.randrange(8)
            both(bytes(mutant), f'mutant-{i}')
        for i in range(24):
            offset = rng.choice([0x100, 0x104, 0x120, 0x128, 0x130, 0x138, 0x180, 0x184, 0x188])
            value = rng.choice([0, NONE, 1, 2, 8, 0x1000, 0xFFFFFFF0])

            def patch(h):
                put(h, offset, '<I', value)

            both(ncsd(patch=patch), f'header-mutant-{i}')
        # a bundle holds the game beside what a publisher's note is, and its game is read where it lies: a
        # cartridge image's, a CIA's, and one whose game entry is not the first
        game = ncch()
        assert both(tar(('game.cci', ncsd(game=game)), ('readme.txt', b'thanks for playing')), 'bundle',
                    extension='bcci')[0] == 0
        assert both(tar(('note.txt', b'a note'), ('game.cia', cia())), 'bundle-cia', extension='bcia')[0] == 0
        # a bare NCCH in a bundle, as a .bcxi
        assert both(tar(('game.cxi', game)), 'bundle-cxi', extension='bcxi')[0] == 0
        # an entry named as a game whose content is none: refused, as Azahar's first-name-match is, whatever the
        # content's own refusal says
        status, text = both(tar(('game.cci', b'not a game'), ('real.cci', ncsd())), 'bundle-not-game',
                            extension='bcci')
        assert status and text, text
        # refusals: no game entry, two entries of one name, a wrong checksum, an entry cut short
        status, text = both(tar(('note.txt', b'a note'), ('other.txt', b'another')), 'bundle-no-game',
                            extension='bcci')
        assert status == 3 and 'holds no game' in text, text
        status, text = both(tar(('game.cci', ncsd()), ('game.cci', ncsd())), 'bundle-duplicate', extension='bcci')
        assert status == 1 and 'two entries named' in text, text

        def break_checksum(blob):
            blob[148] = (blob[148] + 1) % 128

        status, text = both(tar(('game.cci', ncsd()), patch=break_checksum), 'bundle-checksum', extension='bcci')
        assert status == 1 and 'checksum' in text, text
        # an entry's data cut short is refused, while a byte of the trailing zero padding missing is not, as no
        # entry is the less whole for it
        whole = tar(('game.cci', ncsd()))
        status, text = both(bytes(whole[:-1]), 'bundle-cut-pad', extension='bcci')
        assert status == 0, text
        status, text = both(whole[:-1024], 'bundle-cut-data', extension='bcci')
        assert status == 1 and 'ends inside an entry' in text, text
        # mutation smoke tests over the bundle
        rng = random.Random(0x54415221)
        for i in range(24):
            mutant = bytearray(tar(('game.cci', ncsd())))
            for _ in range(1 + i % 4):
                at = rng.randrange(len(mutant))
                mutant[at] ^= 1 << rng.randrange(8)
            both(bytes(mutant), f'bundle-mutant-{i}', extension='bcci')
        # A z3ds game, what Azahar's launcher keeps one in, is unpacked once into the plain container beside the
        # title's folder and read from then on. The probe reads the plain container, so these are the driver's
        # alone: the extraction of the unpacked game is the plain image's
        def alone(blob, name, extension, keys=None, status=0, message=None):
            source, out = root / f'{name}.{extension}', root / name
            source.write_bytes(blob)
            env2 = {**os.environ, 'HOME': str(root), 'USERPROFILE': str(root)}
            args = [driver, '-x', '--output', str(out), str(source)]
            if keys:
                args += ['--keys', str(keys)]
            p = subprocess.run(args, capture_output=True, text=True, timeout=60, env=env2)
            text = p.stdout + p.stderr
            assert p.returncode == status, (name, p.returncode, text)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
            if message:
                assert message in text, (name, text)
            source.unlink()
            return out, text

        def files(folder):
            return {p2.relative_to(folder).as_posix(): p2.read_bytes() for p2 in sorted(folder.rglob('*')) if p2.is_file()}

        plain_image = ncsd()
        out, _ = alone(plain_image, 'zcci-plain', 'cci')
        want = files(out)
        _, text = alone(z3ds(plain_image), 'zcci-game', 'zcci', message='Unpacked')
        assert files(root / 'zcci-game') == want, text
        assert (root / 'zcci-game.unpacked.cci').read_bytes() == plain_image, text
        alone(z3ds(plain_image), 'zcci-game', 'zcci', message='Using the unpacked game')
        # an encrypted one is unpacked, then decrypted, and without the keys it is the decryption that refuses
        if enc_image is not None:
            alone(z3ds(enc_image), 'zcci-encrypted', 'zcci', status=3, message='no 3DS keys')
        # a stream that is not the game its header says, and one whose version no reader takes
        alone(z3ds(b'not a game at all'), 'zcci-garbage', 'zcci', status=1)  # whatever the content's own refusal says
        alone(z3ds(plain_image, patch_header=lambda h: put(h, 8, '<B', 2)), 'zcci-version', 'zcci', status=3,
              message='unsupported z3ds version')
        short = bytearray(z3ds(plain_image))
        status, _ = alone(bytes(short[:-1]), 'zcci-cut', 'zcci', status=1)
        assert not list(root.rglob('.snx-*'))
    print(f'CCI and NCCH read where it lies: {checked} images accepted and refused as their extraction, '
          'the update partition played as the update')


if __name__ == '__main__':
    main()
