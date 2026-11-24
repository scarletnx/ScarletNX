#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The thread-local storage each ELF program given asks of every thread (its PT_TLS segment's size), held to a limit: glibc
takes it out of the thread's stack, and a game makes threads of 64 KiB.

    runtime_tls.py PROGRAM...
    runtime_tls.py --self-test

Exits 0 where each program is within LIMIT, 1 where one is not, 77 (skipped) where the programs are not ELF."""

import struct
import sys
import tempfile

LIMIT = 16 << 10
PT_TLS = 7


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


def check(paths):
    code = 77
    for path in paths:
        with open(path, 'rb') as f:
            size = tls_size(f.read())
        if size is None:
            print('SKIP: %s is not an ELF64 program' % path)
            continue
        if code == 77:
            code = 0
        print('%s: %d bytes of thread-local storage (at most %d)' % (path, size, LIMIT))
        if size > LIMIT:
            print("FAIL: %s: each of its threads' stacks gives %d bytes to it" % (path, size))
            code = 1
    return code


def self_test():
    for order, data in ((1, '<'), (2, '>')):
        for size, want in ((None, 0), (LIMIT, 0), (LIMIT + 1, 1)):
            head = b'\177ELF' + bytes([2, order, 1]) + bytes(9)
            head += struct.pack(data + 'HHIQQQIHHHHHH', 3, 62, 1, 0, 64, 0, 0, 64, 56, 1, 64, 0, 0)
            kind = PT_TLS if size is not None else 1
            phdr = struct.pack(data + 'IIQQQQQQ', kind, 4, 0, 0, 0, 0, size or 1 << 30, 16)
            with tempfile.NamedTemporaryFile() as f:
                f.write(head + phdr)
                f.flush()
                got = check([f.name])
            if got != want:
                print('FAIL: a program of %s bytes (byte order %d): %d, not %d' % (size, order, got, want))
                return 1
    with tempfile.NamedTemporaryFile() as f:
        f.write(b'MZ' + bytes(100))
        f.flush()
        if check([f.name]) != 77:
            print('FAIL: a program that is not ELF was not skipped')
            return 1
    return 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(self_test())
    if len(sys.argv) < 2:
        sys.exit('usage: runtime_tls.py PROGRAM... | --self-test')
    sys.exit(check(sys.argv[1:]))
