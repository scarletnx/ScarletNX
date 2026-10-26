<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# ScarletNX documentation

New here? Start with [Build](./BUILD.md), then [Usage](./USAGE.md).

- **[Build](./BUILD.md)** - prerequisites and builds on each platform
- **[Usage](./USAGE.md)** - extract, recompile and play; settings and folders
- **[GUI launcher](./GUI.md)** - the menu and its themes
- **[Android](./ANDROID.md)**
- **[macOS and iOS](./APPLE.md)**
- **[PowerPC64](./PPC64.md)**
- **[RISC-V and LoongArch](./RISCV-LOONGARCH.md)**
- **[Development](./DEVELOPMENT.md)** - tests and diagnostics
  - **[Testing and reporting](./TESTING.md)** - how to test a build, and the form a report takes
  - **[Performance regression tests](./PERF.md)** - the CPU and GPU ratios CI holds a change to
  - **[edenprobe](./EDENPROBE.md)** - hardware probes CI runs against a baseline
- **[Deprecations](./DEPRECATIONS.md)** - what an older version used, and how it goes away
- **[Status and roadmap](./ROADMAP.md)**
- **[Licenses and credits](./CREDITS.md)**
- Recompiler:
  - **[Architecture](./ARCHITECTURE.md)** - AArch64 to C to a native module
  - **[The recompiler as a library](./LIBRARY.md)**
  - **[Cortex-A57 divergences](./DIVERGENCES.md)** - where it differs from the CPU
- Other programs:
  - **[Linux AArch64 programs (GNU ABI)](./GNUABI.md)**
  - **[3DS port](./3DS.md)**, its **[IPC and startup services](./CTR-IPC.md)** and its
    **[SuperTux target](./3DS-SUPERTUX.md)**
- Switch runtime:
  - **[IPC interfaces](./IPC.md)** - the services a game talks to
  - **[Filesystem](./FILESYSTEM.md)** and its **[shared fonts' stand-ins](./SHARED_FONTS.md)**
  - **[NRO files](./NRO.md)** and **[ldr:ro](./LDR_RO.md)**
  - **[Networking](./NETWORKING.md)**
  - **[Memory accounting](./MEMORY_ACCOUNTING.md)**
- GPU:
  - **[ASTC compute uploads](./ASTC.md)**
  - **[Block linear surfaces and texture conversions on the GPU](./BLOCKLINEAR.md)**
  - **[VIC composition on the GPU](./VIC.md)**
  - **[Guest buffers on unified memory](./UMA_MEMORY.md)**
  - **[NVN reference](./NVN_HLE.md)** - how games draw through NVN, and what the traces showed
  - **[NVDEC H.264 packet submission audit](./NVDEC.md)**
  - **[Shader corrections, batch 001](./shader-audit/fixes-001.md)**
