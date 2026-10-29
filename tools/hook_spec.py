# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The prototypes a hook spec declares (tools/hookgen's grammar, docs/HOOKS.md), for the tools that read the functions'
types from it: gpu/nvn/nvn.hook is the one prototype of each NVN function the runtime implements, and hookgen makes
its PFNNVN...PROC types of the same lines.

    prototypes(path)    {symbol: (result type, [argument types], [argument names])}, as the spec spells them
                        ('const NVNtexture* const*', 'i32', 'NVNshaderStage'), each symbol's first declaration
    header_types(path)  {name: 'enum' | 'pointer' | 'other'} for the typedefs of a C header (the include line's)
    kind(type, types)   the kind hookgen passes a type as: addr for a pointer or a header's pointer type, i32 for a
                        header's enum, else the type itself (bool, u8 ... i64, f32, f64, addr, void)
"""

import re

HOOK = re.compile(r'^\s*(?:@version\([^)]*\)\s*)?hook\s+([\w.$]+)\s*\((.*?)\)\s*(?:->\s*(.*?))?\s*'
                  r'(?:=.*|\buses\b.*|\bpure\b.*)?$')
BUFFER = re.compile(r'\s+(?:in|out|inout)\[[^\]]*\]$')


def prototypes(path):
    protos = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = HOOK.match(line.rstrip('\n'))
            if not m or m.group(1) in protos:
                continue
            types, names = [], []
            for arg in (a.strip() for a in m.group(2).split(',')):
                if not arg:
                    continue
                arg = BUFFER.sub('', arg)
                name = re.search(r'(\w+)$', arg).group(1)
                types.append(' '.join(arg[: -len(name)].split()).replace(' *', '*'))
                names.append(name)
            protos[m.group(1)] = (' '.join((m.group(3) or 'void').split()), types, names)
    return protos


def header_types(path):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    text = re.sub(r'/\*.*?\*/|//[^\n]*|^\s*#[^\n]*', ' ', text, flags=re.S | re.M)
    out = {}
    for start in (m.end() for m in re.finditer(r'\btypedef\b', text)):
        depth, end = 0, start
        while end < len(text) and (text[end] != ';' or depth):  # to its ';', past a struct's members
            depth += {'{': 1, '}': -1}.get(text[end], 0)
            end += 1
        top, inner = text[start:end], None
        while top != inner:  # without the members of a struct (nested ones too) or the values of an enum
            inner, top = top, re.sub(r'\{[^{}]*\}', ' ', top)
        fn = re.search(r'\(\s*\*\s*(\w+)\s*\)', top)
        if fn:
            out[fn.group(1)] = 'pointer'
            continue
        name = re.search(r'(\w+)\s*(?:\[[^\]]*\]\s*)*$', top)
        if name:
            out[name.group(1)] = ('pointer' if '*' in top else 'enum' if re.match(r'\s*enum\b', top) else 'other')
    return out


def kind(t, types):
    if '*' in t:
        return 'addr'
    what = types.get(t)
    return 'addr' if what == 'pointer' else 'i32' if what == 'enum' else t
