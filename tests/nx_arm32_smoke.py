#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Recompile and run a synthetic 32-bit Horizon program (nx_arm32_program.c) with headless Vulkan, built once as A32
code (with a Thumb thread) and once as Thumb code throughout.

The program is compiled for AArch32 with Clang, linked here (no ARM linker is needed: the code is position
independent, -fropi, and the few relocations between its sections are applied below), and packed into an ExeFS: an
NSO with MOD0, an ELF32 dynamic table, a symbol and a REL relocation, and a 32-bit NPDM. The runtime must run it to
its svcExitProcess, logging each step and the pages its block tables are on, and record the indirect call target the
recompiler couldn't find in the hints file, which the next recompile then compiles ahead of time. No game assets.

    nx_arm32_smoke.py --runtime build/scarletnx [--cc clang]
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

CFLAGS = [
    '--target=armv8a-none-eabihf',
    '-mcpu=cortex-a57',
    '-mfpu=crypto-neon-fp-armv8',
    '-mfloat-abi=hard',
    '-O2',
    '-ffreestanding',
    '-fno-builtin',
    '-fropi',
    '-fno-jump-tables',
    '-fno-unwind-tables',
    '-fno-exceptions',
    '-fno-asynchronous-unwind-tables',
]
HEADER = 0x10  # the module's first words: a branch to the entry and MOD0's offset
SHF_WRITE, SHF_ALLOC = 1, 2


def sign(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


class Elf32:
    """An ELF32 relocatable object: its sections, symbols and REL relocations"""

    def __init__(self, data):
        assert data[:4] == b'\x7fELF' and data[4] == 1 and data[5] == 1, 'not a little-endian ELF32 object'
        self.data = data
        (shoff,) = struct.unpack_from('<I', data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
        self.sections = []
        for i in range(shnum):
            name, type_, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
                '<10I', data, shoff + i * shentsize
            )
            self.sections.append(
                dict(
                    name=name,
                    type=type_,
                    flags=flags,
                    offset=offset,
                    size=size,
                    link=link,
                    info=info,
                    align=max(align, 1),
                )
            )
        names = self.sections[shstrndx]
        for s in self.sections:
            s['name'] = self.string(names, s['name'])
        self.symbols = []
        for s in self.sections:
            if s['type'] == 2:  # SHT_SYMTAB
                strings = self.sections[s['link']]
                for at in range(s['offset'], s['offset'] + s['size'], 16):
                    name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, at)
                    self.symbols.append(dict(name=self.string(strings, name), value=value, type=info & 15, shndx=shndx))

    def string(self, section, offset):
        start = section['offset'] + offset
        return self.data[start : self.data.index(b'\0', start)].decode()


