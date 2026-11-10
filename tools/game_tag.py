#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The opaque tag a game goes by in commits, issues, docs and reports, in place of its title or its title ID.

    tools/game_tag.py tag ID...                    the tag of each ID, one per line
    tools/game_tag.py which --games DIR TAG...     the ID and the name of each tag, from the games under DIR
    tools/game_tag.py scan [--games DIR] FILE...   every title ID in the files (- is stdin), and every name of a game
                                                   under DIR, each with its tag: exit 1 when it finds any, so it
                                                   works as a check before a text is published

A tag is `game-` and the first 8 hex digits of HMAC-SHA256(key, the game's ID in its normal form):
  - Switch: 16 hex digits from 01 (0x, any case and nx: allowed), cut to its application's base ID, so that the
    update (base + 0x800), the DLCs (base + 0x1000 + n) and the game's other programs share its tag: nx:<base>;
  - 3DS: 16 hex digits from 0004 (3ds: allowed), an update (0004000E...) or a DLC (0004008C...) taken to its
    application (00040000...): 3ds:<id>;
  - GBA: gba: and the 4-character game code of the cartridge header, in upper case: gba:<code>.
The system prefix keeps two systems from ever sharing a tag. A Switch system title (010000000000xxxx) is no game:
tag refuses one, and scan lets one pass.

The key is 32 random bytes in game-tag.key of the user's config folder ($XDG_CONFIG_HOME/scarletnx or
~/.config/scarletnx, ~/Library/Application Support/scarletnx on macOS, %APPDATA%\\scarletnx on Windows); --key FILE
names another file. It is never in the repository and never printed: a title ID is public, and without the key nobody
can build the table of tags back from the public lists of IDs. Two people get the same tag for a game only with the
same key. tag and scan make the key on first use, with mode 0600 and a line on stderr saying where. which does not: a
tag is found again only with the key that made it, so without a key it says where that key goes. A key that cannot be
read, is not 32 bytes, or that the group or others may read is refused.

which, and the names scan looks for, come from the folders --games names (it may be given more than once), each
searched with its subfolders; nothing is stored:
  - a folder named by a title ID (or ID-LABEL), as the ones of recompiled/: its ID, with the first title of its
    *_data/control.nacp as the name;
  - a file with a title ID in its name: its ID, and for a game file (.nsp, .xci, .nsz, .xcz, .cia, .3ds, .cci) the
    name before the ID (one with no ID gives a name with no tag);
  - a .gba file: the game code of its header, with the file's name, without its (tags), as the name.

scan finds a title ID that stands as a word of its own (0x allowed), and a 3DS one only for an application, demo,
update or DLC. A name is found in any case, accents and apostrophes dropped, with any spaces or punctuation between
its words: the whole name, and each part of it cut at ":", " - ", " + " and " & ", without a leading the, a or an,
that has two words and 5 letters or more. A one-word name counts when it has 6 letters or more and is not a .gba
file's: one-word GBA titles are mostly English words. Nicknames and other short forms are not found.

