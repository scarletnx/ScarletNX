<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NetBSD

ScarletNX builds for NetBSD 11 on x86-64. It is a secondary platform: CI doesn't build it, and support is the code and
the preset below. No NetBSD machine has run it yet.

## Building

The `netbsd-x86_64-cross` preset cross-builds from Linux with Clang and LLD against a NetBSD sysroot.
`scripts/netbsd-sysroot.sh` makes one without root: NetBSD's `base` and `comp` sets (the C library, and its headers,
crt files and libgcc, which `base` lacks), and pkgsrc's SDL3, each checked against the SHA512 list it is published
with.

```bash
scripts/netbsd-sysroot.sh .cache/netbsd-sysroot
export SNX_SYSROOT=$PWD/.cache/netbsd-sysroot
cmake --preset netbsd-x86_64-cross
cmake --build --preset netbsd-x86_64-cross
```

This build has not been built against the sysroot yet, and nothing has run: there is no emulator for NetBSD programs on
Linux, and no NetBSD host.

## What differs from the other hosts

- **C++ is libstdc++.** NetBSD's base uses GCC's, while Clang links libc++ for NetBSD by default:
  the preset names `-stdlib=libstdc++`, and `-lgcc_s` for `_Unwind_Resume`, which Clang's NetBSD driver doesn't link.
- **Shared memory is a memfd**, as on Linux: NetBSD 11 has `memfd_create` in libc (`shm_open` is librt's). Guest
  threads wait on NetBSD's `__futex`, which takes Linux's operations.
- **The system's SDL3** by default (`SNX_SYSTEM_SDL3`), as on FreeBSD: pkgsrc's is 3.4.16, older than the pinned
  source build.
- **No RenderDoc.** It has no NetBSD build: `renderdoc=` says so and captures nothing.
