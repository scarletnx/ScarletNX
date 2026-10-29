#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""engines.c's g_m3d_group marks (G_ACTS) every 3D method below the macros' that does more than store its value.
maxwell3d_method stores any other method without its switch, so a case added to the switch, to keeps_draw or to
shadowed's exceptions without its entry in the table would be skipped silently. Source only, no compiler: the
register names are read from maxwell_3d_regs.h and engines.c's macros."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def body(src, start):
    """The text of the function whose definition starts with start, to its closing brace at column 0."""
    at = src.index(start)
    return src[at:src.index('\n}\n', at)]


def names(*texts):
    """The register names and their values: the enumerators given a value (maxwell_3d_regs.h's method indices), and
    the macros, object-like or of one parameter (engines.c's XFB_START_OFFSET), as text to evaluate"""
    consts, macros = {}, {}
    for text in texts:
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S).replace('\\\n', ' ')
        for name, value in re.findall(r'^\s*([A-Z][A-Z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)u?\s*,?\s*$', text, re.M):
            consts[name] = value
        for name, param, value in re.findall(r'^#define ([A-Z][A-Z0-9_]*)(?:\((\w+)\))?[ \t]+(.+)$', text, re.M):
            macros[name] = (param, value.strip())
    return consts, macros


def evaluate(expr, consts, macros):
    """The value of a register expression of names, macros, literals and arithmetic"""
    for _ in range(16):
        def call(m):
            param, value = macros[m.group(1)]
            return '(' + re.sub(r'\b%s\b' % param, '(' + m.group(2) + ')', value) + ')'
        expr = re.sub(r'\b([A-Z][A-Z0-9_]*)\(([^()]*)\)', lambda m: call(m) if m.group(1) in macros else m.group(0),
                      expr)
        expr = re.sub(r'\b([A-Z][A-Z0-9_]*)\b',
                      lambda m: consts.get(m.group(1)) or ('(' + macros[m.group(1)][1] + ')'
                                                           if m.group(1) in macros and not macros[m.group(1)][0]
                                                           else m.group(1)), expr)
    expr = re.sub(r'\b(0x[0-9A-Fa-f]+|[0-9]+)u\b', r'\1', expr)
    if not re.fullmatch(r'[0-9A-Fa-fx()+\-*/ ]+', expr):
        sys.exit(f'cannot evaluate {expr!r}: the parse no longer fits engines.c')
    return int(eval(expr.replace('/', '//')))


def main():
    src = (ROOT / 'src/nx/gpu/maxwell/engines.c').read_text()
    regs = (ROOT / 'src/nx/gpu/maxwell/maxwell_3d_regs.h').read_text()
    consts, macros = names(regs, src)
    val = lambda e: evaluate(e, consts, macros)
    table_src = src[src.index('static const u8 g_m3d_group'):]
    table_src = table_src[:table_src.index('};')]
    table = set()
    for n, base in re.findall(r'\bM3D_RANGE([0-9]+)\(([^,]+), G_ACTS\)', table_src):
        table |= set(range(val(base), val(base) + int(n)))
    for m in re.findall(r'\[([^\]]+)\] = G_ACTS', table_src):
        table.add(val(m))
    label = r'\bcase\s+([A-Z0-9_x]+(?:\([0-9]+\))?)\s*:'
    cases = {}
    for name, start in (('the method switch', '\nstatic void method_acts('), ('keeps_draw', 'static int keeps_draw(')):
        found = {val(m) for m in re.findall(label, body(src, start))}
        if len(found) < 10:
            sys.exit(f'{name}: only {len(found)} case labels found, the parse no longer fits engines.c')
        cases[name] = found
    shadowed = body(src, 'static int shadowed(')
    cases['shadowed'] = {val(m) for m in re.findall(r'm != ([A-Z0-9_x]+)', shadowed)}
    for lo, hi in re.findall(r'm >= ([A-Z0-9_x]+) && m <= ([A-Z0-9_x]+)', shadowed):
        cases['shadowed'] |= set(range(val(lo), val(hi) + 1))
    if len(cases['shadowed']) < 5:
        sys.exit('shadowed: too few exceptions found, the parse no longer fits engines.c')
    missing = []
    for name, found in cases.items():
        missing += [f'{m:#x} ({name})' for m in sorted(found) if m < 0xE00 and m not in table]
    if missing:
        sys.exit('g_m3d_group lacks G_ACTS for methods that act: ' + ', '.join(missing))
    print(f'g_m3d_group marks all {len(set().union(*cases.values()))} methods that act')


if __name__ == '__main__':
    main()
