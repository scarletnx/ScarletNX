#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The NVN procedures a game's driver has that nvn_direct doesn't implement: what a run stops on next.

    tools/nvn_missing.py work/Game [work/Other ...]       # per game: the driver's table against gpu/nvn/nvn.hook
    tools/nvn_missing.py --all work/Game [...]            # also what the hook answers that this driver has not
    tools/nvn_missing.py --markdown work/Game [...]       # one table row per game before the details

The game folder is the extracted dump (exefs/ + romfs/), read through `scarletnx --dump-modules`
(tools/moduledump.py) as tools/nvn_census.py reads it: the NVN module, its procedure table (the names of its lookup
pool, without the "nvn" prefix) and the API version nvnDeviceGetInteger's switch answers. Each name is looked for in
gpu/nvn/nvn.hook as that file has it, and a name is implemented when one of its entries holds for the game's API
version by hook_select's rule (src/nx/hle/hook/version.c): an entry without @version always holds, an nvn entry holds
while the version lies in its [lo, hi), and an sdk entry never holds for the NVN module, which carries no SDK label.
The `route` lines name the two resolvers, which gpu/nvn/nvn_direct.c wraps with code of its own: covered.

The names left are what a run stops on: "nvn: the game called nvnX, which nvn_direct doesn't implement yet"
(gpu/nvn/nvn_direct.c), one fatal for the first one the game calls. The runtime also logs each name it resolved but
doesn't implement, as it resolves it: this tool says the same from the driver alone, before a run, and for the
functions the game never gets to.

Pure Python, seconds a game. Its output names the game's functions and addresses: it stays on this machine, like the
rest of a game.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE]
import moduledump  # noqa: E402
import nvn_census as nc  # noqa: E402

HOOK = os.path.join(HERE, '..', 'src', 'nx', 'gpu', 'nvn', 'nvn.hook')
UNBOUNDED = 0xFFFFFFFF


class Fail(Exception):
    """What this tool can't read: an @version it can't parse, or a hook file it can't open"""


# ---------------------------------------------------------------------------------------------------------------
# gpu/nvn/nvn.hook


def version_value(key, text):
    """A version of key packed as hook.h's hook_version_text reads it: major.minor for nvn, x.y.z for sdk"""
    parts = text.split('.')
    if not all(p.isdigit() for p in parts) or len(parts) > (3 if key == 'sdk' else 2):
        raise Fail('malformed %s version "%s"' % (key, text))
    v = [int(p) for p in parts] + [0, 0]
    return v[0] << 16 | (v[2] if key == 'sdk' else v[1])


def version_range(text):
    """(key, lo, hi) of an @version(...) text, as tools/hookgen/hookgen.c parses it: key, and the versions [lo, hi)
    that hold"""
    key, lo, hi = 'sdk', 0, UNBOUNDED
    body = text[text.index('(') + 1 : text.rindex(')')]
    for part in body.split(','):
        words = part.split()
        if words and words[0] in ('sdk', 'nvn') and len(words) > 1:  # the key, before its comparison
            key = words[0]
            words = words[1:]
        for op in ('>=', '<=', '==', '>', '<'):
            if op in ' '.join(words):
                at = ' '.join(words).index(op)
                v = version_value(key, ' '.join(words)[at + len(op) :].strip())
                break
        else:
            raise Fail('no comparison in "@version(%s)"' % part)
        if op in ('>=', '=='):
            lo = max(lo, v)
        elif op == '>':
            lo = max(lo, v + 1)
        if op in ('<=', '=='):
            hi = min(hi, v + 1)
        elif op == '<':
            hi = min(hi, v)
    if lo >= hi:
        raise Fail('no version satisfies "@version(%s)"' % body)
    return key, lo, hi


def hook_entries(path=HOOK):
    """({name: [(key, lo, hi), ...]}, [routed names]): every hook gpu/nvn/nvn.hook declares with its version ranges,
    and the resolvers its route lines name"""
    entries, routed, pending = {}, [], None
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            text = line.strip()
            if not text or text.startswith('#'):  # hookgen's grammar: a comment is a line of its own
                continue
            try:
                if text.startswith('@version'):
                    pending = version_range(text)
                    continue
                word, _, rest = text.partition(' ')
                if word == 'route':
                    name = rest.split('(', 1)[0].strip()
                    if not name.startswith('nvn'):
                        raise Fail('route names %s, not an nvn function' % name)
                    routed.append(name)
                elif word == 'hook':
                    name = text[len('hook') :].split('(', 1)[0].strip()
                    if not name.startswith('nvn'):
                        raise Fail('hook names %s, not an nvn function' % name)
                    entries.setdefault(name, []).append(pending)
                elif word in ('family', 'include'):
                    pass
                else:
                    raise Fail('"%s" is not a family, a hook, a route or an @version' % word)
                pending = None  # an @version holds for the declaration after it alone
            except Fail as e:
                raise Fail('%s line %d: %s' % (os.path.basename(path), n, e))
    return entries, routed


