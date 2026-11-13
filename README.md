# ScarletNX
<p align="center">
  <img src="./dist/wimsy.svg" alt="Wimsy, the mascot" width="200"><br>
  <b>Wimsy, the ScarletNX mascot</b>
</p>

Discord: https://discord.gg/tRTRXXbyjp (my god why don't we just use IRC like good people?)

A GPLv3 static ARM64 to x86 recompiler with a Switch, GBA and GNU/Linux ABI translation layer for Solaris, Linux, Windows, FreeBSD and Android.

A game from your own dump is recompiled ahead of time into a native module for the machine it plays on, and runs on a
runtime that answers the console's system calls and services and draws with Vulkan. Experimental drivers run 3DS games,
DS games and PS Vita homebrew the same way, and Game Boy Advance ROMs on an emulated AGB board still under
construction.

## Download and run

The newest build of `main` for Linux (AppImage), Windows and Android is in
[scarletnx/nightly](https://github.com/scarletnx/nightly/releases/latest); [Install](docs/user/install.md) lists each
download. Games need a Vulkan 1.3 driver, and keys and games must be dumped from your own console.

Started with no arguments, ScarletNX opens its [launcher](docs/user/gui.md): **Install Keys** installs your `prod.keys` for
Switch and your `aes_keys.txt` for the encrypted 3DS games, to `~/.switch` and `~/.3ds` (`%USERPROFILE%` on Windows),
and **Load Switch NSP/XCI/NRO** picks a dump, recompiles it and starts the game. The command line does the same with
`scarletnx "Game.xci"` ([Usage](docs/user/usage.md)). To build it yourself, see [Build](docs/dev/build.md).

## Documentation

- [Install](docs/user/install.md): the downloads for each system, keys, and the first start
- [GUI launcher](docs/user/gui.md): the menu, its settings, themes and languages
- [Usage](docs/user/usage.md): extract, recompile and play from the command line, with updates, mods, cheats,
  folders and users
- [Settings](docs/user/settings.md): every key of a game's `.ini` and of the launcher, with its default
- [Testing and reporting](docs/user/testing.md): how to test a build, and the form a report takes
- [Build](docs/dev/build.md): prerequisites and builds on Linux, FreeBSD, macOS and Windows
- [Development](docs/dev/development.md): checks, diagnostics and profiling; [Contributing](CONTRIBUTING.md)
- [Architecture](docs/dev/architecture.md) and [the recompiler as a library](docs/dev/library.md)
- Hosts: [the Android app](docs/hosts/android-app.md), [macOS and iOS](docs/hosts/apple.md),
  [PowerPC64](docs/hosts/ppc64.md), [RISC-V and LoongArch](docs/hosts/riscv-loongarch.md),
  [MIPS64](docs/hosts/mips64.md), [s390x](docs/hosts/s390x.md), [NetBSD](docs/hosts/netbsd.md)
- Other systems: [3DS](docs/platforms/3ds/README.md), [PS Vita](docs/platforms/vita/README.md),
  [Game Boy Advance](docs/platforms/agb/README.md), [DS](docs/platforms/nds/README.md),
  [Linux AArch64 programs](docs/platforms/gnuabi/README.md),
  [Android apps](docs/platforms/android-apps/README.md) (**Load Android APK/XAPK**)
- [Every document](docs/README.md), by audience, platform and layer

## License and credits

ScarletNX is [GPL-3.0-or-later](LICENSE). Code ported from other projects keeps its own license and copyright lines:
[Licenses and credits](docs/CREDITS.md) lists each, and the projects ScarletNX thanks.
