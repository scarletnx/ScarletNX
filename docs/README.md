<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# ScarletNX documentation

New here? Start with [Install](./INSTALL.md), then the [GUI launcher](./GUI.md) or [Usage](./USAGE.md). To build
ScarletNX yourself, see [Build](./BUILD.md).

## Using ScarletNX

- [Install](./INSTALL.md): the downloads for each system, keys and games, and the first start
- [GUI launcher](./GUI.md): the menu, its games, settings, themes and languages
- [Usage](./USAGE.md): extract, recompile and play from the command line; updates, DLC, mods, cheats, NROs, GDB,
  folders, users and the keys in a game
- [Settings](./SETTINGS.md): every key of a game's `.ini` and of the launcher, with its default
- [AOT packs](./AOT_PACKS.md): a game build that links in the functions the JIT compiled while it played
- [Testing and reporting](./TESTING.md): how to test a build, and the form a report takes
- [Deprecations](./DEPRECATIONS.md): what an older version used, how a run tells you, and how it goes away

## Platforms

- [Build](./BUILD.md): prerequisites and builds on Linux, FreeBSD, macOS and Windows, build variants, installing
- [Android](./ANDROID.md): the app, its keys, and building and signing it
- [macOS and iOS](./APPLE.md): the frontend, playing a game on an iPhone, and what is missing
- [PowerPC64](./PPC64.md): building, running a game under qemu, and big-endian hosts
- [RISC-V and LoongArch](./RISCV-LOONGARCH.md): building, and what differs from the other hosts
- [MIPS64](./MIPS64.md): mips64 and mips64el, built from their presets, and what differs from the other hosts

## Other systems

- [3DS](./3DS.md): the 3DS driver, its status, building and testing, and the parity roadmap
  - [CTR IPC and startup services](./CTR-IPC.md): the wire, the generator subset and the services implemented
  - [SuperTux target](./3DS-SUPERTUX.md): the pinned homebrew, how to reproduce its run, and its baseline
- [PS Vita](./VITA.md): the Vita driver, its shader corpus and its limits
- [Game Boy Advance](./AGB.md): the AGB board, playing a ROM, its devices and tests
- [Linux AArch64 programs (GNU ABI)](./GNUABI.md): the sysroot, how code runs, processes and the system calls

## Development

- [Development](./DEVELOPMENT.md): checks, running and diagnosing a game, profiling, shaders and SVCs
- [edenprobe](./EDENPROBE.md): the hardware probes CI runs against a baseline
- [Status and roadmap](./ROADMAP.md): what is next
- [Contributing](../CONTRIBUTING.md): game data, checks, commits and pull requests

## The recompiler

- [Architecture](./ARCHITECTURE.md): AArch64 to a native module, the runtimes' boundaries and what each owns
- [The recompiler as a library](./LIBRARY.md): building, using and the limits of `libscarletnx-recompiler`
- [Cortex-A57 divergences](./DIVERGENCES.md): where the recompiled code differs from the CPU
- [NCE](./NCE.md): the game's code on an AArch64 host's own CPU, Android's design calls and their limits
- [Hooks](./HOOKS.md): native handlers for functions a game exports, declaring and checking one
- [CPU implementation plan](./CPU_TRANSLATOR_PLAN.md): Rosetta 2's design, part by part, applied to ScarletNX
  - [Working on the plan](./CPU_TRANSLATOR_GUIDE.md): setup, rules, and how work is handed back
  - [Progress and measurements](./CPU_TRANSLATOR_PROGRESS.md): preparing the next item and measuring candidates
  - [First CPU milestone](./CPU_SIMD_MILESTONE.md): one measured SIMD improvement

## The Switch runtime

- [IPC interfaces](./IPC.md): the services a game talks to, their language, layout and results
- [Filesystem](./FILESYSTEM.md) and its [shared fonts' stand-ins](./SHARED_FONTS.md)
- [NRO files](./NRO.md) and [ldr:ro](./LDR_RO.md), the NROs a game loads
- [Networking](./NETWORKING.md): sockets, translation tables and their limits
- [Memory accounting](./MEMORY_ACCOUNTING.md): the application's memory budget

## GPU

- [NVN reference](./NVN_HLE.md): how games draw through NVN, and what the traces showed
- [GPU-owned guest memory](./GPU_OWNERSHIP.md): the design of memory the GPU keeps, and its phases
- [Guest buffers on unified memory](./UMA_MEMORY.md)
- [ASTC compute uploads](./ASTC.md)
- [Block linear surfaces and texture conversions on the GPU](./BLOCKLINEAR.md)
- [VIC composition on the GPU](./VIC.md)
- [NVDEC H.264 packet submission audit](./NVDEC.md)
- [Shader corrections, batch 001](./shader-audit/fixes-001.md)

## License

- [Licenses and credits](./CREDITS.md): the components, the code ported from other projects, and special thanks
- [GPL-3.0-or-later](../LICENSE)
