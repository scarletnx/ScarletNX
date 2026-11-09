#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Asset-free CIA -> extracted tree -> native ELF -> checked ARM/Thumb execution."""

import ctypes as C
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from ctr_fixtures import cia, ncsd, ncch, put


# a guest's bad access: the checked bus's fault, or the flat space's host fault (memory/host.c) where the host has one
FAULT = ('guest fault', 'guest data abort')


def application_ncch(words=None, raw=None, text_size=None, ro=None):
    """the game's NCCH: its .code the words (or raw bytes) padded to pages, with the read-only and data words
    behind the text"""
    if raw is None:
        raw = struct.pack('<' + 'I' * len(words), *words)
    size = text_size if text_size is not None else len(raw)
    pages = (size + 4095) // 4096
    code = bytearray((pages + 2) * 4096)
    code[: len(raw)] = raw
    at = pages * 4096
    code[at : at + 16] = b'READONLY-DATA!!!'
    if ro is not None:
        code[at : at + len(ro)] = ro
    code[at + 4096 : at + 4100] = b'DATA'

    def header(ex):
        put(ex, 0x14, '<II', pages, size)
        if size > 0x100000:
            put(ex, 0x20, '<I', 0x2000000)
            put(ex, 0x30, '<I', 0x3000000)

    return ncch(code_blob=bytes(code), patch_exheader=header)


def application(words=None, raw=None, text_size=None, ro=None):
    return cia(application=application_ncch(words, raw, text_size, ro))


def cartridge(words=None, raw=None, text_size=None, ro=None):
    """the same game as application's, in a cartridge image"""
    return ncsd(game=application_ncch(words, raw, text_size, ro))


def ipc_program(wait=False):
    # Hand-encoded ARM11, asset-free. Real SVCs and checked TLS writes; the
    # success path verifies both transport R0 and the generated wire reply.
    words, literals, failures = [0xEE1D4F70, 0xE2845080], [], []

    def literal(register, value):
        literals.append((len(words), register, value))
        words.append(0)

    def require_zero():
        words.append(0xE3500000)
        failures.append(len(words))
        words.append(0)

    literal(1, 0x200000)
    words.append(0xEF00002D)
    require_zero()
    words.append(0xE1A06001)
    literal(0, 0x00010002)
    words.extend([0xE5850000, 0xE3A00020, 0xE5850004, 0xE3E00000, 0xE5850008, 0xE1A00006, 0xEF000032])
    require_zero()
    words.append(0xE5950000)
    literal(1, 0x00010040)
    words.append(0xE1500001)
    failures.append(len(words))
    words.append(0)
    words.append(0xE5950004)
    require_zero()
    if wait:
        literal(1, 0x200010)
        words.extend([0xE891018D, 0xE885018D, 0xE1A00006, 0xEF000032])
    words.extend([0xE3A0002A, 0xEF000003])
    failure = len(words)
    words.append(0xE1200070)
    for at in failures:
        words[at] = 0x1A000000 | ((failure - at - 2) & 0xFFFFFF)
    for at, register, value in literals:
        words[at] = 0xE59F0000 | register << 12 | ((len(words) - at - 2) * 4)
        words.append(value)
    ro = b'srv:\0'.ljust(16, b'\0') + struct.pack('<5I', 0x00050100, 0x5353494D, 0x474E49, 7, 0)
    return application(words, ro=ro)


def fp_program(discover=False):
    # Native module must contain/link actual software FP primitives, including
    # in an installed compiler with no build-tree runtime archive lookup.
    return application(
        [
            0xE3A0000D,
            0xE59F4034,
            0xEE000A10,
            0xEEB80B40,
            0xEE301B00,
            0xEE812B00,
            0xED841B00,
            0xED943B00,
            0xEE333B40,
            0xEEBD0BC3,
            0xEE100A10,
            0xE350000D,
            0x1A000001,
            0xE3A0002A,
            0xEF000003,
            0xE1200070,
            0x00300100,
        ],
        text_size=8192 if discover else None,
    )


