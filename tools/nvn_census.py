#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A static census of a game's NVN driver: its procedure table, its command buffer reserve slow path, who calls it,
the game code that writes command memory itself, and whether those writes depend on the driver's private flags.

    tools/nvn_census.py work/Game [work/Other ...]            # a report per game
    tools/nvn_census.py --markdown work/Game [work/Other ...] # one table row per game, then the details
    tools/nvn_census.py --footprints work/Game [...]          # the bytes the driver writes of each object type

The game folder is the extracted dump (exefs/ + romfs/). Its modules are read through `scarletnx --dump-modules`
(tools/moduledump.py), with the segment bounds from the ExeFS NSO headers, and the relocations, dynamic symbols,
function starts (.eh_frame_hdr) and symbols from the images. Pure Python: a scan of every instruction of a 100 MB
executable takes about 2 seconds a pass, and a game takes under a minute. Its output names the game's functions and
addresses: it stays on this machine, like the rest of a game.

What it reports, per game:

1. The procedure table. The NVN driver (in the sdk NSO, or subsdk0 in older 32-bit SDKs) looks names up with a
   binary search over a sorted pool of names without their "nvn" prefix ("CommandBufferInitialize"), through an array
   of u32 offsets into that pool, and returns the function at the same index of an array of relocated pointers.
   The pool is found by its "CommandBufferInitialize" string, the offset array by the string lengths it steps by,
   and the pointer array as the run of n relocated code pointers whose address the driver takes next to the offset
   array's, else the one run exactly n long, else the one longer run a relocated pointer names. In older drivers
   those procedures are trampolines (`ldr` of a dispatch table pointer, `ldr` of the slot of their own index, `br`)
   into a table filled at run time from a template of the real functions in the same order: the template is the run
   of at least n code pointers that a relocated pointer names, and its functions replace the trampolines. The NVN and
   nnSdk versions come from the SDK's middleware strings ("SDK MW+<vendor>+<vendor>SDK_NVN-<version>"), which are
   the SDK's own labels, not a guess.
2. The command buffer reserve slow path (A64 only). Each nvnCommandBuffer* function checks its cursor against its
   limit inline and calls one shared function when the space is short. The slow path is the BL target that the most
   CommandBuffer functions call, confirmed by its signature: it loads the memory callback and its userdata (the next
   word) from the command buffer and calls the callback with event 0 (command memory). The cursor's offset is read
   from the checks in front of those calls (`ldp cursor, limit, [cmdbuf, #off]`, then add and compare).
3. The callers of the slow path. Every BL or B to it, mapped to its function: the closest start before it, from
   .eh_frame_hdr, the symbol table and the module's BL targets, named by the symbol table when it has one. A call site
   is "table" when its function is one of the table's, "helper" when every direct call to its function comes from
   table functions or other helpers and nothing takes its address, and "internal" otherwise: still NVN-module code,
   since the slow path is local to the NVN module and no other module can BL it.
4. Game code that inlines command writes uses the command buffer's fields itself, with its own fallback. The other
   modules are scanned for the reserve check on the learned cursor offset (`ldp` of cursor and limit, an add to the
   cursor, a compare with the limit, a conditional branch) inside a function that also loads an nvnCommandBuffer*
   procedure pointer, one of those the game's loader stores from nvnDeviceGetProcAddress (the name in x1, the call,
   then the store of x0 to a variable or through a GOT slot).
