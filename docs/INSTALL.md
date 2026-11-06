<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Install

## Downloads

Each push to `main` that builds everywhere becomes a nightly in
[scarletnx/nightly](https://github.com/scarletnx/nightly/releases), the newest 30 kept, with each open pull request's
builds. The latest:

| System | Download | |
| --- | --- | --- |
| Linux x86-64 | [ScarletNX-linux-x86_64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-x86_64.AppImage) | `chmod +x` it first |
| Linux AArch64 | [ScarletNX-linux-aarch64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-aarch64.AppImage) | `chmod +x` it first |
| Linux PowerPC64 LE | [ScarletNX-linux-ppc64le.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-ppc64le.AppImage) | Cross-built ([PowerPC64](PPC64.md)), when its job built |
| Linux PowerPC64 BE | [ScarletNX-linux-ppc64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-ppc64.AppImage) | Cross-built ([big-endian hosts](PPC64.md#big-endian-hosts)), not yet run on a big-endian machine |
| Linux RISC-V 64 | [ScarletNX-linux-riscv64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-riscv64.AppImage) | Cross-built ([RISC-V and LoongArch](RISCV-LOONGARCH.md)), when its job built |
| Linux LoongArch 64 | [ScarletNX-linux-loongarch64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-loongarch64.AppImage) | Cross-built ([RISC-V and LoongArch](RISCV-LOONGARCH.md)), when its job built |
| Windows x86-64 | [ScarletNX-windows-x86_64.zip](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-windows-x86_64.zip) | Run `bin\scarletnx.exe` |
| Android, arm64 and x86_64 | [ScarletNX-android-universal.apk](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-android-universal.apk) | [Android](ANDROID.md) |

Games need a Vulkan 1.3 driver. MIPS64 and s390x have no download: they build from their presets ([MIPS64](MIPS64.md),
[s390x](S390X.md)). macOS and iOS build in CI. An iPhone prepares and runs a game by itself, and no game runs on macOS
yet: see [macOS and iOS](APPLE.md). To build ScarletNX yourself, see [Build](BUILD.md), whose
[Install / AppImage](BUILD.md#install--appimage) installs a build.

## Keys and games

Keys and games must be dumped from your own console. Started with no arguments, the program opens the
[launcher](GUI.md). **Install Keys** installs your `prod.keys`. For NSPs, also put `title.keys` in
`~/.switch` (`%USERPROFILE%\.switch` on Windows). **Load Switch NSP/XCI/NRO** then picks a dump, extracts and
recompiles it and starts the game, which the **Games** tab lists from then on. See [Usage](USAGE.md) for the
command line and [Folders](USAGE.md#folders) for where games, saves and settings go.
