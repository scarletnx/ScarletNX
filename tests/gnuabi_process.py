# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Processes in the Linux runtime, without a cross compiler: a hand-assembled static AArch64 program makes a pipe,
clones (a fork, then a vfork), and its child writes "ok" into the pipe and execs /bin/prog, which the runtime finds in
the sysroot given with -L (the program itself, which exits 40 when it has an argument). The parent reads the pipe and
waits: its exit status is the bytes it read plus the child's status, 42."""

import os
import struct
import subprocess
import sys
import tempfile

BASE = 0x400000


def program(clone_flags):
    code = []
    labels = {}
    fixups = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def label(name):
        labels[name] = len(code) * 4

    def branch(kind, name, reg=0):
        fixups.append((len(code), kind, name, reg))
        emit(0)

    def movz(d, imm):
        emit(0xD2800000 | (imm & 0xFFFF) << 5 | d)

    def svc(number):
        movz(8, number)
        emit(0xD4000001)

    def add_imm(d, n, imm):
        emit(0x91000000 | imm << 10 | n << 5 | d)

    def ldr_x(t, n, imm):
        emit(0xF9400000 | (imm // 8) << 10 | n << 5 | t)

    def ldr_w(t, n, imm):
        emit(0xB9400000 | (imm // 4) << 10 | n << 5 | t)

    def str_x(t, n, imm):
        emit(0xF9000000 | (imm // 8) << 10 | n << 5 | t)

    def mov(d, m):
        emit(0xAA0003E0 | m << 16 | d)

    SP, XZR = 31, 31
    label("start")
    ldr_x(0, SP, 0)  # argc
    emit(0xF100041F)  # cmp x0, #1
    branch("b.ne", "exec_mode")
    emit(0xD10203FF)  # sub sp, sp, #128
    add_imm(0, SP, 0)  # mov x0, sp (an orr with register 31 is xzr)
    movz(1, 0)
    svc(59)  # pipe2(sp, 0)
    movz(0, clone_flags)
    for r in (1, 2, 3, 4):
        movz(r, 0)
    svc(220)  # clone
    branch("cbz", "child", 0)
    mov(19, 0)  # the child's pid
    ldr_w(0, SP, 4)
    svc(57)  # close(write end)
    ldr_w(0, SP, 0)
    add_imm(1, SP, 16)
    movz(2, 16)
    svc(63)  # read(read end, sp + 16, 16)
    mov(20, 0)
    mov(0, 19)
    add_imm(1, SP, 32)
    movz(2, 0)
    movz(3, 0)
    svc(260)  # wait4(pid, sp + 32, 0, 0)
    ldr_w(1, SP, 32)
    emit(0x53087C21)  # lsr w1, w1, #8
    emit(0x8B010280)  # add x0, x20, x1
    svc(93)
    label("child")
    ldr_w(0, SP, 4)
    movz(1, 1)
    movz(2, 0)
    svc(24)  # dup3(write end, 1, 0)
    movz(0, 1)
    branch("adr", "ok", 1)
    movz(2, 2)
    svc(64)  # write(1, "ok", 2)
    branch("adr", "path", 0)
    str_x(0, SP, 40)
    str_x(0, SP, 48)
    str_x(XZR, SP, 56)
    add_imm(1, SP, 40)
    movz(2, 0)
    svc(221)  # execve("/bin/prog", {"/bin/prog", "/bin/prog", NULL}, NULL)
    movz(0, 99)
    svc(93)
    label("exec_mode")
    movz(0, 40)
    svc(93)
    label("ok")
    data = b"ok\0\0/bin/prog\0"
    labels["path"] = labels["ok"] + 4
    for index, kind, name, reg in fixups:
        offset = labels[name] - index * 4
        if kind == "b.ne":
            code[index] = 0x54000001 | ((offset // 4) & 0x7FFFF) << 5
        elif kind == "cbz":
            code[index] = 0xB4000000 | ((offset // 4) & 0x7FFFF) << 5 | reg
        else:
            code[index] = 0x10000000 | (offset & 3) << 29 | ((offset >> 2) & 0x7FFFF) << 5 | reg
    text = b"".join(struct.pack("<I", w) for w in code) + data
    entry = BASE + 0x78
    image = bytearray(0x78) + text
    size = len(image)
    image[0:64] = (
        b"\x7fELF\x02\x01\x01" + bytes(9) + struct.pack("<HHIQQQIHHHHHH", 2, 183, 1, entry, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    )
    image[64:120] = struct.pack("<IIQQQQQQ", 1, 5, 0, BASE, BASE, size, size, 0x1000)
    return bytes(image)


def main():
    runtime = sys.argv[1]
    failures = 0
    usage = subprocess.run([runtime, "--help"], capture_output=True, timeout=30)
    if usage.returncode != 0 or b"--aot" in usage.stdout or b"--cc=" in usage.stdout:
        print(f"--help still advertises the removed AOT path\n{usage.stdout.decode(errors='replace')}")
        failures += 1
    with tempfile.TemporaryDirectory() as folder:
        root = os.path.join(folder, "root")
        os.makedirs(os.path.join(root, "bin"))
        cache = os.path.join(folder, "cache")
        for name, flags in (("fork", 17), ("vfork", 0x4111)):  # SIGCHLD; CLONE_VM | CLONE_VFORK | SIGCHLD
            image = program(flags)
            for path in (os.path.join(folder, name), os.path.join(root, "bin", "prog")):
                with open(path, "wb") as f:
                    f.write(image)
                os.chmod(path, 0o755)
            result = subprocess.run(
                [runtime, "-L", root, "--jit-cache=" + cache, os.path.join(folder, name)],
                capture_output=True,
                timeout=120,
            )
            if result.returncode != 42:
                print(f"{name}: exit {result.returncode}, expected 42\n{result.stderr.decode(errors='replace')}")
                failures += 1

        def run(arguments, environment, status, *lines):
            # the fork program with options or the variables an older version read: its status, and lines of stderr
            nonlocal failures
            result = subprocess.run(
                [runtime, *arguments, os.path.join(folder, "fork")],
                capture_output=True,
                timeout=120,
                env={**os.environ, **environment},
            )
            text = result.stderr.decode(errors="replace")
            missing = [line for line in lines if line not in text]
            if result.returncode != status or missing:
                print(f"{arguments} {environment}: exit {result.returncode}, expected {status}, lacking {missing}")
                print(text)
                failures += 1

        jit = ["--jit-cache=" + cache]
        # each system call on stderr with the process id, the forked child's too: its pipe2 and the child's write
        run(["-L", root, *jit, "--strace"], {}, 42, "] 59(", "] 64(0x")
        run(["-L", root, *jit, "--sample-pc", "--host-page=65536"], {}, 42)
        refused = "--host-page=12288 is not a power of two from the host's page"
        run(["-L", root, *jit, "--host-page=12288"], {}, 2, refused)
        # the variables an older version read (docs/DEPRECATIONS.md), still read
        run([*jit], {"SNX_LINUX_SYSROOT": root}, 42, f"SNX_LINUX_SYSROOT={root} is deprecated, replaced by -L DIR")
        run(
            ["-L", root, *jit],
            {"SNX_GNU_STRACE": "1", "SNX_HOST_PAGE": "12288"},
            42,
            "SNX_GNU_STRACE=1 is deprecated, and this run reads it as --strace",
            "] 59(",
            "SNX_HOST_PAGE=12288 is deprecated, and not a power of two from the host's page",
        )
    if not failures:
        print("GNU ABI pipe, fork, vfork, execve through the sysroot and wait4 passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
