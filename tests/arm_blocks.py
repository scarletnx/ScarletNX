# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The AArch32 blocks a native back end makes, run from a test's states: snx-test-arm-blocks (arm_blocks.c), which makes
them and runs them in a process of its own, for this host or, under Unicorn, for another machine."""

import ctypes as C
import functools
import mmap
import pathlib
import struct
import subprocess
import sys
import tempfile

PC = 0x1000
SIZE = 0x10000  # guest memory
SKIP = 77


class CPU(C.Structure):
    """arm.h's SnxArmCPU"""

    _fields_ = [
        ('r', C.c_uint32 * 16),
        ('cpsr', C.c_uint32),
        ('exit', C.c_uint32),
        ('syndrome', C.c_uint32),
        ('mem', C.POINTER(C.c_uint8)),
        ('mem_size', C.c_uint64),
        ('io', C.c_void_p),  # the ARM7TDMI profile's bus hook: no other profile reads it
        ('exclusive_value', C.c_uint64),
        ('exclusive_address', C.c_uint32),
        ('exclusive_size', C.c_uint32),
        ('exclusive_valid', C.c_uint32),
        ('tpidrurw', C.c_uint32),
        ('tpidruro', C.c_uint32),
        ('fpscr', C.c_uint32),
        ('d', C.c_uint64 * 32),
        ('rt', C.c_void_p),  # the backend's SnxArmRt: none, the runtime library's defaults
    ]


# SnxArmCPU.io and SnxArmCheckedCPU.access
Hook = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32)
Access = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_void_p)


# checked.h's SNX_ARM_CHECKED_ABI, and the words a native block moves a transfer through
CHECKED_ABI = 2


class Checked(C.Structure):
    """checked.h's SnxArmCheckedCPU"""

    _fields_ = [
        ('cpu', CPU),
        ('abi', C.c_uint32),
        ('context', C.c_void_p),
        ('access', Access),
        ('words', C.c_uint32 * 32),
    ]


# arm_blocks.c's: Shared's bus after the state, guest memory's place in the file, the replies
BUS = C.sizeof(Checked)
MEMORY = 4096
REPLY = struct.Struct('=5I')
DONE, IO, ACCESS = range(3)


def arm(programs, pc=PC):
    """A32 programs as blocks at pc"""
    return [('arm', pc, words) for words in programs]


def thumb(programs, pc=PC):
    """Thumb programs, halfwords, as blocks at pc"""
    return [('thumb', pc, halfwords) for halfwords in programs]


def add_variants(parser):
    """--pool, given again for another variant of the blocks (variants)"""
    parser.add_argument('--pool', type=int, action='append', help='the host registers its values are in (0: all)')


def variants(parser, args):
    """Blocks' keyword arguments for each variant args name: the native back end's pools, all the host registers and
    three by default, so that values spill"""
    del parser
    return [{'pool': pool} for pool in args.pool or (0, 3)]