def link(obj):
    """The object's allocated sections, one after another from HEADER, with its relocations applied: the image, and
    each symbol's offset in it (bit 0 set for a Thumb function)"""
    image = bytearray(HEADER)
    placed = {}
    for i, s in enumerate(obj.sections):
        if not s['flags'] & SHF_ALLOC or s['type'] not in (1, 8) or s['name'].startswith('.ARM.ex'):
            continue
        assert not (s['flags'] & SHF_WRITE and s['size']), f'{s["name"]}: the program may not have writable data'
        image += bytes(-len(image) % s['align'])
        placed[i] = len(image)
        image += obj.data[s['offset'] : s['offset'] + s['size']] if s['type'] == 1 else bytes(s['size'])
    image += bytes(-len(image) % 4)
    symbols = {}
    for sym in obj.symbols:
        if sym['shndx'] in placed and sym['name']:
            symbols[sym['name']] = placed[sym['shndx']] + sym['value']
    for s in obj.sections:
        if s['type'] != 9 or s['info'] not in placed:  # SHT_REL against a placed section
            continue
        base = placed[s['info']]
        table = obj.sections[s['link']]
        for at in range(s['offset'], s['offset'] + s['size'], 8):
            offset, info = struct.unpack_from('<II', obj.data, at)
            sym = obj.symbols[info >> 8]
            kind = info & 0xFF
            assert sym['shndx'] in placed, f'relocation against {sym["name"] or "a section"} outside the image'
            target = placed[sym['shndx']] + (sym['value'] if sym['type'] != 3 else 0)  # STT_SECTION: the section
            thumb = target & 1 if sym['type'] == 2 else 0
            target &= ~1 if sym['type'] == 2 else ~0
            p = base + offset
            (word,) = struct.unpack_from('<I', image, p)
            hw1, hw2 = word & 0xFFFF, word >> 16
            if kind == 3:  # R_ARM_REL32
                struct.pack_into('<I', image, p, (((target + word) | thumb) - p) & 0xFFFFFFFF)
            elif kind in (28, 29):  # R_ARM_CALL, R_ARM_JUMP24
                value = target + sign(word << 2, 26) - p
                if thumb:
                    assert kind == 28, 'a B to Thumb code needs a veneer'
                    word = 0xFA000000 | ((value >> 1) & 1) << 24  # BLX
                struct.pack_into('<I', image, p, (word & 0xFF000000) | ((value >> 2) & 0xFFFFFF))
            elif kind in (10, 30):  # R_ARM_THM_CALL, R_ARM_THM_JUMP24
                s_, j1, j2 = (hw1 >> 10) & 1, (hw2 >> 13) & 1, (hw2 >> 11) & 1
                addend = sign(
                    s_ << 24 | (1 - (j1 ^ s_)) << 23 | (1 - (j2 ^ s_)) << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1,
                    25,
                )
                if thumb:
                    value = target + addend - p
                    hw2 |= 0x1000  # BL
                else:
                    assert kind == 10, 'a B.W to A32 code needs a veneer'
                    value = target + addend - (p & ~3)
                    hw2 &= ~0x1000  # BLX
                s_ = (value >> 24) & 1
                hw1 = (hw1 & 0xF800) | s_ << 10 | (value >> 12) & 0x3FF
                hw2 = (
                    (hw2 & 0xD000)
                    | (1 - ((value >> 23) & 1 ^ s_)) << 13
                    | (1 - ((value >> 22) & 1 ^ s_)) << 11
                    | (value >> 1) & 0x7FF
                )
                struct.pack_into('<HH', image, p, hw1, hw2)
            elif kind in (45, 46):  # R_ARM_MOVW_PREL_NC, R_ARM_MOVT_PREL
                value = ((target + sign((word >> 4) & 0xF000 | word & 0xFFF, 16)) | thumb) - p
                value = value >> 16 if kind == 46 else value
                struct.pack_into('<I', image, p, word & 0xFFF0F000 | (value & 0xF000) << 4 | value & 0xFFF)
            elif kind in (49, 50):  # R_ARM_THM_MOVW_PREL_NC, R_ARM_THM_MOVT_PREL
                imm = (hw1 & 15) << 12 | ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | hw2 & 0xFF
                value = ((target + sign(imm, 16)) | thumb) - p
                value = (value >> 16 if kind == 50 else value) & 0xFFFF
                hw1 = hw1 & 0xFBF0 | (value >> 12) | ((value >> 11) & 1) << 10
                hw2 = hw2 & 0x8F00 | ((value >> 8) & 7) << 12 | value & 0xFF
                struct.pack_into('<HH', image, p, hw1, hw2)
            else:
                raise AssertionError(f'relocation type {kind} against {sym["name"]}')
    return image, symbols


def page(n):
    return (n + 0xFFF) & ~0xFFF


