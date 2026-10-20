#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The menu's languages (src/gui/i18n.h): its phrases listed from the code, and each translation compiled into it.

--pot writes po/scarletnx.pot from the phrases of src/gui (snx_tr, SNX_TR_NOOP, SNX_TR_KEYS, SNX_TR_KEY) and the
launcher's own rows in src/nx/runtime/settings.txt (their names, labels, help and sections, which its Settings show
through snx_tr); --check
fails when it is not what the code makes, or a po/*.po would not compile. --out FILE PO... makes FILE, the C source of
every language's catalog (the .mo format, read by snx_mo_lookup). A .po is refused when a translation changes the
printf conversions of its phrase (C's printf takes the arguments in order), changes a foot's keys, has a letter the
menu's font does not draw (scripts/build-glyphs.py), or is not a plain UTF-8 .po of single phrases.
"""

import argparse
import importlib.util
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = 'src/gui'
SETTINGS = 'src/nx/runtime/settings.txt'
POT = 'po/scarletnx.pot'
MARKS = {'snx_tr': None, 'SNX_TR_NOOP': None, 'SNX_TR_KEYS': 'snx-keys', 'SNX_TR_KEY': 'snx-key'}
CODE = re.compile(r'[a-z]{2,3}(_[A-Z]{2})?$')
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}
ESCAPES.update({c: c for c in '\\"\'?'})
CHAR = re.compile(r"'(?:\\.[^']*|[^'\\])'")


class Refused(Exception):
    pass


# ---- the phrases of the code ----
def blank_comments(text):
    """text with its comments as spaces, its strings and line ends where they were"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i : j + 1])
            i = j + 1
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def c_string(text, i, where):
    """the C string literal at text[i] (its opening quote) decoded, and where it ends"""
    raw = bytearray()
    i += 1
    while text[i] != '"':
        c = text[i]
        if c == '\n':
            raise Refused(f'{where}: a string goes past its line')
        if c != '\\':
            raw += c.encode()
            i += 1
            continue
        e = text[i + 1]
        if e in ESCAPES:
            raw += ESCAPES[e].encode()
            i += 2
        elif e in '01234567':
            digits = re.match(r'[0-7]{1,3}', text[i + 1 :]).group()
            raw.append(int(digits, 8))
            i += 1 + len(digits)
        elif e == 'x':
            digits = re.match(r'[0-9A-Fa-f]+', text[i + 2 :]).group()
            raw.append(int(digits, 16) & 0xFF)
            i += 2 + len(digits)
        else:
            raise Refused(f'{where}: an escape C does not have, \\{e}')
    return raw.decode(), i + 1


def phrases_of(text, name):
    """each marked phrase of a source: (phrase, flag or None, line)"""
    code = blank_comments(text)
    found = []
    for m in re.finditer(r'\b(' + '|'.join(MARKS) + r')\s*\(', code):
        line = code.count('\n', 0, m.start()) + 1
        where = f'{name}:{line}'
        i, depth, literal = m.end(), 1, False
        parts, pure = [], True
        while depth:  # the argument, to its closing parenthesis
            if i >= len(code):
                raise Refused(f'{where}: {m.group(1)}( is never closed')
            c = code[i]
            if c == '"':
                value, i = c_string(code, i, where)
                parts.append(value)
                literal = True
                continue
            if c == "'":
                i = CHAR.match(code, i).end()
                pure = False
                continue
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif not c.isspace() and not code.startswith('\\\n', i):  # a macro's line goes on
                pure = False
            i += 1
        if not literal:
            continue  # a phrase marked elsewhere, or the mark's own definition
        if not pure:
            raise Refused(f'{where}: {m.group(1)}() takes one phrase, its literals only, or none at all')
        found.append((''.join(parts), MARKS[m.group(1)], line))
    return found


def settings_phrases(root, phrases):
    """the launcher's rows of settings.txt (its launcher, recompile and llvm tables, and the sections marked launcher):
    each setting's name or label and its help on one line, as ini_list_text lists them, and the sections' names"""
    scope, help_lines, name = None, [], None

    def add(phrase):
        if phrase:
            flags, files = phrases.setdefault(phrase, (set(), []))
            if SETTINGS not in files:
                files.append(SETTINGS)

    def done():
        if help_lines:
            add(' '.join(help_lines))
        help_lines.clear()

    for raw in (root / SETTINGS).read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        word, _, rest = line.partition(' ')
        rest = rest.strip()
        if word in ('table', 'section', 'setting', 'list', 'include', 'repeat', 'end'):
            done()
        if word == 'table':
            scope = rest.split()[1] if len(rest.split()) > 1 else 'runtime'
        elif word == 'section' and rest.split()[-1] == 'launcher':
            head, _, tables = rest.partition(':')
            add(head.strip())
            for table in tables.split(','):
                if '/' in table:
                    add(table.split('/', 1)[1].replace('launcher', '').strip())
        elif scope in ('launcher', 'recompile', 'llvm'):
            if word == 'setting' and not rest.startswith('@'):
                add(rest)
            elif word in ('name', 'label'):
                add(rest)
            elif word == 'help':
                help_lines.append(rest)
    done()


def code_phrases(root):
    """every phrase of the code, in the order first met: {phrase: (flags, files)}"""
    phrases = {}
    for path in sorted((root / SOURCES).iterdir()):
        if path.suffix not in ('.c', '.h', '.m', '.cpp') or path.name == 'i18n.h':
            continue
        name = path.relative_to(root).as_posix()
        for phrase, flag, _ in phrases_of(path.read_text(encoding='utf-8'), name):
            if not phrase:
                raise Refused(f'{name}: an empty phrase')
            flags, files = phrases.setdefault(phrase, (set(), []))
            if flag:
                flags.add(flag)
            if name not in files:
                files.append(name)
    settings_phrases(root, phrases)
    return phrases


def po_quote(s):
    s = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r')
    lines = s.split('\n')
    if len(lines) == 1:
        return f'"{s}"'
    pieces = [line + '\\n' for line in lines[:-1]] + ([lines[-1]] if lines[-1] else [])
    return '""\n' + '\n'.join(f'"{p}"' for p in pieces)


def pot_text(phrases):
    out = [
        '# The phrases of the ScarletNX menu (src/gui), made by scripts/build-languages.py --pot. Do not edit.',
        '# SPDX-License-Identifier: GPL-3.0-or-later',
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: ScarletNX\\n"',
        '"Language: \\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"X-Language-Name: \\n"',
        '',
    ]
    for phrase, (flags, files) in phrases.items():
        if 'snx-keys' in flags:
            out.append(
                '#. Keys: "Key: what it does." sentences. Keep each key, or its translation in this file, and each ~'
            )
        if 'snx-key' in flags:
            out.append("#. A key of the foot's sentences, which a click finds by this word")
        out.append('#: ' + ' '.join(files))
        marks = sorted(flags) + (['c-format'] if conversions(phrase, 'the code') else [])
        if marks:
            out.append('#, ' + ', '.join(marks))
        out.append('msgid ' + po_quote(phrase))
        out.append('msgstr ""')
        out.append('')
    return '\n'.join(out)


# ---- a .po ----
def po_string(line, where):
    if not (line.startswith('"') and line.endswith('"') and len(line) >= 2):
        raise Refused(f'{where}: not a quoted string: {line}')
    body, out, i = line[1:-1], [], 0
    while i < len(body):
        c = body[i]
        if c == '"':
            raise Refused(f'{where}: a quote inside a string, unescaped')
        if c == '\\':
            if i + 1 >= len(body) or body[i + 1] not in 'ntr\\"':
                raise Refused(f'{where}: an escape a .po of this menu does not take')
            out.append({'n': '\n', 't': '\t', 'r': '\r'}.get(body[i + 1], body[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def read_po(path):
    """a .po's entries [(msgid, msgstr, flags, line)], refused when it is not a plain one"""
    try:
        text = path.read_text(encoding='utf-8')
    except UnicodeDecodeError as error:
        raise Refused(f'{path}: not UTF-8 ({error})')
    entries, entry, field = [], None, None
    for number, line in enumerate(text.splitlines() + [''], 1):
        where = f'{path}:{number}'
        line = line.strip()
        obsolete = line.startswith('#~')  # what msgmerge keeps of a phrase the code no longer has, its comments with it
        if not line or obsolete:  # an entry ends
            if entry is not None and ('msgid' in entry or not obsolete):
                if 'msgid' not in entry or 'msgstr' not in entry:
                    raise Refused(f'{path}:{entry["line"]}: an entry without its msgid and msgstr')
                entries.append((entry['msgid'], entry['msgstr'], entry['flags'], entry['line']))
            entry, field = None, None
            continue
        if entry is None:
            entry = {'flags': set(), 'line': number}
        if line.startswith('#'):
            if 'msgid' in entry:
                raise Refused(f'{where}: a comment inside an entry')
            if line.startswith('#,'):
                entry['flags'].update(f.strip() for f in line[2:].split(',') if f.strip())
            continue
        keyword = line.split(None, 1)[0]
        if keyword in ('msgctxt', 'msgid_plural') or keyword.startswith('msgstr['):
            raise Refused(f'{where}: {keyword}: this menu takes single phrases, no contexts or plurals')
        if keyword in ('msgid', 'msgstr'):
            if keyword in entry or (keyword == 'msgstr' and 'msgid' not in entry):
                raise Refused(f'{where}: {keyword} out of place')
            field = keyword
            entry[field] = po_string(line[len(keyword) :].strip(), where)
        elif line.startswith('"') and field:
            entry[field] += po_string(line, where)
        else:
            raise Refused(f'{where}: not a line of a .po: {line}')
    return entries


def header_of(msgstr):
    fields = {}
    for line in msgstr.split('\n'):
        if ':' in line:
            key, value = line.split(':', 1)
            fields[key.strip()] = value.strip()
    return fields


# ---- what a translation must keep ----
CONVERSION = re.compile(r'%([-+ #0\']*)(\*|\d+)?(?:\.(\*|\d*))?(hh|h|ll|l|j|z|t|L)?(.)', re.DOTALL)
CLASSES = {'d': 'signed', 'i': 'signed', 'o': 'unsigned', 'u': 'unsigned', 'x': 'unsigned', 'X': 'unsigned'}
CLASSES.update({c: 'double' for c in 'fFeEgGaA'})
CLASSES.update({'c': 'char', 's': 'string', 'p': 'pointer'})


def conversions(s, where):
    """the arguments s takes as a printf format, in order: a type each (a * width or precision an int)"""
    args, i = [], 0
    while True:
        i = s.find('%', i)
        if i < 0:
            return args
        if s.startswith('%%', i):
            i += 2
            continue
        m = CONVERSION.match(s, i)
        if not m or m.group(5) not in CLASSES:
            if m and m.group(5) in 'n$' or re.match(r'%\d+\$', s[i:]):
                raise Refused(f'{where}: {s!r}: %n and numbered arguments are not taken')
            i += 1  # a % of the text, as in "100%": not a conversion
            continue
        _, width, precision, length, conversion = m.groups()
        args += ['int'] * ((width == '*') + (precision == '*'))
        args.append((length or '') + ' ' + CLASSES[conversion])
        i = m.end()


def sentences_keys(s):
    """each sentence of a foot as main.c's hint_next reads it ("Keys: what. ~Keys: what."): whether it is greyed (~,
    hint_off), and its keys, [] for none"""
    sentences, p = [], 0
    while True:
        while p < len(s) and s[p] == ' ':
            p += 1
        if p >= len(s):
            return sentences
        greyed = s[p] == '~'
        p += greyed
        end = s.find('. ', p)
        if end < 0:
            end = len(s) - (s.endswith('.'))
        colon = s.find(': ', p)
        sentences.append((greyed, s[p:colon].split('/') if 0 <= colon < end else []))
        p = end + 1


def check_entry(msgid, msgstr, keys, words, where):
    if '%' in msgstr and '%' not in msgid:  # a phrase may be shown through a format
        raise Refused(f'{where}: {msgstr!r} has a % its English does not')
    if conversions(msgstr, where) != conversions(msgid, where):
        raise Refused(f'{where}: {msgstr!r} does not take the arguments {msgid!r} does, in the same order')
    if not keys:
        return
    english, translated = sentences_keys(msgid), sentences_keys(msgstr)
    greyed = [sum(g and not k for g, k in side) for side in (english, translated)]
    english, translated = [(g, k) for g, k in english if k], [(g, k) for g, k in translated if k]
    if (
        len(english) != len(translated)
        or any(len(a) != len(b) or g != h for (g, a), (h, b) in zip(english, translated))
        or greyed[0] != greyed[1]
    ):
        raise Refused(
            f'{where}: {msgstr!r} does not have the sentences with keys {msgid!r} has, "Keys: what." each, greyed (~)'
            ' where it has them'
        )
    for (_, a), (_, b) in zip(english, translated):
        for word, now in zip(a, b):
            if word in words and now not in (word, words[word]):  # a word of no key's is the translator's
                raise Refused(f'{where}: {now!r} in {msgstr!r} is not the key {word!r} nor its translation here')


def drawable(root):
    """the code points past ASCII the menu's font draws: CP437's, and the Latin and the wide letters kept from GNU
    Unifont"""
    spec = importlib.util.spec_from_file_location('build_glyphs', Path(__file__).with_name('build-glyphs.py'))
    glyphs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(glyphs)
    drawn = glyphs.codepage(root)
    for kept in (root / glyphs.KEPT, root / glyphs.WIDE_KEPT):
        drawn |= set(glyphs.read_hex(kept.read_text().splitlines(), kept))
    return drawn


def check_drawn(text, drawn, where):
    lacking = sorted({c for c in text if ord(c) > 127 and ord(c) not in drawn})
    if lacking:
        letters = ', '.join(f'U+{ord(c):04X}' for c in lacking)
        raise Refused(
            f"{where}: {text!r} has what the menu's font does not draw ({letters}): it would show as ?. "
            'scripts/build-glyphs.py --from takes from GNU Unifont the glyph of each letter the languages show'
        )


def compile_po(path, phrases, drawn):
    """a .po's code, name and catalog; the phrases the code no longer has, which it still translates. drawn: the code
    points past ASCII the menu's font has"""
    code = path.stem
    if not CODE.match(code) or code == 'en':
        raise Refused(f'{path}: a .po is named for its language, as pt_BR.po or de.po, and English is the code itself')
    entries = read_po(path)
    if not entries or entries[0][0] != '':
        raise Refused(f'{path}: no header (the msgid "" first)')
    header = header_of(entries[0][1])
    if header.get('Language') != code:
        raise Refused(f'{path}: its header says Language: {header.get("Language")}, not {code}')
    if 'charset=utf-8' not in header.get('Content-Type', '').lower().replace(' ', ''):
        raise Refused(f'{path}: its header must say Content-Type: text/plain; charset=UTF-8')
    name = header.get('X-Language-Name', '')
    if not name:
        raise Refused(f'{path}: its header has no X-Language-Name, the language as itself calls it')
    check_drawn(name, drawn, f'{path}: X-Language-Name')
    seen, catalog, stale = set(), {'': entries[0][1]}, []
    keys = {w for w, (flags, _) in phrases.items() if 'snx-key' in flags}
    words = {w: w for w in keys}  # a key not translated here stays itself
    words.update({m: t for m, t, flags, _ in entries[1:] if m in keys and t and 'fuzzy' not in flags})
    for msgid, msgstr, flags, line in entries[1:]:
        where = f'{path}:{line}'
        if not msgid:
            raise Refused(f'{where}: a second header')
        if msgid in seen:
            raise Refused(f'{where}: {msgid!r} twice')
        seen.add(msgid)
        if not msgstr or 'fuzzy' in flags:
            continue  # not translated yet: English
        known = msgid in phrases
        check_drawn(msgstr, drawn, where)
        check_entry(msgid, msgstr, known and 'snx-keys' in phrases[msgid][0], words, where)
        if not known:
            stale.append(msgid)
        catalog[msgid] = msgstr
    return code, name, mo_bytes(catalog), stale


def mo_bytes(catalog):
    """the catalog as GNU gettext's .mo, little-endian, its phrases sorted by their bytes, no hash table"""
    pairs = sorted((k.encode(), v.encode()) for k, v in catalog.items())
    n = len(pairs)
    originals, translations = 28, 28 + 8 * n
    at = 28 + 16 * n
    head, tables, strings = struct.pack('<7I', 0x950412DE, 0, n, originals, translations, 0, at), [[], []], b''
    for side in (0, 1):
        for pair in pairs:
            tables[side].append(struct.pack('<2I', len(pair[side]), at + len(strings)))
            strings += pair[side] + b'\0'
    return head + b''.join(tables[0]) + b''.join(tables[1]) + strings


def c_quote(s):
    return '"' + ''.join(chr(b) if 32 <= b < 127 and chr(b) not in '"\\?' else f'\\{b:03o}' for b in s.encode()) + '"'


def c_text(languages, folder):
    out = [
        f'/* Generated by scripts/build-languages.py from {folder}. Do not edit. */',
        '#include "gui/i18n.h"',
    ]
    for code, _, mo, _ in languages:
        out.append(f'static const unsigned char mo_{code}[] = {{')
        for i in range(0, len(mo), 16):
            out.append('    ' + ' '.join(f'0x{b:02x},' for b in mo[i : i + 16]))
        out.append('};')
    out.append('const SnxLanguage snx_gui_languages[] = {')
    out.append('    {"en", "English", NULL, 0},')
    for code, name, _, _ in languages:
        out.append(f'    {{{c_quote(code)}, {c_quote(name)}, mo_{code}, sizeof mo_{code}}},')
    out.append('};')
    out.append(f'const int snx_gui_language_count = {len(languages) + 1};')
    return '\n'.join(out) + '\n'