5. The driver's private flags. The NVN driver exports variables named nvnPrivate* (nvnPrivate0 and others), and a
   game built with inlined command writes imports them: its inline writes run only while such a flag is nonzero,
   and otherwise call the out-of-line nvnCommandBuffer* function. The driver sets the flag when it initializes, so
   a runtime that replaces NVN where the game loads it (nvnBootstrapLoader) and never runs the driver leaves the
   flag zero and the inline writes unused. The imports come from each module's dynamic section: the DT_RELA, DT_REL
   and DT_JMPREL relocations whose symbol (DT_SYMTAB, named through DT_STRTAB) is undefined in the module and named
   nvnPrivate*, the relocated word being the module's GOT slot for it. The code that reads a flag loads its GOT slot
   (ADRP, then an LDR of that address from the ADRP's register within 8 instructions), and those load sites are
   counted with their functions. An inline check is "gated" when its function loads a flag's GOT slot at a lower
   address than the check, "tested" when, within the 12 instructions before the check, a load through a register
   that such a GOT load set or a MOV copied it to (`ldr wT, [xR]`) is followed by a CBZ or CBNZ of wT, the flag
   read and tested right in front of the cursor check, and "same fallback" when that test is a CBZ to where the
   check branches when the space is short. The last summary column says whether every inline check is tested.

6. Object footprints (--footprints, A64 only). For each object type with SetDefaults or Initialize procedures in
   the table ("TextureView" from TextureViewSetDefaults, "TextureBuilder" from TextureBuilderSetDefaults), the
   highest byte those procedures store through the object: x0 on entry, followed through MOV, ADD and SUB of
   immediates and store writeback, in address order up to the next function start, counting every STR, STUR, STP,
   STNP, STRB, STRH and ST1-ST4 with its width, GPR or SIMD/FP. A direct call with the object in x0 is followed one
   level deep, a tail branch (B out of the function, or BR through a relocated pointer) at any depth, and memset,
   bzero, memcpy and memmove (named by the module's symbols or through its PLT stubs' relocations) with the object in
   x0 and a constant size count that size. The table shows the most any of a type's procedures writes, with nvn.h's
   reserved size, ">h" where it is larger and "<old" where an older SDK's driver writes more. The API version comes
   from nvnDeviceGetInteger's switch (the answers for the API major and minor version).

What it can't prove:
- A function reached only through a pointer (a vtable, a callback) has no direct callers, so it is never a "helper",
  and an "internal" caller can be an ordinary NVN entry point. Indirect calls are not followed.
- The game-side scan is a pattern match. A cursor and limit at offset 0 is a common layout: generic buffer readers
  match the check alone (thousands of them in one engine's main), so a match counts only in a function that also
  loads an nvnCommandBuffer* pointer. Such a function can still have an unrelated check, and an inlined write in a
  function with no such load, with its check as two ldr in another order, or comparing the other way, is missed. A
  count of zero is no proof that a game never writes command memory itself: only a run shows that (the cursor gaps
  nvn_command_gap.c decodes).
- The flag gating is static and local. "Gated" only orders two instructions in one function: the load can be on
  another path than the check. "Tested" follows the flag's address through registers in address order, not control
  flow, and never drops a register once it held the address, so a register reused for something else on another
  path still counts. A flag read in a caller and passed down, or its value kept across a loop, is missed. The GOT
  load scan stops at the first instruction that may write the ADRP's register (an over-approximation by encoding
  group) or 32 instructions on. None of it says what the driver stores in the flag or when: that it is set at
  initialization comes from reading the driver, not from this scan.
- A pair loaded from [sp] is taken for a local, never a command buffer.
- The pointers are the relocated values at load time. Entries the driver replaces at run time (its debug layer's
  hooks) are not seen, and the template of a trampoline table is assumed to be the one the driver copies.
- A footprint is a lower bound of the object's size: an Initialize or SetDefaults procedure need not touch the
  object's tail (padding, fields only later calls set). A store through a register offset is not counted, a call
  is followed only one level, a branch through a pointer filled at run time not at all (DeviceInitialize), and the
  address-order walk can keep a register as the object on a path where it is something else.
- 32-bit programs get steps 1 and the imports of step 5 only: the code scans decode A64.
"""

import argparse
import array
import bisect
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..')]
import moduledump  # noqa: E402

from symbols import Symbols  # noqa: E402

R_AARCH64_RELATIVE = 1027
R_ARM_RELATIVE = 23
ANCHOR = b'CommandBufferInitialize'
IDENT = re.compile(rb'[A-Za-z0-9_]+')
PRIVATE = re.compile(r'nvnPrivate')
DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 2, 4, 5, 6, 7, 8
DT_SYMENT, DT_REL, DT_RELSZ, DT_PLTREL, DT_JMPREL, DT_RELRSZ, DT_RELR = 11, 17, 18, 20, 23, 35, 36
GOT_WINDOW = 32  # instructions an LDR may follow the ADRP of its base
TEST_WINDOW = 12  # instructions before a cursor check that a flag test is looked for in


class Fail(Exception):
    """A step that can't go on, with why"""


def u32_array(data):
    out = array.array('I')
    out.frombytes(data[: len(data) // 4 * 4])
    if sys.byteorder == 'big':
        out.byteswap()
    return out


# ---------------------------------------------------------------------------------------------------------------
# Modules: segments, the dynamic section, relocations, function starts


def nso_segments(game, name):
    """(text, rodata, data) as module-relative [start, end) from the ExeFS NSO header"""
    for folder in ('exefs', 'ExeFS'):
        path = os.path.join(game, folder, name)
        if os.path.isfile(path):
            with open(path, 'rb') as f:
                head = f.read(0x40)
            if head[:4] != b'NSO0':
                break
            segs = []
            for at in (0x10, 0x20, 0x30):
                _, mem, size = struct.unpack_from('<III', head, at)
                segs.append((mem, mem + size))
            return tuple(segs)
    raise Fail('%s: no NSO header in the ExeFS' % name)


def mod0(image):
    at = struct.unpack_from('<I', image, 4)[0]
    if image[at : at + 4] != b'MOD0':
        raise Fail('no MOD0 header')
    fields = struct.unpack_from('<iiiiii', image, at + 4)
    return at, fields  # dynamic, bss start, bss end, eh_frame_hdr start, end, module object (MOD0-relative)


def dynamic(image, is64):
    """{tag: value} of the module's dynamic section (the first of each tag), the values module offsets"""
    at, fields = mod0(image)
    dyn = at + fields[0]
    tags = {}
    entry = 16 if is64 else 8
    fmt = '<qQ' if is64 else '<iI'
    while dyn + entry <= len(image):
        tag, val = struct.unpack_from(fmt, image, dyn)
        dyn += entry
        if tag == 0:
            break
        tags.setdefault(tag, val)
    return tags


def reloc_entries(image, is64, tags):
    """(offset, type, symbol index, addend or None for REL) of every DT_RELA, DT_REL and DT_JMPREL entry"""
    tables = [(tags[DT_RELA], tags.get(DT_RELASZ, 0), True)] if DT_RELA in tags else []
    if DT_REL in tags:
        tables.append((tags[DT_REL], tags.get(DT_RELSZ, 0), False))
    if DT_JMPREL in tags:
        tables.append((tags[DT_JMPREL], tags.get(DT_PLTRELSZ, 0), tags.get(DT_PLTREL, DT_RELA) == DT_RELA))
    for start, size, rela in tables:
        if is64:
            fmt = '<QQq' if rela else '<QQ'
        else:
            fmt = '<IIi' if rela else '<II'
        step = struct.calcsize(fmt)
        for e in range(start, min(start + size, len(image) - step + 1), step):
            got = struct.unpack_from(fmt, image, e)
            info = got[1]
            kind, sym = (info & 0xFFFFFFFF, info >> 32) if is64 else (info & 0xFF, info >> 8)
            yield got[0], kind, sym, got[2] if rela else None


def relocations(image, is64):
    """{module offset of a relocated word: the module offset it points to}, from the relative relocations"""
    tags = dynamic(image, is64)
    word = 8 if is64 else 4
    wfmt = '<Q' if is64 else '<I'

    def word_at(off):
        return struct.unpack_from(wfmt, image, off)[0] if off + word <= len(image) else 0

    relative = R_AARCH64_RELATIVE if is64 else R_ARM_RELATIVE
    out = {}
    for off, kind, _, add in reloc_entries(image, is64, tags):
        if kind == relative:
            out[off] = word_at(off) if add is None else add
    if DT_RELR in tags:
        where = 0
        for e in range(tags[DT_RELR], tags[DT_RELR] + tags.get(DT_RELRSZ, 0), word):
            w = word_at(e)
            if not w & 1:
                out[w] = word_at(w)
                where = w + word
            else:
                bits, i = w >> 1, 0
                while bits:
                    if bits & 1:
                        out[where + i * word] = word_at(where + i * word)
                    bits >>= 1
                    i += 1
                where += (word * 8 - 1) * word
    return out


def cstring(image, off, limit=200):
    end = image.find(b'\0', off, off + limit)
    return image[off:end].decode('latin1') if end >= 0 else ''


def dynsym(image, is64, tags, index):
    """(name, section index, value) of dynamic symbol `index`"""
    at = tags[DT_SYMTAB] + index * tags.get(DT_SYMENT, 24 if is64 else 16)
    if is64:
        name, _, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', image, at)
    else:
        name, value, _, _, _, shndx = struct.unpack_from('<IIIBBH', image, at)
    return cstring(image, tags[DT_STRTAB] + name), shndx, value


def private_symbols(image, is64):
    """({GOT slot offset: name} of the nvnPrivate* symbols the module imports, {name: offset} of those it defines)"""
    tags = dynamic(image, is64)
    if DT_SYMTAB not in tags or DT_STRTAB not in tags:
        return {}, {}
    imports = {}
    for off, _, sym, _ in reloc_entries(image, is64, tags):
        if sym:
            name, shndx, _ = dynsym(image, is64, tags, sym)
            if shndx == 0 and PRIVATE.match(name):
                imports[off] = name
    # The symbol count: DT_HASH's chain count, else the symbol table's distance to the string table
    if DT_HASH in tags:
        count = struct.unpack_from('<I', image, tags[DT_HASH] + 4)[0]
    else:
        count = max(0, (tags[DT_STRTAB] - tags[DT_SYMTAB]) // tags.get(DT_SYMENT, 24 if is64 else 16))
    exports = {}
    for i in range(1, count):
        name, shndx, value = dynsym(image, is64, tags, i)
        if shndx != 0 and PRIVATE.match(name):
            exports[name] = value
    return imports, exports


def eh_function_starts(image):
    """Sorted module offsets of the functions .eh_frame_hdr's search table lists"""
    at, fields = mod0(image)
    hdr = at + fields[3]
    if fields[3] == fields[4] or image[hdr] != 1:
        return []
    ptr_enc, count_enc, table_enc = image[hdr + 1], image[hdr + 2], image[hdr + 3]
    if ptr_enc not in (0x1B, 0x03, 0x0B) or count_enc not in (0x03, 0x0B) or table_enc != 0x3B:
        return []
    count = struct.unpack_from('<I', image, hdr + 8)[0]
    table = image[hdr + 12 : hdr + 12 + count * 8]
    return sorted({pc + hdr for pc, _ in struct.iter_unpack('<ii', table)})


# ---------------------------------------------------------------------------------------------------------------
# A64 decoding over a module's text


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def redefines(x, r):
    """Whether instruction x may write general register r (r < 31). A cheap over-approximation by encoding group:
    data processing writes its Rd field, a load its Rt (and a pair's Rt2), and a call x0-x18 and x30. A store, a
    branch, and FP and SIMD instructions are taken to leave the general registers alone, except a load or store with
    writeback of its base."""
    group = (x >> 25) & 0xF
    if group in (0x8, 0x9, 0x5, 0xD):  # data processing, immediate or register
        return x & 31 == r
    if group & 0x5 == 0x4:  # loads and stores
        if x & 0x3F000000 == 0x08000000:  # exclusives: a store writes its status register
            return (x & 31 == r) if x & (1 << 22) else ((x >> 16) & 31 == r)
        pair = x & 0x3A000000 == 0x28000000
        if pair or x & 0x3B200C00 in (0x38000400, 0x38000C00):  # pairs, and single registers with writeback
            if (x >> 5) & 31 == r and (not pair or x & (1 << 23)):  # a pair's post- and pre-index forms
                return True
        if x & (1 << 26):  # SIMD and FP registers
            return False
        if pair:
            return bool(x & (1 << 22)) and r in (x & 31, (x >> 10) & 31)
        if x & 0x3B000000 == 0x18000000:  # load literal
            return x & 31 == r
        return (x >> 22) & 3 != 0 and x & 31 == r
    if group in (0xA, 0xB):  # branches: a call clobbers the caller-saved registers
        return (x & 0x7C000000 == 0x14000000 and x >> 31 or x & 0xFFFFFC1F == 0xD63F0000) and (r <= 18 or r == 30)
    return False


class Code:
    """A module's text as u32 words, with the whole-text scans the steps share, each made once"""

    def __init__(self, image, text):
        self.text = text
        self.w = u32_array(image[text[0] : text[1]])
        self._branches = self._adrp = self._adrp_add = self._adrp_ldr = None

    def pc(self, i):
        return self.text[0] + i * 4

    def index(self, off):
        return (off - self.text[0]) // 4

    def branches(self):
        """[(site, target, is_bl)] of every B and BL"""
        if self._branches is None:
            base = self.text[0]
            self._branches = [
                (base + i * 4, base + (i + sext(x & 0x3FFFFFF, 26)) * 4, x >> 31)
                for i, x in enumerate(self.w)
                if (x & 0x7C000000) == 0x14000000
            ]
        return self._branches

    def adrp_pages(self):
        """[(index, rd, page)] of every ADRP"""
        if self._adrp is None:
            base = self.text[0]
            self._adrp = [
                (i, x & 31, ((base + i * 4) & ~0xFFF) + (sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21) << 12))
                for i, x in enumerate(self.w)
                if (x & 0x9F000000) == 0x90000000
            ]
        return self._adrp

    def adrp_add(self):
        """[(site, target)] of ADRP xd followed by ADD xd, xd, #lo"""
        if self._adrp_add is None:
            w, n, out = self.w, len(self.w), []
            for i, rd, page in self.adrp_pages():
                if i + 1 < n:
                    b = w[i + 1]
                    if (b & 0xFFC00000) == 0x91000000 and ((b >> 5) & 31) == rd:
                        out.append((self.pc(i), page + ((b >> 10) & 0xFFF)))
            self._adrp_add = out
        return self._adrp_add

    def adrp_ldr(self):
        """[(site, address loaded, rt)] of ADRP xn followed within GOT_WINDOW instructions by LDR xt, [xn, #lo], on
        the straight path from the ADRP: the window ends where xn may be written (redefines) or the path leaves
        (an unconditional branch or a return)."""
        if self._adrp_ldr is None:
            w, n, out = self.w, len(self.w), []
            for i, rd, page in self.adrp_pages():
                for j in range(i + 1, min(i + 1 + GOT_WINDOW, n)):
                    b = w[j]
                    if (b & 0xFFC003E0) == 0xF9400000 | (rd << 5):
                        out.append((self.pc(j), page + ((b >> 10) & 0xFFF) * 8, b & 31))
                    leaves = (b & 0xFC000000) == 0x14000000 or (b & 0xFFFFFC1F) in (0xD61F0000, 0xD65F0000)
                    if leaves or redefines(b, rd):
                        break  # B, BR or RET, or xn written
            self._adrp_ldr = out
        return self._adrp_ldr


def decode(w):
    """A few forms the scans below look at, as tuples, or None"""
    if (w & 0xFFC00000) == 0xA9400000:  # ldp xt, xt2, [xn, #imm]
        return ('ldp', w & 31, (w >> 10) & 31, (w >> 5) & 31, sext((w >> 15) & 0x7F, 7) * 8)
    if (w & 0xFFC00000) == 0xF9400000:  # ldr xt, [xn, #imm]
        return ('ldr', w & 31, (w >> 5) & 31, ((w >> 10) & 0xFFF) * 8)
    if (w & 0xBFC00000) == 0xB9400000 or (w & 0xFFC00000) == 0x39400000:  # ldr wt / ldrb wt, [xn, #imm]
        return ('ldrn', w & 31, (w >> 5) & 31)
    if (w & 0xFF800000) == 0x91000000:  # add xd, xn, #imm
        return ('addi', w & 31, (w >> 5) & 31, ((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0))
    if (w & 0xFF200000) == 0x8B000000:  # add xd, xn, xm, lsl #s
        return ('addr', w & 31, (w >> 5) & 31, (w >> 16) & 31)
    if (w & 0xFF20001F) == 0xEB00001F:  # cmp xn, xm (shifted register)
        return ('cmp', (w >> 5) & 31, (w >> 16) & 31)
    if (w & 0xFF000010) == 0x54000000:  # b.cond
        return ('bcond', w & 15)
    if (w & 0x7E000000) == 0x34000000:  # cbz / cbnz, w or x
        return ('cbz', w & 31)
    if (w & 0xFC000000) == 0x94000000:
        return ('bl',)
    if (w & 0xFFFFFC1F) == 0xD63F0000:
        return ('blr', (w >> 5) & 31)
    if (w & 0xFFFFFFE0) == 0x2A1F03E0:  # mov wd, wzr
        return ('movw', w & 31, 0)
    if (w & 0xFFE00000) == 0x52800000:  # movz wd, #imm
        return ('movw', w & 31, (w >> 5) & 0xFFFF)
    if (w & 0xFFFFFC1F) == 0xD65F0000:
        return ('ret',)
    return None


def reserve_check(words, i):
    """At words[i], `ldp cursor, limit, [base, #off]` (or two ldr) followed within four instructions by an add to the
    cursor and a compare of the sum against the limit, then a conditional branch: (base, off) or None"""
    d = decode(words[i])
    if not d:
        return None
    if d[0] == 'ldp':
        _, cur, lim, base, off = d
    elif d[0] == 'ldr' and i + 1 < len(words):
        e = decode(words[i + 1])
        if not (e and e[0] == 'ldr' and e[2] == d[2] and e[3] == d[3] + 8):
            return None
        cur, lim, base, off = d[1], e[1], d[2], d[3]
    else:
        return None
    if base == 31 or cur == base or lim == base:  # a pair at sp is a local, not a command buffer
        return None
    summed = None
    for j in range(i + 1, min(i + 6, len(words))):
        e = decode(words[j])
        if not e:
            continue
        if e[0] in ('addi', 'addr') and e[2] == cur:
            summed = e[1]
        elif e[0] == 'cmp' and summed is not None and {e[1], e[2]} == {summed, lim}:
            f = decode(words[j + 1]) if j + 1 < len(words) else None
            return (base, off) if f and f[0] == 'bcond' else None
    return None


# ---------------------------------------------------------------------------------------------------------------
# Step 1: the procedure table


def find_table(m, is64, segs, relocs, code=None):
    """{'names': [...], 'funcs': [module offsets], 'pool', 'offsets', 'pointers'} for the NVN module m, or None"""
    hit = re.search(rb'\0' + ANCHOR + rb'\0', m.image)
    if not hit:
        return None
    anchor = hit.start() + 1
    # The run of identifier strings around the anchor, in memory order
    strings = [(anchor, ANCHOR)]
    at = anchor
    while True:
        prev = m.image.rfind(b'\0', 0, at - 1)
        s = m.image[prev + 1 : at - 1]
        if not s or not IDENT.fullmatch(s):
            break
        strings.insert(0, (prev + 1, s))
        at = prev + 1
    at = anchor + len(ANCHOR) + 1
    while True:
        end = m.image.find(b'\0', at)
        s = m.image[at:end]
        if not s or not IDENT.fullmatch(s):
            break
        strings.append((at, s))
        at = end + 1
    k = next(i for i, (a, _) in enumerate(strings) if a == anchor)
    # The offset array steps by the string lengths: find the anchor's neighborhood in the read-only data
    lo, hi = max(1, k - 6), min(len(strings) - 1, k + 6)
    want = [strings[i][0] - strings[i - 1][0] for i in range(lo, hi + 1)]
    ro = segs[1]
    u32 = u32_array(m.image[ro[0] : ro[1]])
    cand = [c for c, (a, b) in enumerate(zip(u32, u32[1:])) if b - a == want[0]]
    for j, step in enumerate(want[1:], 1):
        cand = [c for c in cand if c + j + 1 < len(u32) and u32[c + j + 1] - u32[c + j] == step]
    if len(cand) != 1:
        raise Fail('the name offset array was not found (%d candidates)' % len(cand))
    first = cand[0]  # the u32 index of the entry naming strings[lo - 1]
    base = strings[lo - 1][0] - u32[first]
    starts = {a for a, _ in strings}

    def names_at(i):
        if not 0 <= i < len(u32):
            return None
        a = base + u32[i]
        if a in starts:
            return a
        # The pool's first name can follow other bytes without a NUL between them ("...331BlendStateGet..."), and the
        # walk above then took those bytes into the first string or stopped after it: accept an identifier that
        # ends where the first string, or the chunk before it, ends
        first_end = strings[0][0] + len(strings[0][1])
        nul = m.image.find(b'\0', a)
        if a < first_end and nul in (strings[0][0] - 1, first_end) and IDENT.fullmatch(m.image[a:nul]):
            return a
        return None

    i0 = first
    while names_at(i0 - 1) is not None and u32[i0 - 1] < u32[i0]:
        i0 -= 1
    i1 = first
    while names_at(i1 + 1) is not None and u32[i1 + 1] > u32[i1]:
        i1 += 1
    names = [cstring(m.image, base + u32[i]) for i in range(i0, i1 + 1)]
    if names != sorted(names):
        raise Fail('the names the offset array gives are not sorted')
    n = len(names)
    # The pointer array: a run of exactly n relocated code pointers
    word = 8 if is64 else 4
    text = segs[0]
    pointers = sorted(t for t, a in relocs.items() if text[0] <= (a & ~1) < text[1])
    runs, start, prev = [], None, None
    for t in pointers:
        if prev is not None and t == prev + word:
            prev = t
            continue
        if start is not None:
            runs.append((start, (prev - start) // word + 1))
        start = prev = t
    if start is not None:
        runs.append((start, (prev - start) // word + 1))
    offsets = ro[0] + i0 * 4
    ptrs = bound = None
    if code is not None:
        # The driver's code takes the array's address near where it takes the offset array's: the target of those
        # ADRP/ADD pairs that starts n code pointers
        pairs = code.adrp_add()
        near = [s for s, t in pairs if t == offsets]
        pages = {s // 0x200 + d for s in near for d in (-1, 0, 1)}
        votes = {}
        for s, t in pairs:
            if s // 0x200 in pages and any(r[0] <= t and t + n * word <= r[0] + r[1] * word for r in runs):
                votes[t] = votes.get(t, 0) + 1
        if votes:
            ptrs = max(votes, key=votes.get)
        # The binary search's upper bound, n - 1, is a MOVZ near the offset array's address
        for site in near:
            i = code.index(site)
            for j in range(max(0, i - 8), min(len(code.w), i + 8)):
                d = decode(code.w[j])
                if d and d[0] == 'movw' and d[2] == n - 1:
                    bound = n
    # Without such code: the one run of exactly n pointers, or else the one at least n long that a relocated
    # pointer names (the extra entries after the n are hooks the driver names one by one)
    named = set(relocs.values())
    if ptrs is None:
        exact = [r for r in runs if r[1] == n]
        longer = [r for r in runs if r[1] > n and r[0] in named]
        if len(exact) == 1:
            ptrs = exact[0][0]
        elif not exact and len(longer) == 1:
            ptrs = longer[0][0]
        else:
            raise Fail(
                "the pointer array was not found: no code near the offset array's users (module offset %#x) takes "
                'its address, %d runs are exactly %d code pointers long and %d longer ones are named'
                % (offsets, len(exact), n, len(longer))
            )
    funcs = [relocs[ptrs + i * word] for i in range(n)]
    table = {
        'names': names,
        'funcs': funcs,
        'pool': (strings[0][0], strings[-1][0] + len(strings[-1][1]) + 1),
        'offsets': (offsets, ro[0] + (i1 + 1) * 4),
        'pointers': (ptrs, ptrs + n * word),
        'bound_checked': bound == n,
        'dispatch': None,
    }
    if code is not None:
        resolve_trampolines(code, relocs, runs, named, table)
    return table


def trampoline_slot(code, func):
    """The byte offset into the dispatch table that a procedure `adrp xa; ldr xa, [xa, #g]; ldr xb, [xa, #k]; br xb`
    jumps through (k), or None"""
    i = code.index(func)
    if not 0 <= i < len(code.w) - 3:
        return None
    a, b, c, d = code.w[i : i + 4]
    if (a & 0x9F000000) != 0x90000000:
        return None
    ld1, ld2 = decode(b), decode(c)
    if not (ld1 and ld1[0] == 'ldr' and ld1[1] == ld1[2] == (a & 31)):
        return None
    if not (ld2 and ld2[0] == 'ldr' and ld2[2] == ld1[1]):
        return None
    if (d & 0xFFFFFC1F) != 0xD61F0000 or (d >> 5) & 31 != ld2[1]:  # br xb
        return None
    return ld2[3]


def resolve_trampolines(code, relocs, runs, named, table):
    """Older drivers' procedures are trampolines through a dispatch table the driver fills at run time from a
    template of the real functions, in the same order. Replace the procedures with the template's entries."""
    n = len(table['names'])
    slots = [trampoline_slot(code, f) for f in table['funcs']]
    if sum(s is not None for s in slots) < n // 2:
        return
    agree = sum(1 for i, s in enumerate(slots) if s == 8 * i)
    cands = [r for r in runs if r[1] >= n and r[0] in named and r[0] != table['pointers'][0]]
    if len(cands) != 1:
        raise Fail(
            'the procedures are trampolines through a dispatch table, and %d templates of at least %d code pointers '
            'were found' % (len(cands), n)
        )
    start = cands[0][0]
    table['dispatch'] = {'template': (start, start + n * 8), 'trampolines': len([s for s in slots if s is not None]),
                         'in_order': agree}
    table['funcs'] = [relocs[start + i * 8] for i in range(n)]


def versions(mods):
    """{'NVN': ..., 'nnSdk': ...} from the SDK's middleware strings"""
    out = {}
    for m in mods:
        for hit in re.finditer(rb'SDK MW\+[A-Za-z]+\+[A-Za-z]+S[Dd][Kk]_(NVN|nnSdk)-([0-9_]+)', m.image):
            out.setdefault(hit.group(1).decode(), (hit.group(2).decode().replace('_', '.'), m.name))
    return out


# ---------------------------------------------------------------------------------------------------------------
# Step 2: the slow path


def find_slow_path(code, table):
    """(slow path offset, the facts that confirm it) from the table's CommandBuffer functions"""
    words = code.w
    votes, checks = {}, {}
    for name, func in zip(table['names'], table['funcs']):
        if not name.startswith('CommandBuffer'):
            continue
        i = code.index(func)
        seen = set()
        recent = []
        obj = {0}  # x0, the command buffer, and the registers it is copied to
        for j in range(i, min(i + 400, len(words))):
            w = words[j]
            if (w & 0xFFE0FFE0) == 0xAA0003E0 and ((w >> 16) & 31) in obj:  # mov xd, xobj
                obj.add(w & 31)
            d = decode(w)
            if d and d[0] == 'ret':
                break
            chk = reserve_check(words, j)
            if chk and chk[0] in obj:
                recent.append((j, chk[1]))
            if d and d[0] == 'bl':
                target = code.pc(j + sext(w & 0x3FFFFFF, 26))
                if target not in seen:
                    seen.add(target)
                    votes[target] = votes.get(target, 0) + 1
                for k, off in recent:
                    if j - k < 16:
                        checks.setdefault(target, {}).setdefault(off, 0)
                        checks[target][off] += 1
                recent = []
    if not votes:
        raise Fail('no BL in the CommandBuffer functions')
    ranked = sorted(votes.items(), key=lambda kv: -kv[1])
    for target, count in ranked[:5]:
        sig = slow_path_signature(code, target)
        if sig is not None:
            offs = checks.get(target, {})
            cursor = max(offs, key=offs.get) if offs else None
            return target, {
                'votes': count,
                'functions': sum(1 for n in table['names'] if n.startswith('CommandBuffer')),
                'callback': sig[0],
                'userdata': sig[1],
                'cursor': cursor,
                'ranked': ranked[:3],
            }
    raise Fail('none of the most-called targets %s loads and calls a memory callback with event 0' % ranked[:3])


def slow_path_signature(code, target):
    """(callback offset, userdata offset) when the function at target loads a pointer from its first argument
    (or the register it was copied to), then x3 from the next word, and calls the first with w1 = 0"""
    words = code.w
    i = code.index(target)
    if not 0 <= i < len(words):
        return None
    obj = {0}
    loaded = {}
    w1_zero = False
    for j in range(i, min(i + 64, len(words))):
        w = words[j]
        if (w & 0xFFE0FFE0) == 0xAA0003E0 and ((w >> 16) & 31) in obj:  # mov xd, xobj
            obj.add(w & 31)
            continue
        d = decode(w)
        if not d:
            continue
        if d[0] == 'ldr' and d[2] in obj:
            loaded[d[1]] = d[3]
        elif d[0] == 'movw' and d[1] == 1:
            w1_zero = d[2] == 0
        elif d[0] == 'blr' and d[1] in loaded and w1_zero and loaded.get(3) == loaded[d[1]] + 8:
            return loaded[d[1]], loaded[3]
        elif d[0] == 'ret':
            return None
    return None


# ---------------------------------------------------------------------------------------------------------------
# Step 3: callers


class Functions:
    """Function starts of one module: .eh_frame_hdr's, the symbol table's, and every BL target in the text (a module
    built without unwind tables for most of its code still names its functions by calling them)"""

    def __init__(self, m, code, table_funcs=()):
        starts = set(eh_function_starts(m.image))
        starts.update(v for v in m.syms if v < len(m.image))
        starts.update(table_funcs)
        lo, hi = code.text
        starts.update(t for _, t, bl in code.branches() if bl and lo <= t < hi)
        self.starts = sorted(starts)
        self.start_set = starts
        self.end = hi

    def next_start(self, off):
        """The first function start after off, or the end of the text"""
        i = bisect.bisect_right(self.starts, off)
        return self.starts[i] if i < len(self.starts) else self.end

    def containing(self, off):
        i = bisect.bisect_right(self.starts, off) - 1
        return self.starts[i] if i >= 0 else -1


def classify_callers(code, funcs, table, slow, relocs):
    branches = code.branches()
    start_set = funcs.start_set
    # Direct calls between function starts: callee -> set of calling functions
    callers_of = {}
    for s, t, _ in branches:
        if t in start_set:
            owner = funcs.containing(s)
            if owner != t:
                callers_of.setdefault(t, set()).add(owner)
    # Functions whose address is taken: relocated pointers and ADRP/ADD of their start
    taken = {a for a in relocs.values() if a in start_set}
    taken.update(t for _, t in code.adrp_add() if t in start_set)
    table_set = set(table['funcs'])
    helpers = set()
    changed = True
    while changed:
        changed = False
        for f, cs in callers_of.items():
            if f in table_set or f in helpers or f in taken:
                continue
            if cs and cs <= (table_set | helpers):
                helpers.add(f)
                changed = True
    out = []
    for site, target, _ in branches:
        if target != slow:
            continue
        owner = funcs.containing(site)
        if owner == slow:
            continue  # its own loop back
        kind = 'table' if owner in table_set else 'helper' if owner in helpers else 'internal'
        why = ''
        if kind == 'internal':
            why = 'address taken' if owner in taken else 'no direct callers' if owner not in callers_of else 'callers'
        out.append((site, owner, kind, why))
    return out


# ---------------------------------------------------------------------------------------------------------------
# Step 4 and 5: the game modules' own reserve checks and the flags in front of them


def proc_slots(m, code):
    """{module offset: 'nvnName'} of the game's procedure pointers, from its loader: the name's address in x1, a BLR
    (the device's GetProcAddress), then x0 stored either into a variable (ADRP, STR x0, [xn, #lo]) or through a GOT
    slot (ADRP, LDR xn, [xm, #lo], STR x0, [xn]). The offset is the variable's or the GOT slot's, the address a call
    through the pointer loads with ADRP and LDR."""
    words = code.w
    adrp_at = {i: (rd, page) for i, rd, page in code.adrp_pages()}

    def page_of(reg, j):
        """The page an ADRP of reg within the eight instructions before j loaded"""
        for k in range(j - 1, max(-1, j - 9), -1):
            if k in adrp_at and adrp_at[k][0] == reg:
                return adrp_at[k][1]
        return None

    out = {}
    for site, t in code.adrp_add():
        if not 0 <= t < len(m.image) - 3 or m.image[t : t + 3] != b'nvn':
            continue
        i = code.index(site)
        if words[i + 1] & 31 != 1:  # the ADD's destination: the name goes in x1
            continue
        call = next((j for j in range(i + 2, min(i + 10, len(words))) if (words[j] & 0xFFFFFC1F) == 0xD63F0000), None)
        if call is None:
            continue
        got = {}  # register: GOT slot it was loaded from
        for j in range(call + 1, min(call + 8, len(words))):
            x = words[j]
            d = decode(x)
            if d and d[0] == 'ldr':
                page = page_of(d[2], j)
                if page is not None:
                    got[d[1]] = page + d[3]
            elif (x & 0xFFC0001F) == 0xF9000000:  # str x0, [xn, #lo]
                rn, lo = (x >> 5) & 31, ((x >> 10) & 0xFFF) * 8
                if lo == 0 and rn in got:
                    out[got[rn]] = cstring(m.image, t)
                else:
                    page = page_of(rn, j)
                    if page is not None:
                        out[page + lo] = cstring(m.image, t)
                break
    return out


def flag_tested(code, start, i, loads):
    """Whether the flag is read and tested in front of the check at words[i]. The registers that may hold a flag's
    address are the destinations of its GOT loads earlier in the function (`loads`, {index: register}) and their
    copies (MOV), in address order from the function's start and never dropped, since that order is not the control
    flow. Within the TEST_WINDOW instructions before the check, a load through one of them into wT must be followed
    by a CBZ or CBNZ of wT with nothing writing wT between. Returns the index of that branch, or None."""
    words, held, value = code.w, set(), set()
    for j in range(max(0, code.index(start)), i):
        x = words[j]
        near = j >= i - TEST_WINDOW
        if j in loads:
            held.add(loads[j])
            continue
        d = decode(x)
        if near and d and d[0] in ('ldrn', 'ldr') and d[2] in held:
            value.add(d[1])
            continue
        if near and d and d[0] == 'cbz' and d[1] in value:
            return j
        if (x & 0xFFE0FFE0) == 0xAA0003E0 and ((x >> 16) & 31) in held:  # mov xd, xs
            held.add(x & 31)
            continue
        if value:
            value = {r for r in value if not redefines(x, r)}
    return None


def branch_target(code, j):
    """The target of the B.cond, CBZ or CBNZ at index j"""
    return code.pc(j + sext((code.w[j] >> 5) & 0x7FFFF, 19))


def shares_fallback(code, test, i):
    """Whether the flag test at index `test` is a CBZ (flag zero) to where the check at index i branches when the
    space is short: both lead to the same fallback, the out-of-line call"""
    if code.w[test] & (1 << 24):  # CBNZ
        return False
    bcond = next((j for j in range(i + 1, min(i + 7, len(code.w))) if (code.w[j] & 0xFF000010) == 0x54000000), None)
    return bcond is not None and branch_target(code, test) == branch_target(code, bcond)


def game_inline_checks(m, code, funcs, cursor, flags):
    """For one game module, a dict: 'sites', the reserve checks of the command cursor in functions that also load an
    nvnCommandBuffer* procedure pointer, [(site, function, gated, tested)]; 'raw', the count of the checks without
    that condition; 'other_tested', those outside such functions that a flag test still guards; 'slots', the count of
    procedure pointer slots; 'flag_loads', {flag name: [(load site, function)]} of the flag GOT loads"""
    slots = proc_slots(m, code)
    cb_slots = {a for a, n in slots.items() if n.startswith('nvnCommandBuffer')}
    loads = code.adrp_ldr()
    with_proc = {funcs.containing(s) for s, a, _ in loads if a in cb_slots}
    flag_loads, by_func = {}, {}
    for s, a, rt in loads:
        if a in flags:
            owner = funcs.containing(s)
            flag_loads.setdefault(flags[a], []).append((s, owner))
            by_func.setdefault(owner, []).append((s, rt))
    out = {'sites': [], 'raw': 0, 'other_tested': 0, 'slots': len(slots), 'flag_loads': flag_loads}
    if not cb_slots and not flag_loads:
        return out
    w = code.w
    # Candidates: ldp of a pair at the cursor offset (any base register), then the full check
    want = 0xA9400000 | ((cursor // 8) & 0x7F) << 15
    for i, x in enumerate(w):
        if (x & 0xFFFF8000) != want:
            continue
        chk = reserve_check(w, i)
        if not chk or chk[1] != cursor:
            continue
        out['raw'] += 1
        site = code.pc(i)
        owner = funcs.containing(site)
        before = {code.index(s): rt for s, rt in by_func.get(owner, ()) if s < site}
        test = flag_tested(code, owner, i, before) if before else None
        tested = test is not None
        if owner in with_proc:
            out['sites'].append((site, owner, bool(before), tested, tested and shares_fallback(code, test, i)))
        elif tested:
            out['other_tested'] += 1
    return out


# ---------------------------------------------------------------------------------------------------------------
# Footprints: the bytes of each NVN object that the driver's own whole-object writers store


NVN_H = os.path.join(HERE, '..', 'src', 'nx', 'gpu', 'nvn', 'nvn.h')
WRITERS = ('SetDefaults', 'Initialize')
MEMORY_CALLS = {'memset': 2, 'bzero': 1, 'memcpy': 2, 'memmove': 2}  # the register holding the size


def header_sizes(path=NVN_H):
    """{'NVNtexture': 0xc0, ...}: the reserved arrays of nvn.h's opaque structs"""
    try:
        with open(path, encoding='utf-8') as f:
            text = f.read()
    except OSError:
        return {}
    pattern = r'typedef struct \{\s*(?:unsigned )?char reserved\[(0x[0-9A-Fa-f]+|\d+)\];\s*\} (NVN\w+);'
    return {name: int(size, 0) for size, name in re.findall(pattern, text)}


def store(x):
    """(base, offset, bytes, writeback) of a store, the offset None for a register offset and the writeback the
    base's increment in the pre- and post-index forms; None for anything else"""
    simd = (x >> 26) & 1
    rn = (x >> 5) & 31
    if (x & 0x3A000000) == 0x28000000:  # register pairs
        opc = x >> 30
        if x & (1 << 22) or opc == 3 or (opc == 1 and not simd):  # loads, STGP
            return None
        size = (4 << opc) if simd else (4 if opc == 0 else 8)
        imm = sext((x >> 15) & 0x7F, 7) * size
        index = (x >> 23) & 3  # 0 non-temporal and 2 signed offset, 1 post-index, 3 pre-index
        return rn, 0 if index == 1 else imm, 2 * size, imm if index in (1, 3) else None
    if (x & 0x3B000000) in (0x38000000, 0x39000000):  # single registers
        size, opc = x >> 30, (x >> 22) & 3
        if simd and opc == 0:
            width = 1 << size
        elif simd and opc == 2 and size == 0:
            width = 16
        elif not simd and opc == 0:
            width = 1 << size
        else:
            return None
        if x & (1 << 24):  # unsigned offset
            return rn, ((x >> 10) & 0xFFF) * width, width, None
        kind = (x >> 10) & 3
        if x & (1 << 21):  # register offset, or an atomic
            return (rn, None, width, None) if kind == 2 else None
        imm = sext((x >> 12) & 0x1FF, 9)
        return rn, 0 if kind == 1 else imm, width, imm if kind in (1, 3) else None
    if (x & 0xBFFF0000) == 0x0C000000 or (x & 0xBFE00000) == 0x0C800000:  # ST1 to ST4, multiple structures
        regs = {0x7: 1, 0xA: 2, 0x6: 3, 0x2: 4, 0x0: 4, 0x4: 3, 0x8: 2}.get((x >> 12) & 0xF)
        if regs is None:
            return None
        width = regs * (16 if x & (1 << 30) else 8)
        post = x & (1 << 23) and (x >> 16) & 31 == 31
        return rn, 0, width, width if post else None
    return None


def bitmask_imm(n, immr, imms, bits):
    """The value of a logical immediate"""
    length = (n << 6 | (~imms & 0x3F)).bit_length() - 1
    if length < 1:
        return None
    esize = 1 << length
    levels = esize - 1
    s, r = imms & levels, immr & levels
    if s == levels:
        return None
    elem = (1 << (s + 1)) - 1
    elem = ((elem >> r) | (elem << (esize - r))) & ((1 << esize) - 1)
    out = 0
    for i in range(bits // esize):
        out |= elem << (i * esize)
    return out


def constant(x, const):
    """(register, value) when x sets a register to a constant (MOVZ, MOVN, MOVK of a known value, ORR of a logical
    immediate with the zero register), else None"""
    rd, bits = x & 31, 64 if x >> 31 else 32
    mask = (1 << bits) - 1
    imm, shift = (x >> 5) & 0xFFFF, ((x >> 21) & 3) * 16
    top = x & 0x7F800000
    if top == 0x52800000:
        return rd, imm << shift
    if top == 0x12800000:
        return rd, ~(imm << shift) & mask
    if top == 0x72800000:
        return (rd, (const[rd] & ~(0xFFFF << shift) | imm << shift) & mask) if rd in const else None
    if (x & 0x7F8003E0) == 0x320003E0:
        v = bitmask_imm((x >> 22) & 1, (x >> 16) & 0x3F, (x >> 10) & 0x3F, bits)
        return (rd, v) if v is not None else None
    return None


def memory_calls(m, code, is64):
    """{module offset: name} of memset, bzero, memcpy and memmove in module m: its own symbols, and its PLT stubs
    (`adrp x16; ldr x17, [x16, #lo]; add x16, x16, #lo; br x17`) whose GOT slot a symbol relocation names"""
    named = {v: n for v, n in m.syms.items() if n in MEMORY_CALLS}
    tags = dynamic(m.image, is64)
    if DT_SYMTAB not in tags or DT_STRTAB not in tags:
        return named
    slots = {}
    for off, _, sym, _ in reloc_entries(m.image, is64, tags):
        if sym:
            name = dynsym(m.image, is64, tags, sym)[0]
            if name in MEMORY_CALLS:
                slots[off] = name
    for site, at, rt in code.adrp_ldr():
        if rt == 17 and at in slots and (code.w[code.index(site) - 1] & 0x9F00001F) == 0x90000010:
            named[site - 4] = slots[at]
    return named


def footprint(code, funcs, named, relocs, start, entry, depth, seen, why):
    """The end of the highest object byte the function at start stores, the object's registers on entry being
    `entry` ({register: offset into the object}). Walks the function's instructions in address order, up to the next
    function start, following the object through MOV, ADD and SUB of immediates and the writeback of its stores.
    A direct call with the object in x0 is followed one level deep, a tail branch at any depth, and memset, bzero,
    memcpy and memmove with the object in x0 and a constant size count that size. A BR or BLR through a pointer
    loaded from a relocated word (ADRP, ADD or LDR, LDR) counts as a branch or call to the word's target. Appends to
    `why` the calls followed, the memory calls, and what could not be followed."""
    end = funcs.next_start(start)
    obj, const, top = dict(entry), {}, 0
    addr, loaded = {}, {}  # register: an address from ADRP and ADD, a relocated word's target loaded through one
    for j in range(code.index(start), min(code.index(end), len(code.w))):
        x = code.w[j]
        st = store(x)
        if st:
            rn, off, width, wb = st
            if rn in obj:
                if off is None:
                    why.append(('register offset store', code.pc(j), 0))
                else:
                    top = max(top, obj[rn] + off + width)
                if wb is not None:
                    obj[rn] += wb
            continue
        if (x & 0xFFE0FFE0) == 0xAA0003E0:  # mov xd, xs
            rd, rs = x & 31, (x >> 16) & 31
            if rs in obj:
                obj[rd] = obj[rs]
            else:
                obj.pop(rd, None)
            const.pop(rd, None)
            continue
        if (x & 0xBF000000) == 0x91000000:  # add or sub xd, xn, #imm
            rd, rs = x & 31, (x >> 5) & 31
            imm = ((x >> 10) & 0xFFF) << (12 if x & (1 << 22) else 0)
            if rs in obj and rd != 31:
                obj[rd] = obj[rs] + (-imm if x & (1 << 30) else imm)
            else:
                obj.pop(rd, None)
            const.pop(rd, None)
            continue
        c = constant(x, const)
        if c:
            const[c[0]] = c[1]
            obj.pop(c[0], None)
            continue
        if (x & 0x9F000000) == 0x90000000:  # adrp
            addr[x & 31] = (code.pc(j) & ~0xFFF) + (sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21) << 12)
            loaded.pop(x & 31, None)
        elif (x & 0xFFC00000) == 0xF9400000 and (x >> 5) & 31 in addr:  # ldr xt, [xn, #imm]
            at = addr[(x >> 5) & 31] + ((x >> 10) & 0xFFF) * 8
            if at in relocs:
                loaded[x & 31] = relocs[at]
            else:
                addr[x & 31] = at  # perhaps a GOT slot
        is_bl = (x & 0xFC000000) == 0x94000000
        indirect = (x & 0xFFDFFC1F) == 0xD61F0000  # br or blr
        if indirect and 0 in obj and (x >> 5) & 31 not in loaded:
            why.append(('indirect branch', code.pc(j), 0))
        if is_bl or (x & 0xFC000000) == 0x14000000 or (indirect and (x >> 5) & 31 in loaded):
            if indirect:
                target, is_bl = loaded[(x >> 5) & 31], bool(x & (1 << 21))
            else:
                target = code.pc(j + sext(x & 0x3FFFFFF, 26))
            tail = not is_bl and not start <= target < end
            if (is_bl or tail) and 0 in obj:
                name = named.get(target)
                if name in MEMORY_CALLS:
                    size = const.get(MEMORY_CALLS[name])
                    if size is not None and size < 0x100000:
                        top = max(top, obj[0] + size)
                        why.append((name, code.pc(j), obj[0] + size))
                    else:
                        why.append((name + ' of unknown size', code.pc(j), 0))
                elif target not in seen and (tail or depth == 0):
                    sub = footprint(code, funcs, named, relocs, target, obj if tail else {0: obj[0]},
                                    depth + (not tail), seen | {target}, why)
                    why.append(('tail branch' if tail else 'call', target, sub))
                    top = max(top, sub)
        for regs in (obj, const, addr, loaded):
            for r in [r for r in regs if redefines(x, r)]:
                if not (regs is addr and (x & 0xFF800000) == 0x91000000):
                    del regs[r]
        if (x & 0xFF800000) == 0x91000000 and (x >> 5) & 31 in addr:  # add xd, xn, #lo after an ADRP
            addr[x & 31] = addr[(x >> 5) & 31] + ((x >> 10) & 0xFFF)
    return top


def footprints(code, funcs, table, named, relocs):
    """{type: (bytes, the writer that stores the most, [(writer, bytes, why)])} over the table's SetDefaults and
    Initialize procedures, the type being the name without that suffix ("TextureView", "TextureBuilder")"""
    out = {}
    for name, func in zip(table['names'], table['funcs']):
        kind = next((w for w in WRITERS if name.endswith(w) and len(name) > len(w)), None)
        if kind is None:
            continue
        why = []
        size = footprint(code, funcs, named, relocs, func, {0: 0}, 0, {func}, why)
        t = out.setdefault(name[: -len(kind)], [0, None, []])
        t[2].append((name, size, why))
        if size > t[0] or t[1] is None:
            t[0], t[1] = size, name
    return {k: tuple(v) for k, v in out.items()}


def api_version(m, code, table):
    """(major, minor) that nvnDeviceGetInteger answers for NVN_DEVICE_INFO_API_MAJOR_VERSION and _MINOR_VERSION (0
    and 1), from the switch in it or in the first function it calls: a jump table of byte or halfword entries scaled
    by 4 from an ADR base, or of signed words relative to the table, each case setting w0 or keeping the value set
    before the BR. None when that isn't found."""
    got = device_answers(m, code, table, (0, 1))
    return None if got is None or None in got else tuple(got)


def device_answers(m, code, table, keys):
    """[the constant nvnDeviceGetInteger answers for each item of keys, or None where its case reads the driver's
    state or lies past the switch's bound (its `cmp wN, #max`)], from its switch (api_version), or None when the
    switch isn't found"""
    func = dict(zip(table['names'], table['funcs'])).get('DeviceGetInteger')
    if func is None:
        return None
    w = code.w
    starts = [func]
    for j in range(code.index(func), min(code.index(func) + 16, len(w))):
        if (w[j] & 0xFC000000) == 0x94000000:
            starts.append(code.pc(j + sext(w[j] & 0x3FFFFFF, 26)))
            break
    for start in starts:
        addr, jump, base, default, bound = {}, None, None, None, None
        for j in range(code.index(start), min(code.index(start) + 64, len(w))):
            x = w[j]
            rn = (x >> 5) & 31
            c = constant(x, {})
            if (x & 0x9F000000) == 0x90000000:  # adrp
                addr[x & 31] = (code.pc(j) & ~0xFFF) + (sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21) << 12)
            elif (x & 0x9F000000) == 0x10000000:  # adr
                addr[x & 31] = code.pc(j) + sext(((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2), 21)
            elif (x & 0xFF800000) == 0x91000000 and rn in addr:
                addr[x & 31] = addr[rn] + ((x >> 10) & 0xFFF)
            elif (x & 0xFFE0EC00) == 0x38606800 and rn in addr:  # ldrb wt, [xn, xm]
                jump = (addr[rn], 1)
            elif (x & 0xFFE0EC00) == 0x78606800 and rn in addr:  # ldrh wt, [xn, xm{, lsl #1}]
                jump = (addr[rn], 2)
            elif (x & 0xFFE0FC00) == 0xB8A07800 and rn in addr:  # ldrsw xt, [xn, xm, lsl #2]: table-relative words
                jump = (addr[rn], 4)
            elif (x & 0xFFE0FC00) == 0x8B000800 and rn in addr:  # add xd, xbase, xm, lsl #2
                base = addr[rn]
            elif c and c[0] == 0:
                default = c[1]
            elif (x & 0xFF80001F) == 0x7100001F and bound is None:  # cmp wn, #imm
                bound = (x >> 10) & 0xFFF
            elif (x & 0xFFFFFC1F) == 0xD61F0000:  # br
                break
        else:
            continue
        if jump is None or (jump[1] < 4 and base is None):
            continue
        values = []
        for key in keys:
            at, size = jump
            entry = int.from_bytes(m.image[at + key * size : at + (key + 1) * size], 'little', signed=size == 4)
            target = at + entry if size == 4 else base + entry * 4
            value = None
            first = code.index(target)
            if (bound is not None and key > bound) or not 0 <= first < len(w):
                values.append(None)
                continue
            for k in range(first, min(first + 8, len(w))):
                x = w[k]
                c = constant(x, {})
                if c and c[0] == 0:
                    value = c[1]
                    break
                if (x & 0xFC000000) == 0x14000000 or (x & 0xFFFFFC1F) == 0xD65F0000:
                    value = default
                    break
                if redefines(x, 0):
                    break
            values.append(value)
        if None not in values[:2]:
            return values
    return None


# ---------------------------------------------------------------------------------------------------------------


def function_name(syms, addr):
    """The symbol a function starts at, or the sized symbol it lies in, else sub_<address>"""
    i = bisect.bisect_right(syms.addrs, addr) - 1
    if i >= 0 and syms.addrs[i] == addr:
        return syms.names[i]
    if i >= 0 and syms.sizes[i] and addr < syms.addrs[i] + syms.sizes[i]:
        return '%s+%#x' % (syms.names[i], addr - syms.addrs[i])
    return 'sub_%x' % addr


def census(game):
    r = {'game': os.path.basename(os.path.normpath(game)), 'errors': []}
    mods, is64 = moduledump.load_program(game)
    r['is64'] = is64
    r['versions'] = versions(mods)
    syms = Symbols(mods, is64)
    # Step 5's imports, from every module's dynamic section
    r['private'] = {}
    flags = {}  # module name: {GOT slot: flag name}
    for m in mods:
        if m.name == 'rtld':
            continue
        imports, exports = private_symbols(m.image, is64)
        flags[m.name] = imports
        if imports or exports:
            r['private'][m.name] = {'imports': {m.base + a: n for a, n in imports.items()},
                                    'exports': {n: m.base + v for n, v in exports.items()}}
    nvn = next((m for m in mods if m.name != 'rtld' and re.search(rb'\0' + ANCHOR + rb'\0', m.image)), None)
    if nvn is None:
        r['errors'].append('no module holds the NVN name pool ("%s")' % ANCHOR.decode())
        return r
    r['module'] = nvn.name
    segs = nso_segments(game, nvn.name)
    relocs = relocations(nvn.image, is64)
    code = Code(nvn.image, segs[0]) if is64 else None
    try:
        table = find_table(nvn, is64, segs, relocs, code)
    except Fail as e:
        r['errors'].append('procedure table: %s' % e)
        return r
    r['table'] = table
    b = r['base'] = nvn.base
    r['table_funcs'] = (b + min(table['funcs']), b + max(table['funcs']))
    if not is64:
        r['errors'].append('32-bit program: the slow path, caller, flag and footprint scans decode A64 only')
        return r
    funcs = Functions(nvn, code, table['funcs'])
    r['api'] = api_version(nvn, code, table)
    named = memory_calls(nvn, code, is64)
    r['footprints'] = footprints(code, funcs, table, named, relocs)
    try:
        slow, facts = find_slow_path(code, table)
    except Fail as e:
        r['errors'].append('slow path: %s' % e)
        return r
    r['slow'] = b + slow
    r['facts'] = facts
    calls = classify_callers(code, funcs, table, slow, relocs)
    r['callers'] = [(b + s, b + o, k, why, function_name(syms, b + o)) for s, o, k, why in calls]
    # The other modules: the game's own copies of the reserve check, and the flags in front of them
    r['inline'] = {}
    if facts['cursor'] is None:
        r['errors'].append('game scan: the cursor offset was not learned')
        return r
    for m in mods:
        if m is nvn or m.name == 'rtld':
            continue
        mcode = Code(m.image, nso_segments(game, m.name)[0])
        g = game_inline_checks(m, mcode, Functions(m, mcode), facts['cursor'], flags[m.name])
        g['sites'] = [(m.base + s, m.base + o, function_name(syms, m.base + o), gt, t, fb)
                      for s, o, gt, t, fb in g['sites']]
        g['flag_loads'] = {n: [(m.base + s, m.base + o) for s, o in v] for n, v in g['flag_loads'].items()}
        r['inline'][m.name] = g
    return r


def counts(r):
    """({class: call sites}, functions of the internal sites, game-side inline sites, their functions)"""
    calls = r.get('callers', [])
    by = {k: sum(1 for c in calls if c[2] == k) for k in ('table', 'helper', 'internal')}
    internal = sorted({c[1] for c in calls if c[2] == 'internal'})
    sites = [s for g in r.get('inline', {}).values() for s in g['sites']]
    return by, internal, sites, sorted({s[1] for s in sites})


def report(r, out):
    w = out.write
    w('== %s (%s-bit)\n' % (r['game'], '64' if r['is64'] else '32'))
    for k, (v, mod) in sorted(r['versions'].items()):
        w('  %s version %s (SDK middleware string in %s)\n' % (k, v, mod))
    if 'table' in r:
        t = r['table']
        w('  procedure table in %s: %d entries, %s .. %s%s\n' % (r['module'], len(t['names']), t['names'][0],
          t['names'][-1], ' (the lookup\'s bound agrees)' if t['bound_checked'] else ' (lookup bound not seen)'))
        b = r['table_funcs'][0] - min(t['funcs'])
        w('    name pool %#x-%#x, offsets %#x-%#x, pointers %#x-%#x, functions %#x-%#x\n'
          % (b + t['pool'][0], b + t['pool'][1], b + t['offsets'][0], b + t['offsets'][1], b + t['pointers'][0],
             b + t['pointers'][1], r['table_funcs'][0], r['table_funcs'][1]))
        if t['dispatch']:
            dsp = t['dispatch']
            w('    the procedures are %d trampolines (%d jump through the slot of their own index) into a dispatch '
              'table filled from the template at %#x-%#x: the functions above are the template\'s\n'
              % (dsp['trampolines'], dsp['in_order'], b + dsp['template'][0], b + dsp['template'][1]))
    if 'slow' in r:
        f = r['facts']
        w('  slow path %#x: called by %d of %d CommandBuffer functions (next: %s)\n'
          % (r['slow'], f['votes'], f['functions'],
             ', '.join('%#x x%d' % (r['base'] + t, n) for t, n in f['ranked'][1:])))
        cursor = 'command cursor +%#x, limit +%#x' % (f['cursor'], f['cursor'] + 8) if f['cursor'] is not None else \
            'command cursor not learned'
        w('    memory callback +%#x, userdata +%#x, %s\n' % (f['callback'], f['userdata'], cursor))
    if 'callers' in r:
        by, internal, sites, owners = counts(r)
        w('  slow path call sites: %d (table %d, helper %d, other NVN-module code %d in %d functions)\n'
          % (len(r['callers']), by['table'], by['helper'], by['internal'], len(internal)))
        seen = set()
        for site, owner, kind, why, name in r['callers']:
            if kind == 'internal' and owner not in seen:
                seen.add(owner)
                if len(seen) <= 5:
                    w('    internal: %#x in %s [%s]\n' % (site, name, why))
    for name, p in sorted(r.get('private', {}).items()):
        for flag, at in sorted(p['exports'].items()):
            w('  %s exports %s at %#x\n' % (name, flag, at))
        for got, flag in sorted(p['imports'].items()):
            w('  %s imports %s through the GOT slot %#x\n' % (name, flag, got))
    for name, g in sorted(r.get('inline', {}).items()):
        for flag, v in sorted(g['flag_loads'].items()):
            w('  %s: %d loads of the %s GOT slot in %d functions\n' % (name, len(v), flag, len({o for _, o in v})))
        if not g['slots'] and not g['raw'] and not g['sites']:
            continue
        w('  %s: %d procedure pointer slots, %d cursor checks, %d in %d functions calling nvnCommandBuffer* '
          'pointers, %d of them gated and %d tested by an nvnPrivate* flag, %d with the same fallback\n'
          % (name, g['slots'], g['raw'], len(g['sites']), len({s[1] for s in g['sites']}),
             sum(1 for s in g['sites'] if s[3]), sum(1 for s in g['sites'] if s[4]),
             sum(1 for s in g['sites'] if s[5])))
        shown = set()
        for site, owner, fname, gated, tested, _ in g['sites']:
            if owner not in shown and len(shown) < 5:
                shown.add(owner)
                w('    inline check %#x in %s%s\n' % (site, fname, ' (tested)' if tested else ' (gated)' if gated
                                                      else ''))
        if g['other_tested']:
            w('    %d more cursor checks, in functions without an nvnCommandBuffer* pointer load, are tested by a '
              'flag\n' % g['other_tested'])
        untested = [s for s in g['sites'] if not s[4]]
        for site, owner, fname, gated, _, _ in untested[:5]:
            w('    not tested: %#x in %s%s\n' % (site, fname, ' (gated)' if gated else ''))
    for e in r['errors']:
        w('  FAILED %s\n' % e)


def markdown_row(r):
    v = r['versions'].get('NVN')
    ver = v[0] if v else ('nnSdk %s (guess)' % r['versions']['nnSdk'][0] if 'nnSdk' in r['versions'] else '?')
    entries = str(len(r['table']['names'])) if 'table' in r else 'n/a'
    slow = '%#x' % r['slow'] if 'slow' in r else 'n/a'
    calls = internal = inline = loads = gated = verdict = 'n/a'
    imports = sorted({n for p in r.get('private', {}).values() for n in p['imports'].values()})
    importers = sorted(name for name, p in r.get('private', {}).items() if p['imports'])
    flags = '%s (%s)' % (', '.join(imports), ', '.join(importers)) if imports else 'none'
    if 'callers' in r:
        by, funcs, sites, owners = counts(r)
        calls = '%d (%d table, %d helper)' % (len(r['callers']), by['table'], by['helper'])
        internal = '%d in %d functions' % (by['internal'], len(funcs))
        if 'inline' in r:
            inline = '%d in %d functions' % (len(sites), len(owners))
            fl = [x for g in r['inline'].values() for v in g['flag_loads'].values() for x in v]
            loads = '%d in %d functions' % (len(fl), len({o for _, o in fl}))
            ng, nt, nf = (sum(1 for s in sites if s[k]) for k in (3, 4, 5))
            gated = '%d / %d / %d' % (ng, nt, nf)
            if not sites:
                verdict = 'no inline writes found'
            elif nt == len(sites):
                verdict = 'yes, all %d tested' % nt
            else:
                verdict = 'no, %d of %d tested' % (nt, len(sites))
    note = '; '.join(r['errors'])
    return '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
        r['game'], entries, ver, slow, calls, internal, inline, flags, loads, gated, verdict, note)


def sdk_order(r):
    """A sort key of a result's SDK middleware NVN version, () when unknown"""
    v = r.get('versions', {}).get('NVN')
    return tuple(int(p) for p in v[0].split('.')) if v else ()


def footprint_cell(r, kind, sizes, results):
    """The table cell of one type in one game: the bytes, '>h' when above nvn.h's reserved size, '<old' when an
    older SDK's driver writes more, '?' when nothing was found and a branch could not be followed"""
    if 'footprints' not in r:
        return 'n/a'
    got = r['footprints'].get(kind)
    if got is None:
        return '-'
    size, _, writers = got
    if not size:
        return '?' if any(k == 'indirect branch' for _, _, why in writers for k, _, _ in why) else '0'
    cell = '%#x' % size
    header = sizes.get('NVN' + kind[0].lower() + kind[1:])
    if header is not None and size > header:
        cell += ' >h'
    older = [o['footprints'][kind][0] for o in results if 'footprints' in o and kind in o['footprints']
             and () < sdk_order(o) < sdk_order(r)]
    if older and size < max(older):
        cell += ' <old'
    return cell


def report_footprints(results, out):
    w = out.write
    sizes = header_sizes()
    kinds = sorted({k for r in results for k in r.get('footprints', {})})

    def label(r):
        v = r['versions'].get('NVN')
        api = '%d.%d' % r['api'] if r.get('api') else '?'
        return '%s (%s, API %s)' % (r['game'], v[0] if v else '?', api)

    w('| Type | %s | nvn.h |\n' % ' | '.join(label(r) for r in results))
    w('| --- |%s --- |\n' % (' --- |' * len(results)))
    for kind in kinds:
        header = sizes.get('NVN' + kind[0].lower() + kind[1:])
        w('| %s | %s | %s |\n' % (kind, ' | '.join(footprint_cell(r, kind, sizes, results) for r in results),
                                  '%#x' % header if header is not None else '-'))
    w('\nBytes written by the driver\'s SetDefaults and Initialize procedures of the type (highest offset stored '
      'through the object plus the access width): a lower bound of the object\'s size, since a writer need not touch '
      'its tail. ">h": above nvn.h\'s reserved size. "<old": an older SDK\'s driver writes more. "?": nothing found '
      'and a branch through a run-time pointer not followed. "-": no such procedure. "n/a": not scanned.\n\n')
    for r in results:
        if 'footprints' not in r:
            continue
        w('== %s\n' % label(r))
        for kind in kinds:
            if kind not in r['footprints']:
                continue
            parts = []
            for name, size, why in r['footprints'][kind][2]:
                notes = sorted({k for k, _, v in why if v and v == size} | {k for k, _, _ in why
                                                                             if k.startswith('indirect')})
                parts.append('%s %#x%s' % (name, size, ' (%s)' % ', '.join(notes) if notes else ''))
            w('  %-22s %s\n' % (kind, '; '.join(parts)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('games', nargs='+')
    ap.add_argument('--markdown', action='store_true', help='a table row per game before the details')
    ap.add_argument('--footprints', action='store_true',
                    help='only the bytes each object type\'s SetDefaults and Initialize procedures write, per game')
    args = ap.parse_args()
    results = []
    for game in args.games:
        try:
            results.append(census(game))
        except (Fail, SystemExit) as e:
            name = os.path.basename(os.path.normpath(game))
            results.append({'game': name, 'is64': True, 'versions': {}, 'errors': [str(e)]})
    if args.footprints:
        report_footprints(results, sys.stdout)
        return
    if args.markdown:
        print('| Game | Table entries | NVN version | Slow path | Slow path call sites | From other NVN-module code '
              '| Game-side inline checks | nvnPrivate* imports (modules) | Flag GOT loads '
              '| Inline checks gated / tested / same fallback | Inline writes all gated by the flag | Notes |')
        print('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
        for r in results:
            print(markdown_row(r))
        print()
        print('```')
    for r in results:
        report(r, sys.stdout)
    if args.markdown:
        print('```')


if __name__ == '__main__':
    main()
