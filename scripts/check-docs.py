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
- settings: each key of src/nx/runtime/settings.txt has a row in a table of docs/SETTINGS.md whose header is
  | Key | Default |, and its Default cell says the key's default and the one each platform gives it (`0` (iOS `1`));
  each key a row names is one settings.txt has. Several keys in a row take one default each, or one for them all,
  and a name in parentheses in the Key cell is no key (`controls_<ID>` (ID one of `a`, `b`))

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
SETTINGS = 'src/nx/runtime/settings.txt'
SETTINGS_DOC = 'docs/SETTINGS.md'
# settings.txt's default-if macros, by the platform a Default cell names
PLATFORMS = {'SNX_IOS': 'iOS', '__ANDROID__': 'Android'}
UNSET = object()  # an LLVM option's default: LLVM's own value

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


# ---- settings ----
class SettingsError(Exception):
    def __init__(self, line, text):
        super().__init__(f'{line}: {text}')
        self.line, self.text = line, text


def snake(name):
    """tools/settingsgen's snake(): letters and digits lowercased, every other run an _, a camel-case word split where
    its case changes (LoopVectorization, SLPVectorization: loop_vectorization, slp_vectorization)"""
    out, gap = [], False
    for i, c in enumerate(name):
        if not c.isascii() or not c.isalnum():
            gap = True
            continue
        if (
            out
            and c.isupper()
            and i > 0
            and (
                name[i - 1].islower()
                or name[i - 1].isdigit()
                or (name[i - 1].isupper() and i + 1 < len(name) and name[i + 1].islower())
            )
        ):
            gap = True
        if gap and out:
            out.append('_')
        gap = False
        out.append(c.lower())
    s = ''.join(out)
    return '_' + s if s[:1].isdigit() else s


