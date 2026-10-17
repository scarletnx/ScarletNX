<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# ScarletNX documentation

New here? Start with [Build](./BUILD.md), then [Usage](./USAGE.md).

- **[Build](./BUILD.md)** - prerequisites and builds on each platform
- **[Usage](./USAGE.md)** - extract, recompile and play; settings and folders
- **[Troubleshooting](./TROUBLESHOOTING.md)** - errors a game or the driver prints
- **[GUI launcher](./GUI.md)** - the menu and its themes
- **[Android](./ANDROID.md)**
- **[macOS and iOS](./APPLE.md)**
- **[Development](./DEVELOPMENT.md)** - tests and diagnostics
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
  - **[Filesystem](./FILESYSTEM.md)**
  - **[NRO files](./NRO.md)** and **[ldr:ro](./LDR_RO.md)**
  - **[Networking](./NETWORKING.md)**
  - **[Memory accounting](./MEMORY_ACCOUNTING.md)**
- GPU:
  - **[ASTC compute uploads](./ASTC.md)**
  - **[Guest buffers on unified memory](./UMA_MEMORY.md)**
  - **[Shader corrections, batch 001](./shader-audit/fixes-001.md)**
