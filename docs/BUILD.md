<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Build

## Index

* [Prerequisites](#prerequisites)
* [Linux / FreeBSD](#linux--freebsd)
  * [Debian / Ubuntu](#debian--ubuntu)
  * [Fedora](#fedora)
  * [Arch Linux](#arch-linux)
  * [FreeBSD](#freebsd)
  * [Configure and build](#configure-and-build)
  * [SDL3 troubleshooting](#sdl3-troubleshooting)
* [macOS](#macos)
* [Windows / PowerShell](#windows--powershell)
* [Windows / MSYS2 CLANG64](#windows--msys2-clang64)
* [Rebuild, deploy, cache](#rebuild-deploy-cache)
* [Build variants](#build-variants)
* [Install / AppImage](#install--appimage)

Run from checkout root. [Extract/AOT/run](USAGE.md) · [Tests/diagnostics](DEVELOPMENT.md).

## Prerequisites

- **Linux / FreeBSD:** Clang (CI: 23; FreeBSD's system Clang also works), LLVM's development files
  (`llvm-config`, required by the recompiler), CMake ≥3.25, Ninja, POSIX shell, GNU make (`gmake` on FreeBSD),
  NASM on x86-64, Perl, Python 3 (the menu's languages), Git, pkg-config and `patch`.
  Use a system SDL3 development package, or X11/Wayland and audio development libraries for the bundled
  SDL3 build.
- **macOS:** Apple silicon, macOS 13 or newer, Xcode (its Clang and SDKs), CMake ≥3.25, Ninja, Python 3 and Git.
  LLVM's libraries come from `scripts/apple-llvm.sh` or Homebrew ([steps](#macos)).
- **Windows:** Win10/11 x64. CI builds in MSYS2's CLANG64 environment, which has LLVM's development files
  ([steps](#windows--msys2-clang64)). Bootstrap stages SHA-256-checked llvm-mingw/Clang, CMake, Ninja,
  NASM, Strawberry Perl and w64devkit in `deps/tools`. llvm-mingw has no LLVM libraries, so that route also needs a
  MinGW LLVM's `llvm-config` set with `-DSNX_LLVM_CONFIG=/path/to/llvm-config`. CMake fails without it.
- **Display:** a Vulkan 1.3 driver. Headless runs use Vulkan offscreen and skip SDL video initialization.
  SDL3 remains a required build dependency.
- **FFmpeg:** required by every NX runtime build. CMake builds the pinned source release.
  Avoid spaces in checkout/cache paths.
- **OpenSSL:** required by every build with the NX or 3DS loaders (hashes, ciphers, the ssl service's TLS). CMake
  builds the pinned source release with OpenSSL's Configure, which needs **Perl**.

## Linux / FreeBSD

Install dependencies before configuring. The default preset uses **Ninja** and **Clang/Clang++**.
FFmpeg also uses **GNU make** and, on x86-64, **NASM**, and OpenSSL uses **Perl** and GNU make. These tools are not
downloaded by CMake.
The build patches its copy of FFmpeg with `patch`, which `build-essential`, `base-devel` and FreeBSD's base
system include; Fedora lists it below.

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential git cmake ninja-build clang llvm-dev nasm pkg-config python3 \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev
```

This includes the X11 and audio development libraries for the SDL3 source fallback. Without the audio
ones, SDL3 builds with no sound output and games run silent. If your release provides `libsdl3-dev`,
install it to use the distribution's SDL3 instead:

```sh
sudo apt install libsdl3-dev
```

If apt cannot find that package, skip it. ScarletNX builds the pinned SDL3 automatically.
Installing `libsdl2-dev` does not satisfy the SDL3 dependency.

For native Wayland support in the source fallback, also install:

```sh
sudo apt install libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev libegl-dev
```

Without `libegl-dev` SDL3 quietly leaves Wayland out and builds X11 only. The `SDL_WAYLAND` line of
its configure summary reads `(Wanted: ON): ON` when Wayland is in.

CMake needs version 3.25 or newer, and Clang must support C23/C++23. Upgrade these tools if your
release ships older versions. Install your GPU's Vulkan driver separately. For Intel/AMD or lavapipe
on Debian/Ubuntu, the usual packages are `libvulkan1 mesa-vulkan-drivers`. NVIDIA users need a
Vulkan-capable NVIDIA driver.

### Fedora

```sh
sudo dnf install clang cmake ninja-build nasm make perl git pkgconf-pkg-config patch llvm-devel python3 \
  libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel \
  libXScrnSaver-devel libXtst-devel alsa-lib-devel pulseaudio-libs-devel pipewire-devel
```

These are the Debian/Ubuntu packages above under Fedora's names. `llvm-devel` (`llvm-dev` on
Debian/Ubuntu) is the recompiler's code generator, which the configure requires. Fedora 44
provides SDL3 3.4; install `SDL3-devel` to use it instead of the source fallback:

```sh
sudo dnf install SDL3-devel
```

For native Wayland support in the source fallback, also install:

```sh
sudo dnf install wayland-devel libxkbcommon-devel wayland-protocols-devel libdecor-devel libglvnd-devel
```

`libglvnd-devel` holds the EGL headers. For Intel/AMD or lavapipe, the usual Vulkan packages are
`vulkan-loader mesa-vulkan-drivers`.

### Arch Linux

```sh
sudo pacman -Syu --needed base-devel git cmake ninja clang llvm nasm perl pkgconf python sdl3
```

### FreeBSD

FreeBSD provides Clang in the base system. Install the remaining tools, the SDL3 package, and libusb, which SDL's
HIDAPI loads at run time to read controllers (an Xbox pad is only read that way):

```sh
sudo pkg install git cmake ninja gmake nasm perl5 pkgconf python3 sdl3 llvm libusb
```

The build uses that `sdl3` package by default (`SNX_SYSTEM_SDL3`, see [SDL3 troubleshooting](#sdl3-troubleshooting));
pass `-DSNX_SYSTEM_SDL3=OFF` for CPM's pinned SDL3 source build instead.

### Configure and build

```sh
git clone https://github.com/scarletnx/ScarletNX.git && cd ScarletNX &&
cmake --preset default -B build && cmake --build build
# -> build/{scarletnx,snx-shaderc}
```

### SDL3 troubleshooting

On all platforms, CPM first looks for a compatible SDL3 development package (3.4.18 or newer) through
`SDL3Config.cmake`. If none is found, CPM downloads and builds the SHA-256-pinned SDL3. The fallback is static, so no separate SDL3 runtime
installation is needed. It still needs your platform's window-system development libraries.

- `SNX_SYSTEM_SDL3=ON` (the default on FreeBSD) takes the system's package from `find_package` whatever its version,
  needing only 3.4.16, the version every SDL call in the tree is in, and never downloads. FreeBSD's ports `sdl3`
  (3.4.16) is older than the pin, so without it CPM's version check builds SDL3 from source. `SNX_SYSTEM_SDL3=OFF`
  keeps the pinned build everywhere.

- An old checkout showing `Could not find a package configuration file provided by SDL3` needs either
  the SDL3 **development** package or this updated fallback. Pull the latest checkout and configure again.
- For SDL3 installed in a custom prefix, pass `-DCMAKE_PREFIX_PATH=/path/to/prefix`, or
  `-DSDL3_DIR=/path/to/prefix/lib/cmake/SDL3` (the directory containing `SDL3Config.cmake`).
- To force the source build, pass `-DCPM_DOWNLOAD_SDL3=ON`. This is CPM 0.43.2's per-package override.
  That version does not support `CPM_FORCE_BUNDLED_SDL3`. No bundled override is forced by ScarletNX.
  The source build is recommended on Windows to match the MinGW toolchain and keep SDL3 statically linked.
- If SDL3's configure reports missing X11/Wayland development libraries, install the packages above.
  Do not work around that by disabling both display backends for a windowed build.
- No sound, and the log says `audio: SDL could not start its audio: No available audio device`: the
  SDL3 fallback was configured without audio development libraries (its configure warns `Unable to find
  the alsa development library`). Install the audio packages above and configure again.
- SDL3 is mandatory for the NX runtime, including headless runs. Use `-H` at runtime to skip
  SDL video initialization and run without a display server. The separate headless backend was removed.
  For an old build directory, remove the cached backend selection with `-USNX_BACKEND`.
- If a controller does not appear, check the game's `input: joystick` lines. A GUID with no gamepad mapping needs
  an SDL mapping in `gamecontrollerdb.txt`, as described in [USAGE](USAGE.md). If no joystick appears on Linux,
  check whether `/dev/input/event*` is readable and whether udev marks its event node `ID_INPUT_JOYSTICK=1`.
  The supplied `dist/72-scarletnx-input.rules` grants seat access to recognized joystick event nodes and to the
  hidraw nodes of the console's controllers. Other HIDAPI controllers may need their vendor's own udev rule. On
  FreeBSD the gamepad comes from HIDAPI over libusb or `/dev/uhid*`, never from `/dev/input/event*`: a pad `xb360gp(4)`,
  `hgame(4)` or `webcamd` claimed is invisible to SDL. See [USAGE](USAGE.md) for the access rules and the packages. If
  the gamepad opens but sends no input, compare the log's controller name and GUID with its mapping and test its
  event node with `evtest`.

After installing dependencies, rerun:

```sh
cmake --preset default -B build && cmake --build build
```

If that directory was configured with a different compiler or generator, use a new build directory
(for example `-B build/clang`) rather than mixing the old cache with the new toolchain.

## macOS

Apple silicon only, with Xcode's Clang. The preset is `macos`, and the Linux presets and `default` do not apply.
Install Xcode, then the build tools:

```sh
xcode-select --install
brew install cmake ninja
```

1. **LLVM's libraries**, once (about 10 minutes on an M1). The script builds the AArch64 code generator alone,
   which is the one the recompiler uses:

   ```sh
   scripts/apple-llvm.sh macos ~/apple-llvm/macos
   ```

   Homebrew's LLVM also works (`brew install llvm`). Pass `-DSNX_LLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config"`
   to the configure step below in place of `-DLLVM_DIR`.

2. **Configure and build:**

   ```sh
   cmake --preset macos -DLLVM_DIR=$HOME/apple-llvm/macos/lib/cmake/llvm
   cmake --build --preset macos
   # -> build/macos/{scarletnx,snx-shaderc}
   ```

   `build/macos/scarletnx` runs from the terminal as on Linux ([USAGE](USAGE.md)). It loads the system's Vulkan
   loader, so it needs Homebrew's `molten-vk` and `vulkan-loader`.

3. **The app** (optional), for a double-clickable `ScarletNX.app` that carries its own MoltenVK:

   ```sh
   cmake build/macos -DSNX_MOLTENVK=<moltenvk>
   cpack --config build/macos/CPackConfig.cmake -B out   # out/ScarletNX-macos-arm64.zip
   ```

   `<moltenvk>` is the extracted `MoltenVK-macos.tar` of the
   [Ryujinx MoltenVK release](https://github.com/V380-Ori/Ryujinx.MoltenVK/releases) that
   `.github/workflows/build.yml` pins (`MOLTENVK_RELEASE`, with its SHA-256). **Do not leave it out.** Without it the
   app has no MoltenVK, and on a Mac without Homebrew's it stops at startup with
   `SDL could not open a Vulkan window: Failed to load Vulkan Portability library`. A game's folder gets the library
   beside its copy of the runtime (`snx_deploy_runtime`), which is what the game's own process loads.

CI's `macOS and iOS (arm64)` job builds the app on every push, and its zip, `ScarletNX-macos-arm64-<commit>.zip`,
already has MoltenVK. The app is signed ad hoc, so a downloaded copy may need *Open* from the Finder's menu the first
time (`xattr -cr ScarletNX.app` clears the quarantine). More on the frontend, the data folder, iOS and what is missing
in [macOS and iOS](APPLE.md).

## Windows / PowerShell

```powershell
& {
    $ErrorActionPreference = 'Stop'
    git clone https://github.com/scarletnx/ScarletNX.git
    if ($LASTEXITCODE) { throw 'Clone failed' }
    Set-Location ScarletNX
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
    if ($LASTEXITCODE) { throw 'Bootstrap failed' }
    Set-ExecutionPolicy -Scope Process Bypass
    . .\scripts\env.ps1
    cmake --preset default -B build
    if ($LASTEXITCODE) { throw 'Configure failed' }
    cmake --build build
    if ($LASTEXITCODE) { throw 'Build failed' }
}
# -> build\{scarletnx.exe,snx-shaderc.exe}
```

Each new shell, from the checkout:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git and Windows.

## Windows / MSYS2 CLANG64

The `windows` job in [build.yml](../.github/workflows/build.yml) builds this way. Clang, LLVM and the FFmpeg and
OpenSSL build tools come from MSYS2, not from `bootstrap.ps1`. Keep `env.ps1` out of this shell: llvm-mingw's C++
runtime must not mix with another LLVM build.

1. Install MSYS2 from [msys2.org](https://www.msys2.org/) and open the **MSYS2 CLANG64** shell. Run every step below
   in it.
2. Update MSYS2, then install CI's packages. If the update closes the shell, reopen it and run `pacman -Syu` again.

   ```sh
   pacman -Syu
   pacman -S --needed git make patch perl patchutils \
     mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-llvm mingw-w64-clang-x86_64-lld \
     mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-nasm \
     mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-python mingw-w64-clang-x86_64-spirv-tools \
     mingw-w64-clang-x86_64-glslang
   ```

   Python builds the menu's languages; spirv-tools is only for the `shader_direct` test. CI also installs ccache, which
   isn't needed here.
3. Clone to a path without spaces:

   ```sh
   git clone --depth=1 https://github.com/scarletnx/ScarletNX.git
   cd ScarletNX
   ```

4. Configure and build. These are CI's options without its ccache and `-DSNX_DEPLOY=OFF` ones. They give
   CMake the MSYS2 tools as Windows paths and build the pinned SDL3.

   ```sh
   cmake --preset default \
     -DSNX_LLVM_CONFIG="$(cygpath -m "$(command -v llvm-config)")" \
     -DSNX_FFMPEG_SH="$(cygpath -m /usr/bin/sh.exe)" \
     -DSNX_FFMPEG_MAKE="$(cygpath -m /usr/bin/make.exe)" \
     -DPERL_EXECUTABLE="$(cygpath -m /usr/bin/perl.exe)" \
     -DSNX_OPENSSL_MAKE="$(cygpath -m /usr/bin/make.exe)" \
     -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON
   cmake --build --preset default
   ```

5. The executables land in `build/default`. CI checks them with:

   ```sh
   build/default/scarletnx.exe --help
   ctest --test-dir build/default --output-on-failure
   ```

6. Run `build/default/scarletnx.exe` from the CLANG64 shell, which has the DLLs it imports on PATH
   ([USAGE](USAGE.md)). For a copy that runs without MSYS2, `cmake --install build/default --prefix stage` puts
   `scarletnx.exe` in `stage/bin`. Then copy the DLLs it imports from `/clang64/bin`, as CI's
   "Bundle runtime DLLs and licenses" step does.

## Rebuild, deploy, cache

```sh
cmake --build build
```

Default: Ninja + Clang, `RelWithDebInfo` (`-O2 -g`). Runtime relinks deploy `recompiled/Game/Game[.exe]` beside
matching `Game_code.so/.dll`, in the data folder's `recompiled/` ([USAGE](USAGE.md#folders)); no-op builds skip deployment. Recompiler/ABI edits require [module rebuilds](DEVELOPMENT.md).

CPM uses [pinned sources](../cmake/dependencies.cmake) and a shared `.cache/cpm` cache. Archive downloads are
checked by SHA-256. Offline configuration needs the matching sources already in the cache. Cached sources stay unmodified: FFmpeg's patch goes on a copy in the build tree.
With tests on (`BUILD_TESTING`, the default) and the runtime, libvpx's VP9 test vectors download there too ([tests](../tests/CMakeLists.txt)).
The Switch runtime's shared font stand-ins download only when it is built ([SHARED_FONTS.md](SHARED_FONTS.md)).
Override via CMake/env `CPM_SOURCE_CACHE`. SDL3 uses CPM's system-package discovery with a pinned static
source fallback. Use `-DCPM_DOWNLOAD_SDL3=ON` to request the bundled source build.

FFmpeg, OpenSSL and jemalloc build with their own configure and make, in each build tree. With `SNX_DEPS_CACHE` (CMake
or env) naming a folder, a tree reuses a finished build there whose inputs match (source, configure line, compiler, C
headers and the cmake file that builds it, see [deps_cache.cmake](../cmake/deps_cache.cmake)) and publishes the ones
it builds, so other trees and branches skip them. A change to any input is a new folder; old ones are only left behind.

## Build variants

SDL3, Vulkan and FFmpeg are mandatory for the NX runtime, including when running with `-H`.
There are no renderer or media-decoding disable options. Tools-only and GNUABI-only builds do not need them.

Persistent per-build-dir options (POSIX examples):

```sh
cmake --preset default -B build -DSNX_FFMPEG_ASM=OFF && cmake --build build
```

- `-DSNX_FFMPEG_ASM=OFF`: omit x86-64 NASM assembly; slower FFmpeg decoding.
- `-DCPM_DOWNLOAD_SDL3=ON`: build the pinned SDL3 instead of using a system SDL3 development package.
- `-DSNX_DEPLOY=OFF`: disable runtime deployment.
- `-DSNX_JEMALLOC=OFF`: keep the C library's malloc in `scarletnx` instead of the pinned jemalloc (default ON on Linux
  and FreeBSD, OFF on macOS, where `ON` takes it too; refused elsewhere and with `SNX_TEST_SANITIZERS`). A recompile's
  LLVM threads spend less time in malloc with jemalloc.
- `-DSNX_TRACY=ON`: build the runtime with the pinned Tracy profiler client (default OFF). For development builds
  only; not available on iOS. It turns `SNX_DEPLOY` off, since the profiling runtime must not be copied over a
  game; the `tracy` preset sets both. See [profiling](DEVELOPMENT.md#profiling).
- `-DSNX_RECOMPILED_DIR=/path`: override deployment dir (default: empty, the data folder's `recompiled/` the build
  tree's driver uses). `SNX_GAMES_DIR`, its name before, is still taken while it is empty, said as it configures. A
  tree whose value is the checkout's `games/`, the default before, is set back to empty.
- `-DCPM_SOURCE_CACHE=/path`: override source-download cache.
- `-DSNX_DEPS_CACHE=/path`: reuse and publish finished FFmpeg, OpenSSL and jemalloc builds there (default: the
  environment's `SNX_DEPS_CACHE`, else none).
- `-DSNX_BUILD_NX=OFF`: omit the Switch runtime (default ON).
- `-DSNX_BUILD_GNUABI=OFF`: omit the CPU-only Linux AArch64 runtime, see [GNUABI.md](GNUABI.md) (default ON).
- `-DSNX_BUILD_3DS=OFF`: omit the 3DS layer, see [3DS.md](3DS.md) (default ON).
- `-DSNX_BUILD_GUI=OFF`: omit the SDL3 launcher. The default is ON when `SNX_BUILD_NX` is on and `SNX_TOOLS_ONLY`
  is not, OFF otherwise.
- `-DSNX_TOOLS_ONLY=ON`: build the extraction/recompilation driver without the runtime, see below (default OFF).
- `-DSNX_RECOMPILER_SHARED=ON`, `-DSNX_RECOMPILER_INSTALL=ON`: also build the recompiler as a shared library, and
  install the library, see [LIBRARY.md](LIBRARY.md) (defaults OFF and the value of `SNX_TOOLS_ONLY`).
- `-DSNX_BUILD_TESTING=OFF`: leave the tests out whatever `BUILD_TESTING` says. ON by default when ScarletNX is the
  top-level project, OFF when another project adds it with CPM or `add_subdirectory` ([LIBRARY.md](LIBRARY.md)).

Test options, read when `BUILD_TESTING` is on (CTest's default), all OFF by default:

- `-DSNX_TEST_SANITIZERS=ON`: build the native tests, and the programs and libraries of the repository they run or
  link, with ASan and UBSan (`runtime_architecture` names one left out). The runtime is one, so it is not deployed.
  CI's `Linux (x86_64, ASan + UBSan)` job runs the tests this way, with `LSAN_OPTIONS=use_stacks=0:use_registers=0`
  so the leak check finds the same leaks on every machine.
- `-DSNX_TEST_VULKAN=ON`: add the offscreen Vulkan integration tests, see below.
- `-DSNX_TEST_VP9_VECTORS=ON`: add the `nvdec_vp9_*` tests, which run NVDEC's VP9 jobs on libvpx's test vectors;
  configure downloads the 32 files from the WebM project into `CPM_SOURCE_CACHE` once. CI's Linux, ASan, Windows and
  macOS jobs run them.
- `-DSNX_TEST_A64_FP_DIFF=ON`: add the AArch64 FP differential test, which needs Python unicorn 2.1.4 and a native
  Linux host.
- `-DSNX_TEST_ARM_AOT=ON`, `-DSNX_TEST_ARM_DIFF=ON`: the 3DS ARM11 witnesses and the A32 unicorn comparison, see
  [3DS.md](3DS.md).

```sh
# Build without the GUI launcher; SDL3 remains required by the runtime:
cmake --preset default -B build/headless-vk -DSNX_BUILD_GUI=OFF -DSNX_DEPLOY=OFF &&
cmake --build build/headless-vk
# Run Vulkan without a display server, a surface or a swapchain:
./build/headless-vk/scarletnx "Game.xci" -H

cmake --preset windows-cross && cmake --build --preset windows-cross
# -> build/windows-cross/scarletnx.exe; requires llvm-mingw on PATH
```

All NX builds use SDL3 and support `-H` at runtime. Omit the flag to run with a window.
[Software Vulkan / lavapipe setup](USAGE.md#headless-vulkan--lavapipe).

Offscreen integration test, with an installed Vulkan 1.3 ICD (lavapipe works):

```sh
cmake --preset default -B build/headless-vk -DSNX_BUILD_GUI=OFF \
  -DSNX_DEPLOY=OFF -DSNX_TEST_VULKAN=ON && cmake --build build/headless-vk
# Set VK_DRIVER_FILES to your lavapipe manifest to force CPU rendering.
ctest --test-dir build/headless-vk -R vulkan_headless --output-on-failure
# the same with the validation layer
(cd build/headless-vk && src/nx/snx-test-vulkan -H --vk_validation=1)
```

The test uses synthetic pixels, not a game. It checks offscreen submissions, frame recycling, cropping,
readback hashes and PPM contents without WSI. Validation requires the Khronos validation layer to be installed.
`SNX_TEST_VULKAN` is off by default so ordinary unit tests do not require an ICD.

A cross-built driver extracts too: hactool is part of it. The recompiler still requires LLVM development
files for the target. `SNX_LLVM_CONFIG` must run on the build machine and describe that target installation,
not the build machine's LLVM libraries. Running the `snx-rtlib-gen` build helper also requires a suitable
`CMAKE_CROSSCOMPILING_EMULATOR`.

Host tools only, without SDL/Vulkan/FFmpeg or Python:

```sh
cmake --preset default -B build/tools -DSNX_TOOLS_ONLY=ON && cmake --build build/tools
build/tools/scarletnx -r -h
ctest --test-dir build/tools --output-on-failure
```

The recompiler generates game code with LLVM, its only backend. LLVM's
development files are required: `llvm-config` finds them (`-DSNX_LLVM_CONFIG=/path/to/llvm-config` picks one), and
the configure stops without them on every platform, including Windows and cross-builds. There is no recompiler
build without LLVM. Modules are built for the machine that builds them, with its instruction set
extensions (build == host == target); nothing is detected at run time. Whole functions are batched into units of 90000
instructions (20000 on Android; `--chunk`), with a progress line per compiled unit.

Extraction needs nothing beside the driver: hactool runs inside it. Recompilation deploys the
running full runtime; the tools-only build creates one in its checkout's `build/native-recomp`, or accepts
`--runtime FILE`. A build tree's programs work in the same data folder as an installed copy's: the user's
(`~/.local/share/scarletnx`, `%APPDATA%\scarletnx` on Windows), and on Linux the XDG config, state and cache folders
hold its settings, logs and caches ([USAGE](USAGE.md#folders)). `--data DIR` before the command names another data
folder, `--build-dir DIR` another build tree.
The menu (`SNX_BUILD_GUI`) needs Python 3 to build its languages (`scripts/build-languages.py`,
[GUI](GUI.md#languages)). Python ≥3.12 is optional for `tools/*.py` diagnostics.

## Install / AppImage

```sh
cmake --install build --prefix stage
# -> stage/bin/, stage/share/licenses/scarletnx/
```

AppImage: run inside pkgforge's Arch container with `quick-sharun` ([CI setup](../.github/workflows/build.yml)), as
[Anylinux-AppImages](https://github.com/pkgforge-dev/Anylinux-AppImages) makes them; its
[FAQ](https://github.com/pkgforge-dev/Anylinux-AppImages/blob/main/FAQ.md) is the reference, not the official AppImage
documentation:

```sh
scripts/linux-llvm.sh "$(uname -m)" ~/linux-llvm &&
cmake --preset default -B build/appimage -DSNX_DEPLOY=OFF -DCMAKE_INSTALL_PREFIX=/usr \
  -DLLVM_DIR="$HOME/linux-llvm/lib/cmake/llvm" -DSNX_A64_DISASM=OFF &&
cmake --build build/appimage &&
cmake --install build/appimage &&
GITHUB_REPOSITORY=scarletnx/ScarletNX scripts/make-appimage.sh
# -> dist/ScarletNX-linux-<arch>.AppImage
```

The AppImage links LLVM statically: `scripts/linux-llvm.sh ARCH PREFIX` builds the libraries the recompiler links and
nothing else, with one code generator, the host's (a distribution's libLLVM is one library with every target). It
takes the version in `LLVM_VERSION` (CI gives it the container's, whose `opt` prints the LLVM settings' pipelines),
and with `SNX_SYSROOT` set it cross-builds for another architecture. `SNX_A64_DISASM=OFF` leaves out `scarletnx
--mcp`'s A64 disassembler, which is for working on the runtime.

`scarletnx` is every driver the build has (`src/multicall`): each driver is a library with no `main` of its own,
and the program, whichever drivers are on (`SNX_BUILD_NX` among them), runs the one its name says, so LLVM, SDL and
the code the drivers share are in it once. `scarletnx-switch`, `scarletnx-3ds`, `scarletnx-vita`, `scarletnx-gnuabi`
and `scarletnx-agb` are links to it on Linux and macOS, made in the build folder and installed. Windows, whose zips
keep no links, installs `scarletnx.exe` alone, which takes a driver's name as its first argument
(`scarletnx.exe scarletnx-3ds ...`, how the launcher runs them); its build folder has hard links of those names, for
the tests. Android's and iOS's apps start the program their own way.

[Nightly AppImages](https://github.com/scarletnx/nightly/releases/latest), x86_64 and aarch64 · [Launch](USAGE.md).

A pull request's own builds are linked from a comment on it once CI has built them.

[Licenses/credits](CREDITS.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md).
