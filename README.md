# ScarletNX
<p align="center">
  <img src="./dist/wimsy.svg" alt="Wimsy, the mascot" width="200"><br>
  <b>Wimsy, the ScarletNX mascot</b>
</p>

Discord: https://discord.gg/tRTRXXbyjp (my god why don't we just use IRC like good people?)

A GPLv3 static ARM64 to x86 recompiler with a Switch translation layer for Linux, Windows, FreeBSD and Android.

Forked from MZLab's SwitchRecomp. SwitchRecomp is based off Nexium.

## Quick start

Each push to `main` that builds everywhere becomes a nightly in [scarletnx/nightly](https://github.com/scarletnx/nightly/releases), the newest 30
kept, with each open pull request's builds. The latest:
[ScarletNX-linux-x86_64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-x86_64.AppImage) and
[ScarletNX-linux-aarch64.AppImage](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-linux-aarch64.AppImage) for Linux (`chmod +x` it first),
[ScarletNX-windows-x86_64.zip](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-windows-x86_64.zip) for Windows (run `bin\scarletnx.exe`)
and [ScarletNX-android-universal.apk](https://github.com/scarletnx/nightly/releases/latest/download/ScarletNX-android-universal.apk) ([Android](docs/ANDROID.md)).
Games need a Vulkan 1.3 driver.

Keys and games must be dumped from your own console. Started with no arguments, the program opens the
[launcher](docs/GUI.md). **Setup device keys** installs your `prod.keys`. For NSPs, also put `title.keys` in
`~/.switch` (`%USERPROFILE%\.switch` on Windows). **Load Switch NSP/XCI/NRO** then picks a dump, extracts and
recompiles it and starts the game, which the **Games** tab lists from then on. See [Usage](docs/USAGE.md) for the
command line and [Folders](docs/USAGE.md#folders) for where games, saves and settings go.

The shared ARM64 recompiler lives under `src/common/`. Switch Horizon HLE
and Maxwell graphics live under `src/nx/`. The CPU-only Linux AArch64 runtime lives
under `src/gnuabi/` and builds without the Switch graphics or media dependencies:

```sh
cmake -S . -B build/gnuabi -DSNX_BUILD_NX=OFF
cmake --build build/gnuabi
build/gnuabi/scarletnx-gnuabi -L /path/to/aarch64-sysroot ./test_aarch64
```

It runs static and dynamically linked programs, which start other processes, talk
through pipes and read and write files through a sysroot, as QEMU's `-L` gives one
(`linux/` of the data folder by default). See [Linux ABI execution and QEMU validation](docs/GNUABI.md)
for the sysroot, the JIT and AOT modes, supported syscalls, and current limits. The NX executable also
has `scarletnx-erista`, `scarletnx-mariko`, and `scarletnx-aula` aliases.

The recompiler is also a library on its own, `libscarletnx-recompiler`: a program hands
it ARM64 or AArch32 code in memory and runs the result in its own process with its own
SVC handler, with no Switch runtime behind it. See
[The recompiler as a library](docs/LIBRARY.md).

Install the build tools first. On Debian/Ubuntu:
```bash
sudo apt update
sudo apt install build-essential git cmake ninja-build clang llvm-dev nasm pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev
```
If your distribution provides `libsdl3-dev`, install it too. Otherwise CMake builds a pinned SDL3
using the X11 and audio development packages above. SDL2 is not a substitute for SDL3. You also
need a Vulkan 1.3 driver to run games. See [Build prerequisites](./docs/BUILD.md#prerequisites) for
Wayland, other systems, and the SDL3 troubleshooting steps.

Then build:
```bash
git clone --depth=1 https://github.com/scarletnx/ScarletNX
cd ScarletNX
cmake --preset default -B build && cmake --build build
```

For encrypted dumps, put `prod.keys` (and `title.keys`, for NSPs) in `~/.switch`, or pass them with
`--keys` and `--title-keys`. Decrypted DNSP/DXCI content needs no keys.
```bash
./build/scarletnx "Game.xci"  # extract, recompile, then play (also accepts NSP, DNSP and DXCI)
```
Run the same command again after an interruption. Completed extraction and recompilation are skipped,
but the game always starts anew. The driver keeps games, extractions and keys in its data folder,
`~/.local/share/scarletnx` on Linux ([Folders](./docs/USAGE.md#folders)). To run the steps separately, here in
folders of the current one:
```bash
./build/scarletnx -x "Game.xci" --out work/Game     # -> work/Game/{exefs,romfs}
./build/scarletnx -r work/Game Game --outdir games  # -> games/Game/Game
./build/scarletnx -rx "Game.xci"                   # prepare without playing (UNIX)
./build/scarletnx -Hs "Game.xci"                   # headless, wait for GDB (UNIX)
cd games/Game && ./Game
```

For an SDL3 menu, run `./build/scarletnx` with no arguments. Select a driver with arrow keys and
Enter or hover and click, then choose drag-and-drop or the paginated `games/` browser.
The GUI shows live driver/game
output and combined CPU/RAM usage. See [GUI launcher](docs/GUI.md).

The experimental `scarletnx-3ds` driver and **Load 3DS CIA** menu entry support
decrypted application CIA extraction (`scarletnx-3ds -x game.cia`) and an independent
page-based memory controller. `SNX_3DS_AOT=ON` additionally enables an experimental
**extract -> offline AOT -> native run** pipeline on native 64-bit Linux, using an
explicit ARM11/checked-memory profile without changing shared A57 defaults.
Larger images now use bounded chunked static CFG compilation (16 MiB text cap).
SuperTux now completes minimal APT startup with real synchronization/thread
resources, initializes HID handles and **reaches and executes main**. It then
opens its reconstructed RomFS using NX's shared filesystem backing, executes
scalar VFP in `gettimeofday`, opens its SelfNCCH archive and stopped at FS's
archive timestamp request — real timestamp semantics and a minimal Vulkan-only
GSP/GPU display path (live SDL3 window + PPM frames) now exist.
**No first frame, audio or gameplay is claimed.**
Checked CTR IPC uses a separate SwIPC mode; ARM11 exclusives and serialized
native thread scheduling work. Full VFPv2, cache/resume, HLE and devices remain incomplete. The default
build still stops after extraction.
See the [staged 3DS port](docs/3DS.md) for limits, build instructions and remaining work.

For more detailed information please see: [Build + run](./docs/BUILD.md). For the Android app (arm64 and x86_64),
see [Android](./docs/ANDROID.md). macOS and iOS build in CI, but no game runs on them yet: see
[macOS and iOS](./docs/APPLE.md). For an error a game or the driver prints, see
[Troubleshooting](./docs/TROUBLESHOOTING.md). Every document: [docs](./docs/README.md).

# Special thanks

- yuzu, Suyu, Torzu, Ryujinx, [Ryubing](https://git.ryujinx.app/ryubing/ryujinx) (Ryujinx's continuation), [Citron](https://git.citron-emu.org/citron/emulator) and [Eden](https://git.eden-emu.dev/eden-emu/eden).
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
- LibHac.
- [Nexium](https://nexium-emu.org/) for research and development.
- [Switchbrew](https://switchbrew.org) for research.
- [Azahar](https://github.com/azahar-emu/azahar) and Citra, for all of the 3DS emulation.

Wimsy, the mascot and the runtime's icon (`dist/wimsy.svg`) was drawn by Liz.
