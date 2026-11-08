<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# ScarletNX documentation

## Index

* [Using ScarletNX](#using-scarletnx)
* [Development](#development)
* [Guest platforms](#guest-platforms)
* [Shared layers](#shared-layers)
* [Host systems](#host-systems)
* [History](#history)
* [Moved pages](#moved-pages)
* [License](#license)

New here? Start with [Install](user/install.md), then the [GUI launcher](user/gui.md) or [Usage](user/usage.md). To
build ScarletNX yourself, see [Build](dev/build.md).

The documents are in six folders. `user/` is for players, `dev/` for those who build and change ScarletNX.
`platforms/` has one folder for each guest system that ScarletNX runs, `layers/` has the parts that several of them
share (the CPU translator, the GPU, video), and `hosts/` has the systems that ScarletNX itself runs on. `history/`
keeps audits and measurements as they were taken.

## Using ScarletNX

- [Install](user/install.md): the downloads for each system, keys and games, and the first start
- [GUI launcher](user/gui.md): the menu, its games, settings, themes and languages
- [Usage](user/usage.md): extract, recompile and play from the command line, with updates, DLC, mods, cheats, NROs,
  GDB, folders, users and the keys in a game
- [Settings](user/settings.md): every key of a game's `.ini` and of the launcher, with its default
- [AOT packs](user/aot-packs.md): a game build that links in the functions the JIT compiled while it played
- [Testing and reporting](user/testing.md): how to test a build, and the form a report takes
- [Deprecations](user/deprecations.md): what an older version used, how a run tells you, and how it goes away

## Development

- [Build](dev/build.md): prerequisites and builds on Linux, FreeBSD, macOS and Windows, build variants, installing
- [Development](dev/development.md): checks, running and diagnosing a game, profiling, shaders and SVCs
- [Architecture](dev/architecture.md): AArch64 to a native module, the runtimes' boundaries and what each owns
- [The recompiler as a library](dev/library.md): building, using and the limits of `libscarletnx-recompiler`
- [edenprobe](dev/edenprobe.md): the hardware probes CI runs against a baseline
- [Status and roadmap](dev/roadmap.md): what is next
- [Contributing](../CONTRIBUTING.md): game data, checks, commits and pull requests

## Guest platforms

- Switch (`platforms/nx/`)
  - [IPC interfaces](platforms/nx/ipc.md): the services a game talks to, their language, layout and results
  - [Hooks](platforms/nx/hooks.md): native handlers for functions a game exports, declaring and checking one
  - [NCE](platforms/nx/nce.md): the game's code on an AArch64 host's own CPU, Android's design calls and their limits
  - [Filesystem](platforms/nx/filesystem.md): paths, saves and file I/O, and the shared fonts' stand-ins
  - [NRO files](platforms/nx/nro.md): NRO programs, and the NROs a game loads through `ldr:ro`
  - [Networking](platforms/nx/networking.md): sockets, translation tables and their limits
  - [Memory accounting](platforms/nx/memory-accounting.md): the application's memory budget
- [3DS](platforms/3ds/README.md): the 3DS driver, its status, building and testing
  - [CTR IPC and startup services](platforms/3ds/ipc.md): the wire, the generator subset and the services implemented
  - [SuperTux target](platforms/3ds/supertux.md): the pinned homebrew, how to reproduce its run, and its baseline
- [PS Vita](platforms/vita/README.md): the Vita driver, its shader corpus and its limits
- [Game Boy Advance](platforms/agb/README.md): the AGB board, playing a ROM, its devices and tests
- [Linux AArch64 programs (GNU ABI)](platforms/gnuabi/README.md): the sysroot, how code runs, processes and the
  system calls
- [Android apps](platforms/android-apps/README.md): `scarletnx-android`, which runs an Android app's code on a
  desktop's JVM, and its DEX translator. This is a guest platform. For the ScarletNX app that plays Switch games on a
  phone, see [Android host](hosts/android-app.md).
  - [Interfaces](platforms/android-apps/interfaces.md): each layer of Android an app reaches, and how far the runtime
    emulates it
  - [HWUI](platforms/android-apps/hwui.md): the display lists and their Vulkan renderer
  - [The native bridge](platforms/android-apps/native.md): an app's AArch64 libraries run through the recompiler,
    their imports answered by the host

## Shared layers

- CPU (`layers/cpu/`)
  - [CPU implementation plan](layers/cpu/plan.md): Rosetta 2's design, part by part, applied to ScarletNX
  - [Working on the plan](layers/cpu/guide.md): setup, rules, how work is handed back, and the first milestone
- GPU (`layers/gpu/`)
  - [NVN](layers/gpu/nvn.md): how games draw through NVN, and how the runtime replays it
  - [GPU-owned guest memory](layers/gpu/ownership.md): the design of memory the GPU keeps, guest buffers on
    unified memory, and its phases
  - [ASTC compute uploads](layers/gpu/astc.md)
  - [Block linear surfaces and texture conversions on the GPU](layers/gpu/blocklinear.md)
  - [glslc](layers/gpu/glslc.md): GLSL ES into SPIR-V, for the backends that run OpenGL ES programs on Vulkan
- Video (`layers/media/`)
  - [VIC composition on the GPU](layers/media/vic.md)

## Host systems

- [Android host](hosts/android-app.md): the ScarletNX app that plays Switch games on a phone, its keys, and building
  and signing it
- [macOS and iOS](hosts/apple.md): the frontend, playing a game on an iPhone, and what is missing
- [PowerPC64](hosts/ppc64.md): building, running a game under qemu, and big-endian hosts
- [RISC-V and LoongArch](hosts/riscv-loongarch.md): building, and what differs from the other hosts
- [MIPS64](hosts/mips64.md): mips64 and mips64el, built from their presets, and what differs from the other hosts
- [s390x](hosts/s390x.md): IBM Z, built from its preset, and what differs from the other hosts
- [NetBSD](hosts/netbsd.md): x86-64, cross-built from Linux with its preset, and what differs from the other hosts

## History

Audits and measurements, kept as they were when they were taken. The code has changed since.

- [Cortex-A57 divergences](history/divergences.md): the old audit of the instruction semantics against Unicorn
- [CPU progress and measurements](history/cpu-translator-progress.md): preparing plan items and measuring candidates
- [NVN rendering audit](history/gpu-rendering-audit.md): corrected state and copy semantics, exact GPU checks, and
  open gaps
- [GPU preparation benchmarks](history/gpu-hotpath-bench.md): exact paired tests of CPU work before GPU submission
- [NVN to Vulkan optimization results](history/nvn-vulkan-optimization.md): measured components, samples and
  validation
- [Graphics descriptor measurements](history/gpu-descriptor-bench.md): descriptor backends, cache memory and paired
  timings
- [NVDEC H.264 packet submission audit](history/nvdec-audit.md)
- [Shader corrections, batch 001](history/shader-audit/fixes-001.md)

## Moved pages

These short pages stay at their old paths, because files that older versions wrote and older release notes link to
them. Each one points to its new place.

- [USAGE.md](USAGE.md), now [Usage](user/usage.md)
- [ANDROID.md](ANDROID.md), now [Android host](hosts/android-app.md)
- [DEPRECATIONS.md](DEPRECATIONS.md), now [Deprecations](user/deprecations.md)

## License

- [Licenses and credits](CREDITS.md): the components, the code ported from other projects, and special thanks
- [GPL-3.0-or-later](../LICENSE)
