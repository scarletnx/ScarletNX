<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# RISC-V and LoongArch

ScarletNX builds for 64-bit RISC-V (rv64gc) and LoongArch (LA64 v1.0) Linux. No machine of either has run it yet. CI
cross-builds their AppImages (`ScarletNX-linux-riscv64` and `ScarletNX-linux-loongarch64`, the `appimage-cross` job in
`.github/workflows/build.yml`) against an Arch system root of the architecture and runs them under qemu as installed,
as it does PowerPC64 LE's ([PowerPC64](./PPC64.md)). The nightlies publish them when their jobs built, as
PowerPC64 LE's.

## Building

As PowerPC64 LE's, with the `linux-riscv64-cross` or `linux-loongarch64-cross` preset and an Arch root of the
architecture in `SNX_SYSROOT` (its pkg-config files, its LLVM and its GCC runtime). `scripts/appimage-sysroot.sh ROOT`
installs what the AppImage links into an unpacked Arch root, as CI does, which keeps the root it makes for a week:

```bash
export SNX_SYSROOT=/path/to/the/root
cmake --preset linux-riscv64-cross
cmake --build --preset linux-riscv64-cross
```

The root's `llvm-config` is a program of the target: `scripts/cross-llvm-config.sh` runs it under the qemu of the
machine its ELF header names (`qemu-riscv64`, `qemu-loongarch64`). The AppImages link LLVM statically instead, built
for the architecture by `scripts/linux-llvm.sh riscv64 PREFIX` with `SNX_SYSROOT` set, and configured with
`-DLLVM_DIR=PREFIX/lib/cmake/llvm -DSNX_LLVM_CONFIG=` ([Build](BUILD.md#install--appimage)). CI packages them with
this machine's `mkdwarfs`, as the target's compresses many times slower under qemu, and strips the target's files with
`llvm-strip`, as this machine's `strip` knows its own architecture only.

## What differs from the other hosts

- **No linker relaxation.** The builds and the modules are rv64gc and LA64 v1.0 without it (`cmake/cross.cmake`,
  `src/common/cpu/recompiler/llvm/ir.c`): the runtime library is merged into each module with its functions' own
  target features, and the module's linker (`link_elf.c`, the relocations in `elf_object.c`) moves no code, so it
  refuses `R_RISCV_ALIGN` and `R_LARCH_ALIGN`.
- **STXP's 16-byte compare-and-swap is the runtime's.** These machines have the instruction only with Zacas
  (`amocas.q`) and from LoongArch v1.1 (`sc.q`), so a module calls `RtAPI.cas128` (SNX_ABI 12,
  `src/common/runtime/cas128.c`). It runs the instruction where the kernel (`riscv_hwprobe`) or `CPUCFG` says every
  core has it; elsewhere it swaps the halves under a lock of the address's stripe, each a 64-bit compare-and-swap, and
  undoes the first when the second finds another value. Callers of it see the 16 bytes change at once; a plain store
  to one half between its two swaps is not refused, as a real exclusive monitor would refuse it.
- **No runtime JIT on these two hosts.** The loader's native targets are x86-64, AArch64 and PowerPC64.
  RISC-V and LoongArch still run only the code the AOT recompiler found.
