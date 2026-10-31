#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""How many of a game's functions and calls a hook could reach, per module: what a per-function hook check would
cost compared with routing only the calls to functions with a symbol through their dispatch slot (SNX_ABI 15).

    tools/hook_census.py work/Game [work/Other ...]
    tools/hook_census.py --markdown work/Game [...]       # one table row per module

For each A64 module: its functions (.eh_frame_hdr, the dynamic symbols in its text, and every BL target, as
tools/nvn_census.py finds them), those a dynamic symbol names (the only symbols an NSO carries: the exported ones,
which SNX_ABI 15 and 16 called through their slot before the route list, snx_codegen_routed), the direct calls (BL) and tail branches (a B to
another function's start), how many of them target a function with a symbol, and the indirect calls (BLR, which go
through dispatch already). A call through a slot costs a load and an indirect call instead of a direct call, and
keeps LLVM from inlining the callee; a check at every function's entry costs a load and a compare on every call,
direct or not. The counts are static: how often each call runs takes a profile. Pure Python, seconds a game.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE]
import moduledump  # noqa: E402
import nvn_census as nc  # noqa: E402


def census(game):
    mods, is64 = moduledump.load_program(game)
    rows = []
    if not is64:
        return rows, is64
    for m in mods:
        if m.name == 'rtld':
            continue
        text = nc.nso_segments(game, m.name)[0]
        code = nc.Code(m.image, text)
        funcs = nc.Functions(m, code)
        named = {v for v in m.syms if text[0] <= v < text[1]}
        starts = [s for s in funcs.starts if text[0] <= s < text[1]]
        bl = bl_named = tail = tail_named = 0
        for site, target, is_bl in code.branches():
            if not text[0] <= target < text[1]:
                continue
            if is_bl:
                bl += 1
                bl_named += target in named
            elif target in funcs.start_set and funcs.containing(site) != target:
                tail += 1
                tail_named += target in named
        blr = sum(1 for x in code.w if (x & 0xFFFFFC1F) == 0xD63F0000)
        rows.append({'module': m.name, 'functions': len(starts), 'named': len(named), 'bl': bl, 'bl_named': bl_named,
                     'tail': tail, 'tail_named': tail_named, 'blr': blr, 'text': text[1] - text[0]})
    return rows, is64


def pct(a, b):
    return '%.1f%%' % (100.0 * a / b) if b else '-'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('games', nargs='+')
    ap.add_argument('--markdown', action='store_true')
    args = ap.parse_args()
    if args.markdown:
        print('| Game | Module | Text MB | Functions | With a symbol | BL | BL to a symbol | Tail B '
              '| Tail B to a symbol | BLR |')
        print('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    for game in args.games:
        name = os.path.basename(os.path.normpath(game))
        rows, is64 = census(game)
        if not is64:
            print('%s: a 32-bit program, not scanned' % name)
            continue
        total = {k: sum(r[k] for r in rows) for k in ('functions', 'named', 'bl', 'bl_named', 'tail', 'tail_named',
                                                      'blr', 'text')}
        total['module'] = 'all'
        for r in rows + [total]:
            if args.markdown:
                print('| %s | %s | %.1f | %d | %d (%s) | %d | %d (%s) | %d | %d (%s) | %d |' % (
                    name, r['module'], r['text'] / 1048576.0, r['functions'], r['named'],
                    pct(r['named'], r['functions']), r['bl'], r['bl_named'], pct(r['bl_named'], r['bl']), r['tail'],
                    r['tail_named'], pct(r['tail_named'], r['tail']), r['blr']))
            else:
                print('%s %-8s functions %7d, with a symbol %6d (%s); BL %8d, to a symbol %7d (%s); tail B %6d, to a '
                      'symbol %5d; BLR %6d' % (name, r['module'], r['functions'], r['named'],
                                               pct(r['named'], r['functions']), r['bl'], r['bl_named'],
                                               pct(r['bl_named'], r['bl']), r['tail'], r['tail_named'], r['blr']))


if __name__ == '__main__':
    main()