def make_nso(text, symbols):
    """main: the linked code as .text; MOD0, the dynamic table, its symbols and a REL relocation as .rodata; and in
    .data the word that relocation names, holding nx_arm32_twice's offset (an R_ARM_RELATIVE pointer)"""
    entry = symbols['nx_arm32_start']
    text_size = page(len(text))
    ro_offset = text_size
    data_offset = ro_offset + 0x1000
    data = bytearray(16)
    struct.pack_into('<I', data, 0, symbols['nx_arm32_twice'])
    strings = b'\0nx_arm32_start\0worker\0'
    symtab = bytearray(16)  # the null symbol
    symtab += struct.pack('<IIIBBH', 1, entry, 0, 0x12, 0, 1)  # global function, A32
    symtab += struct.pack('<IIIBBH', 16, symbols['worker'], 0, 0x02, 0, 1)  # local function, Thumb (bit 0 set)
    ro = bytearray(0x400)
    mod0, dynamic, sym_at, str_at, rel_at = 0, 0x40, 0x100, 0x180, 0x200
    ro[mod0 : mod0 + 4] = b'MOD0'
    end = data_offset + len(data) - ro_offset  # the BSS, empty, after .data
    struct.pack_into('<6i', ro, mod0 + 4, dynamic, end, end, 0, 0, 0)
    tags = [
        (6, ro_offset + sym_at),
        (5, ro_offset + str_at),
        (10, len(strings)),
        (11, 16),
        (17, ro_offset + rel_at),
        (18, 8),
        (19, 8),
        (0, 0),
    ]
    for i, (tag, value) in enumerate(tags):
        struct.pack_into('<iI', ro, dynamic + 8 * i, tag, value)
    ro[sym_at : sym_at + len(symtab)] = symtab
    ro[str_at : str_at + len(strings)] = strings
    struct.pack_into('<II', ro, rel_at, data_offset, 23)  # R_ARM_RELATIVE
    code = bytearray(text) + bytes(text_size - len(text))
    # Loader enters the module's start in A32 state: B to an A32 entry, BLX to a Thumb one
    if entry & 1:
        target = entry & ~1
        struct.pack_into('<I', code, 0, 0xFA000000 | (target & 2) << 23 | ((target - 8) >> 2) & 0xFFFFFF)
    else:
        struct.pack_into('<I', code, 0, 0xEA000000 | ((entry - 8) >> 2) & 0xFFFFFF)
    struct.pack_into('<I', code, 4, ro_offset + mod0)
    segments = [(0x100, 0, code), (0x100 + len(code), ro_offset, ro), (0x100 + len(code) + len(ro), data_offset, data)]
    header = bytearray(0x100)
    header[0:4] = b'NSO0'
    for i, (file_offset, memory_offset, body) in enumerate(segments):
        struct.pack_into('<III', header, 0x10 + 0x10 * i, file_offset, memory_offset, len(body))
        struct.pack_into('<I', header, 0x60 + 4 * i, len(body))
    header[0x40:0x60] = bytes(range(1, 33))  # the build ID
    return bytes(header + code + ro + data)


def make_npdm():
    """A 32-bit process (flags: is64 clear, address space type 0) on cores 0 to 2, priorities 28 to 59"""
    meta = bytearray(0x80)
    meta[0:4] = b'META'
    meta[12] = 0  # AArch32, 32-bit address space
    meta[14], meta[15] = 44, 0
    struct.pack_into('<I', meta, 0x1C, 0x40000)
    meta[0x20:0x2B] = b'ARM32 Smoke'
    aci = bytearray(0x40)
    aci[0:4] = b'ACI0'
    struct.pack_into('<Q', aci, 0x10, 0x0100000000ABC000)
    struct.pack_into('<II', aci, 0x30, 0x38, 4)
    caps = struct.pack('<I', 0x7 | 59 << 4 | 28 << 10 | 0 << 16 | 2 << 24)  # ThreadInfo
    aci = aci[:0x38] + caps
    struct.pack_into('<II', meta, 0x70, 0x80, len(aci))
    return bytes(meta + aci)


def run(command, cwd, env, timeout, name, scratch):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, timeout=timeout, check=False)
    (scratch / f'{name}.stdout').write_bytes(result.stdout)
    (scratch / f'{name}.stderr').write_bytes(result.stderr)
    return result


