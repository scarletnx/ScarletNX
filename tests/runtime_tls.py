#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The thread-local storage each ELF program given asks of every thread (its PT_TLS segment's size), held to a limit: glibc
takes it out of the thread's stack, and a game makes threads of 64 KiB.

    runtime_tls.py [--limit BYTES] PROGRAM...
    runtime_tls.py --self-test

Exits 0 where each program is within BYTES (LIMIT without it), 1 where one is not, with its largest thread-local symbols, 77
(skipped) where the programs are not ELF."""

import contextlib
import io
import os
import struct
import sys
import tempfile

LIMIT = 16 << 10
PT_TLS = 7
SHT_SYMTAB = 2
STT_TLS = 6
NAMED = 8  # the symbols a program over its limit is said with


def tls_size(data):
    """PT_TLS's memory size of an ELF64 file's bytes, 0 for none, None where it is not an ELF64 file"""
    if data[:4] != b'\177ELF' or data[4] != 2:
        return None
    order = '<' if data[5] == 1 else '>'
    phoff, phentsize, phnum = (
        struct.unpack_from(order + 'Q', data, 32)[0],
        *struct.unpack_from(order + 'HH', data, 54),
    )
    for k in range(phnum):
        at = phoff + k * phentsize
        if struct.unpack_from(order + 'I', data, at)[0] == PT_TLS:
            return struct.unpack_from(order + 'Q', data, at + 40)[0]
    return 0


def tls_symbols(data):
    """an ELF64 file's thread-local symbols, (size, name), the largest first: none in a stripped one"""
    order = '<' if data[5] == 1 else '>'
    shoff = struct.unpack_from(order + 'Q', data, 40)[0]
    shentsize, shnum = struct.unpack_from(order + 'HH', data, 58)
    sections = [struct.unpack_from(order + 'IIQQQQIIQQ', data, shoff + k * shentsize) for k in range(shnum)]
    found = set()
    for section in sections:
        if section[1] != SHT_SYMTAB:
            continue
        names = sections[section[6]][4]
        for at in range(section[4], section[4] + section[5], 24):
            name, info, _, _, value, size = struct.unpack_from(order + 'IBBHQQ', data, at)
            if info & 15 == STT_TLS:
                found.add((size, value, data[names + name : data.index(b'\0', names + name)].decode()))
    return [(size, name) for size, _, name in sorted(found, key=lambda s: (-s[0], s[1]))]


def check(paths, limit):
    code = 77
    for path in paths:
        with open(path, 'rb') as f:
            data = f.read()
        size = tls_size(data)
        if size is None:
            print('SKIP: %s is not an ELF64 program' % path)
            continue
        if code == 77:
            code = 0
        print('%s: %d bytes of thread-local storage (at most %d)' % (path, size, limit))
        if size > limit:
            largest = ', '.join('%s %d' % (name, n) for n, name in tls_symbols(data)[:NAMED])
            print(
                "FAIL: %s: each of its threads' stacks gives %d bytes to it: %s"
                % (path, size, largest or 'it has no symbols')
            )
            code = 1
    return code


def program(order, size, symbols):
    """an ELF64 program of one segment (PT_TLS of size, or PT_LOAD for None) and a symbol table"""
    byte = '<' if order == 1 else '>'
    names, table = b'\0', bytes(24)
    for name, n, kind in symbols:
        table += struct.pack(byte + 'IBBHQQ', len(names), kind, 0, 1, 0, n)
        names += name.encode() + b'\0'
    at = 64 + 56
    head = b'\177ELF' + bytes([2, order, 1]) + bytes(9)
    head += struct.pack(
        byte + 'HHIQQQIHHHHHH', 3, 62, 1, 0, 64, at + len(table) + len(names), 0, 64, 56, 1, 64, 3 if symbols else 0, 0
    )
    phdr = struct.pack(byte + 'IIQQQQQQ', PT_TLS if size is not None else 1, 4, 0, 0, 0, 0, size or 1 << 30, 16)
    sections = bytes(64)
    sections += struct.pack(byte + 'IIQQQQIIQQ', 0, SHT_SYMTAB, 0, 0, at, len(table), 2, 0, 8, 24)
    sections += struct.pack(byte + 'IIQQQQIIQQ', 0, 3, 0, 0, at + len(table), len(names), 0, 0, 1, 0)
    return head + phdr + table + names + sections


def check_bytes(data, limit):
    """check's code and what it says, for a program of these bytes"""
    # a file of a temporary folder: Windows opens none another handle has (NamedTemporaryFile's)
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, 'program')
        with open(path, 'wb') as f:
            f.write(data)
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = check([path], limit)
    return code, said.getvalue()


def self_test():
    symbols = [('small', 8, STT_TLS), ('a_function', 1 << 20, 2), ('largest', 16384, STT_TLS)] + [
        ('kept%d' % k, 16 + k, STT_TLS) for k in range(NAMED)
    ]
    want_named = ': largest 16384, ' + ', '.join('kept%d %d' % (k, 16 + k) for k in range(NAMED - 1, 0, -1)) + '\n'
    for order in (1, 2):
        for size, limit, want in (
            (None, LIMIT, 0),
            (LIMIT, LIMIT, 0),
            (LIMIT + 1, LIMIT, 1),
            (LIMIT + 1, LIMIT + 1, 0),
            (24577, 24576, 1),
        ):
            for with_symbols in (True, False):
                got, text = check_bytes(program(order, size, symbols if with_symbols else []), limit)
                line = '%d bytes of thread-local storage (at most %d)' % (size or 0, limit)
                fails = ('gives %d bytes to it' % (size or 0)) + (
                    want_named if with_symbols else ': it has no symbols\n'
                )
                if got != want or line not in text or ('FAIL' in text) != want or (want and not text.endswith(fails)):
                    print(
                        'FAIL: a program of %s bytes, at most %d (byte order %d): %d, not %d: %s'
                        % (size, limit, order, got, want, text)
                    )
                    return 1
    got, text = check_bytes(b'MZ' + bytes(100), LIMIT)
    if got != 77:
        print('FAIL: a program that is not ELF was not skipped: %d, %s' % (got, text))
        return 1
    return 0


if __name__ == '__main__':
    args = sys.argv[1:]
    if args == ['--self-test']:
        sys.exit(self_test())
    limit = LIMIT
    if args[:1] == ['--limit']:
        if len(args) < 2 or not args[1].isdecimal():
            sys.exit('--limit takes a number of bytes, not %s' % (args[1] if len(args) > 1 else 'nothing'))
        limit, args = int(args[1]), args[2:]
    if not args:
        sys.exit('usage: runtime_tls.py [--limit BYTES] PROGRAM... | --self-test')
    sys.exit(check(args, limit))