def fs_program():
    # Actual native SVC/CTR sessions, not scripted FS replies. Resolve hello.txt
    # through the returned CTR metadata and check the underlying extracted bytes.
    words, literals, failures = [0xEE1D4F70, 0xE2845080], [], []
    ro = bytearray(b'srv:\0'.ljust(16, b'\0'))

    def literal(register, value):
        literals.append((len(words), register, value))
        words.append(0)

    def equal(register, value):
        literal(12, value)
        words.append(0xE150000C | register << 16)
        failures.append(len(words))
        words.append(0)

    def send(handle, command):
        literal(3, 0x200000 + len(ro))
        ro.extend(struct.pack('<' + 'I' * len(command), *command))
        for i in range(len(command)):
            words.extend([0xE5930000 | i * 4, 0xE5850000 | i * 4])
        words.extend([0xE1A00000 | handle, 0xEF000032])
        equal(0, 0)
        words.append(0xE5950004)
        equal(0, 0)

    literal(1, 0x200000)
    words.append(0xEF00002D)
    equal(0, 0)
    words.append(0xE1A06001)
    send(6, [0x10002, 0x20, 0xFFFFFFFF])
    send(6, [0x50100, 0x553A7366, 0x00524553, 7, 1])
    words.append(0xE595700C)
    send(7, [0x8010002, 0x20, 0xFFFFFFFF])
    send(7, [0x8030204, 0, 3, 1, 1, 2, 12, 1, 0, 0x4802, 0x300120, 0x30002, 0x300100])
    words.append(0xE595800C)
    send(8, [0x8040000])
    words.extend([0xE595A008, 0xE595000C])
    equal(0, 0)
    send(8, [0x80200C2, 0, 0, 512, 0x200C, 0x300400])
    words.extend([0xE5950008, 0xE150000A])  # short EOF length equals GetSize
    failures.append(len(words))
    words.append(0)
    literal(9, 0x300400)
    words.append(0xE5990000)
    equal(0, 40)
    words.extend(
        [
            0xE599200C,
            0xE0892002,
            0xE592200C,
            0xE599301C,
            0xE0893003,
            0xE0833002,
            0xE5932008,
            0xE5993024,
            0xE0822003,
            0xE0892002,
            0xE5920000,
        ]
    )
    equal(0, 0x6C6C6568)
    words.append(0xE5D20004)
    equal(0, 0x6F)
    send(8, [0x8080000])
    words.extend([0xE1A00008, 0xEF000023])
    equal(0, 0)
    words.extend([0xE3A0002A, 0xEF000003])
    failure = len(words)
    words.append(0xE1200070)
    for at in failures:
        words[at] = 0x1A000000 | ((failure - at - 2) & 0xFFFFFF)
    for at, register, value in literals:
        words[at] = 0xE59F0000 | register << 12 | ((len(words) - at - 2) * 4)
        words.append(value)
    return application(words, ro=ro)


def thread_program():
    # Real guest SVCs: main blocks, a lower-priority native worker signals the
    # event, and the saved main context resumes. No scripted HLE effects.
    words, literals, failures = [], [], []

    def literal(register, value):
        literals.append((len(words), register, value))
        words.append(0)

    def require_zero():
        words.append(0xE3500000)
        failures.append(len(words))
        words.append(0)

    words.extend([0xE3A01000, 0xEF000017])
    require_zero()
    words.append(0xE1A07001)
    literal(5, 0x300000)
    words.extend([0xE5857000, 0xE3A00031])
    literal(1, lambda: 0x100000 + child * 4)
    words.append(0xE3A02025)
    literal(3, 0x0FFFF000)
    words.extend([0xE3E04001, 0xEF000008])
    require_zero()
    words.extend([0xE1A00007, 0xE3E02000, 0xE3E03000, 0xEF000024])
    require_zero()
    words.extend([0xE5950004, 0xE350002A])
    failures.append(len(words))
    words.extend([0, 0xEF000003])
    child = len(words)
    literal(4, 0x300000)
    words.extend([0xE5940000, 0xE3A0102A, 0xE5841004, 0xEF000018, 0xE1200070])
    failure = len(words) - 1
    for at in failures:
        words[at] = 0x1A000000 | ((failure - at - 2) & 0xFFFFFF)
    for at, register, value in literals:
        words[at] = 0xE59F0000 | register << 12 | ((len(words) - at - 2) * 4)
        words.append(value() if callable(value) else value)
    return application(words)


