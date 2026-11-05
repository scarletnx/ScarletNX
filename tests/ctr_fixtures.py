# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Synthetic CTR format fixtures. No console assets, keys or signed content."""

import hashlib
import os
import shutil
import struct
import subprocess

TITLE = 0x0004000001234500
NONE = 0xFFFFFFFF
sha = lambda b: hashlib.sha256(b).digest()
align = lambda n, a: (n + a - 1) & -a


def put(b, offset, fmt, *values):
    struct.pack_into(fmt, b, offset, *values)


def repeat_blz(size):
    # Reverse decode three 'A' literals then distance-3 backreferences.
    ops = [(False, b'A')] * 3
    left = size - 3
    while left:
        count = min(18, left)
        if count < 3:
            ops.extend([(False, b'A')] * count)
        else:
            ops.append((True, struct.pack('<H', (count - 3) << 12)))
        left -= count
    groups = []
    for at in range(0, len(ops), 8):
        group = ops[at : at + 8]
        flags = sum(int(ref) << (7 - i) for i, (ref, _) in enumerate(group))
        groups.append(b''.join(data for _, data in reversed(group)) + bytes([flags]))
    payload = b''.join(reversed(groups))
    total = len(payload) + 8
    assert total <= size
    return payload + struct.pack('<II', (8 << 24) | total, size - total)


def romfs(file_name='hello.txt', directory_name='dir', patch=None, duplicate=False):
    root_name = file_name.encode('utf-16le', errors='surrogatepass')
    child_name = 'é😀.bin'.encode('utf-16le')
    directory = directory_name.encode('utf-16le', errors='surrogatepass')
    dirs = bytearray(24 + align(24 + len(directory), 4))
    put(dirs, 0, '<6I', 0, NONE, 24, 0, NONE, 0)
    put(dirs, 24, '<6I', 0, NONE, NONE, NONE if duplicate else align(32 + len(root_name), 4), NONE, len(directory))
    dirs[48 : 48 + len(directory)] = directory
    first_size = align(32 + len(root_name), 4)
    if duplicate:
        child_name = root_name
    files = bytearray(first_size + align(32 + len(child_name), 4))
    put(files, 0, '<IIQQII', 0, first_size if duplicate else NONE, 0, 5, NONE, len(root_name))
    files[32 : 32 + len(root_name)] = root_name
    put(files, first_size, '<IIQQII', 0 if duplicate else 24, NONE, 5, 4, NONE, len(child_name))
    files[first_size + 32 : first_size + 32 + len(child_name)] = child_name
    header = bytearray(0x28)
    dh = 0x28
    dt = dh + 4
    fh = dt + len(dirs)
    ft = fh + 4
    data = align(ft + len(files), 16)
    put(header, 0, '<10I', 0x28, dh, 4, dt, len(dirs), fh, 4, ft, len(files), data)
    if patch:
        patch(header, dirs, files)
    level3 = bytearray(data + 9)
    level3[:0x28] = header
    put(level3, dh, '<I', 0)
    put(level3, fh, '<I', 0)
    level3[dt : dt + len(dirs)] = dirs
    level3[ft : ft + len(files)] = files
    level3[data:] = b'helloDATA'
    padded3 = bytes(level3).ljust(0x1000, b'\0')
    level2 = sha(padded3).ljust(0x1000, b'\0')
    level1 = sha(level2).ljust(0x1000, b'\0')
    archive = bytearray(0x4000)
    archive[:4] = b'IVFC'
    put(archive, 4, '<II', 0x10000, 32)
    for at, logical, length in [(0xC, 0, 32), (0x24, 0x1000, 32), (0x3C, 0x2000, len(level3))]:
        put(archive, at, '<QQII', logical, length, 12, 0)
    put(archive, 0x54, '<I', 0x5C)
    archive[0x60:0x80] = sha(level1)
    archive[0x1000:0x2000] = padded3
    archive[0x2000:0x3000] = level1
    archive[0x3000:0x4000] = level2
    return bytes(archive)