def smoke(args, runtime, repo, source, scratch, mode):
    """the program built with -m<mode>: recompiled and run twice, the second time after a recompile with the hints the
    first run left"""
    scratch.mkdir()
    obj = scratch / 'program.o'
    subprocess.run([args.cc, *CFLAGS, f'-m{mode}', '-c', str(source), '-o', str(obj)], check=True)
    text, symbols = link(Elf32(obj.read_bytes()))
    exefs = scratch / 'exefs'
    exefs.mkdir()
    (exefs / 'main').write_bytes(make_nso(text, symbols))
    (exefs / 'main.npdm').write_bytes(make_npdm())
    env = os.environ.copy()
    env.pop('SNX_GAME', None)
    name = f'ARM32 Smoke {mode}'
    recompile = [
        str(runtime),
        '--repo',
        str(repo),
        '-r',
        str(exefs),
        name,
        '--runtime',
        str(runtime),
        '--jobs',
        '2',
        '--outdir',
        str(scratch / 'output'),
    ]
    folder = scratch / 'output' / name
    thrice = 0x200000 + symbols['nx_arm32_thrice']  # bit 0 set for Thumb code
    late = f'compiled {"Thumb" if thrice & 1 else "A32"} block {thrice & ~1:x} while running'
    for attempt in (1, 2):
        compiled = run(recompile, scratch, env, args.timeout, f'compile{attempt}', scratch)
        if compiled.returncode or b'AArch32:' not in compiled.stdout:
            raise RuntimeError(f'{mode}: recompilation {attempt} returned {compiled.returncode}')
        result = run([str(folder / name), '-H', '-v'], folder, env, args.timeout, f'run{attempt}', scratch)
        log = (folder / f'{name}.log').read_text(encoding='utf-8', errors='replace')
        (scratch / f'run{attempt}.log').write_text(log)
        for line in (
            'A32: started',
            'A32: TLS ok',
            'A32: ticks ok',
            'A32: indirect calls ok',
            'A32: Thumb thread ok',
            'A32: done',
        ):
            if f'[game] {line}' not in log:
                raise RuntimeError(f'{mode}: run {attempt}: no "{line}" in the log')
        if 'AArch32: the block tables: ' not in log:
            raise RuntimeError(f"{mode}: run {attempt}: no line on the block tables' pages in the log")
        if result.returncode or 'The game exited (code 0)' not in log:
            raise RuntimeError(f'{mode}: run {attempt}: the runtime returned {result.returncode}')
        if attempt == 1 and late not in log:
            raise RuntimeError(f"{mode}: run 1: the runtime didn't compile nx_arm32_thrice ({thrice:x}) itself")
        if attempt == 1 and f'{thrice:x}' not in (folder / f'{name}.hints').read_text().split():
            raise RuntimeError(f'{mode}: run 1: nx_arm32_thrice is not in the hints file')
        if attempt == 2 and late in log:
            raise RuntimeError(f"{mode}: run 2: the recompile with the hints file didn't compile nx_arm32_thrice")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--artifact-dir', type=Path, default=Path('build/nx-arm32-failures'))
    args = parser.parse_args()
    runtime, repo, artifacts = args.runtime.resolve(), args.repo.resolve(), args.artifact_dir.resolve()
    source = Path(__file__).resolve().with_name('nx_arm32_program.c')
    with tempfile.TemporaryDirectory(prefix='scarletnx-nx-arm32-') as temporary:
        scratch = Path(temporary)
        try:
            for mode in ('arm', 'thumb'):
                smoke(args, runtime, repo, source, scratch / mode, mode)
            print('Synthetic 32-bit program recompiled and ran: SVCs, Thumb thread, coprocessor registers, hints.')
            return 0
        except (OSError, RuntimeError, AssertionError, subprocess.SubprocessError) as error:
            artifacts.mkdir(parents=True, exist_ok=True)
            saved = artifacts / 'nx-arm32'
            if saved.exists():
                shutil.rmtree(saved)
            shutil.copytree(scratch, saved)
            print(f'NX AArch32 smoke: {error}; saved {saved}', file=sys.stderr)
            return 1


if __name__ == '__main__':
    raise SystemExit(main())