def settings_keys(path):
    """{key: (line, {platform or None: default})} of settings.txt, its <N> and <ID> kept in the key. A text with no
    default is empty; an LLVM pass (@lowercase) is on, 1, and an LLVM option (@CamelCase) LLVM's own until it is set"""
    keys, current, table_kind = {}, None, None

    def done():
        if current is None:
            return
        line, key, llvm_id, default, ifs, kind = (
            current['line'],
            current['key'],
            current['llvm'],
            current['default'],
            current['ifs'],
            current['kind'],
        )
        if llvm_id is not None:
            option = any(c.isupper() for c in llvm_id)
            key = key or ('llvm_' if option else 'llvm_pass_') + snake(llvm_id)
            if default is None:
                default = UNSET if option else '1'
        if key is None:
            raise SettingsError(line, 'a setting with no key')
        if default is None and kind == 'text':
            default = ''
        if default is None:
            raise SettingsError(line, f'{key} has no default')
        values = {None: default}
        for macro, value in ifs:
            if macro not in PLATFORMS:
                raise SettingsError(
                    line,
                    f'{key}: default-if {macro} names no platform scripts/check-docs.py knows '
                    f'({", ".join(sorted(PLATFORMS))})',
                )
            values[PLATFORMS[macro]] = value
        keys[key] = (line, values)

    for number, raw in enumerate(path.read_text(encoding='utf-8').split('\n'), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        word, _, rest = line.partition(' ')
        rest = rest.strip()
        if word in ('table', 'section', 'include', 'list', 'repeat', 'end'):
            done()
            current = None
            if word == 'table':
                table_kind = rest.split()[1] if len(rest.split()) > 1 else 'runtime'
            continue
        if word == 'setting':
            done()
            llvm = rest[1:] if table_kind == 'llvm' and rest.startswith('@') else None
            current = {'line': number, 'key': None, 'llvm': llvm, 'default': None, 'ifs': [], 'kind': None}
            continue
        if current is None:
            continue
        value = '' if rest == '""' else rest
        if word == 'key':
            current['key'] = rest
        elif word in ('int', 'float', 'text'):
            current['kind'] = word
        elif word == 'default':
            current['default'] = value
        elif word == 'default-if':
            macro, _, value = rest.partition(' ')
            current['ifs'].append((macro, '' if value.strip() == '""' else value.strip()))
    done()
    return keys


DEFAULT_ITEM = re.compile(
    r'\s*(`[^`]*`|none|empty|LLVM\'s own)'
    r'(?:\s*\(((?:\s*(?:' + '|'.join(sorted(PLATFORMS.values())) + r')\s+(?:`[^`]*`|none|empty),?)+)\))?\s*(?:,|$)'
)
PLATFORM_VALUE = re.compile(r'(' + '|'.join(sorted(PLATFORMS.values())) + r')\s+(`[^`]*`|none|empty)')


def cell_value(word):
    if word in ('none', 'empty'):
        return ''
    if word == "LLVM's own":
        return UNSET
    return word[1:-1]


def defaults_of(cell):
    """the defaults a Default cell gives, in order, each {platform or None: value}; None when it can't be read"""
    out, at = [], 0
    while at < len(cell):
        m = DEFAULT_ITEM.match(cell, at)
        if not m or m.end() == at:
            return None
        values = {None: cell_value(m.group(1))}
        for p in PLATFORM_VALUE.finditer(m.group(2) or ''):
            values[p.group(1)] = cell_value(p.group(2))
        out.append(values)
        at = m.end()
    return out


def shown(values):
    def one(v):
        return "LLVM's own" if v is UNSET else 'empty' if v == '' else f'`{v}`'

    extra = ', '.join(f'{p} {one(v)}' for p, v in values.items() if p is not None)
    return one(values[None]) + (f' ({extra})' if extra else '')


def cells(line):
    """the cells of a Markdown table row, split as GitHub does: at each | but an escaped one, \\|, inside code too"""
    row = line.strip()
    row = row[1:] if row.startswith('|') else row
    row = row[:-1] if row.endswith('|') and not row.endswith('\\|') else row
    return [cell.strip().replace('\\|', '|') for cell in re.split(r'(?<!\\)\|', row)]


def check_settings(root, failures):
    doc_path, settings_path = root / SETTINGS_DOC, root / SETTINGS
    if not settings_path.is_file():
        return
    try:
        keys = settings_keys(settings_path)
    except SettingsError as error:
        failures.add(SETTINGS, error.line, error.text)
        return
    if not doc_path.is_file():
        failures.add(SETTINGS_DOC, 0, f'is not there: it has a row for each key of {SETTINGS}')
        return
    rows, in_table, fenced = {}, False, False
    for number, line in enumerate(doc_path.read_text(encoding='utf-8').split('\n'), 1):
        if FENCE.match(line):
            fenced = not fenced
        if fenced or not line.lstrip().startswith('|'):
            in_table = False
            continue
        row = cells(line)
        if not in_table:
            in_table = len(row) >= 2 and row[0] == 'Key' and row[1] == 'Default'
            continue
        if set(''.join(row)) <= set('-: '):
            continue  # the header's rule
        names = re.findall(r'`([^`]+)`', re.sub(r'\([^)]*\)', '', row[0]))  # what a parenthesis says is no key
        if not names:
            failures.add(SETTINGS_DOC, number, f'a row with no `key` in its Key cell: {row[0]!r}')
            continue
        defaults = defaults_of(row[1]) if len(row) > 1 else None
        if defaults is None or (len(defaults) != 1 and len(defaults) != len(names)):
            failures.add(
                SETTINGS_DOC,
                number,
                f'Default {row[1] if len(row) > 1 else ""!r} is not one default, or one for each of '
                f"{', '.join(names)}, each `value`, none, empty or LLVM's own, with (iOS `value`) or (Android `value`)",
            )
            continue
        for i, name in enumerate(names):
            given = defaults[0] if len(defaults) == 1 else defaults[i]
            if name in rows:
                failures.add(SETTINGS_DOC, number, f'`{name}` has a row already, at line {rows[name]}')
                continue
            rows[name] = number
            if name not in keys:
                failures.add(SETTINGS_DOC, number, f'`{name}` is no key of {SETTINGS}')
                continue
            line_number, values = keys[name]
            if given != values:
                failures.add(
                    SETTINGS_DOC,
                    number,
                    f'`{name}` says the default is {shown(given)}; {SETTINGS}:{line_number} makes it {shown(values)}',
                )
    for key, (line_number, values) in keys.items():
        if key not in rows:
            failures.add(SETTINGS, line_number, f'`{key}` (default {shown(values)}) has no row in {SETTINGS_DOC}')


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
    check_settings(root, failures)
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        print(f'check-docs.py: {len(failures)} to fix', file=sys.stderr)
        return 1
    print(f'check-docs.py: {len(docs)} documents, their links, indexes and the settings they list hold')
    return 0


if __name__ == '__main__':
    sys.exit(main())