def ncch(
    compressed=False,
    with_romfs=True,
    romfs_options=None,
    patch_exheader=None,
    patch_exefs=None,
    encrypted=False,
    patch_header=None,
    code_blob=None,
):
    exheader = bytearray(0x800)
    exheader[:8] = b'TESTCTR\0'
    exheader[0xD] = int(compressed)
    put(exheader, 0x10, '<3I', 0x100000, 1, 8)
    put(exheader, 0x1C, '<I', 0x4000)
    put(exheader, 0x20, '<3I', 0x200000, 1, 16)
    put(exheader, 0x30, '<4I', 0x300000, 1, 4, 0x1000)
    put(exheader, 0x200, '<Q', TITLE)
    exheader[0x20F] = 0x30  # the main thread's priority (ARM11 local capabilities), an application's usual one
    if patch_exheader:
        patch_exheader(exheader)
    code = bytearray(0x3000)
    put(code, 0, '<II', 0xE3A0002A, 0xEF000003)  # ARM MOV r0,#42; SVC #3 (not executed by extraction)
    code[0x1000:0x1010] = b'READONLY-DATA!!!'
    code[0x2000:0x2004] = b'DATA'
    stored = repeat_blz(len(code)) if compressed else bytes(code)
    if code_blob is not None:
        stored = code_blob
    exefs = bytearray(align(0x200 + align(len(stored), 512) + 4, 512))
    exefs[:8] = b'.code\0\0\0'
    put(exefs, 8, '<II', 0, len(stored))
    exefs[16:24] = b'icon\0\0\0\0'
    put(exefs, 24, '<II', align(len(stored), 512), 4)
    exefs[0x200 : 0x200 + len(stored)] = stored
    exefs[0x200 + align(len(stored), 512) : 0x204 + align(len(stored), 512)] = b'ICON'
    exefs[0x1E0:0x200] = sha(stored)
    exefs[0x1C0:0x1E0] = sha(b'ICON')
    if patch_exefs:
        patch_exefs(exefs)
    rom = romfs(**(romfs_options or {})) if with_romfs else b''
    header = bytearray(0x200)
    header[0x100:0x104] = b'NCCH'
    total = 0xA00 + len(exefs) + len(rom)
    put(header, 0x104, '<I', total // 512)
    put(header, 0x112, '<H', 2)
    put(header, 0x118, '<Q', TITLE)
    header[0x150:0x160] = b'CTR-P-SYNTHETIC\0'
    header[0x160:0x180] = sha(exheader[:0x400])
    put(header, 0x180, '<I', 0x400)
    header[0x18C:0x190] = bytes([1, 2, 0, (0 if encrypted else 4) | (0 if rom else 2)])
    put(header, 0x1A0, '<3I', 0xA00 // 512, len(exefs) // 512, 1)
    header[0x1C0:0x1E0] = sha(exefs[:0x200])
    if rom:
        put(header, 0x1B0, '<3I', (0xA00 + len(exefs)) // 512, len(rom) // 512, 1)
        header[0x1E0:0x200] = sha(rom[:0x200])
    if patch_header:
        patch_header(header)
    return bytes(header + exheader + exefs + rom)


UPDATE_TITLE = (TITLE & 0xFFFFFFFF) | 0x0004000E00000000


def titled(tid):
    """The patches giving an NCCH and its ExHeader another program ID, as an update partition's title has: the
    NCCH header's at 0x118 and the ExHeader's at 0x200 must both say it, as the loader checks them against each
    other."""

    def header(h):
        put(h, 0x118, '<Q', tid)

    def exheader(d):
        put(d, 0x200, '<Q', tid)

    return header, exheader


def ncsd(game=None, update=None, media_size=None, patch=None):
    """A cartridge image (a CCI): an NCSD header, then partitions in 0x200-byte media units, each an NCCH. The
    game is partition 0, on the media unit after the header's, as a card lays it out; the update is partition 7
    when given, and the slots a manual and a DLP child usually hold are zero."""
    if game is None:
        game = ncch()
    body, table = bytearray(), bytearray(0x40)
    for slot, content in [(0, game)] + ([(7, update)] if update else []):
        content = content.ljust(align(len(content), 0x200), b'\0')
        put(table, slot * 8, '<II', (0x200 + len(body)) // 0x200, len(content) // 0x200)
        body += content
    header = bytearray(0x200)
    header[0x100:0x104] = b'NCSD'
    put(header, 0x104, '<I', (media_size if media_size is not None else 0x200 + len(body)) // 0x200)
    header[0x110] = 1  # the game partition is a normal one, as a card's is
    if update is not None:
        header[0x110 + 7] = 1
    header[0x120:0x160] = table
    put(header, 0x180, '<I', 0)  # no additional header: the ExHeaders live in the NCCHs
    put(header, 0x184, '<I', 0)  # sector zero holds the header itself, as a dump's does
    if patch:
        patch(header)
    return bytes(header + body)


def z3ds(blob, patch_header=None):
    """A z3ds file of blob, as Azahar's launcher keeps one: the 0x20-byte header, then the game as a zstd seekable
    stream, its seek table after the frame as the skippable frame the format's footer is."""
    zstd = shutil.which('zstd')
    assert zstd, 'the zstd CLI is not on PATH'
    run = subprocess.run([zstd, '-q', '-c'], input=blob, capture_output=True, env={**os.environ, 'ZSTD_CLEVEL': '1'})
    assert run.returncode == 0, run.stderr
    frames = run.stdout
    header = bytearray(0x20)
    header[0:4] = b'Z3DS'
    header[8] = 1  # the version, reserved after it
    put(header, 0xA, '<H', 0x20)  # the header's own size
    put(header, 0xC, '<I', 0)  # no metadata
    put(header, 0x10, '<Q', len(frames))
    put(header, 0x18, '<Q', len(blob))
    table = struct.pack('<III', 1, len(blob), 1) + struct.pack('<I', 0x8F92EAB1)
    footer = struct.pack('<II', 0x184D2A5E, len(table)) + table
    # the header's compressed size is the stream's whole, the seek table's footer with it, as Azahar's writer counts it
    put(header, 0x10, '<Q', len(frames) + len(footer))
    if patch_header:
        patch_header(header)
    return bytes(header) + frames + footer


def tar(*entries, patch=None):
    """A tar bundle, what Azahar's .bcci/.bcxi/.bcia is: ustar headers of 512 bytes, each entry's data after its
    header rounded to 512, two zero blocks at the end, every header's checksum set."""
    out = bytearray()
    for name, blob in entries:
        h = bytearray(512)
        n = name.encode()[:100]
        h[: len(n)] = n
        h[100:108] = b'0000644\0'
        h[108:116] = h[116:124] = b'0000000\0'
        h[124:136] = f'{len(blob):011o}\0'.encode()
        h[136:148] = b'00000000000\0'
        h[148:156] = b' ' * 8  # the checksum, taken over the field as spaces
        h[156:157] = b'0'
        h[257:263] = b'ustar\0'
        h[263:265] = b'00'
        put(h, 148, '<6sbb', b'%06o' % sum(h), 0, 32)  # six octal digits, NUL, space
        out += h
        out += blob
        out += b'\0' * (-len(blob) % 512)
    out += bytes(1024)
    if patch:
        patch(out)
    return bytes(out)


def cia(
    application=None,
    signature=0x10004,
    boot=5,
    title=TITLE,
    ticket_title=None,
    encrypted=False,
    info_ranges=None,
    patch_chunks=None,
    patch_body=None,
    manual_size=512,
    with_meta=True,
    certificates=b'',
):
    if application is None:
        application = ncch()
    body_at = {0x10000: 0x240, 0x10003: 0x240, 0x10001: 0x140, 0x10004: 0x140, 0x10002: 0x40, 0x10005: 0x40}[signature]
    ticket = bytearray(body_at + 0x164 + 0x14)
    put(ticket, 0, '>I', signature)
    put(ticket, body_at + 0x9C, '>Q', title if ticket_title is None else ticket_title)
    put(ticket, body_at + 0x164, '>HHIIHHI', 1, 0x14, 0x14, 0x14, 0, 0x14, 0)
    chunks = bytearray(3 * 0x30)
    manual = b'M' * manual_size
    for i, (index, content) in enumerate([(0, manual), (5, application), (7, b'absent')]):
        put(chunks, i * 0x30, '>IHHQ', 0x1111 * (i + 1), index, int(encrypted and i == 1), len(content))
        chunks[i * 0x30 + 16 : (i + 1) * 0x30] = sha(content)
    if patch_chunks:
        patch_chunks(chunks)
    body = bytearray(0x9C4)
    put(body, 0x4C, '>Q', title)
    put(body, 0x9C, '>HHH', 1, 3, boot)
    for i, (first, count) in enumerate(info_ranges or [(0, 1), (1, 2)]):
        put(body, 0xC4 + i * 0x24, '>HH', first, count)
        body[0xC8 + i * 0x24 : 0xE8 + i * 0x24] = sha(chunks[first * 0x30 : (first + count) * 0x30])
    body[0xA4:0xC4] = sha(body[0xC4:])
    if patch_body:
        patch_body(body)
    tmd = bytearray(body_at) + body + chunks
    put(tmd, 0, '>I', signature)
    header = bytearray(0x2020)
    put(
        header,
        0,
        '<IHHIIIIQ',
        0x2020,
        0,
        0,
        len(certificates),
        len(ticket),
        len(tmd),
        0x400 if with_meta else 0,
        len(manual) + len(application),
    )
    header[0x20] = 0x84  # present indices 0 and 5; index 7 deliberately absent
    archive = header.ljust(0x2040, b'\0')
    for section in [certificates, ticket, tmd]:
        archive += section
        archive += b'\0' * (align(len(archive), 64) - len(archive))
    archive += manual + application
    if with_meta:
        archive += b'\0' * (align(len(archive), 64) - len(archive)) + b'\0' * 0x400
    return bytes(archive)


DLC_TITLE = (TITLE & 0xFFFFFF00) | 0x0004008C00000000


def dlc_content(title=DLC_TITLE, file_name='hello.txt', with_romfs=True):
    """An add-on content: an NCCH of the DLC title holding a RomFS with the one file."""

    def header(h):
        put(h, 0x118, '<Q', title)

    return ncch(patch_header=header, romfs_options={'file_name': file_name}, with_romfs=with_romfs)


def dlc_cia(contents, title=DLC_TITLE, signature=0x10004):
    """A DLC CIA: contents is a list of (TMD index, NCCH), the TMD listing them in that order, each present in the
    CIA's bitmap. It has no boot content."""
    body_at = {0x10000: 0x240, 0x10003: 0x240, 0x10001: 0x140, 0x10004: 0x140, 0x10002: 0x40, 0x10005: 0x40}[signature]
    ticket = bytearray(body_at + 0x164 + 0x14)
    put(ticket, 0, '>I', signature)
    put(ticket, body_at + 0x9C, '>Q', title)
    put(ticket, body_at + 0x164, '>HHIIHHI', 1, 0x14, 0x14, 0x14, 0, 0x14, 0)
    chunks = bytearray(len(contents) * 0x30)
    for i, (index, content) in enumerate(contents):
        put(chunks, i * 0x30, '>IHHQ', 0x1111 * (i + 2), index, 0, len(content))
        chunks[i * 0x30 + 16 : (i + 1) * 0x30] = sha(content)
    body = bytearray(0x9C4)
    put(body, 0x4C, '>Q', title)
    put(body, 0x9C, '>HHH', 1, len(contents), 0)
    put(body, 0xC4, '>HH', 0, len(contents))
    body[0xC8:0xE8] = sha(chunks)
    body[0xA4:0xC4] = sha(body[0xC4:])
    tmd = bytearray(body_at) + body + chunks
    put(tmd, 0, '>I', signature)
    header = bytearray(0x2020)
    total = sum(len(c) for _, c in contents)
    put(header, 0, '<IHHIIIIQ', 0x2020, 0, 0, 0, len(ticket), len(tmd), 0, total)
    for index, _ in contents:
        header[0x20 + (index >> 3)] |= 0x80 >> (index & 7)
    archive = header.ljust(0x2040, b'\0')
    for section in [b'', ticket, tmd]:
        archive += section
        archive += b'\0' * (align(len(archive), 64) - len(archive))
    for _, content in contents:
        archive += content
    return bytes(archive)
