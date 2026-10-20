#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The ARM7TDMI profile (the AGB board's CPU) and A57, on the same words.

Each witness runs under both profiles, at O0 and O2: the ARM7TDMI's rules (misaligned loads rotate and stores
align, PC + 12 stores, STM's written back base, an unaligned LDM base, Thumb's POP {pc} without interworking and
the bus hook) must hold there, and none of them may reach A57's blocks.
"""

import argparse
import ctypes as C
import mmap
import pathlib
import subprocess
import tempfile

import arm_test as A

PC = 0x1000
SIZE = 0x04010000  # up to the AGB board's I/O window at 0x04000000, so the hook's addresses are mapped
Hook = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32)

ARM = {
    'str_ldr': (0xE3A0002A, 0xE5840001, 0xE5941001),  # MOV r0, #42; STR r0, [r4, #1]; LDR r1, [r4, #1]
    'ldrh': (0xE1D410B1,),  # LDRH r1, [r4, #1]
    'ldrsh': (0xE1D410F1,),  # LDRSH r1, [r4, #1]
    'str_pc': (0xE584F000,),  # STR pc, [r4]
    'stm_pc': (0xE8848000,),  # STMIA r4, {pc}
    'stm_base': (0xE8A40012,),  # STMIA r4!, {r1, r4}
    'ldm': (0xE8940005,),  # LDMIA r4, {r0, r2}
    'io_load': (0xE5940000,),  # LDR r0, [r4]
    'io_store': (0xE1C410B0,),  # STRH r1, [r4]
}
THUMB = {
    'pop_pc': (0xBD01,),  # POP {r0, pc}
}


def build(args, root, programs, emitter, profile, opt):
    words = root / f'{emitter}-words.txt'
    words.write_text(''.join(f'{PC:x} ' + ' '.join(f'{w:x}' for w in program) + '\n' for program in programs.values()))
    stem = root / f'{emitter}-{profile}-O{opt}'
    subprocess.run([getattr(args, emitter), str(words), f'{stem}.o', opt, profile], check=True)
    subprocess.run([args.cc, '-shared', f'{stem}.o', args.runtime, '-o', f'{stem}.so'], check=True)
    handle = C.CDLL(f'{stem}.so')
    blocks = {}
    for i, name in enumerate(programs):
        fn = getattr(handle, f'block_{i}')
        fn.argtypes, fn.restype = [C.POINTER(A.CPU)], None
        blocks[name] = fn
    return blocks


def word(memory, address):
    return int.from_bytes(memory[address : address + 4], 'little')


def put(memory, address, data):
    memory[address : address + len(data)] = data


class Run:
    """one block on a fresh CPU and zeroed memory; the hook records what reaches it"""

    def __init__(self, block, thumb=False, **registers):
        self.backing = mmap.mmap(-1, SIZE)
        self.memory = (C.c_uint8 * SIZE).from_buffer(self.backing)
        self.calls = []
        self.hook = Hook(lambda cpu, address, size_write, value: self.access(address, size_write, value))
        self.cpu = A.CPU()
        self.cpu.r[15] = PC
        self.cpu.cpsr = 0x10 | (32 if thumb else 0)
        for name, value in registers.items():
            self.cpu.r[int(name[1:])] = value
        self.cpu.mem = self.memory
        self.cpu.mem_size = SIZE
        self.cpu.io = C.cast(self.hook, C.c_void_p)
        self.block = block

    def access(self, address, size_write, value):
        self.calls.append((address, size_write, value))
        return 0xCAFE

    def __call__(self):
        self.block(C.byref(self.cpu))
        return self.cpu


def check(arm, thumb, profile):
    seven = profile == 'arm7tdmi'
    run = Run(arm['str_ldr'], r4=0x02008000)
    cpu = run()
    # the store lands in the aligned word; the load reads that word and rotates it by the misalignment
    if seven:
        assert (word(run.memory, 0x02008000), cpu.r[1]) == (42, 0x2A000000), (hex(word(run.memory, 0x02008000)), hex(cpu.r[1]))
    else:
        assert (word(run.memory, 0x02008001), cpu.r[1]) == (42, 42), (hex(word(run.memory, 0x02008001)), hex(cpu.r[1]))
    run = Run(arm['ldrh'], r4=0x02008000)
    put(run.memory, 0x02008000, b'\x34\x12\x78\x56')
    assert run().r[1] == (0x34000012 if seven else 0x7812), hex(run.cpu.r[1])
    run = Run(arm['ldrsh'], r4=0x02008000)
    put(run.memory, 0x02008000, b'\x34\x85\x78\x56')
    assert run().r[1] == (0xFFFFFF85 if seven else 0x7885), hex(run.cpu.r[1])  # ARMv4T: the byte, sign extended
    for name in ('str_pc', 'stm_pc'):
        run = Run(arm[name], r4=0x02008000)
        assert run().exit == 0 and word(run.memory, 0x02008000) == PC + (12 if seven else 8), name
    run = Run(arm['stm_base'], r1=7, r4=0x02008000)
    cpu = run()
    assert cpu.exit == 0 and cpu.r[4] == 0x02008008 and word(run.memory, 0x02008000) == 7
    assert word(run.memory, 0x02008004) == (0x02008008 if seven else 0x02008000)  # the base after the first: written back
    run = Run(arm['ldm'], r4=0x02008002)
    assert run().exit == (0 if seven else 7)  # A57: SNX_ARM_ALIGNMENT
    # the I/O window goes through the hook, with the documented size_write; elsewhere memory is plain
    run = Run(arm['io_load'], r4=0x04000004)
    assert run().exit == 0 and run.cpu.r[0] == (0xCAFE if seven else 0)
    assert run.calls == ([(0x04000004, 4, 0)] if seven else [])
    run = Run(arm['io_store'], r1=0x12345678, r4=0x04000006)
    assert run().exit == 0 and run.calls == ([(0x04000006, 0x102, 0x12345678)] if seven else [])
    assert word(run.memory, 0x04000004) == (0 if seven else 0x56780000)
    run = Run(arm['io_load'], r4=0x02000000)
    put(run.memory, 0x02000000, b'\x01\x02\x03\x04')
    assert run().r[0] == 0x04030201 and not run.calls
    # the BIOS answers with the board's open bus, so its reads go through the hook too
    run = Run(arm['io_load'], r4=0x00000100)
    assert run().exit == 0 and run.calls == ([(0x00000100, 4, 0)] if seven else [])
    # POP {pc}: ARMv8-A interworks on bit 0, the ARM7TDMI stays in Thumb
    run = Run(thumb['pop_pc'], thumb=True, r13=0x02008000)
    put(run.memory, 0x02008000, (42).to_bytes(4, 'little') + (0x2000).to_bytes(4, 'little'))
    cpu = run()
    assert (cpu.exit, cpu.r[0], cpu.r[13], cpu.r[15]) == (0, 42, 0x02008008, 0x2000)
    assert bool(cpu.cpsr & 32) == seven


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm-emitter', dest='arm', required=True)
    parser.add_argument('--thumb-emitter', dest='thumb', required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--runtime', required=True, help='libsnx_arm_rt.a')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-arm7tdmi-') as directory:
        root = pathlib.Path(directory)
        for opt in ('0', '2'):
            for profile in ('arm7tdmi', 'a57'):
                check(
                    build(args, root, ARM, 'arm', profile, opt),
                    build(args, root, THUMB, 'thumb', profile, opt),
                    profile,
                )
                print(f'  {profile} O{opt}: {len(ARM) + len(THUMB)} witnesses passed', flush=True)


if __name__ == '__main__':
    main()
