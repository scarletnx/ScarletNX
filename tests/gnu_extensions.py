#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The GNU extensions that have an ISO C counterpart stay out of ScarletNX's own C and C++ (#68).

Each spelling below is refused in the tracked sources of src/ and tests/, comments left out, but in the files named
beside it, which hold the one use with no ISO form or the reference a test compares with:
- __thread: C11's _Thread_local;
- __int128: common/utilities/bits.h's 128-bit products and quotients, which take it only where the compiler has it;
  the 16-byte compare-and-swaps (helpers/integer.c, runtime/cas128.c), which have no ISO form; the compiler's own
  128-bit division the game modules carry (recompiler/llvm/libc.inc);
- __builtin_bswap, clz, ctz, popcount and memcpy: bits.h's helpers, and memcpy;
- the noreturn, aligned and unused attributes: _Noreturn, _Alignas, [[maybe_unused]];
- range designators ([a ... b] =) and case ranges (case a ... b:): one designator or label each.
GNU's atomics (__atomic_*), inline assembly and the attributes with no ISO form (visibility, weak, format, may_alias,
target, no_builtin, noinline, always_inline, cleanup) are not in this list. Third-party code (hactool's lz4.h) and the
guest programs the tests compile for the console (tests/gnuabi, tests/qemu_ref) are left out.

    gnu_extensions.py [--repo DIR]
"""

import argparse
import re
import sys
from pathlib import Path

RULES = [
    ('__thread', r'\b__thread\b', set()),
    (
        '__int128',
        r'__u?int128',
        {
            'src/common/utilities/bits.h',
            'src/common/cpu/aarch64/helpers/integer.c',
            'src/common/runtime/cas128.c',
            'src/common/cpu/recompiler/llvm/libc.inc',
            'tests/bits.c',
        },
    ),
    (
        '__builtin_bswap/clz/ctz/popcount/memcpy',
        r'__builtin_(bswap|clz|ctz|popcount|memcpy)',
        {'src/common/utilities/bits.h', 'tests/bits.c'},
    ),
    ('the noreturn, aligned or unused attribute', r'__attribute__\s*\(\(\s*(noreturn|aligned|unused)\b', set()),
    ('a range designator', r'\[[^\]\n]*\.\.\.[^\]\n]*\]\s*=', set()),
    ('a case range', r'\bcase\b[^:\n]*\.\.\.', set()),
]
LEFT_OUT = ('src/nx/runtime/driver/hactool/lz4.h', 'tests/gnuabi/', 'tests/qemu_ref/', 'tests/gnu_extensions.py')
SOURCES = ('*.c', '*.h', '*.inc', '*.cpp', '*.hpp')


def strip_comments(text):
    """text with its comments and the insides of its string and character literals blanked, its line breaks kept"""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        elif text[i] in '"\'':
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            out.append(q + ' ' * (j - i - 1) + q)
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def check_text(name, text):
    """name's GNU spellings outside its comments and literals, as file:line: what"""
    found = []
    stripped = strip_comments(text)
    for what, pattern, allowed in RULES:
        if name in allowed:
            continue
        for m in re.finditer(pattern, stripped):
            found.append('%s:%d: %s' % (name, stripped.count('\n', 0, m.start()) + 1, what))
    return found


def self_test():
    """each rule finds its spelling once, and none finds it in a comment or a string"""
    sample = (
        'static __thread int a;\n'
        'unsigned __int128 b;\n'
        'int c = __builtin_ctz(1);\n'
        'void d(void) __attribute__((noreturn));\n'
        'int e[4] = {[0 ... 3] = 1};\n'
        'void g(int x) { switch (x) { case 1 ... 2: break; } }\n'
        '/* __thread __int128 [0 ... 3] = */ const char* f = "__builtin_bswap32( case 1 ... 2:";\n'
    )
    found = [line.split(': ', 1)[1] for line in check_text('sample.c', sample)]
    return found == [rule[0] for rule in RULES], found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo', default=str(Path(__file__).resolve().parent.parent))
    args = parser.parse_args()
    ok, lines = self_test()
    if not ok:
        print('gnu_extensions: the rules miss their own sample: %s' % lines)
        return 1
    repo = Path(args.repo)
    # the tree as it is, not git's index: git refuses a checkout another user owns, as a CI container's root sees it
    files = sorted(
        str(path.relative_to(repo)).replace('\\', '/')
        for top in ('src', 'tests')
        for path in (repo / top).rglob('*')
        if path.is_file() and any(path.match(s) for s in SOURCES)
    )
    found = []
    for name in files:
        if not name.startswith(LEFT_OUT):
            found += check_text(name, (repo / name).read_text(errors='replace'))
    for line in found:
        print(line)
    if found:
        print('%d GNU extension(s) with an ISO C counterpart (tests/gnu_extensions.py says which)' % len(found))
        return 1
    print('gnu_extensions: %d files, none' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
