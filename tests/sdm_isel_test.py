#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The x86-64 back end's instruction selection in the tree (tools/sailgen/sdm/x64_isel.inc) is what the Intel SDM gives.

    sdm_isel_test.py --sailgen SAILGEN --pdf PDF --pdftotext PDFTOTEXT

On every machine: isel.py says what it lacks and exits 77 without the PDF or pdftotext, refuses another file by its
SHA-256, finds where a file differs, and the file in the tree names the revision. Where there is the PDF, SNX_SDM_PDF's
or the CPM cache's, and pdftotext: it makes the file again and the tree's is the same, byte for byte, and one byte changed
in it is found. Without them the test is skipped, and says which is missing.
"""

import argparse
import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, 'tools', 'sailgen', 'sdm', 'isel.py')
KEPT = os.path.join(ROOT, 'tools', 'sailgen', 'sdm', 'x64_isel.inc')
SKIP = 77  # ctest's SKIP_RETURN_CODE (tests/CMakeLists.txt)
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print('FAIL: ' + what, file=sys.stderr)


def run(arguments, sailgen):
    p = subprocess.run(
        [sys.executable, SCRIPT, '--sailgen', sailgen] + arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return p.returncode, p.stdout + p.stderr


def said(isel, sailgen, kept, temp):
    """what isel.py says and finds without the manual"""
    none = os.path.join(temp, 'none.pdf')
    other = os.path.join(temp, 'other.pdf')
    content = b'another revision\n'
    with open(other, 'wb') as f:
        f.write(content)
    for arguments, code, text in (
        (
            ['--pdf', '', '--pdftotext', sailgen],
            SKIP,
            'SKIP: no PDF of the Intel SDM, Volume 2 (%s): pass' % isel.REVISION,
        ),
        (
            ['--pdf', none, '--pdftotext', sailgen],
            SKIP,
            'SKIP: no PDF of the Intel SDM, Volume 2 (%s) at %s' % (isel.REVISION, none),
        ),
        (['--pdf', other, '--pdftotext', ''], SKIP, "SKIP: no pdftotext (poppler's)"),
        (
            ['--pdf', other, '--pdftotext', sailgen],
            1,
            'its SHA-256 is %s, not %s' % (hashlib.sha256(content).hexdigest(), isel.SHA256),
        ),
    ):
        got, out = run(arguments + ['--check', KEPT], sailgen)
        check(
            got == code and text in out,
            'isel.py %s: exit %d (expected %d), and it says %r (expected %r)'
            % (' '.join(arguments), got, code, out, text),
        )
    check(not isel.difference(kept, kept), 'a file differs from itself')
    middle = kept.index(b'sgx_byte(b, 0x', len(kept) // 2) + len(b'sgx_byte(b, 0x')
    digit = b'1' if kept[middle : middle + 1] == b'0' else b'0'
    lines = kept.count(b'\n', 0, middle) + 1
    changed = kept[:middle] + digit + kept[middle + 1 :]
    for what, bytes_, line in (
        ('a byte of the code changed', changed, lines),
        ('the last line end gone', kept[:-1], kept.count(b'\n') + 1),
        ('a byte more', kept + b'\n', kept.count(b'\n') + 2),
        ('the first byte gone', kept[1:], 1),
    ):
        why = isel.difference(kept, bytes_)
        check(why is not None and why.startswith('line %d is ' % line), '%s: %r' % (what, why))
    check(('revision %s' % isel.REVISION).encode() in kept[:600], 'the file in the tree does not name the revision')
    check(isel.SHA256.encode() in kept[:600], "the file in the tree does not name the PDF's SHA-256")
    return changed


def main():
    parser = argparse.ArgumentParser()
    for name in ('sailgen', 'pdf', 'pdftotext'):
        parser.add_argument('--' + name, default='')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('isel', SCRIPT)
    isel = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(isel)
    with open(KEPT, 'rb') as f:
        kept = f.read()
    if os.name == 'nt':  # a Windows checkout's line ends
        kept = kept.replace(b'\r\n', b'\n')
    with tempfile.TemporaryDirectory(prefix='snx-sdm-isel-') as temp:
        changed = said(isel, args.sailgen, kept, temp)
        why = isel.missing(args.pdf, args.pdftotext)
        if why:
            print('SKIP: ' + why)
            return 1 if failures else SKIP
        made = ['--pdf', args.pdf, '--pdftotext', args.pdftotext]
        got, out = run(made + ['--check', KEPT], args.sailgen)
        check(got == 0, out)
        mutant = os.path.join(temp, 'mutant.inc')
        with open(mutant, 'wb') as f:
            f.write(changed)
        got, out = run(made + ['--check', mutant], args.sailgen)
        check(
            got == 1 and 'differs from what %s gives: line ' % isel.REVISION in out,
            'a byte changed is not found: %r' % out,
        )
    print('sailgen_sdm_isel: %s' % ('FAILED' if failures else 'x64_isel.inc is what %s gives' % isel.REVISION))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
