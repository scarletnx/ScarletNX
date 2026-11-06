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

What was run under qemu-user 10.0, against gnu-cross 20261001's buildroots with LLVM 23.1.3: the whole build of both,
and their test suites with a `binfmt_misc` of their own. Of 589 tests, mips64el passes 480 and mips64 466, among them
`elf_relocate` (MIPS64 objects linked into a module, loaded with `dlopen` and run), `native_recompiler`, `unit_cache`,
`arm_module_chain`, `3ds_aot_workers`, the Linux runtime's `gnuabi_*` but `gnuabi_elf32`, `cas128`, `endian` and
`build_info`. Both fail what needs the runtime JIT (`gnuabi_elf32`, `agb_*`, `jit_*`, the `llvm_*` steps' JIT halves),
`llvm_fp_fast` (fast paths of x86-64 and AArch64 alone), the `hookgen_refuses_*` tests (a cross build runs the build
machine's `snx-hookgen` through qemu too), and what qemu-user doesn't show as a machine would (`memory_own*` read the
thread's pc from `/proc`, `gdbstub_native` attaches with ptrace, `gui_sdl_events` runs past its time). mips64 fails 15
more, in code without a MIPS-specific path: the 3DS runtime's `3ds_dsp` and `3ds_pica` (not ported to big-endian hosts,
[PowerPC64](./PPC64.md#big-endian-hosts)), `driver_workflow` past its 60 seconds, tests that read lanes in the host's
order (`llvm_rsqrte`, `fp_host_witnesses` and the other FP steps), runtime code that writes a guest word in it
(`nvn_draw_init`'s point size, `src/nx/gpu/nvn/nvn_draw.c`), and the services' and GPU's tests `nvdrv_service`,
`ldn_service`, `filesystem_service`, `nvn_commands`, `nvn_names`, `fermi2d_blit`, `blocklinear_glsl`, `draw_state_mx`,
`vulkan_release` and `mcp`, not run on big-endian PowerPC64 to compare.

## What differs from the other hosts

- **The module's GOT is the ABI's.** n64 code reaches every function and global through the GOT, from gp, which each
  function computes from its own address (`%hi(%neg(%gp_rel(f)))`). A game's units call more functions than a 16-bit
  offset from gp reaches (8190 slots), so the recompiler builds for 32-bit GOT offsets (`+xgot`, `new_ir` in
  `src/common/cpu/recompiler/llvm/ir.c`). The module's linker (`link_elf.c`, the relocations in `elf_object.c`) lays
  the GOT out as glibc's loader relocates it: two reserved slots, the 64 KiB pages that `%got_page` loads (within
  16 bits of gp, so they cover 512 MiB of a module), then the symbols' slots, all local (`DT_MIPS_LOCAL_GOTNO`), so
  the loader adds the module's base to each. The only dynamic relocations are addresses in data, `R_MIPS_REL32` with
  `R_MIPS_64`, and the module has no `DT_RELACOUNT`, as glibc's MIPS loader does nothing for the relocations it counts.
- **Guest tail calls need LLVM's `-mips-tail-calls`**, which `snx_ir_init` sets on a MIPS64 host: without it LLVM's
  MIPS backend makes no call a jump, and a `musttail` one is a fatal error. It jumps to a pointer, or to a function it
  may take for the module's own, so every callee of a `musttail` call is made hidden (`mips_tail_callees` in
  `ir.c`), as every function a module calls is linked into it.
- **STXP's 16-byte compare-and-swap is the runtime's lock.** MIPS64 has no paired compare-and-swap that every core
  runs (Release 6's LLDP and SCDP are optional), so a module calls `RtAPI.cas128`, which takes the address's stripe
  lock and swaps the halves one at a time (`src/common/runtime/cas128.c`), as on RISC-V without Zacas
  ([RISC-V and LoongArch](./RISCV-LOONGARCH.md)).
- **No runtime JIT.** The JIT loader's machines are x86-64, AArch64 and PowerPC64 (`HOST_MACHINE` in `code_loader.c`),
  so MIPS64 runs only the code the AOT recompiler found, as RISC-V and LoongArch do.
- **Big-endian `mips64` converts guest values** as big-endian PowerPC64 does
  ([Big-endian hosts](./PPC64.md#big-endian-hosts)), through the same code: what decides is the byte order, never the
  architecture.
- **Kernels come with 4, 16 or 64 KiB pages.** jemalloc is built for 64 KiB (`cmake/jemalloc.cmake`), and the
  runtime works in the host's pages as on PowerPC64
  ([Host pages larger than 4 KiB](./PPC64.md#host-pages-larger-than-4-kib)).
