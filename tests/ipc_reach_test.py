# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""tools/ipc_reach.py over module dumps made here, as `scarletnx --dump-modules` writes them: a module with a MOD0,
its dynamic section, a PLT stub whose GOT slot a JUMP_SLOT relocation names, and functions that ask a session or
don't. Each way a function asks (an indirect call; a call to a Create...ByHipc; through a function with no symbol,
only with --deep), the names inside the namespace only with --nested, a hooked function only with --all, the SDK
versions of the games that have each, the mangled namespace as the plain one, and a 32-bit program skipped, said."""

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/ipc_reach.py'

RET, BLR_X8, LDR_X8_X0, MOV_W0_0 = 0xD65F03C0, 0xD63F0100, 0xF9400008, 0x52800000
CREATE = '_ZN2nn2sf4hipc6detail17CreateProxyByHipcEv'
GOT = 0x1000  # the stub's slot, 8 bytes into the GOT's page
STUB = 0x500
UNNAMED = 0x480


def bl(at, to):
    return 0x94000000 | ((to - at) // 4 & 0x3FFFFFF)


def module(version, functions):
    """(image, symbols) of a 64-bit module whose functions {name: (offset, [words])} lie at their offsets"""
    image = bytearray(GOT + 0x10)
    struct.pack_into('<I', image, 4, 0x10)  # the MOD0's offset
    image[0x10:0x14] = b'MOD0'
    struct.pack_into('<i', image, 0x14, 0x40 - 0x10)  # the dynamic section, from the MOD0
    strtab = b'\0' + CREATE.encode() + b'\0'
    dyn = [(5, 0x200), (6, 0x100), (23, 0x300), (2, 24), (20, 7), (0, 0)]  # STRTAB SYMTAB JMPREL PLTRELSZ PLTREL
    for i, (tag, val) in enumerate(dyn):
        struct.pack_into('<qQ', image, 0x40 + 16 * i, tag, val)
    struct.pack_into('<IBBHQQ', image, 0x100 + 24, 1, 0x12, 0, 0, 0, 0)  # symbol 1: CREATE, undefined
    image[0x200 : 0x200 + len(strtab)] = strtab
    struct.pack_into('<QQq', image, 0x300, GOT + 8, 1 << 32 | 1026, 0)  # its JUMP_SLOT
    stub = [0xB0000010, 0xF9400611, 0x91002210, 0xD61F0220]  # adrp x16, GOT's page; ldr x17, [x16, #8]; add; br x17
    for i, w in enumerate(stub):
        struct.pack_into('<I', image, STUB + 4 * i, w)
    name = b'SDK MW+Nintendo+NintendoSdk_nnSdk-' + version.encode()
    image[0x600 : 0x600 + len(name)] = name
    for i, w in enumerate([LDR_X8_X0, BLR_X8, RET]):  # the function with no symbol that Deep calls
        struct.pack_into('<I', image, UNNAMED + 4 * i, w)
    symbols = []
    for sym, (off, words) in functions.items():
        for i, w in enumerate(words):
            struct.pack_into('<I', image, off + 4 * i, w(off + 4 * i) if callable(w) else w)
        symbols.append((off, 4 * len(words), sym))
    return bytes(image), symbols


def functions():
    return {
        '_ZN2nn8settings8IndirectEv': (0x400, [LDR_X8_X0, BLR_X8, RET]),
        '_ZN2nn8settings5PlainEv': (0x420, [MOV_W0_0, RET]),
        '_ZN2nn8settings6CreateEv': (0x440, [lambda at: bl(at, STUB), RET]),
        '_ZN2nn8settings4DeepEv': (0x460, [lambda at: bl(at, UNNAMED), RET]),
        '_ZN2nn8settings6detail6DetailEv': (0x4A0, [LDR_X8_X0, BLR_X8, RET]),
        '_ZN2nn8settings6HookedEv': (0x4C0, [LDR_X8_X0, BLR_X8, RET]),
        '_ZN2nn8settings5ThingC1Ev': (0x4E0, [LDR_X8_X0, BLR_X8, RET]),
        '_ZN2nn3aoc8IndirectEv': (0x5C0, [LDR_X8_X0, BLR_X8, RET]),  # another namespace
    }


def write_dump(folder, version, funcs, is64=True):
    image, symbols = module(version, funcs)
    os.makedirs(folder)
    with open(os.path.join(folder, 'sdk.bin'), 'wb') as f:
        f.write(image)
    with open(os.path.join(folder, 'modules.txt'), 'w', encoding='utf-8') as f:
        f.write(f'program 1 {1 if is64 else 0}\n')
        f.write(f'module sdk 8000000 {len(image):x} {len(image):x}\n')
        f.writelines(f'symbol {off:x} {size:x} 2 {name}\n' for off, size, name in symbols)


class IpcReach(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='snx-ipc-reach-test-')
        t = Path(self.tmp.name)
        self.new, self.old, self.a32 = t / 'new', t / 'old', t / 'a32'
        write_dump(self.new, '15_3_2', functions())
        write_dump(self.old, '13_3_0', {'_ZN2nn8settings8IndirectEv': (0x400, [LDR_X8_X0, BLR_X8, RET])})
        write_dump(self.a32, '4_4_0', functions(), is64=False)
        self.hooks = t / 'hooks'
        self.hooks.mkdir()
        (self.hooks / 'settings.hook').write_text(
            'family nn_settings\nhook _ZN2nn8settings6HookedEv() = settings_hooked\n'
        )

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        r = subprocess.run(
            [sys.executable, str(TOOL), '--hooks', str(self.hooks), *map(str, args)],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        rows = {line.split(' | ')[0]: line.split(' | ')[1:] for line in r.stdout.splitlines()}
        return rows, r.stderr

    def test_plain(self):
        rows, said = self.run_tool('nn::settings', self.new, self.old)
        self.assertEqual(sorted(rows), ['_ZN2nn8settings6CreateEv', '_ZN2nn8settings8IndirectEv'])
        self.assertEqual(rows['_ZN2nn8settings8IndirectEv'], ['an indirect call', '13.3.0,15.3.2'])
        self.assertEqual(rows['_ZN2nn8settings6CreateEv'], ['calls ' + CREATE, '15.3.2'])
        self.assertIn('2 functions of nn::settings ask a session, in 2 games (2 modules with the namespace)', said)

    def test_mangled_namespace(self):
        self.assertEqual(self.run_tool('2nn8settings', self.new)[0], self.run_tool('nn::settings', self.new)[0])

    def test_deep(self):
        rows, _ = self.run_tool('--deep', 'nn::settings', self.new)
        self.assertEqual(rows['_ZN2nn8settings4DeepEv'], ['through sub_480: an indirect call', '15.3.2'])
        self.assertNotIn('_ZN2nn8settings5PlainEv', rows)

    def test_nested(self):
        rows, _ = self.run_tool('--nested', 'nn::settings', self.new)
        self.assertIn('_ZN2nn8settings6detail6DetailEv', rows)
        self.assertIn('_ZN2nn8settings5ThingC1Ev', rows)
        self.assertNotIn('_ZN2nn3aoc8IndirectEv', rows)
        self.assertNotIn('_ZN2nn8settings6detail6DetailEv', self.run_tool('nn::settings', self.new)[0])

    def test_hooked(self):
        self.assertNotIn('_ZN2nn8settings6HookedEv', self.run_tool('nn::settings', self.new)[0])
        rows, _ = self.run_tool('--all', 'nn::settings', self.new)
        self.assertEqual(rows['_ZN2nn8settings6HookedEv'], ['an indirect call', '15.3.2', 'hooked'])
        self.assertEqual(rows['_ZN2nn8settings8IndirectEv'], ['an indirect call', '15.3.2'])

    def test_32_bit_skipped(self):
        rows, said = self.run_tool('nn::settings', self.a32)
        self.assertEqual(rows, {})
        self.assertIn('a32: a 32-bit program, which this reads no A32 of', said)


if __name__ == '__main__':
    unittest.main()
