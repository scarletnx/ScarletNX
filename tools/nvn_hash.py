#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Keys for a game's NVN driver: hashes of its code, function by function, that are the same for the same code at
another address, and the layouts its procedures keep, read by running them. What a key of NVN layouts by the driver's
own code (instead of, or beside, its API version) would compare.

    tools/nvn_hash.py work/Game [work/Other ...]       # a summary per game
    tools/nvn_hash.py --json DIR work/Game [...]       # also DIR/<game>.json with every hash and signature
    tools/nvn_hash.py --compare DIR/*.json             # compare games: the keys, every pair, what differs
    tools/nvn_hash.py --graphics work/Game [...]       # which graphics API each game's modules use

The procedure table, the API version and every constant nvnDeviceGetInteger answers (its switch), and the object
footprints come from tools/nvn_census.py, the SDK labels with their build type ("15.3.1-Release") from the
middleware strings. Each procedure the table names is hashed four ways (A64 drivers; a 32-bit driver gets the first
only, with relocated words and the ARM and Thumb BL immediates masked):

- body: SHA-256 of the function's words from its start to the next function start (.eh_frame_hdr, the symbols, the
  BL targets and the table's functions, nvn_census's Functions), with every operand that depends on where the code
  or its data sits masked: the imm26 of a B or BL that leaves the function, the immediate of ADRP and ADR, the low
  12 bits an ADD, LDR or STR adds to a register an ADRP set (tracked in address order until the register is written
  again), the imm19 of a load literal, a CBZ, TBZ or B.cond that leaves the function, and any relocated word.
  Branches inside the function are relative and stay. Each masked reference adds the kind of what it names (code,
  rodata, data, bss) to the hash, and a load from read-only data adds the bytes it loads.
- linked: SHA-256 of the body hash and the body hashes, in order, of the functions it calls or branches to or whose
  address it takes (one level).
- closure: SHA-256 of the sorted set of linked hashes of every function reachable from it that way (the function,
  its callees, theirs...). Equal closures mean the same code runs from that entry, up to where in memory it and its
  data sit, as far as direct references reach: a call through a pointer (a vtable, the driver's run-time dispatch
  table) is not followed, and of the data only what a single load reads is hashed (not a table the code indexes).
- norm, norm_closure: the masked instructions as llvm-objdump prints them, the register numbers dropped and the
  lines sorted: equal for code a compiler only allocated registers for or scheduled differently.

A function's role says what nvn_direct takes from it (layout_role): object sizes (SetDefaults, Initialize), device
constants (DeviceGetInteger), command memory accounting (the CommandBuffer memory functions), constant bank 0 and
handles (the CommandBuffer bind, draw and dispatch functions, the Device handle getters), or nothing. The layout key
hashes the closures of the procedures with a role.

Layout signatures don't depend on the code's form at all: each SetDefaults, Set* and Get* procedure of a builder,
state or view type runs on a small A64 emulator (Emu) over an object of distinct bytes, with its arguments typed by
its prototype in gpu/nvn/nvn.hook (tools/hook_spec.py) and set to distinct values, and its signature is the bytes
it changes and what it returns. The semantic key hashes the signatures, the device's constant answers, the command
buffer's offsets and the footprints.

--graphics reads each module's dynamic symbols (exports and imports) and strings for nvnBootstrapLoader, Vulkan
(vkGetInstanceProcAddr, vkCreateInstance), EGL and OpenGL (eglGetProcAddress, glGetString). Pure Python with
llvm-objdump, as nvn_census.py: a game takes seconds. Its output names functions and addresses of the game: it
stays on this machine.
"""

import argparse
import array
import bisect
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE]
import hook_spec  # noqa: E402
import moduledump  # noqa: E402
import nvn_census as nc  # noqa: E402

# ---------------------------------------------------------------------------------------------------------------
# What nvn_direct takes from each procedure

CMDMEM = re.compile(r'CommandBuffer(Initialize|Finalize|AddCommandMemory|AddControlMemory|SetMemoryCallback'
                    r'(Data)?|GetCommandMemory|GetControlMemory|GetMemoryCallback|BeginRecording|EndRecording|Reset'
                    r'|CallCommands|CopyCommands)')
CBANK = re.compile(r'CommandBuffer(Bind(StorageBuffer|UniformBuffer|Texture|Image|VertexBuffer|Program|Separate)'
                   r'|Draw|Dispatch)|Device(GetTextureHandle|GetImageHandle|GetSeparate|GetTexelFetchHandle)')


DEVICE_ITEMS = 0x80  # nvnDeviceGetInteger items read from the driver's switch (NVN 55.15 has 0x69)


def layout_role(name):
    if name == 'DeviceGetInteger':
        return 'device constants'
    if any(name.endswith(w) and len(name) > len(w) for w in nc.WRITERS):
        return 'object size' if not CMDMEM.match(name) else 'object size, command memory'
    if CMDMEM.match(name):
        return 'command memory'
    if CBANK.match(name):
        return 'constant bank 0, handles'
    return ''


# ---------------------------------------------------------------------------------------------------------------
# Masking


def segment_kind(segs, bss_end, off):
    if segs[0][0] <= off < segs[0][1]:
        return 'code'
    if segs[1][0] <= off < segs[1][1]:
        return 'rodata'
    if segs[2][0] <= off < segs[2][1]:
        return 'data'
    if segs[2][1] <= off < bss_end:
        return 'bss'
    return 'outside'


class Hasher:
    """Masked hashes of one A64 module's functions (start offsets), each computed once"""

    def __init__(self, image, segs, bss_end, code, funcs, relocs):
        self.image, self.segs, self.bss_end = image, segs, bss_end
        self.code, self.funcs, self.relocs = code, funcs, relocs
        self.reloc_words = {o for o in relocs if segs[0][0] <= o < segs[0][1]}
        self._body, self._links, self._linked, self._closure = {}, {}, {}, {}

    def extent(self, start):
        return start, self.funcs.next_start(start)

    def scan(self, start):
        """(masked words as bytes, references [(kind, target)] in order)"""
        code = self.code
        lo, hi = self.extent(start)
        w = code.w
        i0, i1 = code.index(lo), min(code.index(hi), len(w))
        pages = {}  # register: ADRP page
        out = array.array('I')
        refs = []
        for i in range(i0, i1):
            x = w[i]
            pc = code.pc(i)
            m = x
            if pc in self.reloc_words or pc - 4 in self.reloc_words:  # half of a relocated 8-byte word
                out.append(0)
                continue
            op = x & 0x7C000000
            if op == 0x14000000:  # B, BL
                t = pc + nc.sext(x & 0x3FFFFFF, 26) * 4
                if not lo <= t < hi:
                    m = x & 0xFC000000
                    refs.append(('code', t, 0))
            elif (x & 0x9F000000) == 0x90000000:  # ADRP
                page = (pc & ~0xFFF) + (nc.sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21) << 12)
                pages[x & 31] = page
                out.append(x & 0x9F00001F)
                continue
            elif (x & 0x9F000000) == 0x10000000:  # ADR
                t = pc + nc.sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21)
                m = x & 0x9F00001F
                refs.append((segment_kind(self.segs, self.bss_end, t), t, 0))
            elif (x & 0x3B000000) == 0x18000000:  # LDR literal
                t = pc + nc.sext((x >> 5) & 0x7FFFF, 19) * 4
                m = x & 0xFF00001F
                opc = x >> 30
                width = (4 << opc) if x & 0x04000000 else (8 if opc == 1 else 4 if opc in (0, 2) else 0)
                refs.append((segment_kind(self.segs, self.bss_end, t), t, width))
            elif (x & 0x7E000000) == 0x34000000 or (x & 0xFF000010) == 0x54000000:  # CBZ/CBNZ, B.cond
                t = pc + nc.sext((x >> 5) & 0x7FFFF, 19) * 4
                if not lo <= t < hi:
                    m = x & 0xFF00001F
                    refs.append(('code', t, 0))
            elif (x & 0x7E000000) == 0x36000000:  # TBZ/TBNZ
                t = pc + nc.sext((x >> 5) & 0x3FFF, 14) * 4
                if not lo <= t < hi:
                    m = x & 0xFFF8001F
                    refs.append(('code', t, 0))
            else:
                rn = (x >> 5) & 31
                if rn in pages:
                    if (x & 0x7F800000) == 0x11000000:  # ADD (immediate), 32 or 64 bit, without shift
                        t = pages[rn] + ((x >> 10) & 0xFFF)
                        m = x & ~(0xFFF << 10)
                        refs.append((segment_kind(self.segs, self.bss_end, t), t, 0))
                    elif (x & 0x3B000000) == 0x39000000:  # LDR/STR (unsigned offset), any size
                        size = x >> 30
                        if x & 0x04000000 and (x >> 23) & 1:  # SIMD&FP with opc<1> set: 128-bit
                            size = 4
                        t = pages[rn] + ((x >> 10) & 0xFFF) * (1 << size)
                        m = x & ~(0xFFF << 10)
                        loads = (x >> 22) & 1 if x & 0x04000000 else (x >> 22) & 3  # SIMD&FP: opc<0>; else opc
                        refs.append((segment_kind(self.segs, self.bss_end, t), t, (1 << size) if loads else 0))
            for r in list(pages):
                if nc.redefines(x, r):
                    del pages[r]
            out.append(m)
        return out.tobytes(), refs

    def masked(self, start):
        return self.scan(start)[0]

    def body(self, start):
        if start not in self._body:
            data, refs = self.scan(start)
            h = hashlib.sha256(data)
            for kind, t, width in refs:
                h.update(kind.encode())
                if kind == 'code' and t in self.funcs.start_set:
                    h.update(b'f')  # a function's start, not a branch into one
                elif kind == 'rodata' and width:
                    h.update(self.image[t : t + width])  # a constant the code loads
            self._body[start] = h.hexdigest()
            self._links[start] = [t for kind, t, _ in refs if kind == 'code']
        return self._body[start]

    def links(self, start):
        self.body(start)
        return self._links[start]

    def target_start(self, t):
        """The function a code reference lands in"""
        return t if t in self.funcs.start_set else self.funcs.containing(t)

    def linked(self, start):
        if start not in self._linked:
            h = hashlib.sha256(self.body(start).encode())
            for t in self.links(start):
                s = self.target_start(t)
                h.update(self.body(s).encode() if s >= 0 else b'?')
                h.update(b'%d' % (t - s))  # where in it, relative
            self._linked[start] = h.hexdigest()
        return self._linked[start]

    def reach(self, start):
        seen, stack = {start}, [start]
        while stack:
            f = stack.pop()
            for t in self.links(f):
                s = self.target_start(t)
                if s >= 0 and s not in seen:
                    seen.add(s)
                    stack.append(s)
        return seen

    def closure(self, start):
        if start not in self._closure:
            r = self.reach(start)
            h = hashlib.sha256()
            for x in sorted(self.linked(f) for f in r):
                h.update(x.encode())
            self._closure[start] = (h.hexdigest(), len(r))
        return self._closure[start]


REGISTER = re.compile(r'\b([xwbhsdqv])([0-9]+)\b')
BRANCH = re.compile(r'^(b|bl|cbz|cbnz|tbz|tbnz|b\.[a-z]+)$')
ADDRESS = re.compile(r'(?<![#\w])0x[0-9a-f]+')  # a PC-relative target as llvm-objdump prints it (ADRP, ADR, literal)


def normalized(hasher, starts):
    """{start: SHA-256 of the function's masked instructions as text, with the register numbers dropped and the lines
    sorted}: equal for code that a compiler only allocated registers for or scheduled differently. Disassembled with
    llvm-objdump (as tools/disasm.py), all the functions in one run."""
    starts = sorted(starts)
    blob, where = bytearray(), []
    for f in starts:
        where.append(len(blob))
        blob += hasher.masked(f)
    lines = {}
    with tempfile.TemporaryDirectory() as tmp:
        raw, elf = os.path.join(tmp, 'code.bin'), os.path.join(tmp, 'code.elf')
        with open(raw, 'wb') as f:
            f.write(bytes(blob))
        subprocess.run(['llvm-objcopy', '-I', 'binary', '-O', 'elf64-littleaarch64', '--rename-section',
                        '.data=.text,alloc,load,readonly,code', raw, elf], check=True)
        out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--triple=aarch64', elf], check=True,
                             capture_output=True, text=True).stdout
    for line in out.splitlines():
        head, sep, rest = line.partition(':')
        if not sep:
            continue
        try:
            addr = int(head.strip(), 16)
        except ValueError:
            continue
        text = rest.split('//')[0].split('<')[0].strip()
        parts = text.split(None, 1)
        if not parts:
            continue
        if BRANCH.match(parts[0]):
            ops = parts[1].split(', ') if len(parts) > 1 else []
            text = ' '.join([parts[0]] + [REGISTER.sub(r'\1', o) for o in ops[:-1]] + ['L'])
        else:
            text = ADDRESS.sub('A', REGISTER.sub(r'\1', text))
        lines[addr] = text
    out = {}
    for i, f in enumerate(starts):
        lo, hi = where[i], where[i + 1] if i + 1 < len(where) else len(blob)
        body = sorted(lines.get(a, '?') for a in range(lo, hi, 4))
        out[f] = hashlib.sha256('\n'.join(body).encode()).hexdigest()
    return out


# ---------------------------------------------------------------------------------------------------------------
# Layout signatures: what a builder's or state's procedures do to the object, run on a few A64 instructions


OBJ, OUT, STACK = 1 << 40, 2 << 40, 3 << 40  # where the object, the pointer arguments and the stack sit
REGION = 1 << 40


class Stop(Exception):
    """An instruction the emulator doesn't take"""


def pattern(addr):
    """The byte a fresh region holds at addr: distinct per offset, so a value read says where it was read"""
    off = addr % REGION
    return (off ^ (off >> 8) * 0x5B ^ (addr // REGION) * 0x3D) & 0xFF


class Emu:
    """Runs one procedure of the driver on an object of pattern bytes (pattern()) with the argument registers set to
    distinct values, and records every byte it stores outside its stack, and what it returns. A load from the
    module's image reads it (module offsets are the addresses, as ADRP computes them), anything else reads the
    pattern. Calls are followed (memset, bzero, memcpy and memmove by name), at most MAX_STEPS instructions."""

    MAX_STEPS = 4000

    def __init__(self, image, code, named):
        self.image, self.code, self.named = image, code, named

    def read(self, a, n):
        if a < len(self.image) and a + n <= len(self.image):
            return bytes(self.image[a : a + n])
        return bytes(self.mem.get(a + i, pattern(a + i)) for i in range(n))

    def write(self, a, data):
        for i, b in enumerate(data):
            self.mem[a + i] = b

    def x(self, r, sp=False):
        return (self.sp if sp else 0) if r == 31 else self.r[r]

    def setx(self, r, v, sp=False, bits=64):
        v &= (1 << bits) - 1
        if r == 31:
            if sp:
                self.sp = v
            return
        self.r[r] = v

    def run(self, start, args=None, fargs=None):
        """args {x register: value}, fargs {v register: bytes}: the others hold distinct junk, x0 the object"""
        self.r = [0x0101010101010101 * (0x10 + i) for i in range(31)]
        self.v = [bytes([0x30 + i]) * 16 for i in range(32)]
        self.r[0] = OBJ
        for i, v in (args or {}).items():
            self.r[i] = v
        for i, v in (fargs or {}).items():
            self.v[i] = v.ljust(16, b'\0')
        self.r[30] = 0
        self.sp = STACK + 0x10000
        self.mem = {}
        if hasattr(self, 'nzcv'):
            del self.nzcv
        pc, depth, steps = start, [], 0
        while True:
            steps += 1
            if steps > self.MAX_STEPS:
                raise Stop('too many steps')
            if pc == 0:
                break
            if pc in self.named:
                self.call_named(self.named[pc])
                pc = self.r[30]
                continue
            i = self.code.index(pc)
            if not 0 <= i < len(self.code.w):
                raise Stop('pc %#x outside the code' % pc)
            pc = self.step(self.code.w[i], pc)
        writes = {a: b for a, b in self.mem.items() if not STACK <= a < STACK + REGION and b != pattern(a)}
        return writes, self.r[0], self.v[0][:8]

    def call_named(self, name):
        dst, a1, n = self.r[0], self.r[1], self.r[2]
        if name == 'bzero':
            n, a1 = a1, None
        if n > 0x10000:
            raise Stop('%s of %#x bytes' % (name, n))
        if name in ('memset', 'bzero'):
            self.write(dst, bytes([(a1 or 0) & 0xFF]) * n)
        else:
            self.write(dst, self.read(a1, n))

    def addr_imm(self, x, scale):
        return self.x((x >> 5) & 31, True) + ((x >> 10) & 0xFFF) * scale

    def step(self, x, pc):
        nxt = pc + 4
        rd, rn = x & 31, (x >> 5) & 31
        sf = x >> 31
        bits = 64 if sf else 32
        c = nc.constant(x, {r: v for r, v in enumerate(self.r)})
        if c is not None and (x & 0x7F800000) in (0x52800000, 0x12800000, 0x72800000) or \
                (c is not None and (x & 0x7F8003E0) == 0x320003E0):
            self.setx(c[0], c[1], bits=bits)
            return nxt
        if (x & 0x9F000000) == 0x90000000:  # adrp
            self.setx(rd, (pc & ~0xFFF) + (nc.sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21) << 12))
            return nxt
        if (x & 0x9F000000) == 0x10000000:  # adr
            self.setx(rd, pc + nc.sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21))
            return nxt
        if (x & 0x1F000000) == 0x11000000:  # add/sub (immediate)
            imm = ((x >> 10) & 0xFFF) << (12 if x & (1 << 22) else 0)
            a = self.x(rn, True)
            v = a + (-imm if x & (1 << 30) else imm)
            if x & (1 << 29):
                self.flags(a, imm, bool(x & (1 << 30)), bits)
                self.setx(rd, v, bits=bits)
                return nxt
            self.setx(rd, v, sp=True, bits=bits)
            return nxt
        if (x & 0x1F200000) == 0x0B000000 or (x & 0x1F000000) == 0x0A000000:  # add/sub, logical (shifted register)
            rm, sh, amt = (x >> 16) & 31, (x >> 22) & 3, (x >> 10) & 0x3F
            b = self.x(rm)
            if sh == 0:
                b = b << amt
            elif sh == 1:
                b = (b & ((1 << bits) - 1)) >> amt
            else:
                raise Stop('shift %d' % sh)
            a = self.x(rn)
            if (x & 0x1F000000) == 0x0A000000:
                if x & (1 << 21):
                    b = ~b
                opc = (x >> 29) & 3
                v = (a & b, a | b, a ^ b, a & b)[opc]
                if opc == 3:
                    self.flags_logical(v, bits)
            else:
                v = a - b if x & (1 << 30) else a + b
                if x & (1 << 29):
                    self.flags(a, b, bool(x & (1 << 30)), bits)
            self.setx(rd, v, bits=bits)
            return nxt
        if (x & 0x1F800000) == 0x12000000:  # logical (immediate)
            n = (x >> 22) & 1
            imm = nc.bitmask_imm(n, (x >> 16) & 0x3F, (x >> 10) & 0x3F, bits)
            if imm is None:
                raise Stop('logical immediate')
            a = self.x(rn)
            opc = (x >> 29) & 3
            v = (a & imm, a | imm, a ^ imm, a & imm)[opc]
            if opc == 3:
                self.flags_logical(v, bits)
            self.setx(rd, v, sp=opc != 3, bits=bits)
            return nxt
        if (x & 0x1F800000) == 0x13000000:  # bitfield: SBFM, BFM, UBFM
            opc, immr, imms = (x >> 29) & 3, (x >> 16) & 0x3F, (x >> 10) & 0x3F
            src, mask = self.x(rn), (1 << bits) - 1
            if imms >= immr:  # extract bits immr..imms to the bottom
                width = imms - immr + 1
                field = (src >> immr) & ((1 << width) - 1)
                pos = 0
            else:  # insert bits 0..imms at bits - immr
                width = imms + 1
                field = src & ((1 << width) - 1)
                pos = bits - immr
            if opc == 1:  # bfm keeps the other bits of rd
                keep = self.x(rd) & ~(((1 << width) - 1) << pos) & mask
                v = keep | (field << pos)
            elif opc == 0:  # sbfm
                if field >> (width - 1) & 1:
                    field -= 1 << width
                v = (field << pos) & mask
            elif opc == 2:
                v = field << pos
            else:
                raise Stop('bitfield')
            self.setx(rd, v, bits=bits)
            return nxt
        if (x & 0x7F200000) == 0x13800000:  # extr (ror)
            rm, lsb = (x >> 16) & 31, (x >> 10) & 0x3F
            v = ((self.x(rn) << bits | (self.x(rm) & ((1 << bits) - 1))) >> lsb)
            self.setx(rd, v, bits=bits)
            return nxt
        if (x & 0x1FE00000) == 0x1A800000:  # csel, csinc, csinv, csneg
            rm, cond, op = (x >> 16) & 31, (x >> 12) & 15, ((x >> 30) & 1) << 1 | ((x >> 10) & 1)
            if self.holds(cond):
                v = self.x(rn)
            else:
                b = self.x(rm)
                v = (b, b + 1, ~b, -b)[op]
            self.setx(rd, v, bits=bits)
            return nxt
        if (x & 0x1FE00410) == 0x1A400000:  # ccmp, ccmn (register or immediate)
            cond, imm = (x >> 12) & 15, (x >> 16) & 31
            b = imm if x & (1 << 11) else self.x(imm)
            if self.holds(cond):
                self.flags(self.x(rn), b, bool(x & (1 << 30)), bits)
            else:
                self.nzcv = x & 15
            return nxt
        if (x & 0x1F200000) == 0x0B200000:  # add/sub (extended register)
            rm, opt, amt = (x >> 16) & 31, (x >> 13) & 7, (x >> 10) & 7
            b = self.x(rm)
            size = 8 << (opt & 3)
            b &= (1 << size) - 1
            if opt & 4 and b >> (size - 1):
                b -= 1 << size
            b <<= amt
            a = self.x(rn, True)
            v = a - b if x & (1 << 30) else a + b
            if x & (1 << 29):
                self.flags(a, b, bool(x & (1 << 30)), bits)
            self.setx(rd, v, sp=not x & (1 << 29), bits=bits)
            return nxt
        if (x & 0x7F000000) == 0x1B000000:  # madd, msub, smaddl, umaddl...
            rm, ra, op31, o0 = (x >> 16) & 31, (x >> 10) & 31, (x >> 21) & 7, (x >> 15) & 1
            a, b = self.x(rn), self.x(rm)
            if op31 in (1, 5):  # smaddl, umaddl: 32-bit sources
                a, b = a & 0xFFFFFFFF, b & 0xFFFFFFFF
                if op31 == 1:
                    a, b = nc.sext(a, 32), nc.sext(b, 32)
            elif op31 != 0:
                raise Stop('multiply high')
            prod = a * b
            self.setx(rd, self.x(ra) - prod if o0 else self.x(ra) + prod, bits=bits)
            return nxt
        if (x & 0x7FE0F800) == 0x1AC00800:  # udiv, sdiv
            rm, mask = (x >> 16) & 31, (1 << bits) - 1
            a, b = self.x(rn) & mask, self.x(rm) & mask
            if x & (1 << 10):
                a = a - (1 << bits) if a >> (bits - 1) else a
                b = b - (1 << bits) if b >> (bits - 1) else b
                v = 0 if b == 0 else abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
            else:
                v = 0 if b == 0 else a // b
            self.setx(rd, v, bits=bits)
            return nxt
        if (x & 0x7FE0F000) == 0x1AC02000:  # lslv, lsrv, asrv, rorv
            rm, op = (x >> 16) & 31, (x >> 10) & 3
            amt, a, mask = self.x(rm) % bits, self.x(rn) & ((1 << bits) - 1), (1 << bits) - 1
            if op == 0:
                v = a << amt
            elif op == 1:
                v = a >> amt
            elif op == 2:
                v = (a - (1 << bits) if a >> (bits - 1) else a) >> amt
            else:
                v = (a >> amt | a << (bits - amt)) & mask
            self.setx(rd, v, bits=bits)
            return nxt
        if (x & 0xFF000010) == 0x54000000:  # b.cond
            return pc + nc.sext((x >> 5) & 0x7FFFF, 19) * 4 if self.holds(x & 15) else nxt
        if (x & 0x3A000000) == 0x28000000:  # load/store pair
            simd, opc, load = (x >> 26) & 1, x >> 30, (x >> 22) & 1
            size = (4 << opc) if simd else (4 if opc == 0 else 8)
            imm = nc.sext((x >> 15) & 0x7F, 7) * size
            index = (x >> 23) & 3
            base = self.x(rn, True)
            a = base if index == 1 else base + imm
            rt, rt2 = rd, (x >> 10) & 31
            for k, r in enumerate((rt, rt2)):
                self.transfer(simd, load, r, a + k * size, size, opc == 1 and not simd and load)
            if index in (1, 3):
                self.setx(rn, base + imm, sp=True)
            return nxt
        if (x & 0x3B000000) in (0x38000000, 0x39000000):  # load/store register
            simd, size, opc = (x >> 26) & 1, x >> 30, (x >> 22) & 3
            width = 1 << size
            if simd and opc in (2, 3) and size == 0:
                width = 16
            load = (opc & 1) if simd else opc != 0
            signed = not simd and opc >= 2
            if not simd and size == 3 and opc == 2:
                return nxt  # prfm
            base = self.x(rn, True)
            if x & (1 << 24):
                a, wb = base + ((x >> 10) & 0xFFF) * width, None
            else:
                kind = (x >> 10) & 3
                if x & (1 << 21):
                    if kind != 2:
                        raise Stop('atomic')
                    rm, ext, s = (x >> 16) & 31, (x >> 13) & 7, (x >> 12) & 1
                    off = self.x(rm)
                    if ext == 2:
                        off &= 0xFFFFFFFF
                    elif ext == 6:
                        off = nc.sext(off & 0xFFFFFFFF, 32)
                    a, wb = base + (off << (size if s else 0)), None
                else:
                    imm = nc.sext((x >> 12) & 0x1FF, 9)
                    a = base if kind == 1 else base + imm
                    wb = base + imm if kind in (1, 3) else None
            self.transfer(simd, load, rd, a, width, signed, opc == 2 and signed)
            if wb is not None:
                self.setx(rn, wb, sp=True)
            return nxt
        if (x & 0x3B000000) == 0x18000000:  # load literal
            a = pc + nc.sext((x >> 5) & 0x7FFFF, 19) * 4
            opc, simd = x >> 30, (x >> 26) & 1
            width = (4 << opc) if simd else (8 if opc == 1 else 4)
            self.transfer(simd, 1, rd, a, width, opc == 2 and not simd, True)
            return nxt
        if (x & 0xFFFFFFE0) in (0x6F00E400, 0x2F00E400) or (x & 0xBFFFFC00) == 0x0F000400 or \
                (x & 0xBFFFFC00) == 0x0F00E400:  # movi of zero
            self.v[rd] = bytes(16)
            return nxt
        if (x & 0xBFE0FC00) == 0x0E000C00:  # dup (general)
            imm5 = (x >> 16) & 31
            esize = 1 << ((imm5 & -imm5).bit_length() - 1)
            val = (self.x(rn) & ((1 << (8 * esize)) - 1)).to_bytes(esize, 'little')
            self.v[rd] = (val * (16 // esize))[: 16 if x & (1 << 30) else 8].ljust(16, b'\0')
            return nxt
        if (x & 0xFF3FFC00) in (0x1E270000, 0x9E670000):  # fmov s/d from w/x
            n = 8 if sf else 4
            self.v[rd] = (self.x(rn) & ((1 << (8 * n)) - 1)).to_bytes(n, 'little').ljust(16, b'\0')
            return nxt
        if (x & 0xFF3FFC00) in (0x1E260000, 0x9E660000):  # fmov w/x from s/d
            n = 8 if sf else 4
            self.setx(rd, int.from_bytes(self.v[rn][:n], 'little'))
            return nxt
        if (x & 0xFF201FE0) in (0x1E201000, 0x1E601000):  # fmov s/d, #imm
            imm8 = (x >> 13) & 0xFF
            sign = -1.0 if imm8 & 0x80 else 1.0
            e = ((imm8 >> 4) & 7) ^ 4
            e = e - 8 if e > 3 else e
            val = sign * (16 + (imm8 & 15)) / 16.0 * 2.0 ** (e + 1) / 2
            dbl = (x >> 22) & 1
            self.v[rd] = (struct.pack('<d', val) if dbl else struct.pack('<f', val)).ljust(16, b'\0')
            return nxt
        if (x & 0xBFE0FC00) == 0x0EA01C00:  # mov (orr) vector
            self.v[rd] = self.v[rn] if x & (1 << 30) else self.v[rn][:8].ljust(16, b'\0')
            return nxt
        if (x & 0xFC000000) == 0x94000000:  # bl
            self.r[30] = nxt
            return pc + nc.sext(x & 0x3FFFFFF, 26) * 4
        if (x & 0xFC000000) == 0x14000000:  # b
            return pc + nc.sext(x & 0x3FFFFFF, 26) * 4
        if (x & 0xFFFFFC1F) == 0xD65F0000:  # ret
            return self.x(rn)
        if x == 0xD503201F or (x & 0xFFFFF01F) == 0xD503201F:  # nop, hints
            return nxt
        if (x & 0x7E000000) == 0x34000000:  # cbz, cbnz on a known value
            v = self.x(rd) & ((1 << bits) - 1)
            take = (v == 0) != bool(x & (1 << 24))
            return pc + nc.sext((x >> 5) & 0x7FFFF, 19) * 4 if take else nxt
        if (x & 0x7E000000) == 0x36000000:  # tbz, tbnz on a known value
            bit = ((x >> 31) << 5) | ((x >> 19) & 31)
            take = ((self.x(rd) >> bit) & 1) == bool(x & (1 << 24))
            return pc + nc.sext((x >> 5) & 0x3FFF, 14) * 4 if take else nxt
        raise Stop('instruction %08x' % x)

    def flags(self, a, b, sub, bits):
        mask = (1 << bits) - 1
        a, b = a & mask, b & mask
        if sub:
            res, carry = (a - b) & mask, a >= b
            over = ((a ^ b) & (a ^ res)) >> (bits - 1) & 1
        else:
            full = a + b
            res, carry = full & mask, full >> bits
            over = (~(a ^ b) & (a ^ res)) >> (bits - 1) & 1
        self.nzcv = (res >> (bits - 1) & 1) << 3 | (res == 0) << 2 | bool(carry) << 1 | over

    def flags_logical(self, v, bits):
        v &= (1 << bits) - 1
        self.nzcv = (v >> (bits - 1) & 1) << 3 | (v == 0) << 2

    def holds(self, cond):
        if not hasattr(self, 'nzcv'):
            raise Stop('a condition before any flags')
        n, z, c, v = (self.nzcv >> 3) & 1, (self.nzcv >> 2) & 1, (self.nzcv >> 1) & 1, self.nzcv & 1
        base = (z, c, n, v, c and not z, n == v, n == v and not z, True)[cond >> 1]
        return (not base) if cond & 1 and cond != 15 else bool(base)

    def transfer(self, simd, load, r, a, width, signed=False, wide=False):
        if load:
            data = self.read(a, width)
            if simd:
                self.v[r] = data.ljust(16, b'\0')
            else:
                v = int.from_bytes(data, 'little')
                if signed and v >> (8 * width - 1):
                    v -= 1 << (8 * width)
                self.setx(r, v, bits=64 if (wide or width == 8) else 32)
        else:
            data = self.v[r][:width] if simd else (self.x(r) & ((1 << (8 * width)) - 1)).to_bytes(width, 'little')
            self.write(a, data)


SETTER = re.compile(r'^([A-Z][A-Za-z]*?(Builder|State|View))(SetDefaults|Set[A-Za-z0-9]+|Get[A-Za-z0-9]+)$')
NVN_HOOK = os.path.join(HERE, '..', 'src', 'nx', 'gpu', 'nvn', 'nvn.hook')
FLOATS = ('f32', 'f64')


def prototypes(path=NVN_HOOK, header=nc.NVN_H):
    """{procedure name in upper case: (return kind, [argument types])} from gpu/nvn/nvn.hook's prototypes, in the
    spec's types (a pointer as 'const NVNtexture*', a float as f32): the result as the kind hookgen passes it (addr
    for a pointer, i32 for an enum)"""
    try:
        protos = hook_spec.prototypes(path)
        types = hook_spec.header_types(header)
    except OSError:
        return {}
    return {name[3:].upper(): (hook_spec.kind(ret, types), args) for name, (ret, args, _) in protos.items()}


def call_args(argtypes):
    """({x register: value}, {v register: bytes}) for a procedure's arguments after the object: a pointer points to a
    region of its own (OUT + n * 0x10000), an integer is a distinct constant, a float a distinct value"""
    x, v, xi, vi = {}, {}, 1, 0
    for k, t in enumerate(argtypes[1:], 1):
        if '*' in t:
            x[xi] = OUT + k * 0x10000
            xi += 1
        elif t in FLOATS:
            v[vi] = struct.pack('<f' if t == 'f32' else '<d', 1.5 + k)
            vi += 1
        else:
            x[xi] = 0x0101010101010101 * (0x10 + k)
            xi += 1
    return x, v


def layout_signatures(image, code, names, funcs, named):
    """{procedure: signature} for the SetDefaults, Set* and Get* procedures of the builder, state and view types:
    the bytes it changes in the object and through its pointer arguments ("o+off=byte", "p<n>+off=byte"), and its
    return value as nvn.hook types it, or why it couldn't be run. The arguments are distinct constants and the object
    and pointed-to memory distinct bytes (pattern), so a setter's stores say which bits of the object hold its
    arguments and a getter's result where it read it. Equal signatures mean the procedure keeps those fields where
    the other driver keeps them."""
    emu = Emu(image, code, named)
    protos = prototypes()
    out = {}
    for n, f in zip(names, funcs):
        if not SETTER.match(n):
            continue
        ret, argtypes = protos.get(n.upper(), ('?', ['?']))
        x, v = call_args(argtypes)
        try:
            writes, x0, d0 = emu.run(f, x, v)
        except Stop as e:
            out[n] = 'stopped: %s' % e
            continue
        parts = []
        for a in sorted(writes):
            region = 'o' if OBJ <= a < OBJ + REGION else 'p%d' % ((a - OUT) // 0x10000) if OUT <= a < OUT + REGION \
                else 'm'
            off = a - OBJ if region == 'o' else (a - OUT) % 0x10000 if region[0] == 'p' else a
            parts.append('%s+%x=%02x' % (region, off, writes[a]))
        if ret in FLOATS:
            parts.append('ret %s' % d0[: 4 if ret == 'f32' else 8].hex())
        elif ret not in ('void', '?'):
            wide = ret in ('addr', 'u64', 'i64')
            parts.append('ret %x' % (x0 & ((1 << (64 if wide else 32)) - 1)))
        out[n] = ' '.join(parts)
    return out


# ---------------------------------------------------------------------------------------------------------------
# 32-bit drivers: the body hash only, with relocated words and the immediates of ARM and Thumb branches masked


def body32(image, lo, hi, thumb, relocs):
    out = bytearray(image[lo:hi])
    for o in relocs:
        if lo <= o < hi - 3:
            out[o - lo : o - lo + 4] = b'\0\0\0\0'
    if thumb:
        i = 0
        while i + 4 <= len(out):
            h1, h2 = struct.unpack_from('<HH', out, i)
            if (h1 & 0xF800) in (0xE800, 0xF000, 0xF800):  # a 32-bit instruction
                if (h1 & 0xF800) == 0xF000 and (h2 & 0xC000) == 0xC000:  # BL, BLX
                    struct.pack_into('<HH', out, i, 0xF000, h2 & 0xD000)
                i += 4
            else:
                i += 2
    else:
        for i in range(0, len(out) - 3, 4):
            x = struct.unpack_from('<I', out, i)[0]
            if (x & 0x0E000000) == 0x0A000000:  # B, BL, BLX (imm)
                struct.pack_into('<I', out, i, x & 0xFF000000)
    return hashlib.sha256(bytes(out)).hexdigest()


# ---------------------------------------------------------------------------------------------------------------


LABEL = re.compile(rb'SDK MW\+[A-Za-z]+\+[A-Za-z]+S[Dd][Kk]_(NVN|nnSdk)-([0-9_]+)(-[A-Za-z]+)?')


def labels(mods):
    """{'NVN': ..., 'nnSdk': ...}: the SDK middleware labels with their build type ("15.3.1-Release"), which
    nvn_census's versions and the hooks' version key (hle/hook/version.c) leave out"""
    out = {}
    for m in mods:
        for hit in LABEL.finditer(m.image):
            out.setdefault(hit.group(1).decode(), hit.group(2).decode().replace('_', '.') +
                           (hit.group(3) or b'').decode())
    return out


def bss_end(image, segs):
    at, fields = nc.mod0(image)
    return at + fields[2]


def hash_game(game):
    r = {'game': os.path.basename(os.path.normpath(game)), 'errors': []}
    mods, is64 = moduledump.load_program(game)
    r['is64'] = is64
    r['versions'] = {k: v[0] for k, v in nc.versions(mods).items()}
    r['labels'] = labels(mods)
    r['modules'] = [m.name for m in mods]
    nvn = next((m for m in mods if m.name != 'rtld' and re.search(rb'\0' + nc.ANCHOR + rb'\0', m.image)), None)
    if nvn is None:
        r['errors'].append('no NVN name pool')
        return r
    r['module'] = nvn.name
    segs = nc.nso_segments(game, nvn.name)
    relocs = nc.relocations(nvn.image, is64)
    code = nc.Code(nvn.image, segs[0]) if is64 else None
    try:
        table = nc.find_table(nvn, is64, segs, relocs, code)
    except nc.Fail as e:
        r['errors'].append('procedure table: %s' % e)
        return r
    names, funcs = table['names'], table['funcs']
    r['trampolines'] = bool(table.get('dispatch'))
    r['names_hash'] = hashlib.sha256('\n'.join(names).encode()).hexdigest()
    procs = {}
    if not is64:
        starts = sorted({f & ~1 for f in funcs} | {v & ~1 for v in nvn.syms} | {segs[0][1]})
        for n, f in zip(names, funcs):
            lo = f & ~1
            hi = starts[bisect.bisect_right(starts, lo)] if lo < starts[-1] else lo
            procs[n] = {'at': lo, 'size': hi - lo, 'body': body32(nvn.image, lo, hi, f & 1, relocs),
                        'role': layout_role(n)}
        r['procs'] = procs
        return r
    fn = nc.Functions(nvn, code, funcs)
    h = Hasher(nvn.image, segs, bss_end(nvn.image, segs), code, fn, relocs)
    for n, f in zip(names, funcs):
        lo, hi = h.extent(f)
        closure, reach = h.closure(f)
        procs[n] = {'at': f, 'size': hi - lo, 'body': h.body(f), 'linked': h.linked(f), 'closure': closure,
                    'reach': reach, 'role': layout_role(n)}
    r['procs'] = procs
    # every function a procedure reaches, for --compare to say which code differs
    helpers = set()
    for n, f in zip(names, funcs):
        reach = h.reach(f)
        procs[n]['reach_at'] = sorted(reach)
        helpers |= reach
    r['functions'] = {'%#x' % f: [h.body(f), h.linked(f), h.extent(f)[1] - f] for f in sorted(helpers)}
    norm = normalized(h, helpers)
    for n, f in zip(names, funcs):
        procs[n]['norm'] = norm[f]
        procs[n]['norm_closure'] = hashlib.sha256(''.join(sorted({norm[g] for g in procs[n]['reach_at']})).encode()
                                                  ).hexdigest()
    r['api'] = nc.api_version(nvn, code, table)
    answers = nc.device_answers(nvn, code, table, tuple(range(DEVICE_ITEMS)))
    r['device'] = {'%#x' % i: v for i, v in enumerate(answers or []) if v is not None}
    named = nc.memory_calls(nvn, code, is64)
    r['footprints'] = {k: v[0] for k, v in nc.footprints(code, fn, table, named, relocs).items()}
    r['signatures'] = layout_signatures(nvn.image, code, names, funcs, named)
    try:
        slow, facts = nc.find_slow_path(code, table)
        r['cmdbuf'] = {'callback': facts['callback'], 'userdata': facts['userdata'], 'cursor': facts['cursor']}
        r['slow_path'] = {'body': h.body(slow), 'closure': h.closure(slow)[0]}
    except nc.Fail as e:
        r['errors'].append('slow path: %s' % e)
    return r


def cursor_text(off):
    return '?' if off is None else '+%#x' % off


def short(x):
    return x[:12] if x else '-'


def label(r):
    api = '%d.%d' % tuple(r['api']) if r.get('api') else '?'
    lab = r.get('labels') or r['versions']
    return '%s (%s-bit, nnSdk %s, NVN %s, API %s)' % (r['game'], '64' if r['is64'] else '32', lab.get('nnSdk', '?'),
                                                       lab.get('NVN', '?'), api)


def layout_key(r, roles=None):
    """SHA-256 of the sorted (name, closure) of every procedure with a layout role (or the given roles)"""
    h = hashlib.sha256()
    for n, p in sorted(r.get('procs', {}).items()):
        if p['role'] and (roles is None or any(x in p['role'] for x in roles)):
            h.update(('%s %s\n' % (n, p.get('closure') or p['body'])).encode())
    return h.hexdigest()


def semantic_key(r):
    """SHA-256 of what nvn_direct depends on as this tool can read it: the layout signatures, the device's constant
    answers, the command buffer's offsets and the object footprints, none of which depends on how the compiler laid
    the code out"""
    h = hashlib.sha256()
    h.update(json.dumps([r.get('signatures'), r.get('device'), r.get('cmdbuf'), r.get('footprints')],
                        sort_keys=True).encode())
    return h.hexdigest()


def summary(r, out):
    out.write('== %s\n' % label(r))
    for e in r['errors']:
        out.write('  error: %s\n' % e)
    if 'procs' not in r:
        return
    p = r['procs']
    out.write('  %s: %d procedures%s, names %s, body sizes %d bytes\n' % (
        r['module'], len(p), ' (trampolines resolved)' if r.get('trampolines') else '', short(r['names_hash']),
        sum(x['size'] for x in p.values())))
    if r['is64']:
        out.write('  layout key %s (object size %s, device constants %s, command memory %s, constant bank 0 %s)\n' % (
            short(layout_key(r)), short(layout_key(r, ['object size'])), short(layout_key(r, ['device constants'])),
            short(layout_key(r, ['command memory'])), short(layout_key(r, ['constant bank 0']))))
        sig = r.get('signatures', {})
        out.write('  semantic key %s (%d layout signatures, %d not run; %d device answers)\n' % (
            short(semantic_key(r)), len(sig), sum(v.startswith('stopped') for v in sig.values()),
            len(r.get('device', {}))))
        if r.get('cmdbuf'):
            out.write('  command buffer: cursor %s, callback +%#x\n' % (cursor_text(r['cmdbuf']['cursor']),
                                                                      r['cmdbuf']['callback']))


# ---------------------------------------------------------------------------------------------------------------
# Comparing games


def compare(results, out):
    w = out.write
    rs = [r for r in results if 'procs' in r]
    rs.sort(key=lambda r: (not r['is64'], tuple(r.get('api') or (0, 0)), r['versions'].get('nnSdk', '')))
    w('| Game | bits | nnSdk | NVN label | API | procs | names | layout key | sizes | constants | cmd memory | cbank0 '
      '| cmdbuf cursor/callback | semantic key |\n|%s\n' % (' --- |' * 14))
    for r in rs:
        api = '%d.%d' % tuple(r['api']) if r.get('api') else '?'
        cb = r.get('cmdbuf')
        w('| %s | %d | %s | %s | %s | %d | %s | %s | %s | %s | %s | %s | %s | %s |\n' % (
            r['game'], 64 if r['is64'] else 32, (r.get('labels') or r['versions']).get('nnSdk', '?'),
            (r.get('labels') or r['versions']).get('NVN', '?'), api,
            len(r['procs']), short(r['names_hash'])[:8], short(layout_key(r))[:8],
            short(layout_key(r, ['object size']))[:8], short(layout_key(r, ['device constants']))[:8],
            short(layout_key(r, ['command memory']))[:8], short(layout_key(r, ['constant bank 0']))[:8],
            '%s/+%#x' % (cursor_text(cb['cursor']), cb['callback']) if cb else '-',
            short(semantic_key(r))[:8] if r['is64'] else '-'))
    w('\n')
    # Every pair of 64-bit games: procedures in common, equal bodies, equal closures, and the differing ones by role
    a64 = [r for r in rs if r['is64']]
    w('Pairs (common procedures: equal body / equal closure; differing closures with a layout role; footprints that '
      'differ)\n\n')
    for i, a in enumerate(a64):
        for b in a64[i + 1:]:
            common = sorted(set(a['procs']) & set(b['procs']))
            eqb = sum(a['procs'][n]['body'] == b['procs'][n]['body'] for n in common)
            eqc = sum(a['procs'][n]['closure'] == b['procs'][n]['closure'] for n in common)
            roles = {}
            for n in common:
                if a['procs'][n]['closure'] != b['procs'][n]['closure'] and a['procs'][n]['role']:
                    roles.setdefault(a['procs'][n]['role'], []).append(n)
            fa, fb = a.get('footprints', {}), b.get('footprints', {})
            fdiff = sorted(k for k in set(fa) & set(fb) if fa[k] != fb[k])
            same_api = a.get('api') == b.get('api')
            sa, sb = a.get('signatures', {}), b.get('signatures', {})
            ran = [n for n in sorted(set(sa) & set(sb)) if not sa[n].startswith('stopped') and
                   not sb[n].startswith('stopped')]
            sdiff = [n for n in ran if sa[n] != sb[n]]
            w('%s %s / %s: %d common, body %d, closure %d; only in one: %d/%d; footprints differ: %s\n' % (
                'SAME-API' if same_api else 'other', label(a), label(b), len(common), eqb, eqc,
                len(set(a['procs']) - set(b['procs'])), len(set(b['procs']) - set(a['procs'])),
                ', '.join('%s %#x/%#x' % (k, fa[k], fb[k]) for k in fdiff) or 'none'))
            w('    layout signatures run in both: %d, differing: %d%s\n' % (
                len(ran), len(sdiff), (' (' + ' '.join(sdiff[:10]) + (' ...' if len(sdiff) > 10 else '') + ')')
                if sdiff else ''))
            for role, ns in sorted(roles.items()):
                w('    %-28s %d differ: %s\n' % (role, len(ns), ' '.join(ns[:12]) + (' ...' if len(ns) > 12 else '')))
    w('\n')
    # Same API version, different code: the functions whose code differs, and what reaches them
    w('Same API version, different code: the functions of the first whose body has no equal in the second ("changed"),'
      ' or whose body has but whose callees differ ("rewired"), with the procedures that reach them; device answers '
      'that differ\n\n')
    for i, a in enumerate(a64):
        for b in a64[i + 1:]:
            if a.get('api') != b.get('api') or layout_key(a) == layout_key(b):
                continue
            w('%s / %s\n' % (label(a), label(b)))
            diff(a, b, out)
            diff(b, a, out)
    w('\n')
    # Same closure across different API versions, per layout role
    w('Layout-role procedures whose closure is equal across games of different API versions\n\n')
    for i, a in enumerate(a64):
        for b in a64[i + 1:]:
            if a.get('api') == b.get('api'):
                continue
            common = [n for n in sorted(set(a['procs']) & set(b['procs'])) if a['procs'][n]['role']]
            eq = [n for n in common if a['procs'][n]['closure'] == b['procs'][n]['closure']]
            w('  %s / %s: %d of %d\n' % (a['game'], b['game'], len(eq), len(common)))
    w('\n')
    # Stability: per layout procedure, the number of distinct closures among the games
    w('Distinct closures per layout-role procedure over %d A64 drivers (procedure: count)\n\n' % len(a64))
    names = sorted({n for r in a64 for n, p in r['procs'].items() if p['role']})
    hist = {}
    for n in names:
        vals = {r['procs'][n]['closure'] for r in a64 if n in r['procs']}
        hist.setdefault(len(vals), []).append(n)
    for k in sorted(hist):
        w('  %d distinct: %d procedures%s\n' % (k, len(hist[k]), (': ' + ' '.join(hist[k][:20])) if k <= 2 else ''))


def diff(a, b, out):
    """a's functions with no equal in b, each with the procedures reaching it"""
    fa, fb = a.get('functions', {}), b.get('functions', {})
    bodies = {v[0] for v in fb.values()}
    linked = {v[1] for v in fb.values()}
    reached = {}
    for n, p in a['procs'].items():
        for f in p.get('reach_at', []):
            reached.setdefault('%#x' % f, []).append(n)
    changed = [(k, v) for k, v in fa.items() if v[0] not in bodies]
    rewired = [(k, v) for k, v in fa.items() if v[0] in bodies and v[1] not in linked]
    out.write('  %s: %d of %d functions changed, %d rewired\n' % (a['game'], len(changed), len(fa), len(rewired)))
    for kind, group in (('changed', changed), ('rewired', rewired)):
        for k, v in sorted(group, key=lambda kv: -len(reached.get(kv[0], []))):
            names = sorted(reached.get(k, []))
            own = [n for n in names if a['procs'][n]['at'] == int(k, 16)]
            roles = sorted({a['procs'][n]['role'] for n in names if a['procs'][n]['role']})
            out.write('    %s %s (%d bytes)%s: reached from %d procedures (%s)%s\n' % (
                kind, k, v[2], ' = ' + '/'.join(own) if own else '', len(names), ', '.join(roles) or 'no layout role',
                (': ' + ' '.join(names[:6]) + (' ...' if len(names) > 6 else '')) if not own else ''))
    da, db = a.get('device', {}), b.get('device', {})
    items = sorted(set(da) | set(db), key=lambda x: int(x, 16))
    d = ['%s %s/%s' % (k, da.get(k, '?'), db.get(k, '?')) for k in items if da.get(k) != db.get(k)]
    out.write('    device answers that differ: %s\n' % (', '.join(d) or 'none'))


# ---------------------------------------------------------------------------------------------------------------
# Graphics APIs

GFX = {
    'nvn': (rb'\0nvnBootstrapLoader\0',),
    'vulkan': (rb'\0vkGetInstanceProcAddr\0', rb'\0vkCreateInstance\0'),
    'egl/gl': (rb'\0eglGetProcAddress\0', rb'\0eglInitialize\0', rb'\0glGetString\0'),
}


def dyn_names(image, is64):
    """{name: 'export' | 'import'} of the module's dynamic symbols"""
    tags = nc.dynamic(image, is64)
    if nc.DT_SYMTAB not in tags or nc.DT_STRTAB not in tags:
        return {}
    if nc.DT_HASH in tags:
        count = struct.unpack_from('<I', image, tags[nc.DT_HASH] + 4)[0]
    else:
        count = max(0, (tags[nc.DT_STRTAB] - tags[nc.DT_SYMTAB]) // tags.get(nc.DT_SYMENT, 24 if is64 else 16))
    out = {}
    for i in range(1, count):
        try:
            name, shndx, _ = nc.dynsym(image, is64, tags, i)
        except struct.error:
            break
        out[name] = 'export' if shndx else 'import'
    return out


def graphics(game):
    mods, is64 = moduledump.load_program(game)
    r = {'game': os.path.basename(os.path.normpath(game)), 'is64': is64, 'versions': {k: v[0] for k, v in
                                                                                       nc.versions(mods).items()}}
    found = {}
    for m in mods:
        if m.name == 'rtld':
            continue
        syms = dyn_names(m.image, is64)
        for api, pats in GFX.items():
            for p in pats:
                n = p.decode().replace('\\0', '')
                if n in syms:
                    found.setdefault(api, set()).add('%s %s %s' % (m.name, syms[n], n))
                elif re.search(p, m.image):
                    found.setdefault(api, set()).add('%s string %s' % (m.name, n))
        for s in (rb'libvulkan', rb'libEGL', rb'libGLESv2', rb'vulkan', rb'OpenGL', rb'GL_VENDOR'):
            if s in m.image:
                found.setdefault('strings', set()).add('%s:%s' % (m.name, s.decode()))
    r['found'] = {k: sorted(v) for k, v in found.items()}
    r['modules'] = [(m.name, len(m.image)) for m in mods]
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('games', nargs='+')
    ap.add_argument('--json', metavar='DIR', help='write DIR/<game>.json with every hash')
    ap.add_argument('--compare', action='store_true', help='the arguments are JSON files from --json: compare them')
    ap.add_argument('--graphics', action='store_true', help='the graphics APIs each game\'s modules use')
    args = ap.parse_args()
    if args.compare:
        results = []
        for path in args.games:
            with open(path, encoding='utf-8') as f:
                results.append(json.load(f))
        compare(results, sys.stdout)
        return
    for game in args.games:
        if args.graphics:
            r = graphics(game)
            print('== %s (%d-bit, nnSdk %s, NVN %s)' % (r['game'], 64 if r['is64'] else 32,
                                                       r['versions'].get('nnSdk', '?'), r['versions'].get('NVN', '?')))
            print('  modules: %s' % ', '.join('%s %d' % m for m in r['modules']))
            for k, v in sorted(r['found'].items()):
                print('  %s: %s' % (k, '; '.join(v)))
            continue
        try:
            r = hash_game(game)
        except (nc.Fail, SystemExit) as e:
            r = {'game': os.path.basename(os.path.normpath(game)), 'is64': True, 'versions': {}, 'errors': [str(e)]}
        summary(r, sys.stdout)
        if args.json:
            os.makedirs(args.json, exist_ok=True)
            with open(os.path.join(args.json, r['game'] + '.json'), 'w', encoding='utf-8') as f:
                json.dump(r, f, indent=1, sort_keys=True)


if __name__ == '__main__':
    main()
