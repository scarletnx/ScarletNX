#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""isel: x64_isel.inc, the x86-64 back end's instruction selection, from Intel's SDM.

    isel.py --sailgen SAILGEN --pdf PDF --pdftotext PDFTOTEXT [-o FILE | --check FILE]

PDF is the Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2, revision 325383-093US (cpmfile.json's
intel-sdm-vol2 names where Intel has it), refused by its SHA-256 otherwise. pdftotext -layout makes its text, sailgen --sdm
cuts that into instruction pages with the overrides beside this file, and sailgen --isel selects by x64.ops; the text and
the pages are kept in a temporary folder. -o writes FILE (x64_isel.inc beside this file unless it is given), which is
checked in: the build needs neither the manual nor pdftotext. --check compares FILE with what it would write, byte for
byte, and fails with the first line that differs. Without the PDF or pdftotext it says which is missing and exits 77.
"""

import argparse
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REVISION = '325383-093US'
SHA256 = 'd137a7887787bbb06c1a8b8bafd51958d8d2e16b4dff987a03226c845cca1516'
SKIP = 77


def source():
    """what the file's header says of where it is from, and how it is made again"""
    return (
        "The pages are of Volume 2, revision %s (the PDF's SHA-256 is\n"
        '   %s);\n'
        '   tools/sailgen/sdm/isel.py makes this file again from that PDF' % (REVISION, SHA256)
    )


def missing(pdf, pdftotext):
    """why it cannot be made here, None where it can"""
    if not pdf:
        return (
            'no PDF of the Intel SDM, Volume 2 (%s): pass -DSNX_SDM_PDF=, or -DSNX_TEST_SDM=ON downloads it' % REVISION
        )
    if not os.path.isfile(pdf):
        return 'no PDF of the Intel SDM, Volume 2 (%s) at %s: -DSNX_TEST_SDM=ON downloads it' % (REVISION, pdf)
    if not pdftotext:
        return "no pdftotext (poppler's) on this machine"
    return None


def refused(pdf):
    """why the PDF is not the revision the selection is made from, None where it is"""
    with open(pdf, 'rb') as f:
        got = hashlib.sha256(f.read()).hexdigest()
    if got == SHA256:
        return None
    return (
        "%s isn't revision %s of the Intel SDM, Volume 2: its SHA-256 is %s, not %s. Pass that revision's PDF, or move to "
        "another revision: cpmfile.json's intel-sdm-vol2, REVISION and SHA256 here, and x64_isel.inc made again"
        % (pdf, REVISION, got, SHA256)
    )


def make(sailgen, pdf, pdftotext):
    """the file's bytes, and the SHA-256 of the pages they are made from"""
    with tempfile.TemporaryDirectory(prefix='snx-sdm-') as temp:
        text, pages, out = [os.path.join(temp, name) for name in ('sdm.txt', 'sdm.pages', 'x64_isel.inc')]
        overlays = []
        for path in sorted(glob.glob(os.path.join(HERE, '*.sdm'))):
            overlays += ['--overlay', path]
        subprocess.check_call([pdftotext, '-layout', pdf, text])
        subprocess.check_call([sailgen, '--sdm', text, '-o', pages] + overlays)
        ops = os.path.join(HERE, '..', 'x64.ops')
        subprocess.check_call([sailgen, '--isel', pages, '--ops', ops, '--source', source(), '-o', out])
        with open(pages, 'rb') as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        with open(out, 'rb') as f:
            return f.read(), digest


def difference(made, kept):
    """where the file kept differs from the one made, None where it is the same"""
    if os.name == 'nt':  # a Windows checkout's line ends
        kept = kept.replace(b'\r\n', b'\n')
    if made == kept:
        return None
    a, b = made.split(b'\n'), kept.split(b'\n')
    line = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
    show = [repr(x[line].decode('utf-8', 'replace')) if line < len(x) else 'the end of the file' for x in (a, b)]
    return 'line %d is %s, not %s (%d bytes, not %d)' % (line + 1, show[1], show[0], len(kept), len(made))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sailgen', required=True, help="sailgen, the build's snx-sailgen")
    parser.add_argument('--pdf', default='', help='the PDF of the SDM')
    parser.add_argument('--pdftotext', default='', help="poppler's pdftotext")
    what = parser.add_mutually_exclusive_group()
    what.add_argument('-o', '--output', metavar='FILE', default=os.path.join(HERE, 'x64_isel.inc'))
    what.add_argument('--check', metavar='FILE', help='compare FILE, and write nothing')
    args = parser.parse_args(argv)
    why = missing(args.pdf, args.pdftotext)
    if why:
        print('SKIP: ' + why)
        return SKIP
    why = refused(args.pdf)
    if why:
        print(why, file=sys.stderr)
        return 1
    made, pages = make(args.sailgen, args.pdf, args.pdftotext)
    if not args.check:
        with open(args.output, 'wb') as f:
            f.write(made)
        print('%s: %d bytes' % (args.output, len(made)))
        return 0
    with open(args.check, 'rb') as f:
        kept = f.read()
    why = difference(made, kept)
    if not why:
        print('%s is what %s gives: %d bytes' % (args.check, REVISION, len(made)))
        return 0
    version = subprocess.run([args.pdftotext, '-v'], stderr=subprocess.PIPE, text=True).stderr.split('\n')[0]
    print(
        '%s differs from what %s gives: %s. It was made with %s, whose pages have the SHA-256 %s; '
        'tools/sailgen/sdm/isel.py makes it again' % (args.check, REVISION, why, version, pages),
        file=sys.stderr,
    )
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
