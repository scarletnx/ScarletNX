#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The documentation held to the checkout, each failure a line that names its file and line:

- links: each relative link of README.md, CONTRIBUTING.md, ROADMAP.md and docs/ (Markdown's [text](target) and
  [name]: target, HTML's src= and href=, outside code) names a file or folder that is there. Its anchor is a heading of
  the Markdown it names, as GitHub makes one (lowercase, punctuation dropped but - and _, each space a -, a repeated
  one numbered), or an <a name=> or id= in it; on another file, #L<n> or #L<n>-L<m>, lines the file has
- indexes: a document over 100 lines with 3 or more ## and ### sections has an ## Index of them, and any document's
  ## Index lists its sections as they are. --index writes them: an index where it stands, a new one under the title
- the docs' own index: docs/README.md links every document of docs/

    scripts/check-docs.py [--root DIR] [--index]
"""

import argparse
import bisect
import re
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
DOCS = ('README.md', 'CONTRIBUTING.md', 'ROADMAP.md', 'docs')
INDEX_LINES = 100  # a document longer than this carries an ## Index
INDEX_SECTIONS = 3  # of at least this many sections

FENCE = re.compile(r'^\s*(```|~~~)')
HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
INDEX_LINK = re.compile(r'^\s*[*-] \[.*\]\(#[^)]*\)\s*$')
CODE_SPAN = re.compile(r'(`+)(.+?)\1')
REFERENCE = re.compile(r'^ {0,3}\[[^\]]+\]:\s*(<[^>]*>|\S+)', re.M)
HTML_LINK = re.compile(r'\b(?:src|href)\s*=\s*"([^"]*)"')
HTML_ANCHOR = re.compile(r'<a\s[^>]*\bname\s*=\s*"([^"]+)"|\bid\s*=\s*"([^"]+)"')
SCHEME = re.compile(r'^[A-Za-z][A-Za-z0-9+.-]*:')
LINE_ANCHOR = re.compile(r'^L(\d+)(?:-L(\d+))?$')


class Failures(list):
    def add(self, path, line, text):
        self.append(f'{path}:{line}: {text}' if line else f'{path}: {text}')


# ---- Markdown ----
def prose_lines(text):
    """the lines of text with fenced code and inline code blanked, so neither links nor makes headings"""
    out, fenced = [], False
    for line in text.split('\n'):
        if FENCE.match(line):
            fenced = not fenced
            out.append('')
            continue
        out.append('' if fenced else CODE_SPAN.sub(lambda m: ' ' * len(m.group(0)), line))
    return out


def headings(text):
    """(line index, level, text) of each heading outside fenced code"""
    out, fenced = [], False
    for i, line in enumerate(text.split('\n')):
        if FENCE.match(line):
            fenced = not fenced
            continue
        m = None if fenced else HEADING.match(line)
        if m:
            out.append((i, len(m.group(1)), m.group(2)))
    return out


def slug(text, seen):
    """the anchor GitHub gives a heading: its text as shown (links' words, no code marks or emphasis), lowercase,
    letters, digits, - and _ kept, each space a -, numbered from -1 when an earlier heading had it"""
    text = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', text)
    text = re.sub(r'<[^>]+>', '', text)
    text = re.sub(r'[`*]', '', text)
    text = re.sub(r'(?<![A-Za-z0-9])_+|_+(?![A-Za-z0-9])', '', text)  # emphasis; a_b keeps its _
    base = re.sub(r'[^\w\- ]', '', text.lower()).replace(' ', '-')
    n = seen.get(base, 0)
    seen[base] = n + 1
    return base if n == 0 else f'{base}-{n}'


def anchors(text):
    """every anchor a Markdown document has: its headings' and its HTML names and ids"""
    seen, out = {}, set()
    for _, _, name in headings(text):
        out.add(slug(name, seen))
    for line in prose_lines(text):
        for m in HTML_ANCHOR.finditer(line):
            out.add(m.group(1) or m.group(2))
    return out


def links(text):
    """(line number, target) of each link of a Markdown document, outside code"""
    lines = prose_lines(text)
    prose = '\n'.join(lines)
    starts = [0]  # where each line starts in prose
    for line in lines[:-1]:
        starts.append(starts[-1] + len(line) + 1)

    def number(offset):
        return bisect.bisect_right(starts, offset)

    out = []
    for m in re.finditer(r'\]\(', prose):
        # the target runs to the ) that closes the (, a <target> or a "title" aside
        i, depth = m.end(), 1
        while i < len(prose) and depth:
            depth += {'(': 1, ')': -1}.get(prose[i], 0)
            i += 1
        inner = prose[m.end() : i - 1].strip()
        if inner.startswith('<'):
            target = inner[1 : inner.find('>')] if '>' in inner else inner[1:]
        else:
            target = inner.split()[0] if inner.split() else ''
        out.append((number(m.start()), target))
    for m in REFERENCE.finditer(prose):
        out.append((number(m.start()), m.group(1).strip('<>')))
    for m in HTML_LINK.finditer(prose):
        out.append((number(m.start()), m.group(1)))
    return sorted(out)


def documents(root):
    out = []
    for name in DOCS:
        path = root / name
        if path.is_dir():
            out += sorted(p for p in path.rglob('*.md') if p.is_file())
        elif path.is_file():
            out.append(path)
    return out


def check_links(root, docs, failures):
    texts = {}

    def text_of(path):
        if path not in texts:
            texts[path] = path.read_text(encoding='utf-8', errors='replace')
        return texts[path]

    for doc in docs:
        name = doc.relative_to(root).as_posix()
        for line, target in links(text_of(doc)):
            if not target or SCHEME.match(target) or target.startswith('//'):
                continue
            path, _, anchor = target.partition('#')
            path = unquote(path.split('?')[0])
            if path:
                dest = (root / path.lstrip('/')) if path.startswith('/') else (doc.parent / path)
                try:
                    dest.resolve().relative_to(root.resolve())
                except ValueError:
                    failures.add(name, line, f'{target}: outside the checkout')
                    continue
                if not dest.exists():
                    failures.add(name, line, f'{target}: {path} is not there')
                    continue
            else:
                dest = doc
            if not anchor:
                continue
            if dest.is_dir():
                failures.add(name, line, f'{target}: #{anchor} on a folder, which has no anchors')
            elif dest.suffix.lower() == '.md':
                if anchor not in anchors(text_of(dest)):
                    where = 'this document' if dest == doc else dest.resolve().relative_to(root.resolve()).as_posix()
                    failures.add(name, line, f'{target}: {where} has no heading or name with the anchor #{anchor}')
            else:
                m = LINE_ANCHOR.match(anchor)
                if not m:
                    failures.add(name, line, f'{target}: #{anchor} on a file that is not Markdown takes #L<n> alone')
                    continue
                count = len(text_of(dest).splitlines())
                last = int(m.group(2) or m.group(1))
                if last > count:
                    failures.add(name, line, f'{target}: line {last} is past the end of {path} ({count} lines)')


# ---- indexes ----
def index_block(lines):
    """(start, end) of the ## Index heading and the links under it, blank lines around them taken; None for none. A
    section named Index whose text opens with anything but a link is a section, not the index"""
    for i, level, name in headings('\n'.join(lines)):
        if level == 2 and name.strip().lower() == 'index':
            first = next((line for line in lines[i + 1 :] if line.strip()), '')
            if not INDEX_LINK.match(first):
                continue
            end = i + 1
            while end < len(lines) and (not lines[end].strip() or INDEX_LINK.match(lines[end])):
                end += 1
            return i, end
    return None


def indexed(text):
    """text with its ## Index made again: the ## and ### headings after the title, ### under their ##. An index
    already there is replaced where it is, a new one goes right under the title, and a document of fewer than
    INDEX_SECTIONS sections has none"""
    lines = text.split('\n')
    block = index_block(lines)
    if block:
        at, end = block
        rest = lines[:at] + lines[end:]
    else:
        rest = lines
    heads = headings('\n'.join(rest))
    title = next((i for i, level, _ in heads if level == 1), None)
    listed = [(i, level, name) for i, level, name in heads if level in (2, 3) and title is not None and i > title]
    if title is None or len(listed) < INDEX_SECTIONS:
        return '\n'.join(rest)
    if not block:
        at = title + 1
        while at < len(rest) and not rest[at].strip():
            rest = rest[:at] + rest[at + 1 :]
    # anchors in document order, the index's own among them where it stands
    seen, entries, mine = {}, [], False
    for i, level, name in heads:
        if not mine and i >= at:
            slug('Index', seen)
            mine = True
        a = slug(name, seen)
        if (i, level, name) in listed:
            entries.append(('  ' if level == 3 else '') + f'* [{name}](#{a})')
    if not mine:
        slug('Index', seen)
    tail = rest[at:]
    while tail and not tail[0].strip():
        tail = tail[1:]
    head = rest[:at]
    while head and not head[-1].strip():
        head = head[:-1]
    return '\n'.join(head + ['', '## Index', ''] + entries + [''] + tail)


def has_index(text):
    return any(level == 2 and name.strip().lower() == 'index' for _, level, name in headings(text))


def check_indexes(root, docs, failures, write):
    for doc in docs:
        if doc.name == 'README.md' and doc.parent == root:
            continue  # the project's front page
        text = doc.read_text(encoding='utf-8')
        made = indexed(text)
        long = len(text.rstrip('\n').split('\n')) > INDEX_LINES
        if not (long or has_index(text)) or made == text:
            continue
        name = doc.relative_to(root).as_posix()
        if write:
            doc.write_text(made, encoding='utf-8')
            print(f'{name}: index written')
        elif has_index(text):
            failures.add(name, 0, '## Index does not list its sections as they are: run scripts/check-docs.py --index')
        else:
            failures.add(
                name,
                0,
                f'over {INDEX_LINES} lines with {INDEX_SECTIONS} or more sections and no ## Index: '
                'run scripts/check-docs.py --index',
            )


def check_docs_index(root, failures):
    index = root / 'docs/README.md'
    if not index.is_file():
        failures.add('docs/README.md', 0, 'is not there: it lists every document of docs/')
        return
    linked = set()
    for _, target in links(index.read_text(encoding='utf-8')):
        path = unquote(target.partition('#')[0])
        if path and not SCHEME.match(path):
            linked.add((index.parent / path).resolve())
    for doc in sorted((root / 'docs').rglob('*.md')):
        if doc != index and doc.resolve() not in linked:
            failures.add('docs/README.md', 0, f'does not link {doc.relative_to(root / "docs").as_posix()}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT, help="the checkout (default: this script's)")
    parser.add_argument('--index', action='store_true', help='write the ## Index of each document that needs one')
    args = parser.parse_args()
    root = args.root.resolve()
    docs = documents(root)
    failures = Failures()
    check_indexes(root, docs, failures, args.index)
    check_links(root, docs, failures)
    check_docs_index(root, failures)
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        print(f'check-docs.py: {len(failures)} to fix', file=sys.stderr)
        return 1
    print(f'check-docs.py: {len(docs)} documents, their links and indexes hold')
    return 0


if __name__ == '__main__':
    sys.exit(main())