class Blocks:
    """The blocks of (isa, pc, words or halfwords), made by runner's back end for profile and memory (flat, checked),
    its values in pool host registers (0: all of them; .name says which). functions[i]
    runs the ith on a CPU (a Checked where memory is checked), or is None where the back end refused it; names[i] is
    what its decoder has of each instruction, why[i] what the refusal said. A state's flat memory (mem_size bytes, at
    most size) is copied there and back, but where it is .memory, which holds the runs' own."""

    def __init__(self, runner, blocks, size=SIZE, profile='a57', memory='flat', pool=None):
        self.blocks, self.checked = blocks, memory == 'checked'
        self.directory = tempfile.TemporaryDirectory(prefix='snx-arm-blocks-')
        root = pathlib.Path(self.directory.name)
        listed, state = root / 'blocks.txt', root / 'state'
        listed.write_text(''.join(f'{isa} {pc:x} {" ".join(f"{u:x}" for u in units)}\n' for isa, pc, units in blocks))
        self.file = state.open('w+b')
        self.file.truncate(MEMORY + size)
        self.map = mmap.mmap(self.file.fileno(), 0)
        self.base = C.addressof(C.c_uint8.from_buffer(self.map))
        self.memory = (C.c_uint8 * size).from_buffer(self.map, MEMORY)
        self.size, self.at = size, C.addressof(self.memory)
        command = [runner, str(listed), str(state), '--profile', profile, '--memory', memory]
        command += ['--pool', str(pool)] if pool is not None else []
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.functions, self.names, self.why = [], [], []
        for index in range(len(blocks)):
            said = self.said()
            if said.startswith('made '):
                self.functions.append(functools.partial(self.run, index))
                self.names.append(said.split()[2:])
                self.why.append(None)
            elif said.startswith('refused '):
                self.functions.append(None)
                self.names.append([])
                self.why.append(said.removeprefix('refused '))
            else:
                raise AssertionError(f'{command}: {said!r} of block {index}')
        said = self.said()
        if not said.startswith('ready '):
            raise AssertionError(f'{command}: {said!r} after its {len(blocks)} blocks')
        self.machine = said.removeprefix('ready ')
        self.name = f'{self.machine}, {pool or "all"} registers'

    def said(self):
        """the next line of what it says of the blocks; a machine it has no back end or Unicorn for ends the test"""
        line = self.process.stdout.readline().decode()
        if line.startswith('SKIP:'):
            print(line, end='', flush=True)
            self.process.wait()
            sys.exit(SKIP)
        if not line:
            raise AssertionError(
                f'{self.process.args}: ended, status {self.process.wait()}, before its blocks were made'
            )
        return line.rstrip('\n')

    def refuse(self):
        """fail where the back end refused a block, naming each; the blocks"""
        refused = [f'{b}: {why}' for b, why in zip(self.blocks, self.why) if why is not None]
        assert not refused, f'{self.machine}: {len(refused)} blocks refused: ' + '; '.join(refused[:20])
        return self

    def run(self, index, state):
        cpu = getattr(state, 'cpu', state)
        moved = 0
        if not self.checked:
            guest = C.cast(cpu.mem, C.c_void_p).value
            moved = 0 if guest == self.at else min(cpu.mem_size, self.size)
            C.memmove(self.at, guest, moved)
        C.memmove(self.base, C.addressof(state), C.sizeof(state))
        self.send(index)
        while True:
            kind, a, b, c, _ = self.reply(index)
            if kind == DONE:
                break
            if kind == IO:
                self.send(C.cast(cpu.io, Hook)(None, a, b, c) & 0xFFFFFFFF)
            else:
                self.send(state.access(None, a, b, c, self.base + BUS) & 0xFFFFFFFF)
        C.memmove(C.addressof(state), self.base, C.sizeof(state))
        if moved:
            C.memmove(guest, self.at, moved)

    def clear(self):
        """zero the runs' guest memory"""
        self.file.truncate(MEMORY)
        self.file.truncate(MEMORY + self.size)

    def send(self, value):
        try:
            self.process.stdin.write(struct.pack('=I', value))
            self.process.stdin.flush()
        except BrokenPipeError:
            raise AssertionError(f'{self.process.args}: ended, status {self.process.wait()}') from None

    def reply(self, index):
        data = self.process.stdout.read(REPLY.size)
        if len(data) != REPLY.size:
            isa, pc, units = self.blocks[index]
            words = ' '.join(f'{u:x}' for u in units)
            raise AssertionError(f'{self.machine}: ended, status {self.process.wait()}, running {isa} {pc:x} {words}')
        return REPLY.unpack(data)

    def close(self):
        """end the runner: its status is the test's to see, as a sanitizer's report at its exit"""
        self.process.stdin.close()
        status = self.process.wait()
        assert not status, f'{self.machine}: the runner ended with status {status}'
