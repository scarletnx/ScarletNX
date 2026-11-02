# ScarletNX
<p align="center">
  <img src="./dist/wimsy.svg" alt="Wimsy, the mascot" width="200"><br>
  <b>Wimsy, the ScarletNX mascot</b>
</p>

Discord: https://discord.gg/tRTRXXbyjp (my god why don't we just use IRC like good people?)

A GPLv3 static ARM64 to x86 recompiler with a Switch, GBA and GNU/Linux ABI translation layer for Solaris, Linux, Windows, FreeBSD and Android.

A game from your own dump is recompiled ahead of time into a native module for the machine it plays on, and runs on a
runtime that answers the console's system calls and services and draws with Vulkan. Experimental drivers run 3DS games
and PS Vita homebrew the same way, and Game Boy Advance ROMs on an emulated AGB board still under construction.

## Download and run

The newest build of `main` for Linux (AppImage), Windows and Android is in
[scarletnx/nightly](https://github.com/scarletnx/nightly/releases/latest); [Install](docs/INSTALL.md) lists each
download. Games need a Vulkan 1.3 driver, and keys and games must be dumped from your own console.

Started with no arguments, ScarletNX opens its [launcher](docs/GUI.md): **Install Keys** installs your `prod.keys`, and
**Load Switch NSP/XCI/NRO** picks a dump, recompiles it and starts the game. The command line does the same with
`scarletnx "Game.xci"` ([Usage](docs/USAGE.md)). To build it yourself, see [Build](docs/BUILD.md).

## Documentation

- [Install](docs/INSTALL.md): the downloads for each system, keys, and the first start
- [GUI launcher](docs/GUI.md): the menu, its settings, themes and languages
- [Usage](docs/USAGE.md): extract, recompile and play from the command line; updates, mods, cheats, folders and users
- [Settings](docs/SETTINGS.md): every key of a game's `.ini` and of the launcher, with its default
- [Testing and reporting](docs/TESTING.md): how to test a build, and the form a report takes
- [Build](docs/BUILD.md): prerequisites and builds on Linux, FreeBSD, macOS and Windows
- [Development](docs/DEVELOPMENT.md): checks, diagnostics and profiling; [Contributing](CONTRIBUTING.md)
- [Architecture](docs/ARCHITECTURE.md) and [the recompiler as a library](docs/LIBRARY.md)
- Platforms: [Android](docs/ANDROID.md), [macOS and iOS](docs/APPLE.md), [PowerPC64](docs/PPC64.md),
  [RISC-V and LoongArch](docs/RISCV-LOONGARCH.md), [MIPS64](docs/MIPS64.md)
- Other systems: [3DS](docs/3DS.md), [PS Vita](docs/VITA.md), [Game Boy Advance](docs/AGB.md),
  [Linux AArch64 programs](docs/GNUABI.md)
- [Every document](docs/README.md), by subject

## License and credits

ScarletNX is [GPL-3.0-or-later](LICENSE). Code ported from other projects keeps its own license and copyright lines:
[Licenses and credits](docs/CREDITS.md) lists each, and the projects ScarletNX thanks.