Exit: 0 done (scan: nothing found); 1 which: a tag not found, scan: something found; 2 refused (a bad ID, tag or
option, the key, a folder or a file that cannot be read).
"""

import argparse
import hashlib
import hmac
import os
import re
import stat
import sys
import tempfile
import unicodedata

KEY_NAME = 'game-tag.key'
KEY_BYTES = 32
GAME_FILES = ('.nsp', '.xci', '.nsz', '.xcz', '.cia', '.3ds', '.cci')
NX_FIRST_GAME = 0x0100000000010000  # below it, the console's own titles
NX_BASE_MASK = ~0x1FFF  # an application ID has its low 13 bits clear; updates, DLCs and other programs set them
CTR_GAMES = {0x00040000: 0x00040000, 0x00040002: 0x00040002, 0x0004000E: 0x00040000, 0x0004008C: 0x00040000}
ID_FOLDER = re.compile(r'(01[0-9A-Fa-f]{14}|0004[0-9A-Fa-f]{12})(-|$)')
TAG = re.compile(r'game-[0-9a-f]{8}')
WORD = re.compile(r'[0-9A-Za-z]+')
NACP_TITLES = 16  # language entries of 0x300 bytes, each a 0x200-byte UTF-8 name then the publisher
GBA_CODE_AT = 0xAC  # the 4-character game code of a cartridge header


class TagError(ValueError):
    """A refusal; its text names the value refused."""


def note(text):
    print(f'game-tag: {text}', file=sys.stderr)


# --- IDs and tags ---------------------------------------------------------------------------------------------------


def normal(raw):
    """The normal form of a title ID or game code (nx:..., 3ds:..., gba:...), or TagError naming it."""
    s, system = raw.strip(), None
    for prefix in ('nx', '3ds', 'gba'):
        if s.lower().startswith(prefix + ':'):
            system, s = prefix, s[len(prefix) + 1 :]
            break
    if system == 'gba':
        if not re.fullmatch(r'[0-9A-Za-z]{4}', s):
            raise TagError(f"'{raw}' is not a GBA game code: gba: and the 4 letters or digits of the header")
        return 'gba:' + s.upper()
    h = s[2:] if s[:2] in ('0x', '0X') else s
    if not re.fullmatch(r'[0-9A-Fa-f]{16}', h):
        raise TagError(f"'{raw}' is not a title ID: 16 hex digits (01... Switch, 0004... 3DS) or gba:CODE")
    v = int(h, 16)
    if system is None:
        system = 'nx' if v >> 56 == 0x01 else '3ds' if v >> 48 == 0x0004 else None
        if system is None:
            raise TagError(f"'{raw}' is neither a Switch (01...) nor a 3DS (0004...) title ID")
    if system == 'nx':
        if v >> 56 != 0x01:
            raise TagError(f"'{raw}' is not a Switch title ID: 01 and 14 more hex digits")
        if v < NX_FIRST_GAME:
            raise TagError(f"'{raw}' is a Switch system title (below 0100000000010000), not a game: it takes no tag")
        return f'nx:{v & NX_BASE_MASK:016X}'
    if v >> 48 != 0x0004:
        raise TagError(f"'{raw}' is not a 3DS title ID: 0004 and 12 more hex digits")
    high = v >> 32
    return f'3ds:{CTR_GAMES.get(high, high):08X}{v & 0xFFFFFFFF:08X}'


def find_ids(text):
    """(line, ID as written, normal form) of each game's title ID standing as a word of its own in text."""
    out = []
    for n, line in enumerate(text.split('\n'), 1):
        for m in WORD.finditer(line):
            t = m.group()
            h = t[2:] if len(t) == 18 and t[:2] in ('0x', '0X') else t
            if len(h) != 16 or not re.fullmatch(r'[0-9A-Fa-f]{16}', h):
                continue
            v = int(h, 16)
            if (v >> 56 == 0x01 and v >= NX_FIRST_GAME) or (v >> 32) in CTR_GAMES:
                out.append((n, t, normal(h)))
    return out


def key_file():
    """game-tag.key of the user's config folder (snx_user_dir in common.c), or TagError when nothing names a home."""
    if sys.platform == 'win32':
        base = os.environ.get('APPDATA', '')
        if not base:
            raise TagError('APPDATA is not set, so the key has no folder: name its file with --key')
    elif sys.platform == 'darwin':
        base = os.path.expanduser('~/Library/Application Support')
    else:
        xdg = os.environ.get('XDG_CONFIG_HOME', '')  # the XDG spec ignores a relative one
        base = xdg if os.path.isabs(xdg) else os.path.expanduser('~/.config')
    return os.path.join(base, 'scarletnx', KEY_NAME)


