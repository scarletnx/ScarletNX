<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# PowerPC64

## Index

* [Building on an x86-64 Debian machine](#building-on-an-x86-64-debian-machine)
* [Running a game under qemu](#running-a-game-under-qemu)
* [Host pages larger than 4 KiB](#host-pages-larger-than-4-kib)
* [What differs from the other hosts](#what-differs-from-the-other-hosts)
* [Big-endian hosts](#big-endian-hosts)
* [The PowerPC 970 and POWER4](#the-powerpc-970-and-power4)

ScarletNX builds for 64-bit PowerPC Linux from POWER8 on, and big-endian builds from the PowerPC 970 (the PowerMac
G5) and POWER4 on. The little-endian port (ppc64le) works: the test suite
passes under qemu-user, and Super Haxagon, recompiled under qemu, played a level to its game over screen there with
lavapipe as the Vulkan driver. No real POWER machine has run it yet. CI cross-builds its AppImage
(`ScarletNX-linux-ppc64le.AppImage`, the `appimage-cross` job in `.github/workflows/build.yml`) against an Arch POWER
system root and runs it under qemu as installed; the test suite's job, `linux-ppc64le`, is commented out.
The big-endian port has a job of its own, `linux-ppc64`, which builds the Linux runtime and runs its tests under
qemu-ppc64.

On a big-endian host the Linux runtime runs AArch64 and 32-bit Arm programs, with Debian ports' glibc
(`linux-ppc64-cross`, ELFv1) and with Arch POWER's (`linux-ppc64-elfv2-cross`, ELFv2): under qemu-ppc64, an
AArch64-hosted GCC compiles a C file and the program it made runs, both dynamically linked against glibc. The Switch
runtime is ported to big-endian ELFv2 too, its services, IPC, memory, GPU front end and video, and the renderer
treats the Vulkan device as little-endian, as the drivers for big-endian PowerPC64 are. Its test suite passes under
qemu-ppc64, but no game has run on a big-endian machine yet. See [Big-endian hosts](#big-endian-hosts).

## Building on an x86-64 Debian machine

The tools, once:

```bash
sudo apt install clang lld llvm ninja-build cmake ccache mmdebstrap qemu-user qemu-user-binfmt binfmt-support \
    glslang-tools spirv-tools debian-ports-archive-keyring
```

A sysroot with the target's libraries and LLVM, made without root by `scripts/ppc64-sysroot.sh` (about 1 GB). It is
staged in `/var/tmp`, as `/tmp` is often too small. `ppc64` makes the big-endian one from Debian's ports archive:

```bash
scripts/ppc64-sysroot.sh ppc64el .cache/sysroot-ppc64el
export SNX_SYSROOT=$PWD/.cache/sysroot-ppc64el
```

Then the build and the tests, which run under qemu-ppc64le:

```bash
cmake --preset linux-ppc64le-cross -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build --preset linux-ppc64le-cross
ctest --preset linux-ppc64le-cross -j"$(nproc)"
```

The target's `llvm-config` is a PowerPC program, so `scripts/cross-llvm-config.sh` runs it under qemu. The presets
point pkg-config at the sysroot and set `QEMU_LD_PREFIX` for the tests. One test is skipped under the emulator:
`gui_process_tree` reads a resident size that qemu reports as zero.

## Running a game under qemu

The recompiler runs on the machine the module is for, so under qemu it takes a while (Super Haxagon: about 20
minutes, against 4 natively on x86-64):

```bash
export QEMU_LD_PREFIX=$SNX_SYSROOT VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
qemu-ppc64le build/linux-ppc64le-cross/scarletnx -r app.nro --name App --outdir games/ppc64le
cd games/ppc64le/App && qemu-ppc64le ./App -H --seconds=120 --shot=/path/to/shot
```

## Host pages larger than 4 KiB

Most ppc64le distributions, Debian's among them, run kernels with 64 KiB pages, and Apple silicon and some AArch64
kernels use 16 KiB ones. The Switch maps memory in 4 KiB pages, which a host can only commit and protect in its own,
so `src/nx/memory/mem.c` keeps the guest's pages in its block list and commits whole host pages under them. Freeing a
guest page decommits its host page once nothing else in it is mapped, and zeroes the page otherwise, so it reads zero
when mapped again, as on the console. The differences from a 4 KiB host:

- A guest access to a free page that shares a host page with a mapped one doesn't fault.
- Views of memory (MapMemory, shared memory, ldr's code) are copies, as on Windows, instead of the same pages. A view
  can only share host pages with its source when both sit at the same place in a host page, and games pick their
  addresses in 4 KiB steps. The services reach shared memory through the guest's mapping while it lasts.
- The Linux runtime (`src/gnuabi/`) protects each host page as the most permissive of its guest pages, so a write to
  compiled code beside writable data in one host page isn't seen.

The runtime's setting `host_page=16384` or `65536` makes any machine work in those pages, which is how the tests and
CI check these paths on 4 KiB hosts (`ldr_service_64k_pages`, `nx_runtime_homebrew_64k_pages`; for `scarletnx-gnuabi`,
`SNX_HOST_PAGE` in the environment, `gnuabi_memory_64k_pages`); `memory_accounting` runs as on 16 and 64 KiB hosts by
itself.

## What differs from the other hosts

- **The baseline is POWER4** but see [below](#the-powerpc-970-and-power4). Before it there is
  no `lqarx`/`stqcx.`, and the 128-bit compare-and-swap that STXP becomes would be a libatomic call,
  which a module doesn't link.
- **Guest tail calls go through stubs.** Without POWER10's PC-relative code, LLVM refuses a guaranteed tail call to a
  function it can't see shares the caller's TOC: another unit's function, or a pointer. A module has one TOC, so the
  jump is safe, and `ppc_tail_calls` in `src/common/cpu/recompiler/llvm/ir.c` sends those calls to a naked stub of the
  same module that branches on. A pointer's stub that misses calls `RtAPI.miss` from a frame of its own and keeps r2
  in that frame: r1 may still be the runtime's frame that called into the module, whose saved TOC it reloads.
- **The linker** (`src/common/cpu/recompiler/llvm/link_elf.c`, with the relocations in `elf_object.c`) handles the
  TOC relocations, ELFv2's local entry points, ELFv1's `.opd` descriptors, and 16-byte stubs for a call past `bl`'s
  32 MiB.
- **One TOC for the Linux runtime's code.** Its just-in-time steps are loaded into one code arena
  (`code_loader.c`, `library.c`'s `arena_load`) with the runtime library once, and the arena has one TOC, which every
  step's code keeps in r2. Elsewhere each step is a module of its own, linked and loaded with `dlopen`. On PowerPC64
  that gave each step its own TOC, and a guest's jump from one step into another came back to its caller with the
  other step's TOC. Loaded code calls the runtime only through pointers (RtAPI), which LLVM calls with r2 saved. A
  forked child copies the arena's pages, which are shared memory, into its own (`snx_recompiler_forked`).
- **The runtime JIT shares the game module's TOC.** Its units call the AOT module's functions directly, so their
  arena is placed within 32-bit reach of the module's TOC, which every unit keeps in r2
  (`snx_code_loader_new_toc` in `code_loader.c`, `aot_toc` in `jit_service.c`, which reads the TOC from an ELFv1
  descriptor or an ELFv2 global entry).
- **A32 blocks call the 32-bit runtime library in their arena.** The executor (`common/cpu/arm/exec.c`) loads
  `armlib` into its code arena first, as `arena_load` does with `rtlib` for A64, so blocks share one TOC and reach
  the runtime only through `cpu->rt`. The Linux runtime runs 32-bit programs on PowerPC64 with it.
- **Rounding stays on the slow path.** LLVM 19 lowers `llvm.roundeven` to a libm call on POWER8, and fails on its
  vector form, so FRINTN and the rounding conversions use the software path. Fused multiply-add is a fast path, as
  POWER always has it.
- **SDL3 is built without AltiVec** (`cmake/dependencies.cmake`): its blitters prefetch with `dstt`, which the ISA
  phased out and qemu traps.

## Big-endian hosts

AArch64 is little-endian, so on a big-endian host guest memory keeps the guest's byte order and everything that reads
or writes a guest value as a number converts it:

- The recompiler swaps where generated code reads memory as numbers (`ir_rd`, `ir_wr`, `ir_ldacq`, `ir_strel` in
  `ir_state.c`) and where it compares in memory (`cas` in `ir_helpers.c`, `snx_cas128`). LLVM folds the swaps into
  POWER's byte-reversed loads and stores. The vector registers hold the guest's bytes too: the views that read a lane
  as a number swap it (`ir_vd`, `ir_ve`, the lane helpers of `ir_vec.c`), and Q loads, LD1 to LD4, TBL and EXT stay
  byte copies. The emitters decide by the target's data layout, so a little-endian host's code is what it was. The
  helpers and the tests that set and check lanes read and write a lane as a number through `SNX_VS` and its kin in
  `cpu.h`, which swap on a big-endian host.
- The Linux runtime's system calls read and write the guest's structures through `common/utilities/endian.h`
  (`snx_ld_le32`, `snx_st_le64` and their kin), which compile to plain copies on a little-endian host.
- `scripts/le-unchanged.sh` builds a base commit and the working tree on a little-endian machine and lists every
  object whose code differs, and whether the recompiler's output for an AArch64 program changed.

The build links with GNU ld, since LLD links no ELFv1 object: `binutils-powerpc64-linux-gnu` on Debian. The system
root is Debian ports' unstable:

```bash
scripts/ppc64-sysroot.sh ppc64 .cache/sysroot-ppc64
export SNX_SYSROOT=$PWD/.cache/sysroot-ppc64
cmake --preset linux-ppc64-cross -DSNX_BUILD_NX=OFF -DSNX_BUILD_3DS=OFF -DSNX_BUILD_VITA=OFF -DSNX_BUILD_AGB=OFF \
    -DSNX_BUILD_GUI=OFF
cmake --build --preset linux-ppc64-cross --target scarletnx
```

`scarletnx` is then the Linux runtime alone, and `scarletnx-gnuabi` a link to it.

CI's `linux-ppc64` job builds it this way against that system root, made weekly, and runs the Linux runtime's tests
(`gnuabi_*`, `endian`, `cas128`) under qemu-ppc64. Debian ports' unstable can break for a day, so the job may fail
without failing the run.

Arch POWER's big-endian glibc is ELFv2, as LLD links it. Its preset (`linux-ppc64-elfv2-cross`, `SNX_PPC64_ELFV2`)
builds for ELFv2 with `-mabi=elfv2` and the Linux runtime alone. Its system root is pkgforge's Arch image of the
architecture with LLVM and GCC's runtime installed into it, as the `appimage-cross` job makes it. The runtime's own
code generator takes its ABI from the runtime itself (`_CALL_ELF`, `new_ir` in `ir.c`), as LLVMs differ in which
ABI their `powerpc64-*-linux-gnu` picks: Debian's 19 ELFv1, Arch POWER's 22 ELFv2.

What was run under qemu-ppc64: the Linux runtime's tests (`gnuabi_*`, `endian`, `cas128`), `tools/gnuabi_diff.py
--suite all` against qemu-aarch64 (the CPU suite covers SIMD, floating point, exclusives and unaligned accesses), a
glibc program, and the AArch64-hosted GCC 16.2 compiling a C file with a glibc system root.

### The Switch runtime

Everything the Switch runtime reads or writes in guest memory as a number converts it, with converting helpers that
compile to nothing on a little-endian host:

- `gr16`/`gr32`/`gr64` and `gw16`/`gw32`/`gw64` (`memory/mem.h`) for a guest address, endian.h's `snx_ld_le32` and
  kin for a pointer into guest memory, and `snx_le32_array` and kin for a structure of words copied out of it or
  about to be copied in. Bitfields over guest words are shifts and masks of the converted word.
- IPC (`docs/IPC.md`): the generated dispatchers convert raw arguments and outputs, so a handler's are host
  numbers, while buffers keep the guest's order and the handler converts what it reads in them. The kernel converts
  the HIPC and CMIF headers and descriptors, and the compare-and-swaps on guest words (mutex tags, condition
  variables, address arbitration) take converted operands.
- Shared memory the runtime writes (HID, time, applets) is little-endian. The services' formats that are big-endian
  on the console, such as the shared fonts' header and the Opus packet header, keep the console's order.
- The GPU's command processor converts GPFIFO entries and push buffer words where it reads them, the shader
  translator reads instructions and the SPH as little-endian words, texture headers and samplers too, and what the
  engines write to guest memory (semaphores, reports, constant buffer updates, I2M data) is little-endian.
- **The Vulkan device is taken to be little-endian**, as Mesa's drivers on big-endian PowerPC64 are, while the API
  (structs, command arguments, SPIR-V words) is the host's order. Guest data goes to the device as its bytes; what
  the CPU computes for the device (push constants, uniform and index buffers it builds, indirect commands) is
  written little-endian, and what it reads back is converted. lavapipe on a big-endian host is not little-endian,
  so it doesn't show a game's rendering there.
- hactool's headers are converted as they are read, or at each field where they are signed or written back, so
  extraction and recompiling work on a big-endian machine.

The build (the Arch POWER system root of the `appimage-cross` job, with GCC's runtime and LLVM installed):

```bash
export SNX_SYSROOT=/path/to/arch-power-ppc64
cmake --preset linux-ppc64-elfv2-cross
cmake --build --preset linux-ppc64-elfv2-cross
ctest --preset linux-ppc64-elfv2-cross -j"$(nproc)"
```

The Vita and Game Boy Advance runtimes are ported the same way. Their guests are little-endian Arm, and what reads or
writes guest memory as a number goes through endian.h: `vita_read16`, `vita_read32` and their writes
(`vita/runtime/process.h`) for a guest address, and `snx_ld_le32` and its kin for the loader's files, GXP programs,
the HLE's structures and the GBA's bus, video memory and wave RAM. Network order and the trophy pack stay big-endian
(`snx_ld_be32`). The Vita renderer's push constants are swapped to little-endian for the device, and the GBA's frame
is a texture of packed 0xAABBGGRR words (`SDL_PIXELFORMAT_ABGR8888`) in either order. Their tests pass under
qemu-ppc64, jsmolka's GBA test ROMs among them: their modules are linked by the backend's linker, as the Switch's are.
No Vita or GBA game has run on a big-endian machine yet.

The 3DS runtime is built too, as the AppImage carries every runtime, but it isn't ported: its board, services and
GPU read the guest's memory as host numbers, so it doesn't run 3DS games on a big-endian host.

A rebuild that runs CMake again needs the preset's `PKG_CONFIG_LIBDIR` and `PKG_CONFIG_SYSROOT_DIR` in the
environment: `cmake --build --preset` sets them, a plain `cmake --build` of the folder doesn't, and SDL then finds the
build machine's Wayland and DRM.

## The PowerPC 970 and POWER4

For the PowerPC 970 of the PowerMac G5. The `linux-ppc64-g5-cross` preset is the ELFv2 build for the 970.
Clang, FFmpeg and the recompiler's runtime library are built for it, and the recompiler builds a game's module for
the host's CPU, or for this one where the host's name isn't known. What changes before POWER8:

- **STXP's 16-byte compare-and-swap goes through the runtime** (`RtAPI.cas128`, `common/runtime/cas128.c`), as on
  RISC-V and LoongArch. It runs POWER8's `lqarx`/`stqcx.` where the kernel reports ISA 2.07, and takes a lock of the
  address's stripe elsewhere, which keeps the 16 bytes atomic against other STXPs but not against plain stores.
- **Single-precision vector lanes can flush denormals.** Without VSX, LLVM puts them on AltiVec, whose VSCR[NJ] is
  set when Linux starts a thread, so denormals flush to zero where an emitter (`ir_vec_fp.c`) uses the host's vector
  arithmetic. Double lanes and scalars use the FPU.
- Rounding, `floor`, `trunc`, `rint`, `minnum` and the half conversions would be libm calls on these CPUs, which the
  emitters already avoid on PowerPC64.

The PowerMac G5's own graphics cards have no Vulkan driver, so the 970 build needs a later card that Mesa drives.
Under `qemu-ppc64 -cpu 970` the build stops in Arch POWER's `libLLVM`, in its static constructors before `main`, while
a program without LLVM runs there: the 970 build is checked for instructions the 970 lacks (none outside OpenSSL's
routines for later CPUs, which it picks by the machine's capabilities), and runs under qemu as a POWER8.