def pot_phrases(pot):
    """the phrases a .pot lists, with their flags, as code_phrases gives them"""
    phrases = {}
    for msgid, _, flags, _ in read_po(pot)[1:]:
        phrases[msgid] = (flags, [])
    return phrases


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT, help="the checkout (default: this script's)")
    parser.add_argument('--pot', action='store_true', help=f'write {POT} from the code')
    parser.add_argument('--check', action='store_true', help=f'fail when {POT} or a po/*.po is not what it should be')
    parser.add_argument('--out', type=Path, help='the C source of the catalogs of the .po files given')
    parser.add_argument('--phrases', type=Path, help="the .pot the .po files translate (default: the checkout's)")
    parser.add_argument('po', nargs='*', type=Path)
    args = parser.parse_args()
    try:
        if args.pot or args.check:
            text = pot_text(code_phrases(args.root))
            pot = args.root / POT
            if args.pot:
                pot.parent.mkdir(exist_ok=True)
                pot.write_bytes(text.encode())
                return 0
            if not pot.exists() or pot.read_bytes().decode().replace('\r\n', '\n') != text:
                raise Refused(f'{POT} is not what the code says: run scripts/build-languages.py --pot')
            phrases, drawn = pot_phrases(pot), drawable(args.root)
            for po in sorted((args.root / 'po').glob('*.po')):
                compile_po(po, phrases, drawn)
            return 0
        if not args.out:
            parser.error('--pot, --check or --out')
        phrases = pot_phrases(args.phrases or args.root / POT)
        drawn = drawable(args.root)
        languages = sorted((compile_po(po, phrases, drawn) for po in args.po), key=lambda language: language[0])
        for code, _, _, stale in languages:
            if stale:
                print(f'po/{code}.po: {len(stale)} translations of phrases the menu no longer has', file=sys.stderr)
        folders = sorted({po.resolve().parent.name + '/' for po in args.po}) or ['no .po']
        text = c_text(languages, ', '.join(folders))
        if not args.out.exists() or args.out.read_text() != text:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(text)
        return 0
    except Refused as refused:
        print(f'build-languages.py: {refused}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
