<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# MIPS64

ScarletNX builds for 64-bit MIPS Linux, MIPS64 Release 2 and later with the n64 ABI, big-endian (`mips64`) and
little-endian (`mips64el`). It is a secondary architecture: CI builds neither, and support is the code and the
presets below. No MIPS machine has run it yet; [Building](#building) says what ran under qemu-user.

## Building

The `linux-mips64-cross` and `linux-mips64el-cross` presets cross-build with Clang against a system root of the
architecture, as PowerPC64's do ([PowerPC64](./ppc64.md)), and link with the target's GNU ld.
[gnu-cross](https://github.com/cross-tools/gnu-cross/releases)'s buildroots (`mips64-unknown-linux-gnu.tar.xz`,
`mips64el-unknown-linux-gnu.tar.xz`) are one: glibc in `<triple>/sysroot`, and beside it GNU ld and GCC's runtime
(crt files, libgcc, libstdc++), which Clang takes from the folder `SNX_GCC_TOOLCHAIN` names
(`CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN`, which `cmake/cross.cmake` passes on to FFmpeg's, OpenSSL's and the runtime
library's compiles as `--gcc-toolchain`). Their `libgcc.a` has absolute `.eh_frame` pointers, which LLD refuses in a
position-independent program and GNU ld makes relative.

```bash
root=/path/to/mips64el-unknown-linux-gnu
export SNX_SYSROOT=$root/mips64el-unknown-linux-gnu/sysroot SNX_GCC_TOOLCHAIN=$root
cmake --preset linux-mips64el-cross
cmake --build --preset linux-mips64el-cross
ctest --preset linux-mips64el-cross
```

`mips64` in place of `mips64el` builds the big-endian one. The tests run under `qemu-mips64` or `qemu-mips64el`
(`CMAKE_CROSSCOMPILING_EMULATOR`), with the sysroot as `-L`; the ones that start a program of the target themselves
(a Python driver, a fork and exec) need the kernel to run it through qemu too, a `binfmt_misc` handler, which
Linux 6.7 and later let a user namespace of its own register without root.

The test suite of this build has not been run under qemu-user yet.

## What differs from the other hosts

- **The module's GOT is the ABI's.** The runtime library's n64 code reaches functions and globals through the GOT,
  from gp, which each function computes from its own address (`%hi(%neg(%gp_rel(f)))`); the back end's own code calls
  and names what it needs PC-relative (`MG_R_CALL` in `src/common/cpu/recompiler/mips64/mips64_codegen.c`). The module's
  linker (`link_elf.c`, the relocations in `elf_object.c`) lays
  the GOT out as glibc's loader relocates it: two reserved slots, the 64 KiB pages that `%got_page` loads (within
  16 bits of gp, so they cover 512 MiB of a module), then the symbols' slots, all local (`DT_MIPS_LOCAL_GOTNO`), so
  the loader adds the module's base to each. The only dynamic relocations are addresses in data, `R_MIPS_REL32` with
  `R_MIPS_64`, and the module has no `DT_RELACOUNT`, as glibc's MIPS loader does nothing for the relocations it counts.
- **STXP's 16-byte compare-and-swap is the runtime's lock.** MIPS64 has no paired compare-and-swap that every core
  runs (Release 6's LLDP and SCDP are optional), so a module calls `RtAPI.cas128`, which takes the address's stripe
  lock and swaps the halves one at a time (`src/common/runtime/cas128.c`), as on RISC-V without Zacas
  ([RISC-V and LoongArch](./riscv-loongarch.md)).
- **No runtime JIT.** The JIT loader's machines are x86-64, AArch64 and PowerPC64 (`HOST_MACHINE` in `code_loader.c`),
  so MIPS64 runs only the code the AOT recompiler found, as RISC-V and LoongArch do.
- **Big-endian `mips64` converts guest values** as big-endian PowerPC64 does
  ([Big-endian hosts](./ppc64.md#big-endian-hosts)), through the same code: what decides is the byte order, never the
  architecture.
- **Kernels come with 4, 16 or 64 KiB pages.** jemalloc is built for 64 KiB (`cmake/jemalloc.cmake`), and the
  runtime works in the host's pages as on PowerPC64
  ([Host pages larger than 4 KiB](./ppc64.md#host-pages-larger-than-4-kib)).
