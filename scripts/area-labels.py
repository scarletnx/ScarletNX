#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""area-labels.py TITLE: prints the labels a title's `[area]` prefix and words ask for, one a line. A prefix is one
or more areas, as in `[gpu]` or `[macos, linux]`; the areas that name no label are skipped.
.github/workflows/pr-checks.yml adds the labels to a pull request whose title was opened or edited,
and .github/workflows/issue-labels.yml to an issue."""

import re
import sys

LABELS = {
    'area: build': ('cmake', 'hactool', 'scripts', 'setup', 'tools'),
    'area: ci': ('appimage', 'ci', 'flatpak'),
    'area: gpu': ('gpu', 'graphics', 'mme', 'shader', 'surface', 'vk'),
    'area: gui': ('gui', 'ui', 'ux'),
    'area: kernel': ('kernel', 'memory'),
    'area: recompiler': ('jit', 'loader', 'recompiler'),
    'area: runtime': ('driver', 'ini', 'runtime', 'settings'),
    'area: services': ('audio', 'fs', 'hle', 'network', 'nvdrv', 'pl', 'service', 'swipc', 'users', 'vi'),
    'area: test': ('test', 'tests'),
    'kind: documentation': ('docs',),
    'platform: 3ds': ('3ds',),
    'platform: android': ('android',),
    'platform: freebsd': ('bsd', 'fbsd', 'freebsd'),
    'platform: gba': ('gba',),
    'platform: ios': ('ios',),
    'platform: linux': ('linux',),
    'platform: loongarch': ('loongarch',),
    'platform: macos': ('macos',),
    'platform: ppc64be': ('ppc64be',),
    'platform: riscv': ('riscv',),
    'platform: windows': ('win32', 'windows'),
}
# kinds read from the words after the prefix: a title that starts with the verb, or names the symptom anywhere. A
# prefix like [bug] is not used by the project, so these are a guess a maintainer corrects by changing the label
KINDS = (
    ('kind: bug', re.compile(r'^(fix|fixes|fixed)\b|\b(crash(es|ed)?|freez(e|es)|hangs?|broken|wrong)\b', re.I)),
    ('kind: enhancement', re.compile(r'^(add|allow|implement|support|let|make|show|enable|port)\b', re.I)),
)
BY_AREA = {area: label for label, areas in LABELS.items() for area in areas}
PREFIX = re.compile(r'\s*\[([^\]]*)\]')


def labels(title):
    """the labels for the areas of title's leading bracketed prefixes, then the one kind its words ask for"""
    found = []
    end = 0
    while match := PREFIX.match(title, end):
        end = match.end()
        for area in match.group(1).split(','):
            label = BY_AREA.get(area.strip().lower())
            if label and label not in found:
                found.append(label)
    rest = title[end:].strip()
    for label, words in KINDS:
        if words.search(rest) and label not in found:
            found.append(label)
            break
    return found


def main(argv):
    if len(argv) != 2:
        print('usage: area-labels.py TITLE', file=sys.stderr)
        return 2
    for label in labels(argv[1]):
        print(label)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