def implemented(entries, name, api):
    """1 when one of name's entries holds for the API version api (0: unknown), as hook_select picks one. An entry
    without @version is (None,)"""
    for entry in entries.get(name, []):
        if entry is None or (entry[0] == 'nvn' and api and entry[1] <= api < entry[2]):
            return 1
    return 0


def api_text(api):
    return 'unknown' if not api else '%d.%d' % (api >> 16, api & 0xFFFF)


# ---------------------------------------------------------------------------------------------------------------
# a game's driver


def driver(game):
    """({'names': [...], 'api': the API version packed, or 0, 'version': the NVN middleware label, 'module': the
    NVN module's name, 'errors': [...]}) of a game's NVN driver"""
    r = {'names': [], 'api': 0, 'version': None, 'errors': []}
    mods, is64 = moduledump.load_program(game)
    label = nc.versions(mods).get('NVN')
    r['version'] = label[0] if label else None
    if not is64:
        r['errors'].append('32-bit program: the code scans decode A64 only')
        return r
    nvn = next((m for m in mods if m.name != 'rtld' and re.search(rb'\0' + nc.ANCHOR + rb'\0', m.image)), None)
    if nvn is None:
        r['errors'].append('no module holds the NVN name pool ("%s")' % nc.ANCHOR.decode())
        return r
    r['module'] = nvn.name
    segs = nc.nso_segments(game, nvn.name)
    code = nc.Code(nvn.image, segs[0])
    try:
        table = nc.find_table(nvn, True, segs, nc.relocations(nvn.image, True), code)
    except nc.Fail as e:
        r['errors'].append('procedure table: %s' % e)
        return r
    r['names'] = table['names']
    api = nc.api_version(nvn, code, table)
    r['api'] = api[0] << 16 | api[1] if api else 0
    return r


def missing_of(d, entries, routed):
    """(the names of d's driver the hook doesn't answer for its API version, as nvn.hook spells them, and the names
    it answers that this driver has not)"""
    have = set(d['names'])
    missing = sorted('nvn' + n for n in have
                     if 'nvn' + n not in set(routed) and not implemented(entries, 'nvn' + n, d['api']))
    extra = sorted(n for n in entries if n[3:] not in have)
    return missing, extra


# ---------------------------------------------------------------------------------------------------------------
# the reports


def game_report(game, d, entries, routed, args, out):
    w = out.write
    name = os.path.basename(os.path.normpath(game))
    w('== %s (NVN %s, API %s)\n' % (name, d['version'] or '?', api_text(d['api'])))
    for e in d['errors']:
        w('  FAILED %s\n' % e)
    if d['errors']:
        return
    missing, extra = missing_of(d, entries, routed)
    have = set(d['names'])
    w('  procedure table in %s: %d entries, %d of them answered by gpu/nvn/nvn.hook for API %s\n'
      % (d['module'], len(set(d['names'])), len(set(d['names'])) - len(missing), api_text(d['api'])))
    w('  missing: %d (a run stops on the first one the game calls)\n' % len(missing))
    for n in missing:
        w('    %s\n' % n)
    if args.all:
        others = [n for n in extra + routed if n[3:] not in set(d['names'])]
        w('  not in this driver: %d the hook answers for API %s, %d of them routed\n'
          % (len(others), api_text(d['api']), len([n for n in others if n in set(routed)])))
        if args.names:
            for n in sorted(others):
                w('    %s%s\n' % (n, ' (routed)' if n in set(routed) else ''))


def markdown_row(game, d, entries, routed):
    missing, _ = missing_of(d, entries, routed)
    return '| %s | %s | %s | %d | %d | %s |' % (
        os.path.basename(os.path.normpath(game)), d['version'] or '?', api_text(d['api']), len(set(d['names'])),
        len(missing), ', '.join(missing[:6]) + (' ...' if len(missing) > 6 else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('games', nargs='+')
    ap.add_argument('--all', action='store_true', help='also list what the hook answers that the driver has not')
    ap.add_argument('--names', action='store_true', help='name them too (with --all)')
    ap.add_argument('--markdown', action='store_true', help='one table row per game before the details')
    args = ap.parse_args()
    try:
        entries, routed = hook_entries()
    except (Fail, OSError) as e:
        raise SystemExit(str(e))
    facts = []
    for game in args.games:
        try:
            facts.append((game, driver(game)))
        except (nc.Fail, SystemExit) as e:
            facts.append((game, {'names': [], 'api': 0, 'version': None, 'module': '?',
                                 'errors': [str(e).strip().split('\n')[0]]}))
    if args.markdown:
        print('| Game | NVN label | API | Table entries | Missing | First missing |')
        print('| --- | --- | --- | --- | --- | --- |')
        for game, d in facts:
            print(markdown_row(game, d, entries, routed))
        print()
        print('```')
    for game, d in facts:
        game_report(game, d, entries, routed, args, sys.stdout)
    if args.markdown:
        print('```')
    return 0


if __name__ == '__main__':
    sys.exit(main())