class Key:
    """The key of one run. make: read when the first tag needs it, and made when missing; else read now or refused."""

    def __init__(self, path, make):
        if path == '':
            raise TagError('--key names a file, and it is empty')
        self.path = key_file() if path is None else path
        self.data = None if make else load_key(self.path, make=False)

    def tag(self, norm):
        if self.data is None:
            self.data = load_key(self.path, make=True)
        return 'game-' + hmac.new(self.data, norm.encode('ascii'), hashlib.sha256).hexdigest()[:8]


def load_key(path, make):
    try:
        with open(path, 'rb') as f:
            st = os.fstat(f.fileno())
            data = f.read(KEY_BYTES + 1) if stat.S_ISREG(st.st_mode) else None
    except FileNotFoundError:
        if make:
            return make_key(path)
        raise TagError(
            f'no key at {path}: a tag is found again only with the key that made it. Put that key there '
            f'({KEY_BYTES} bytes, mode 0600), or name its file with --key'
        )
    except OSError as e:
        raise TagError(f'cannot read the key {path}: {e.strerror}')
    if data is None:
        raise TagError(f'the key {path} is not a file')
    if os.name == 'posix' and st.st_mode & 0o077:  # Windows has no such bits: the profile's ACL keeps it
        raise TagError(
            f'the key {path} has mode {st.st_mode & 0o777:04o}, so others on this machine may read it; '
            f'it wants 0600 (chmod 600 {path})'
        )
    if len(data) != KEY_BYTES:
        raise TagError(f'the key {path} is {st.st_size} bytes; a key is {KEY_BYTES} random bytes')
    return data


def make_key(path):
    folder = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(folder, mode=0o700, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=folder, prefix='.game-tag.key.')  # mode 0600
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(os.urandom(KEY_BYTES))
                f.flush()
                os.fsync(f.fileno())
            try:
                os.link(tmp, path)  # the key appears whole or not at all; of two runs racing, both read the first
            except FileExistsError:
                pass
            else:
                note(
                    f'no key at {path}: made one ({KEY_BYTES} random bytes, mode 0600). Keep it: a tag made with it '
                    f'is found again, and is the same for someone else, only with this key'
                )
        finally:
            os.unlink(tmp)
    except OSError as e:
        raise TagError(f'cannot make the key {path}: {e.strerror}')
    return load_key(path, make=False)


# --- the games under the folders given ------------------------------------------------------------------------------


def nacp_name(folder):
    """The first title of the first *_data/control.nacp in folder (American English comes first), or None."""
    try:
        subs = sorted(x for x in os.listdir(folder) if x.endswith('_data'))
    except OSError:
        return None
    for sub in subs:
        try:
            with open(os.path.join(folder, sub, 'control.nacp'), 'rb') as f:
                data = f.read(NACP_TITLES * 0x300)
        except OSError:
            continue
        for i in range(NACP_TITLES):
            name = data[i * 0x300 : i * 0x300 + 0x200].split(b'\0')[0].decode('utf-8', 'replace').strip()
            if name:
                return name
    return None


def id_folder(path, name, found):
    m = ID_FOLDER.match(name)
    if not m:
        return
    try:
        norm = normal(m.group(1))
    except TagError:
        return  # a system title's folder
    found.append((norm, nacp_name(path), 'folder', 0))


def file_name(stem):
    """The game's name in a dump's file name: what comes before the first [ or ID, without a version or region."""
    name = re.split(r'\[|(?<![0-9A-Za-z])(?:01[0-9A-Fa-f]{14}|0004[0-9A-Fa-f]{12})(?![0-9A-Za-z])', stem)[0]
    name = re.sub(r'\s+v\d+(\.\d+)*$', '', name.strip())
    name = re.sub(r'\s+(usa|us|eur|eu|jpn|jp)$', '', name, flags=re.IGNORECASE).strip()
    return name or None


def gba_name(rom):
    """A ROM's file name without its (tags) and with a trailing ', The' back in front."""
    name = re.sub(r'\s*[\(\[][^\)\]]*[\)\]]', '', os.path.splitext(rom)[0]).strip()
    m = re.fullmatch(r'(.*), (The|A|An)', name)
    return f'{m.group(2)} {m.group(1)}' if m else name


