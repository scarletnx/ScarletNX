#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The functions games export that no hook runs yet: what is left to implement natively.

    tools/export_census.py extract [--dumps /tank/switch] [--out work/exefs] [--keys prod.keys] [--only TITLE ...]
    tools/export_census.py report work/exefs/TITLE [...]
    tools/export_census.py summary work/exefs/*

extract: each dump's ExeFS alone (scarletnx -x --exefs-only), the smallest dump first, into OUT/<title ID>, with the
game's name in name.txt; one extracted already is left as it is.

report: each module's exported functions (a dynamic symbol of a function, as --dump-modules reads them: the only
names an NSO carries), each marked hooked when a .hook file under src/ declares a hook for its symbol, and sorted by
what it is: libc (a C function the host's C library has too), libc++ (std::, operator new and delete, the C++ ABI's
__cxa), nn (the SDK), nvn, or other (a game's or a middleware's), with a tag for the libraries a native one could take
over (jpeg, png, zlib...). Writes GAME/exports.tsv (every function) and GAME/left.txt (those left, by module and
kind, demangled), and prints the counts.

summary: the functions left across the games given, with how many games export each, into OUT/left_all.tsv (OUT the
folder holding the games), the most common first.

The program is build/scarletnx, or SNX_SCARLETNX. Pure Python, and llvm-cxxfilt or c++filt to demangle when one is
on the PATH.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path[:0] = [HERE]
import moduledump  # noqa: E402

STT_FUNC = 2
# libraries a native implementation could take over, by what their functions' names hold
TAGS = [('jpeg', re.compile(r'jpeg|jpg|JPEG|Jpeg|jinit_|jcopy_|jpeg_')), ('png', re.compile(r'png', re.I)),
        ('zlib', re.compile(r'^(deflate|inflate|compress|uncompress|crc32|adler32|zlib|gz)', re.I)),
        ('zstd', re.compile(r'zstd|ZSTD')), ('lz4', re.compile(r'LZ4|lz4')), ('opus', re.compile(r'opus', re.I)),
        ('vorbis', re.compile(r'vorbis|^ov_', re.I)), ('freetype', re.compile(r'^FT_|FreeType')),
        ('webp', re.compile(r'webp', re.I)), ('ssl', re.compile(r'^(SSL_|EVP_|BIO_|X509|RSA_|SHA\d|MD5)')),
        ('curl', re.compile(r'^curl_')), ('lua', re.compile(r'^lua')), ('mono', re.compile(r'^mono_|Mono')),
        ('unity', re.compile(r'Unity|il2cpp')), ('havok', re.compile(r'^_ZN\d+hk[a-z]?[A-Z]|^hk[a-z]?[A-Z]\w*::|Havok'))]


def hooked_symbols():
    """The symbols a hook declaration under src/ names (hookgen's 'hook SYMBOL(' lines, versioned or not)"""
    found = set()
    for root, _, files in os.walk(os.path.join(REPO, 'src')):
        for f in files:
            if not f.endswith('.hook'):
                continue
            with open(os.path.join(root, f), encoding='utf-8') as spec:
                for line in spec:
                    m = re.match(r'\s*(?:@version\([^)]*\)\s*)?hook\s+([^\s(]+)\(', line)
                    if m:
                        found.add(m.group(1))
    return found


def host_libc():
    """The functions the host's C library exports (nm -D), for telling a game's C library functions apart"""
    for path in ('/lib/libc.so.7', '/lib/x86_64-linux-gnu/libc.so.6', '/usr/lib/libc.so.6', '/lib64/libc.so.6'):
        if not os.path.isfile(path):
            continue
        nm = shutil.which('llvm-nm') or shutil.which('nm')
        if not nm:
            break
        out = subprocess.run([nm, '-D', '--defined-only', path], capture_output=True, text=True, check=False).stdout
        return {line.split()[-1].split('@')[0] for line in out.splitlines() if line.split()}
    return set()


def demangle(names):
    tool = shutil.which('llvm-cxxfilt') or shutil.which('c++filt')
    if not tool or not names:
        return {n: n for n in names}
    out = subprocess.run([tool], input='\n'.join(names) + '\n', capture_output=True, text=True, check=False).stdout
    lines = out.splitlines()
    return {n: (lines[i] if i < len(lines) else n) for i, n in enumerate(names)}


def kind_of(name, plain, libc):
    if not name.startswith('_Z'):
        if name in libc or name.startswith('__cxa_') or name.startswith('_Unwind'):
            return 'libc++' if name.startswith(('__cxa_', '_Unwind')) else 'libc'
        if name.startswith('nvn'):
            return 'nvn'
        if name.startswith('nn'):
            return 'nn'
        return 'other'
    if plain.startswith(('std::', 'operator new', 'operator delete', '__cxxabiv1::', 'typeinfo', 'vtable for std::')):
        return 'libc++'
    if plain.startswith('nn::') or 'nn::' in plain.split('(')[0][:8]:
        return 'nn'
    if plain.startswith('nvn'):
        return 'nvn'
    return 'other'


def tag_of(name, plain):
    for tag, pattern in TAGS:
        if pattern.search(name) or pattern.search(plain):
            return tag
    return ''


def game_name(game):
    try:
        with open(os.path.join(game, 'name.txt'), encoding='utf-8') as f:
            return f.read().strip()
    except OSError:
        return os.path.basename(os.path.normpath(game))


def report(game, hooked, libc, quiet=False):
    os.environ.setdefault('SNX_SCARLETNX', os.path.join(REPO, 'build', 'rel', 'scarletnx'))
    folder = moduledump._dump(game)
    rows = []
    with open(os.path.join(folder, 'modules.txt'), encoding='utf-8') as f:
        module = None
        for line in f:
            kind, _, rest = line.rstrip('\n').partition(' ')
            if kind == 'module':
                module = rest.split()[0]
            elif kind == 'symbol' and module:
                _, _, stype, name = rest.split(' ', 3)
                if int(stype) == STT_FUNC:
                    rows.append((module, name))
    plain = demangle(sorted({n for _, n in rows}))
    out = []
    for module, name in rows:
        p = plain.get(name, name)
        out.append((module, kind_of(name, p, libc), 'hooked' if name in hooked else 'left', tag_of(name, p), name, p))
    with open(os.path.join(game, 'exports.tsv'), 'w', encoding='utf-8') as f:
        f.write('module\tkind\tstate\ttag\tsymbol\tdemangled\n')
        for r in out:
            f.write('\t'.join(r) + '\n')
    with open(os.path.join(game, 'left.txt'), 'w', encoding='utf-8') as f:
        f.write('# %s: the exported functions no hook runs, by module and kind\n' % game_name(game))
        for module in dict.fromkeys(r[0] for r in out):
            for kind in ('libc', 'libc++', 'nn', 'nvn', 'other'):
                left = sorted((r for r in out if r[0] == module and r[1] == kind and r[2] == 'left'),
                              key=lambda r: r[5])
                if not left:
                    continue
                f.write('\n## %s: %s (%d)\n' % (module, kind, len(left)))
                for r in left:
                    tag = ' [%s]' % r[3] if r[3] else ''
                    f.write('%s%s\n' % (r[5], tag) if r[5] == r[4] else '%s%s\t%s\n' % (r[5], tag, r[4]))
    if not quiet:
        print('%s (%s)' % (game_name(game), game))
        print('  %-10s %8s %8s %8s %8s %8s %8s   %s' % ('module', 'funcs', 'hooked', 'libc', 'libc++', 'nn', 'nvn',
                                                     'other (tags)'))
        for module in dict.fromkeys(r[0] for r in out):
            mine = [r for r in out if r[0] == module]
            left = [r for r in mine if r[2] == 'left']
            by = {k: sum(1 for r in left if r[1] == k) for k in ('libc', 'libc++', 'nn', 'nvn', 'other')}
            tags = {}
            for r in left:
                if r[3]:
                    tags[r[3]] = tags.get(r[3], 0) + 1
            print('  %-10s %8d %8d %8d %8d %8d %8d   %d %s' % (
                module, len(mine), len(mine) - len(left), by['libc'], by['libc++'], by['nn'], by['nvn'], by['other'],
                ' '.join('%s:%d' % kv for kv in sorted(tags.items(), key=lambda kv: -kv[1]))))
        print('  left: %s' % os.path.join(game, 'left.txt'))
    return out


def extract(args):
    tool = os.environ.get('SNX_SCARLETNX', os.path.join(REPO, 'build', 'rel', 'scarletnx'))
    dumps = [os.path.join(args.dumps, f) for f in os.listdir(args.dumps)
             if f.lower().endswith(('.nsp', '.xci')) and os.path.isfile(os.path.join(args.dumps, f))]
    dumps.sort(key=os.path.getsize)
    if args.only:
        want = {t.lower() for t in args.only}
        dumps = [d for d in dumps if os.path.splitext(os.path.basename(d))[0].lower() in want]
    os.makedirs(args.out, exist_ok=True)
    for dump in dumps:
        title = os.path.splitext(os.path.basename(dump))[0].upper()
        game = os.path.join(args.out, title)
        if os.path.isfile(os.path.join(game, 'exefs', 'main')):
            print('%s: extracted already' % title)
            continue
        if os.path.isdir(game):
            shutil.rmtree(game)
        cmd = [tool, '-x', dump, '--exefs-only', '--out', game]
        if args.keys:
            cmd += ['--keys', args.keys]
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        m = re.search(r'The game: (.*?), version', result.stdout)
        if result.returncode or not os.path.isfile(os.path.join(game, 'exefs', 'main')):
            print('%s: not extracted: %s' % (title, (result.stderr or result.stdout).strip().splitlines()[-1:]))
            continue
        with open(os.path.join(game, 'name.txt'), 'w', encoding='utf-8') as f:
            f.write((m.group(1) if m else title) + '\n')
        print('%s: %s (%d MB dump)' % (title, m.group(1) if m else '?', os.path.getsize(dump) >> 20))


def summary(games, hooked, libc):
    counts, where = {}, {}
    for game in games:
        rows = report(game, hooked, libc, quiet=True)
        for r in {(r[1], r[3], r[4], r[5]) for r in rows if r[2] == 'left'}:
            counts[r] = counts.get(r, 0) + 1
            where.setdefault(r, []).append(os.path.basename(os.path.normpath(game)))
    out = os.path.join(os.path.dirname(os.path.normpath(games[0])), 'left_all.tsv')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('games\tkind\ttag\tsymbol\tdemangled\n')
        for r in sorted(counts, key=lambda r: (-counts[r], r[0], r[3])):
            f.write('%d\t%s\t%s\t%s\t%s\n' % (counts[r], r[0], r[1], r[2], r[3]))
    by = {}
    for r, n in counts.items():
        by[r[0]] = by.get(r[0], 0) + 1
    print('%d games: %d distinct functions left (%s), in %s' % (
        len(games), len(counts), ', '.join('%s %d' % kv for kv in sorted(by.items())), out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    x = sub.add_parser('extract')
    x.add_argument('--dumps', default='/tank/switch')
    x.add_argument('--out', default=os.path.join(REPO, 'work', 'exefs'))
    x.add_argument('--keys')
    x.add_argument('--only', nargs='*')
    r = sub.add_parser('report')
    r.add_argument('games', nargs='+')
    s = sub.add_parser('summary')
    s.add_argument('games', nargs='+')
    args = ap.parse_args()
    if args.cmd == 'extract':
        extract(args)
        return
    hooked, libc = hooked_symbols(), host_libc()
    if args.cmd == 'report':
        for game in args.games:
            report(game, hooked, libc)
    else:
        summary([g for g in args.games if os.path.isdir(os.path.join(g, 'exefs'))], hooked, libc)


if __name__ == '__main__':
    main()
