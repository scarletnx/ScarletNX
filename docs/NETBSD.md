<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NetBSD

ScarletNX builds for NetBSD 11 on x86-64. It is a secondary platform: CI doesn't build it, and support is the code and
the preset below. No NetBSD machine has run it yet; [Building](#building) says what was built.

## Building

The `netbsd-x86_64-cross` preset cross-builds from Linux with Clang and LLD against a NetBSD sysroot.
`scripts/netbsd-sysroot.sh` makes one without root: NetBSD's `base` and `comp` sets (the C library, and its headers,
crt files and libgcc, which `base` lacks), and pkgsrc's LLVM, SDL3, zstd and libxml2, each checked against the SHA512
list it is published with.

```bash
scripts/netbsd-sysroot.sh .cache/netbsd-sysroot
export SNX_SYSROOT=$PWD/.cache/netbsd-sysroot
cmake --preset netbsd-x86_64-cross -DSNX_LLVM_AS=/path/to/llvm-21/bin/llvm-as
cmake --build --preset netbsd-x86_64-cross
```

The runtime library's bitcode is read by pkgsrc's LLVM (21), so a Clang newer than it writes bitcode that LLVM can't
read. The build then assembles the library's IR with an llvm-as of that version, which has to run on the build
machine: pkgsrc's is a NetBSD program. `SNX_LLVM_AS` names one (a Linux build of LLVM 21), or a Clang no newer than 21
builds the library itself; the configure stops and says so when neither is given.

What was built, against NetBSD 11.1's sets and pkgsrc 2026Q3 (LLVM 21.1.8, SDL3 3.4.16), with Clang 22 and LLD: the
whole tree, `scarletnx` and every test program, linked against the sysroot's libraries (`libLLVM.so.21.1`,
`libSDL3.so.0`, `libstdc++.so.9`), with `/usr/pkg/lib` as their run path. Nothing ran: there is no emulator for
NetBSD programs on Linux, and no NetBSD host.

## What differs from the other hosts

- **C++ is libstdc++.** NetBSD's base and pkgsrc's LLVM use GCC's, while Clang links libc++ for NetBSD by default:
  the preset names `-stdlib=libstdc++`, and `-lgcc_s` for `_Unwind_Resume`, which Clang's NetBSD driver doesn't link.
- **LLVM is pkgsrc's shared library.** Its CMake package says its tools link `libLLVM.so` (`LLVM_LINK_LLVM_DYLIB`),
  which holds every target, so the program links it as a distribution's (`cmake/llvm.cmake`).
- **Shared memory is a memfd**, as on Linux: NetBSD 11 has `memfd_create` in libc (`shm_open` is librt's). Guest
  threads wait on NetBSD's `__futex`, which takes Linux's operations.
- **The system's SDL3** by default (`SNX_SYSTEM_SDL3`), as on FreeBSD: pkgsrc's is 3.4.16, older than the pinned
  source build.
- **No RenderDoc.** It has no NetBSD build: `renderdoc=` says so and captures nothing.
