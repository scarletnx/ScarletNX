# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A game's modules as the runtime lays them out, for symbols.py and disasm.py.

The recompiler's own loader reads them: `scarletnx --dump-modules PROGRAM DIR` writes each module's image
(DIR/<name>.bin) and an index of its base, size and symbols (DIR/modules.txt, the format is in src/nx/loader/loader.h).
The dump is cached per program folder under $XDG_CACHE_HOME/scarletnx/module-dumps (~/.cache without it) and made again when the
program's ExeFS files or the scarletnx binary change.

scarletnx is build/scarletnx in this checkout, or the program the environment variable SNX_SCARLETNX names.
"""

import hashlib
import os
import shutil
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Module:
    """name, base and size (the whole mapping, BSS included), image (the bytes mapped, without the BSS), and the
    symbols: syms {module offset: name}, sizes {module offset: size in bytes, 0 when unknown}; matched {module offset:
    (instructions, name)}, the functions it doesn't export that the loader named from the sdk module's exports"""

    def __init__(self, name, base, size):
        self.name, self.base, self.size = name, base, size
        self.image = b''
        self.syms, self.sizes, self.matched = {}, {}, {}


def scarletnx():
    named = os.environ.get('SNX_SCARLETNX')
    if named:
        return named
    for leaf in ('scarletnx', 'scarletnx.exe'):
        path = os.path.join(REPO, 'build', leaf)
        if os.path.isfile(path):
            return path
    raise SystemExit('build/scarletnx is missing: build it, or name the program with SNX_SCARLETNX')


def _inputs(program):
    """What the dump depends on: the NRO, or the files of the ExeFS (the folder holding main, as the loader finds it)"""
    if os.path.isfile(program):
        return [program]
    for folder in (os.path.join(program, 'exefs'), os.path.join(program, 'ExeFS'), program):
        if os.path.isfile(os.path.join(folder, 'main')):
            return sorted(os.path.join(folder, f) for f in os.listdir(folder))
    return []


def _stamp(program, tool):
    lines = []
    for path in [tool, *_inputs(program)]:
        try:
            st = os.stat(path)
        except OSError:
            continue
        lines.append('%s %d %d' % (path, st.st_size, st.st_mtime_ns))
    return '\n'.join(lines) + '\n'


def _dump(program):
    """The folder holding the program's dump, made or refreshed"""
    program = os.path.abspath(program)
    tool = shutil.which(scarletnx()) or scarletnx()
    cache = os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache')
    folder = os.path.join(cache, 'scarletnx', 'module-dumps', hashlib.sha256(program.encode()).hexdigest()[:16])
    stamp = _stamp(program, tool)
    stamp_path = os.path.join(folder, 'stamp')
    try:
        with open(stamp_path, encoding='utf-8') as f:
            if f.read() == stamp and os.path.isfile(os.path.join(folder, 'modules.txt')):
                return folder
    except OSError:
        pass
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    result = subprocess.run([tool, '--dump-modules', program, folder], capture_output=True, text=True, check=False)
    if result.returncode:
        raise SystemExit('%s --dump-modules failed:\n%s' % (tool, (result.stderr or result.stdout).strip()))
    with open(stamp_path, 'w', encoding='utf-8') as f:
        f.write(stamp)
    return folder


def load_program(program):
    """(modules, is64) of an extracted game (a folder with exefs/) or an NRO"""
    folder = _dump(program)
    mods, is64, current = [], True, None
    with open(os.path.join(folder, 'modules.txt'), encoding='utf-8') as f:
        for line in f:
            kind, _, rest = line.rstrip('\n').partition(' ')
            if kind == 'program':
                is64 = rest.split()[1] == '1'
            elif kind == 'module':
                name, base, size, _ = rest.split()
                current = Module(name, int(base, 16), int(size, 16))
                with open(os.path.join(folder, name + '.bin'), 'rb') as image:
                    current.image = image.read()
                mods.append(current)
            elif kind == 'symbol' and current is not None:
                value, size, _, name = rest.split(' ', 3)
                current.syms[int(value, 16)] = name
                current.sizes[int(value, 16)] = int(size, 16)
            elif kind == 'match' and current is not None:
                value, words, name = rest.split(' ', 2)
                current.matched[int(value, 16)] = (int(words), name)
    return mods, is64