def descriptor_offset(blob):
    # Locate the exported descriptor in an ELF64 test artifact, in the byte order the host's module has.
    assert blob[:5] == b'\x7fELF\x02' and blob[5] in (1, 2)
    order = '<' if blob[5] == 1 else '>'
    eh = struct.unpack_from(order + '16sHHIQQQIHHHHHH', blob)
    sections = [struct.unpack_from(order + 'IIQQQQIIQQ', blob, eh[6] + i * eh[11]) for i in range(eh[12])]
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        strings = sections[section[6]]
        names = blob[strings[4] : strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            symbol = struct.unpack_from(order + 'IBBHQQ', blob, at)
            name = names[symbol[0] :].split(b'\0', 1)[0]
            if name == b'snx_ctr_module_v1':
                storage = sections[symbol[3]]
                return storage[4] + symbol[4] - storage[3]
    raise AssertionError('descriptor not exported')


class Descriptor(C.Structure):
    _fields_ = [
        ('magic', C.c_uint64),
        ('fields', C.c_uint32 * 14),
        ('identity', C.c_uint8 * 32),
        ('arm', C.c_void_p),
        ('thumb', C.c_void_p),
    ]


def module_of(folder):
    """the game folder's module, named after it as a Switch game's is"""
    return folder / f'{folder.name}_code.{"dll" if sys.platform == "win32" else "so"}'


def main():
    # absolute, not resolved: scarletnx-3ds may be a link to scarletnx, which runs the driver its name says
    driver, probe = [str(Path(p).absolute()) for p in sys.argv[1:3]]
    cases = 0
    with tempfile.TemporaryDirectory(prefix='snx-ctr-aot-') as temp:
        root = Path(temp)

        def run(blob, status=0, message='Guest ExitProcess', extra=(), extension='cia'):
            nonlocal cases
            cases += 1
            source, output = root / f'input {cases};literal.{extension}', root / f'out {cases};literal'
            source.write_bytes(blob)
            p = subprocess.run(
                [driver, *extra, '--output', str(output), str(source)], text=True, capture_output=True, timeout=90
            )
            text = p.stdout + p.stderr
            assert p.returncode == status, (cases, p.returncode, text)
            messages = message if isinstance(message, tuple) else (message,)
            assert any(m in text for m in messages), (cases, text)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, (cases, text)
            assert not list(root.rglob('.snx-*')), (cases, 'private staging leaked')
            if status:
                assert 'Guest ExitProcess' not in text
            elif 'Guest ExitProcess' in text:  # a game that ended well says so last, as every driver's
                assert p.stdout.rstrip().endswith('\nExit.'), (cases, p.stdout)
            return output, text

        extract = ('--extract-container',)
        basic, text = run(
            application([0xE3A0002A, 0xEF000003]), message='Guest ExitProcess: r0=42 blocks=2', extra=extract
        )
        assert text.index('Extracting ') < text.index('Recompiling ') < text.index('Running ARM11')
        assert (basic / 'romfs/hello.txt').read_bytes() == b'hello'
        assert (basic / 'contents/00002222.app').is_file()
        # the module alone is published, not the objects it was linked from
        assert module_of(basic).is_file() and not list(basic.rglob('*.o')) and not (basic / "native").exists()
        subprocess.run([probe, str(basic), 'basic'], check=True, capture_output=True)
        # Like the Switch driver, the extraction and the module are used again, with no error and no recompile
        before = module_of(basic).read_bytes()
        source = root / 'repeat.cia'
        source.write_bytes(application([0xE3A0002A, 0xEF000003]))
        p = subprocess.run(
            [driver, *extract, '--output', str(basic), str(source)], text=True, capture_output=True, timeout=20
        )
        assert p.returncode == 0 and 'Guest ExitProcess: r0=42' in p.stdout, p.stdout + p.stderr
        assert 'Using the extraction' in p.stdout and 'Extracting' not in p.stdout and 'Recompiling' not in p.stdout
        assert module_of(basic).read_bytes() == before
        # -r compiles again and does not play
        p = subprocess.run(
            [driver, *extract, '-r', '--output', str(basic), str(source)], text=True, capture_output=True, timeout=20
        )
        assert p.returncode == 0 and 'Recompiling' in p.stdout and 'Running ARM11' not in p.stdout, p.stdout + p.stderr
        assert not (basic / 'native.previous').exists() and not list(root.rglob('.snx-*'))
        # By default nothing is extracted: the CIA is read where it lies, and its folder keeps only the module
        extraction = {'exefs', 'romfs', 'contents', 'cia-header.bin', 'ticket.bin', 'tmd.bin', 'metadata.bin',
                      'certificates.bin', 'ncch.bin', 'exheader.bin', 'manifest.ini'}
        direct, text = run(application([0xE3A0002A, 0xEF000003]), message='Guest ExitProcess: r0=42 blocks=2')
        assert 'Extracting' not in text and text.index('Recompiling ') < text.index('Running ARM11')
        assert not extraction & {p.name for p in direct.iterdir()} and module_of(direct).is_file()
        module = module_of(direct).read_bytes()
        # -r reads the CIA too, and a run uses the module again
        p = subprocess.run([driver, '-r', '--output', str(direct), str(source)], text=True, capture_output=True,
                           timeout=20)
        assert p.returncode == 0 and 'Recompiling' in p.stdout and 'Extracting' not in p.stdout, p.stdout + p.stderr
        assert not extraction & {p.name for p in direct.iterdir()}
        p = subprocess.run([driver, '--output', str(direct), str(source)], text=True, capture_output=True, timeout=20)
        assert p.returncode == 0 and 'r0=42' in p.stdout and 'Recompiling' not in p.stdout, p.stdout + p.stderr
        # The image is the same read either way: the module the extraction's run compiled serves the CIA's, whose
        # run takes the extraction away, and an extraction asked for again lands beside the module
        p = subprocess.run([driver, '--output', str(basic), str(source)], text=True, capture_output=True, timeout=20)
        assert p.returncode == 0 and 'Removing the extraction' in p.stdout and 'Recompiling' not in p.stdout, p.stdout
        assert not extraction & {p.name for p in basic.iterdir()} and module_of(basic).is_file()
        p = subprocess.run(
            [driver, *extract, '--output', str(direct), str(source)], text=True, capture_output=True, timeout=20
        )
        assert p.returncode == 0 and 'Extracting' in p.stdout and 'Recompiling' not in p.stdout, p.stdout + p.stderr
        assert (direct / 'romfs/hello.txt').read_bytes() == b'hello' and module_of(direct).read_bytes() == module
        assert not list(root.rglob('.snx-*')) and not list(root.glob('*.extracting'))
        basic = direct  # the extraction the checks below read
        run(application([0xE3A0002A, 0xEF000003]), message='r0=42', extra=('--model', 'new'))
        # Data+BSS, read-only mapping, TLS, stack, indirect BLX to Thumb and BX return.
        words = [
            0xE3A0002A,
            0xE59F103C,
            0xE5810000,
            0xE2811A01,
            0xE5810000,
            0xE59F2030,
            0xE5922000,
            0xEE1D3F70,
            0xE5830000,
            0xE92D4001,
            0xE3A00000,
            0xE8BD4001,
            0xE59F4018,
            0xE12FFF34,
            0xEF000003,
            0xE1A00000,
            0x4770202B,
            0,
            0x300000,
            0x200000,
            0x100041,
        ]
        workload, _ = run(application(words), message='Guest ExitProcess: r0=43 blocks=17', extra=extract)
        subprocess.run([probe, str(workload), 'workload'], check=True, capture_output=True)
        # Legacy BL halves execute as separate precompiled Thumb entries.
        legacy = struct.pack('<II6H', 0xE28F0001, 0xE12FFF10, 0xF000, 0xF802, 0xDF03, 0x46C0, 0x202C, 0x4770)
        run(application(raw=legacy), message='Guest ExitProcess: r0=44 blocks=7')
        run(application([0xEAFFFFFE]), 3, 'block budget exhausted', extra=('--max-blocks', '3'))
        # a budget across the run's dispatch slices (a million blocks each) is still exact
        run(application([0xEAFFFFFE]), 3, 'blocks=2500001', extra=('--max-blocks', '2500001'))
        run(application([0xEF00007F]), 3, 'unsupported guest operation')
        # a frames/ that cannot be made is named, with the system's reason
        blocked = root / f'out {cases + 1};literal'
        blocked.mkdir()
        (blocked / 'frames').write_bytes(b'')
        run(application([0xE3A0002A, 0xEF000003]), 1, f'cannot create {blocked.resolve() / "frames"}: ')
        run(application([0xE1900F9F]), 1, FAULT)  # LDREX of an unmapped address
        run(application([0xEF000021, 0xE1A00001, 0xEF000023, 0xEF000003]), message='Guest ExitProcess: r0=0 blocks=4')
        commit = [0xE3A00003, 0xE3A01302, 0xE3A02000, 0xE3A03A01, 0xE3A04003, 0xEF000001]
        locked, _ = run(
            application(
                commit
                + [0xE1A04001, 0xE1940F9F, 0xE2800001, 0xE1842F90, 0xE3520000, 0x1AFFFFFA, 0xE3A0002A, 0xEF000003]
            ),
            message='Guest ExitProcess: r0=42 blocks=14',
            extra=extract,
        )
        subprocess.run([probe, str(locked), 'kernel'], check=True, capture_output=True)
        run(
            application(
                commit
                + [
                    0xE1A06001,
                    0xEF000021,
                    0xE1A00001,
                    0xE1A01006,
                    0xE3A02001,
                    0xE3A03001,
                    0xE3A04000,
                    0xE3A05000,
                    0xEF000022,
                    0xEF000003,
                ]
            ),
            3,
            'guest blocked (no runnable thread)',
        )
        run(fp_program(), message='Guest ExitProcess: r0=42')
        run(fp_program(discover=True), message='Guest ExitProcess: r0=42')
        run(fs_program(), message='Guest ExitProcess: r0=42')
        run(thread_program(), message='Guest ExitProcess: r0=42')
        run(ipc_program(), message='Guest ExitProcess: r0=42')
        # a service the runtime does not know: a stand-in answering success, the default, else a wait forever as
        # the console's for a service never registered
        run(ipc_program(wait=True), message=('the runtime has no service MISSING: its commands answer success',))
        run(ipc_program(wait=True), 3, 'wait_service=MISSING', extra=('--ctr_auto_stub_ipc=0',))
        run(application([0xEE300A01, 0xE3A0002A, 0xEF000003]), message='Guest ExitProcess: r0=42')  # VFPv2 VADD.F32
        run(application([0xE3010234]), 1, FAULT)  # MOVW is not ARM11
        run(application([0xE3A01000, 0xE5910000]), 1, FAULT)  # unmapped data
        run(application([0xE59F1004, 0xE5810000, 0xEF000003, 0x200000]), 1, FAULT)  # RO store
        run(application([0xE59FF000, 0xE1A00000, 0x200000]), 1, 'exit=11')  # non-X branch
        # the RX page's padding, outside the declared text, runs as the console runs it (compiled at run time) up to
        # the read-only page after it, whose fetch faults
        run(application([0xEA000002]), 1, 'exit=11 syndrome=0x00101000')
        # Exhaustive discovery includes the last valid ARM word at the cap.
        maximum = bytearray(4096)
        put(maximum, 0, '<I', 0xEA0003FD)
        put(maximum, 4092, '<I', 0xEF000003)
        run(application(raw=bytes(maximum)), message='Guest ExitProcess: r0=0 blocks=2')
        run(application([0xEF000003], text_size=4100), message='Guest ExitProcess: r0=0 blocks=1')
        # Sparse RO function pointer -> indirect BLX -> Thumb, beyond the old cap.
        sparse = bytearray(0x2010)
        put(sparse, 0, '<6I', 0xE59F300C, 0xE5933000, 0xE12FFF33, 0xEF000003, 0xE1A00000, 0x200000)
        put(sparse, 0x2000, '<HH', 0x202F, 0x4770)
        run(application(raw=sparse, ro=struct.pack('<I', 0x102001)), message='Guest ExitProcess: r0=47 blocks=3')
        # More than 512 block entries make several LLVM chunks, which the workers compile in parallel.
        chained = bytearray(0x5000)
        for i in range(600):
            put(chained, i * 32, '<I', 0xEA000006)
        put(chained, 600 * 32, '<II', 0xE3A00031, 0xEF000003)
        run(application(raw=chained), message='Guest ExitProcess: r0=49 blocks=601')
        # A target the static discovery can't prove (a register's) compiles at run time, and goes in the title's
        # hints, which the next recompile starts from: then it is a static block.
        uncovered = bytearray(0x1800)
        put(uncovered, 0, '<3I', 0xE3A01601, 0xE2811C01, 0xE12FFF11)
        put(uncovered, 0x100, '<I', 0xEF000003)
        hinted, text = run(application(raw=uncovered), message='Compiled at run time: 1 blocks, 1 new')
        assert (hinted / f'{hinted.name}.hints').read_text().split() == ['00100100']
        source = root / f'input {cases};literal.cia'
        p = subprocess.run([driver, '-r', '--output', str(hinted), str(source)], text=True, capture_output=True, timeout=90)
        assert p.returncode == 0 and 'Seeding ARM11 discovery with 1 blocks' in p.stdout, p.stdout + p.stderr
        p = subprocess.run([driver, '--output', str(hinted), str(source)], text=True, capture_output=True, timeout=90)
        assert 'Guest ExitProcess' in p.stdout and 'Compiled at run time' not in p.stdout, p.stdout + p.stderr
        too_large, _ = run(application([0xEF000003], text_size=16 * 1024 * 1024 + 4), 3, 'static AOT text limit')
        assert not module_of(too_large).exists()
        extracted, text = run(application([0xE3A0002A, 0xEF000003]), message='Extracted', extra=('-x',))
        assert 'Running ARM11' not in text and not module_of(extracted).exists()
        # Compiler input is revalidated from raw files, never trusted manifest fields.
        (extracted / 'exefs/.code').write_bytes(b'truncated')
        p = subprocess.run(
            [driver, *extract, '-r', '--output', str(extracted), str(source)],
            text=True,
            capture_output=True,
            timeout=20,
        )
        assert p.returncode == 1 and 'bounded expanded .code' in p.stderr and not module_of(extracted).exists()
        assert not list(root.rglob('.snx-*'))
        # Real dlopen/dlsym ABI validation against freshly generated, trusted ELF copies.
        original = module_of(basic).read_bytes()
        offset = descriptor_offset(original)
        assert C.sizeof(Descriptor) == 112
        identity = hashlib.sha256(
            b'ScarletNX CTR image identity v1\0'
            + (basic / 'exheader.bin').read_bytes()
            + (basic / 'exefs/.code').read_bytes()
        ).digest()
        assert original[offset + Descriptor.identity.offset : offset + Descriptor.identity.offset + 32] == identity
        mutations = [offset, offset + Descriptor.identity.offset + 31]
        mutations += [offset + Descriptor.fields.offset + i * 4 for i in range(14)]
        for i, at in enumerate(mutations):
            data = bytearray(original)
            data[at] ^= 1
            path = root / f'bad-module-{i}.so'
            path.write_bytes(data)
            subprocess.run([probe, str(basic), 'reject', str(path)], check=True, capture_output=True)
        # Null function-table pointers are unit-tested in C: patching the ELF
        # pointer bytes alone would be undone by the dynamic loader's RELA.
        # A different expanded image identity is rejected by the same module, so the driver compiles it again: an
        # extracted code that changed is recompiled, without hashing anything else.
        raw = bytearray((basic / 'exefs/.code').read_bytes())
        raw[0] ^= 1
        (basic / 'exefs/.code').write_bytes(raw)
        subprocess.run([probe, str(basic), 'reject'], check=True, capture_output=True)
        p = subprocess.run(
            [driver, *extract, '--output', str(basic), str(source)], text=True, capture_output=True, timeout=20
        )
        assert p.returncode == 0 and 'Recompiling' in p.stdout and 'r0=43' in p.stdout, p.stdout + p.stderr
        subprocess.run([probe, str(workload), 'reject', str(root / 'missing.so')], check=True, capture_output=True)
        readelf = shutil.which('readelf')
        if readelf:
            deps = subprocess.check_output([readelf, '-d', str(module_of(workload))], text=True)
            assert 'LLVM' not in deps and 'scarletnx' not in deps
        # A cartridge image plays the same game: its first run compiles, and that module then serves the CIA of the
        # same game, as the image is the game's ExHeader and code, whichever container named them
        card_out, text = run(
            cartridge([0xE3A0002A, 0xEF000003]), message='Guest ExitProcess: r0=42 blocks=2', extension='cci'
        )
        assert 'Recompiling ' in text
        card = root / f'input {cases};literal.cci'  # the image run() just played
        same = root / f'same {cases};literal.cia'
        same.write_bytes(application([0xE3A0002A, 0xEF000003]))
        p = subprocess.run([driver, '--output', str(card_out), str(same)], text=True, capture_output=True, timeout=90)
        assert p.returncode == 0 and 'r0=42' in p.stdout and 'Recompiling' not in p.stdout, p.stdout + p.stderr
        p = subprocess.run([driver, '--output', str(card_out), str(card)], text=True, capture_output=True, timeout=90)
        assert p.returncode == 0 and 'r0=42' in p.stdout and 'Recompiling' not in p.stdout, p.stdout + p.stderr
    print(f'CTR end-to-end static AOT: {cases} pipelines, native ABI mutations and image identity checks passed')


if __name__ == '__main__':
    main()
