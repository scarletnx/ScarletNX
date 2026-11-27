#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Which of a namespace's SDK functions ask a service, by SDK version: the functions a native namespace has to hook
before the service's session can go (hle/services/sm.c's g_native_only).

    tools/ipc_reach.py nn::settings work/exefs/*           # those with no hook yet
    tools/ipc_reach.py --all --nested nn::aoc work/exefs/* # every one, those of the names inside it too

NAMESPACE is nn::settings or its mangled form 2nn8settings. Each GAME is an extracted dump (a folder with exefs/) or
an NRO, which tools/moduledump.py reads through `scarletnx --dump-modules`, or a folder that command wrote
(modules.txt and the modules' images, as its cache keeps them). In every 64-bit module, an exported function of the
namespace itself (_ZN or _ZNK, the namespace, one name; with --nested also of a namespace or a class inside it, such
as nn::settings::detail, a constructor or destructor among them) asks a session when its code
  - calls through a register (blr or br, not to x30): a method of a service object, as every SDK client calls one,
  - or calls a function named Create...ByHipc, Create...ByDfc or Get...Service (sf's proxies, a service's getter);
with --deep also through a function with no symbol, or a detail one of any namespace, that it calls, three calls
deep. A function that reaches a session only through another exported function of the namespace is that one's to
hook, and is not listed. Calls are read through the module's PLT: a stub's GOT slot names its symbol by relocation.

Prints each function a .hook file under src/ (or under --hooks DIR) doesn't declare, or every one with --all:
    SYMBOL | why | the SDK versions whose games have it | hooked (with --all)
sorted by symbol, then a line with the games and modules read. Reads only.
"""

import argparse
import bisect
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path[:0] = [HERE]
import moduledump
from nvn_census import dynamic, dynsym, reloc_entries

RET = 0xD65F03C0
R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT = 1025, 1026
MAX_FUNCTION = 0x2000  # bytes: a symbol said larger is no client function, and is not read
EXTENT_MAX = 0x800  # bytes read of a function with no symbol, up to the next symbol or its last ret
DEEP = 3  # calls followed with --deep
ASKS = re.compile(r'Create\w*By(Hipc|Dfc)|Get\w*ServiceE')
SDK_VERSION = re.compile(rb'_nnSdk-(?:[A-Za-z]+_)?(\d+)_(\d+)_(\d+)')


def mangled(namespace):
    """2nn8settings from nn::settings (a mangled one as it is)"""
    if '::' not in namespace:
        return namespace
    return ''.join(f'{len(part)}{part}' for part in namespace.split('::'))


def hooked(folder):
    """The symbols the .hook files under folder declare a hook for (hookgen's 'hook SYMBOL(' lines)"""
    found = set()
    for root, _, files in os.walk(folder):
        for name in files:
            if name.endswith('.hook'):
                with open(os.path.join(root, name), encoding='utf-8') as spec:
                    found |= set(re.findall(r'^\s*hook (\S+?)\(', spec.read(), re.MULTILINE))
    return found


def load(game):
    """(modules, is64) of a game, or of a folder --dump-modules wrote"""
    if os.path.isfile(os.path.join(game, 'modules.txt')):
        return moduledump.load_folder(game)
    return moduledump.load_program(game)


def plt_names(image):
    """{module offset of a GOT slot: the symbol its JUMP_SLOT or GLOB_DAT relocation names}"""
    tags = dynamic(image, True)
    return {
        off: dynsym(image, True, tags, sym)[0]
        for off, kind, sym, _ in reloc_entries(image, True, tags)
        if sym and kind in (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT)
    }


def word(image, off):
    return struct.unpack_from('<I', image, off)[0]


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Reader:
    """One module's functions, read for the calls that ask a session"""

    def __init__(self, mod):
        self.image = mod.image
        self.byaddr = mod.syms
        self.starts = sorted(mod.syms)
        self.got = plt_names(mod.image)
        self.memo = {}

    def target(self, at):
        """The symbol a call to at lands in: the module's own, or the one a PLT stub there goes to; '' for none"""
        if at in self.byaddr:
            return self.byaddr[at]
        if at < 0 or at + 8 > len(self.image):
            return ''
        adrp, ldr = word(self.image, at), word(self.image, at + 4)
        if adrp & 0x9F00001F == 0x90000010 and ldr & 0xFFC003FF == 0xF9400211:  # adrp x16; ldr x17, [x16, #lo]
            page = (at & ~0xFFF) + (signed(((adrp >> 5) & 0x7FFFF) << 2 | (adrp >> 29) & 3, 21) << 12)
            return self.got.get(page + ((ldr >> 10) & 0xFFF) * 8, '')
        return ''

    def extent(self, at):
        """The end of a function with no symbol: the next symbol, or the ret (or tail branch) no branch jumps past"""
        i = bisect.bisect_right(self.starts, at)
        hi = min(self.starts[i] if i < len(self.starts) else len(self.image), at + EXTENT_MAX, len(self.image) - 3)
        far = x = at
        while x < hi:
            w = word(self.image, x)
            if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:  # b.cond, cbz, cbnz
                far = max(far, x + signed((w >> 5) & 0x7FFFF, 19) * 4)
            elif w & 0x7E000000 == 0x36000000:  # tbz, tbnz
                far = max(far, x + signed((w >> 5) & 0x3FFF, 14) * 4)
            elif (w == RET or w & 0xFC000000 == 0x14000000) and far <= x:
                return x + 4
            x += 4
        return hi

    def asks(self, lo, hi, depth):
        """Why the code in [lo, hi) asks a session, None when it doesn't"""
        why = None
        for at in range(lo, hi, 4):
            w = word(self.image, at)
            if w & 0xFFFFFC1F in (0xD63F0000, 0xD61F0000) and (w >> 5) & 31 != 30:  # blr, br: not a return
                return 'an indirect call'
            if w & 0x7C000000 != 0x14000000:  # b, bl
                continue
            to = at + signed(w & 0x3FFFFFF, 26) * 4
            if lo <= to < hi:
                continue
            name = self.target(to)
            if ASKS.search(name):
                return 'calls ' + name
            if (
                depth
                and not why
                and 0 <= to < len(self.image) - 8
                and (not name or ('6detail' in name and to in self.byaddr))
            ):
                if to not in self.memo:
                    self.memo[to] = None  # a call back into one being read finds nothing more
                    self.memo[to] = self.asks(to, self.extent(to), depth - 1)
                if self.memo[to]:
                    why = 'through {}: {}'.format(name or f'sub_{to:x}', self.memo[to])
        return why


def namespace_function(symbol, pattern, nested):
    """Whether symbol is a function of the namespace itself (or, nested, of a namespace or class inside it)"""
    hit = pattern.match(symbol)
    if not hit:
        return False
    rest = symbol[hit.end() + int(hit.group(1)) :]
    while nested:
        inner = re.match(r'(\d+)', rest)
        if not inner:
            break
        rest = rest[inner.end() + int(inner.group(1)) :]
    if nested and re.match(r'[CD][0-2]E', rest):  # a class's constructor or destructor
        rest = rest[2:]
    return rest.startswith('E')


def sdk_version(mods):
    """The SDK version its middleware string names, in the sdk module or else main; ? for none"""
    for name in ('sdk', 'main'):
        for mod in mods:
            hit = SDK_VERSION.search(mod.image) if mod.name == name else None
            if hit:
                return '.'.join(part.decode() for part in hit.groups())
    return '?'


def version_key(v):
    return tuple(int(x) if x.isdigit() else -1 for x in v.split('.'))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('namespace')
    p.add_argument('games', nargs='+')
    p.add_argument('--nested', action='store_true', help='the functions of the namespaces and classes inside it too')
    p.add_argument('--deep', action='store_true', help='also through functions with no symbol and detail ones')
    p.add_argument('--all', action='store_true', help='the hooked functions too')
    p.add_argument('--hooks', default=os.path.join(REPO, 'src'), help='the folder whose .hook files are read')
    args = p.parse_args(argv)
    pattern = re.compile(r'^_ZNK?' + re.escape(mangled(args.namespace)) + r'(\d+)')
    have = hooked(args.hooks)
    found, games, modules = {}, 0, 0
    for game in args.games:
        mods, is64 = load(game)
        games += 1
        if not is64:
            print(f'{game}: a 32-bit program, which this reads no A32 of', file=sys.stderr)
            continue
        sdk = sdk_version(mods)
        for mod in mods:
            names = [(off, name) for off, name in mod.syms.items() if namespace_function(name, pattern, args.nested)]
            if not names:
                continue
            modules += 1
            reader = Reader(mod)
            for off, name in names:
                size = mod.sizes.get(off, 0)
                if (name in have and not args.all) or not size or size > MAX_FUNCTION or off + size > len(mod.image):
                    continue
                why = reader.asks(off, off + size, DEEP if args.deep else 0)
                if why:
                    found.setdefault(name, [why, set()])[1].add(sdk)
    for name, (why, sdks) in sorted(found.items()):
        line = f'{name} | {why} | {",".join(sorted(sdks, key=version_key))}'
        print(line + (' | hooked' if name in have else '') if args.all else line)
    print(
        f'{len(found)} functions of {args.namespace} ask a session, in {games} games ({modules} modules with the '
        'namespace)',
        file=sys.stderr,
    )
    return 0


if __name__ == '__main__':
    sys.exit(main())
