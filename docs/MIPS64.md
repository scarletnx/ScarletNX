<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# MIPS64

ScarletNX builds for 64-bit MIPS Linux, MIPS64 Release 2 and later with the n64 ABI, big-endian (`mips64`) and
little-endian (`mips64el`). It is a secondary architecture: CI builds neither, and support is the code and the
presets below. No MIPS machine has run it yet; [Building](#building) says what ran under qemu-user.

## Building

The `linux-mips64-cross` and `linux-mips64el-cross` presets cross-build with Clang against a system root of the
architecture, as PowerPC64's do ([PowerPC64](./PPC64.md)), and link with the target's GNU ld.
[gnu-cross](https://github.com/cross-tools/gnu-cross/releases)'s buildroots (`mips64-unknown-linux-gnu.tar.xz`,
`mips64el-unknown-linux-gnu.tar.xz`) are one: glibc in `<triple>/sysroot`, and beside it GNU ld and GCC's runtime
(crt files, libgcc, libstdc++), which Clang takes from the folder `SNX_GCC_TOOLCHAIN` names
(`CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN`, which `cmake/cross.cmake` passes on to FFmpeg's, OpenSSL's and the runtime
library's compiles as `--gcc-toolchain`). Their `libgcc.a` has absolute `.eh_frame` pointers, which LLD refuses in a
position-independent program and GNU ld makes relative. They carry no LLVM: `scripts/linux-llvm.sh` builds LLVM's
static libraries for the architecture, Mips its only code generator.

```bash
root=/path/to/mips64el-unknown-linux-gnu
export SNX_SYSROOT=$root/mips64el-unknown-linux-gnu/sysroot SNX_GCC_TOOLCHAIN=$root
scripts/linux-llvm.sh mips64el .cache/llvm-mips64el -- -DCMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN=$root \
    -DCMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN=$root
cmake --preset linux-mips64el-cross -DLLVM_DIR=$PWD/.cache/llvm-mips64el/lib/cmake/llvm -DSNX_LLVM_CONFIG=
cmake --build --preset linux-mips64el-cross
ctest --preset linux-mips64el-cross
```

`mips64` in place of `mips64el` builds the big-endian one. The tests run under `qemu-mips64` or `qemu-mips64el`
(`CMAKE_CROSSCOMPILING_EMULATOR`), with the sysroot as `-L`; the ones that start a program of the target themselves
(a Python driver, a fork and exec) need the kernel to run it through qemu too, a `binfmt_misc` handler, which
Linux 6.7 and later let a user namespace of its own register without root. A system root with its own LLVM (a
distribution's) takes `scripts/cross-llvm-config.sh` instead, which runs the root's `llvm-config` under the qemu of
the machine its ELF header names.

## What differs from the other hosts

- **Big-endian `mips64` converts guest values** as big-endian PowerPC64 does ([Big-endian hosts](./PPC64.md#big-endian-hosts)),
  through the same code: what decides is the byte order, never the architecture.
- **Kernels come with 4, 16 or 64 KiB pages.** jemalloc is built for 64 KiB (`cmake/jemalloc.cmake`), and the
  runtime works in the host's pages as on PowerPC64 ([Host pages larger than 4 KiB](./PPC64.md#host-pages-larger-than-4-kib)).