def gba_code(path):
    """The normal form of the game code in a GBA ROM's header, or None when the file has none."""
    try:
        with open(path, 'rb') as f:
            f.seek(GBA_CODE_AT)
            code = f.read(4)
    except OSError as e:
        note(f'cannot read {path}: {e.strerror}: its game code is not known, so its name has no tag')
        return None
    try:
        return normal('gba:' + code.decode('ascii'))
    except (TagError, UnicodeDecodeError):
        return None


def local_games(folders):
    """(normal ID or None, name or None, source, rank) of every game under folders; rank 0 is the better name."""
    found = []
    for folder in folders:
        if not os.path.isdir(folder):
            raise TagError(f"--games '{folder}' is not a folder")
        id_folder(folder, os.path.basename(os.path.abspath(folder)), found)
        for top, dirs, files in os.walk(folder):
            dirs[:] = sorted(d for d in dirs if not d.endswith('_data'))  # a game's own files are no dumps
            for d in dirs:
                id_folder(os.path.join(top, d), d, found)
            for f in sorted(files):
                stem, ext = os.path.splitext(f)
                if ext.lower() == '.gba':
                    found.append((gba_code(os.path.join(top, f)), gba_name(f), 'gba', 0))
                    continue
                name = file_name(stem) if ext.lower() in GAME_FILES else None
                ids = {norm for _, _, norm in find_ids(f)}
                for norm in sorted(ids):
                    found.append((norm, name, 'file', 1))
                if name and not ids:
                    found.append((None, name, 'file', 1))
    return found


# --- names in a text ------------------------------------------------------------------------------------------------


def words(text):
    """(word, line) of text folded for name matching: ASCII lower case, accents and apostrophes dropped."""
    out, cur, line, start = [], [], 1, 1
    for c in text:
        if c in "'\u2018\u2019":
            continue
        folded = ''
        if unicodedata.category(c)[0] in 'LN':
            folded = ''.join(x for x in unicodedata.normalize('NFKD', c) if not unicodedata.combining(x)).lower()
        if folded and folded.isascii() and folded.isalnum():
            if not cur:
                start = line
            cur.append(folded)
        elif cur:
            out.append((''.join(cur), start))
            cur = []
        if c == '\n':
            line += 1
    if cur:
        out.append((''.join(cur), start))
    return out


def candidates(name, one_word):
    """The word sequences that stand for name in a text: the whole name and its parts, each without a leading
    article, of two words and 5 letters or more; a whole name of one word of 6 letters or more when one_word."""
    out = set()
    for i, part in enumerate([name] + re.split(r':|\s[-\u2013\u2014+&]\s', name)):
        seq = tuple(w for w, _ in words(part))
        if seq[:1] in (('the',), ('a',), ('an',)):
            seq = seq[1:]
        if (len(seq) >= 2 and len(''.join(seq)) >= 5) or (i == 0 and one_word and len(seq) == 1 and len(seq[0]) >= 6):
            out.add(seq)
    return out


def name_index(games, key):
    """First word -> [(word sequence, {tag or None})], longest first."""
    by_seq = {}
    for norm, name, source, _ in games:
        if name:
            for seq in candidates(name, one_word=source != 'gba'):  # one-word GBA titles are mostly English words
                by_seq.setdefault(seq, set()).add(key.tag(norm) if norm else None)
    index = {}
    for seq, tags in by_seq.items():
        if len(tags) > 1:
            tags.discard(None)  # a dump file's name with no ID, and the same name with one: the tag is known
        index.setdefault(seq[0], []).append((seq, tags))
    for lst in index.values():
        lst.sort(key=lambda x: -len(x[0]))
    return index


def find_names(text, index):
    """(line, the words found, {tag or None}) of each game name of index in text."""
    ws, out, i = words(text), [], 0
    while i < len(ws):
        for seq, tags in index.get(ws[i][0], ()):
            if tuple(w for w, _ in ws[i : i + len(seq)]) == seq:
                out.append((ws[i][1], ' '.join(seq), tags))
                i += len(seq)
                break
        else:
            i += 1
    return out


# --- commands -------------------------------------------------------------------------------------------------------


def cmd_tag(args):
    key = Key(args.key, make=True)
    norms = [normal(a) for a in args.ids]  # every ID checked before any tag is printed
    for n in norms:
        print(key.tag(n))
    return 0


def cmd_which(args):
    key = Key(args.key, make=False)
    for a in args.tags:
        if not TAG.fullmatch(a):
            raise TagError(f"'{a}' is not a tag: game- and 8 hex digits in lower case")
    if not args.games:
        raise TagError('which searches the folders --games names, and none is given: which --games DIR TAG...')
    games = local_games(args.games)
    by_tag = {}
    for norm, name, _, rank in games:
        if norm:
            by_tag.setdefault(key.tag(norm), {}).setdefault(norm, []).append((rank, name))
    rc = 0
    for a in args.tags:
        hits = by_tag.get(a)
        if not hits:
            n = len({norm for norm, _, _, _ in games if norm})
            print(f'{a} not found (the tags of {n} IDs under {", ".join(args.games)} were tried)')
            rc = 1
            continue
        for norm, names in sorted(hits.items()):
            seen, shown = set(), []
            for _, name in sorted(names, key=lambda x: x[0]):
                f = ' '.join(w for w, _ in words(name or ''))
                if name and f not in seen:
                    seen.add(f)
                    shown.append(name)
            print(f'{a} {norm} {" | ".join(shown) or "(no name there)"}')
    return rc


def cmd_scan(args):
    key = Key(args.key, make=True)
    texts = []
    for a in args.files:
        try:
            if a == '-':
                texts.append(('-', sys.stdin.read()))
            else:
                with open(a, encoding='utf-8', errors='replace') as f:
                    texts.append((a, f.read()))
        except OSError as e:
            raise TagError(f'cannot read {a}: {e.strerror}')
    index = name_index(local_games(args.games), key)
    hits = 0
    for path, text in texts:
        found = [(n, 0, f'{key.tag(norm)} {raw}') for n, raw, norm in find_ids(text)]
        for n, seen, tags in find_names(text, index):
            shown = sorted(t for t in tags if t) + (['untagged (no ID there)'] if None in tags else [])
            found.append((n, 1, f'{", ".join(shown)} "{seen}"'))
        for n, _, what in sorted(found):
            print(f'{path}:{n}: {what}')
        hits += len(found)
    if hits:
        note(f'{hits} title ID(s) or game name(s) in {len(texts)} file(s)')
    return 1 if hits else 0


def main(argv):
    parser = argparse.ArgumentParser(
        prog='game_tag.py', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    key = argparse.ArgumentParser(add_help=False)
    key.add_argument('--key', metavar='FILE', help="the key's file (default: game-tag.key of the user's config folder)")
    games = argparse.ArgumentParser(add_help=False)
    games.add_argument(
        '--games', metavar='DIR', action='append', default=[], help='a folder of games; it may be given more than once'
    )
    commands = parser.add_subparsers(dest='command', required=True, metavar='tag | which | scan')
    sub = commands.add_parser('tag', parents=[key], help='the tag of each ID')
    sub.add_argument('ids', metavar='ID', nargs='+')
    sub.set_defaults(run=cmd_tag)
    sub = commands.add_parser('which', parents=[key, games], help='the ID and the name of each tag')
    sub.add_argument('tags', metavar='TAG', nargs='+')
    sub.set_defaults(run=cmd_which)
    sub = commands.add_parser('scan', parents=[key, games], help='the title IDs and game names in each file')
    sub.add_argument('files', metavar='FILE', nargs='+')
    sub.set_defaults(run=cmd_scan)
    args = parser.parse_args(argv)  # a bad option or no command: its message and exit 2
    try:
        return args.run(args)
    except TagError as e:
        note(e)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
