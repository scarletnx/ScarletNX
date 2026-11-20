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
* [Third-party packages](#third-party-packages)
* [Build variants](#build-variants)
* [Install / AppImage](#install--appimage)

Run from checkout root. [Extract/AOT/run](../user/usage.md) · [Tests/diagnostics](development.md).

## Prerequisites

- **Linux / FreeBSD:** Clang (CI: 23; FreeBSD's system Clang also works), LLVM's development files
  (`llvm-config`, required by the recompiler), CMake ≥3.28, Ninja, POSIX shell, GNU make (`gmake` on FreeBSD),
  NASM on x86-64, Perl, Python 3 (the menu's languages, SPIRV-Tools' build), Git, pkg-config, `patch` and meson
  (dav1d's build).
  Use a system SDL3 development package, or X11/Wayland and audio development libraries for the bundled
  SDL3 build.
- **macOS:** Apple silicon, macOS 13 or newer, Xcode (its Clang and SDKs), CMake ≥3.28, Ninja, meson, Python 3 and
  Git.
  LLVM's libraries come from `scripts/apple-llvm.sh` or Homebrew ([steps](#macos)).
- **Windows:** Win10/11 x64, or Windows 11 on Arm. CI builds in MSYS2's CLANG64 environment, and on Arm in its
  CLANGARM64 one, which have LLVM's development files ([steps](#windows--msys2-clang64)). Bootstrap stages
  SHA-256-checked llvm-mingw/Clang, CMake, Ninja, NASM, Strawberry Perl and w64devkit in `deps/tools`. llvm-mingw has
  no LLVM libraries, so that route also needs a MinGW LLVM's `llvm-config` set with
  `-DSNX_LLVM_CONFIG=/path/to/llvm-config`. CMake fails without it.
- **Shaders:** glslang's `glslangValidator` compiles them at build time (CMake fetches its pinned release on Linux
  x86-64, Windows x86-64 and macOS; elsewhere it takes the system's `glslang`), and SPIRV-Tools' `spirv-opt` and
  `spirv-val` optimize and validate them (`cmake/glsl.cmake`). Those two are the SPIRV-Tools release CMake pins, built
  from source for the machine doing the build, which needs Python 3 (`cmake/spirv_tools.cmake`).
  `-DSPIRV-Tools_FORCE_SYSTEM=ON` takes the system's instead, which must then be that release or newer, and
  `-DSNX_SPIRV_OPT`, `-DSNX_SPIRV_VAL` and `-DSNX_SPIRV_DIS` (the tests' disassembler) name others.
- **Display:** a Vulkan 1.3 driver. Headless runs use Vulkan offscreen and skip SDL video initialization.
  SDL3 remains a required build dependency.
- **Compiler cache (optional):** `ccache`, or `sccache` when `ccache` is not found, makes a cold build after a
  clean faster. The build works without either.
- **FFmpeg:** required by every NX runtime build and by the Android driver (its image codec). CMake builds the pinned
  source release, and with the Android driver its image codecs, libswscale and zlib for its PNG codec.
  Avoid spaces in checkout/cache paths.
- **dav1d:** FFmpeg's AV1 decoder for the NX runtime's movies, built from the pinned release with meson and Ninja for
  every target, cross builds, Android and iOS included (`cmake/dav1d.cmake`). Without meson the configure says so
  and the runtime plays AV1 movies black; `-DSNX_DAV1D=OFF` leaves it out.
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
sudo apt install build-essential git cmake ninja-build meson clang llvm-dev nasm pkg-config python3 \
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

CMake needs version 3.28 or newer (CPMUtil, which fetches the third-party packages, needs it), and Clang must support
C23/C++23. Upgrade these tools if your
release ships older versions. Install your GPU's Vulkan driver separately. For Intel/AMD or lavapipe
on Debian/Ubuntu, the usual packages are `libvulkan1 mesa-vulkan-drivers`. NVIDIA users need a
Vulkan-capable NVIDIA driver.

### Fedora

```sh
sudo dnf install clang cmake ninja-build meson nasm make perl git pkgconf-pkg-config patch llvm-devel python3 \
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
sudo pacman -Syu --needed base-devel git cmake ninja meson clang llvm nasm perl pkgconf python sdl3
```

### FreeBSD

FreeBSD provides Clang in the base system. Install the remaining tools, the SDL3 package, and libusb, which SDL's
HIDAPI loads at run time to read controllers (an Xbox pad is only read that way):

```sh
sudo pkg install git cmake ninja meson gmake nasm perl5 pkgconf python3 sdl3 llvm libusb glslang
```

The build uses that `sdl3` package by default (`SNX_SYSTEM_SDL3`, see [SDL3 troubleshooting](#sdl3-troubleshooting));
pass `-DSNX_SYSTEM_SDL3=OFF` for the pinned SDL3 source build instead.

The `llvm` package is LLVM 19, which the Switch, 3DS and Vita drivers build with. The Android driver
([Android apps](../platforms/android-apps/README.md)) compiles an app's native code at run time with the same LLVM, and LLVM 19's
code generator crashes on some of it (a trap in its DAG combiner at an engine's login). Build it with LLVM 22: install
`llvm22` and pass `-DSNX_LLVM_CONFIG=/usr/local/llvm22/bin/llvm-config` to the configure step.

### Configure and build

```sh
git clone https://github.com/scarletnx/ScarletNX.git && cd ScarletNX &&
cmake --preset default -B build && cmake --build build
# -> build/{scarletnx,snx-shaderc}
```

### SDL3 troubleshooting

On all platforms, CPMUtil first looks for a compatible SDL3 development package (3.4.18 or newer) through
`SDL3Config.cmake`. If none is found, it downloads and builds the pinned SDL3. The fallback is static, so no separate SDL3 runtime
installation is needed. It still needs your platform's window-system development libraries.

- `SNX_SYSTEM_SDL3=ON` (the default on FreeBSD) takes the system's package from `find_package` from 3.4.12, Arch
  POWER's, which has every SDL name the tree uses, and never downloads. FreeBSD's ports `sdl3` (3.4.16) is older than
  the pin, so without it CPMUtil's version check builds SDL3 from source. `SNX_SYSTEM_SDL3=OFF` keeps the pinned build
  everywhere.

- An old checkout showing `Could not find a package configuration file provided by SDL3` needs either
  the SDL3 **development** package or this updated fallback. Pull the latest checkout and configure again.
- For SDL3 installed in a custom prefix, pass `-DCMAKE_PREFIX_PATH=/path/to/prefix`, or
  `-DSDL3_DIR=/path/to/prefix/lib/cmake/SDL3` (the directory containing `SDL3Config.cmake`).
- To force the source build, pass `-DSDL3_FORCE_BUNDLED=ON`, CPMUtil's per-package option (`-DCPM_DOWNLOAD_SDL3=ON`,
  CPM.cmake's from before it, is read as this). No bundled override is forced by ScarletNX. The source build is
  recommended on Windows to match the MinGW toolchain and keep SDL3 statically linked.
- If SDL3's configure reports missing X11/Wayland development libraries, install the packages above.
  Do not work around that by disabling both display backends for a windowed build.
- No sound, and the log says `audio: SDL could not start its audio: No available audio device`: the
  SDL3 fallback was configured without audio development libraries (its configure warns `Unable to find
  the alsa development library`). Install the audio packages above and configure again.
- SDL3 is mandatory for the NX runtime, including headless runs. Use `-H` at runtime to skip
  SDL video initialization and run without a display server. The separate headless backend was removed.
  For an old build directory, remove the cached backend selection with `-USNX_BACKEND`.
- If a controller does not appear, check the game's `input: joystick` lines. A GUID with no gamepad mapping needs
  an SDL mapping in `gamecontrollerdb.txt`, as described in [Usage](../user/usage.md). If no joystick appears on Linux,
  check whether `/dev/input/event*` is readable and whether udev marks its event node `ID_INPUT_JOYSTICK=1`.
  The supplied `dist/72-scarletnx-input.rules` grants seat access to recognized joystick event nodes and to the
  hidraw nodes of the console's controllers. Other HIDAPI controllers may need their vendor's own udev rule. On
  FreeBSD the gamepad comes from HIDAPI over libusb or `/dev/uhid*`, never from `/dev/input/event*`: a pad `xb360gp(4)`,
  `hgame(4)` or `webcamd` claimed is invisible to SDL. See [Usage](../user/usage.md) for the access rules and the packages. If
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
brew install cmake ninja meson
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

   `build/macos/scarletnx` runs from the terminal as on Linux ([Usage](../user/usage.md)). It loads the system's Vulkan
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
in [macOS and iOS](../hosts/apple.md).

   `-DSNX_KOSMICKRISP=<sdk>/macOS` also carries Mesa's KosmicKrisp and its Vulkan loader, from a LunarG Vulkan SDK's
   macOS folder. `vulkan_driver=auto`, the default, then plays KosmicKrisp on a Mac that runs it (Apple silicon,
   macOS 26 or later), and the MoltenVK the app carries elsewhere ([macOS and iOS](../hosts/apple.md#kosmickrisp-experimental)).

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

The `windows` job in [build.yml](../../.github/workflows/build.yml) builds this way. Clang, LLVM and the FFmpeg and
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
     mingw-w64-clang-x86_64-meson mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-python \
     mingw-w64-clang-x86_64-glslang
   ```

   Python builds the menu's languages and runs SPIRV-Tools' build, which optimizes and validates the shaders. CI also
   installs ccache, which is optional here, as sccache is. On Windows on Arm, open the **MSYS2 CLANGARM64** shell
   instead and install the same packages with `mingw-w64-clang-aarch64-` in place of `mingw-w64-clang-x86_64-`, as the
   job's AArch64 leg does; the steps below are the same there, and its DLLs are in `/clangarm64/bin`.
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
   ([Usage](../user/usage.md)). For a copy that runs without MSYS2, `cmake --install build/default --prefix stage` puts
   `scarletnx.exe` in `stage/bin`. Then copy the DLLs it imports from `/clang64/bin`, as CI's
   "Bundle runtime DLLs and licenses" step does.

## Rebuild, deploy, cache

```sh
cmake --build build
```

Default: Ninja + Clang, `RelWithDebInfo` (`-O2 -g`), without ThinLTO
([`SNX_LTO`](#build-variants), which the nightlies use). Runtime relinks deploy `recompiled/Game/Game[.exe]` beside
matching `Game_code.so/.dll`, in the data folder's `recompiled/` ([Usage](../user/usage.md#folders)); no-op builds skip deployment. Recompiler/ABI edits require [module rebuilds](development.md).

CPMUtil downloads each package [cpmfile.json](../../cpmfile.json) pins once into a shared `.cache/cpm` cache, at
`<package>/<version>`, checked by its SHA-512 ([Third-party packages](#third-party-packages)). Offline configuration
needs the matching sources already in the cache, or `-D<package>_CUSTOM_DIR=/path` naming a folder that holds one (a
package of one file is checked against its pin there too; a patched package, FFmpeg's and FidelityFX-FSR2's, is
refused there, as CPMUtil would take it unpatched). A patched package is kept patched in the cache, extracted and patched
again when its patches change. With tests on (`BUILD_TESTING`, the default) and the runtime, libvpx's VP9 test vectors download
there too ([tests](../../tests/CMakeLists.txt)). The Switch runtime's shared font stand-ins download only when it is
built ([Shared fonts](../platforms/nx/filesystem.md#shared-fonts)), and GNU Unifont's `.hex` (the menu's CJK and Hangul
letters of game titles) only when the menu is. Override via CMake/env `CPM_SOURCE_CACHE`. SDL3 is
the system's package when `find_package` finds 3.4.18 or newer, else the pinned static source build;
`-DSDL3_FORCE_BUNDLED=ON` asks for the source build.

Where a package comes from is CPMUtil's to say, by its options ([packages.cmake](../../cmake/packages.cmake)). LZ4, the
Vulkan headers and SPIRV-Tools' programs are the pinned ones unless the system's are asked for:
`-Dlz4_FORCE_SYSTEM=ON`, `-DVulkan-Headers_FORCE_SYSTEM=ON` (the `VulkanHeaders` package, with its
`share/vulkan/registry/vk.xml`), `-DSPIRV-Tools_FORCE_SYSTEM=ON` (`spirv-opt`, `spirv-val` and `spirv-dis` on `PATH`),
or `-DCPMUTIL_FORCE_SYSTEM=ON` for all of them and SDL3. Each must then be there at the pinned version or newer, or the
configure stops; LZ4 then links as the system's library. The configure says where each came from: `[CPMUtil] Using
system package lz4@1.10.0`, or `[CPMUtil] Using bundled package lz4@v1.10.0` for the pinned source. Every other package
is the pinned one whatever they say: the build compiles it into a target of its own, with its own flags or a change of
ours (FFmpeg, OpenSSL and the libcurl built on it, jemalloc, zstd, xxHash's x86 dispatcher, libdeflate, miniz, lunasvg
and plutovg), or its package cannot be found this way (ENet installs none, yyjson's carries no version). Asking for the
system's one of those (`-Dyyjson_FORCE_SYSTEM=ON`) stops the configure, and `CPMUTIL_FORCE_SYSTEM` lists the ones it
leaves pinned. `-D<package>_FORCE_BUNDLED=ON` and `-DCPMUTIL_FORCE_BUNDLED=ON` (CPMUtil's default on Android and iOS)
keep the pinned ones. For a package in a prefix of your own, add `-DCMAKE_PREFIX_PATH=/path/to/prefix`.

CPM.cmake's options from before CPMUtil are read, and said once per build tree with the option that replaces them:
`-DCPM_USE_LOCAL_PACKAGES=ON` (also from the environment) takes LZ4, the Vulkan headers and SPIRV-Tools' programs from
the system where it has them at the pinned version or newer, and the pinned ones otherwise, as it did;
`-DCPM_DOWNLOAD_SDL3=ON` is `-DSDL3_FORCE_BUNDLED=ON`.

FFmpeg, OpenSSL and jemalloc build with their own configure and make, and SPIRV-Tools with its own CMake, in each build
tree. With `SNX_DEPS_CACHE` (CMake or env) naming a folder, a tree reuses a finished build there whose inputs match
(source, configure line, compiler, C headers and the cmake file that builds it, see
[deps_cache.cmake](../../cmake/deps_cache.cmake)) and publishes the ones it builds, so other trees and branches skip
them. A change to any input is a new folder; old ones are only left behind.

A compiler cache is optional. At configure, `ccache` is used for the C and C++ compiles when it is found, and
`sccache` is used when `ccache` is not found. With neither installed the build compiles as it does without one. The
cache helps a cold build after a clean, while Ninja already reuses the objects of an incremental build.
`-DSNX_COMPILER_CACHE=off` disables it, `-DSNX_COMPILER_CACHE=ccache` or `sccache` chooses one, and an explicit
`-DCMAKE_C_COMPILER_LAUNCHER` or `-DCMAKE_CXX_COMPILER_LAUNCHER` is used as given (CI does this with `ccache`). `ccache`
comes first because it is faster for local C and C++ than `sccache`, which suits a shared or remote cache.

## Third-party packages

The libraries, tools and test data the build downloads are pinned in [cpmfile.json](../../cpmfile.json) and added
with [CPMUtil](../../cmake/cpmutil/CPMUtil.cmake), crueter's wrapper of CPM.cmake as Eden carries it, from
[packages.cmake](../../cmake/packages.cmake). An entry gives the package's `version`, where it comes from (`repo` and an
`artifact` for a release's asset, `repo` alone for the archive of a tag or commit, or a `url`) and the SHA-512 of that
file in `hash`; CPMUtil's [documentation](https://git.crueter.xyz/CMake/CPMUtil/src/branch/master/docs/CPMUtil.md)
gives the other fields. The file is in CPMUtil's form, which `jq --indent 4 -S . cpmfile.json` or
`python3 -m json.tool --indent 4 --sort-keys cpmfile.json` writes.

To update a package:

1. Change its `version`, and its `artifact` or `url` where they name it (`%VERSION%` in an artifact, or
   `numeric_version`, as lz4's entry has it, spare that).
2. Download the file, check it against the digest its release publishes (GitHub shows each asset's SHA-256), and put
   its SHA-512 in `hash`: `cmake -E sha512sum <file>`. Never the hash of what a configure happened to fetch.
3. Configure: the new version downloads into `CPM_SOURCE_CACHE/<package>/<version>`.

To add one:

1. Pin it in cpmfile.json as above, under a lowercase key, as Eden's are; `package` gives the CMake name where that
   differs (`"openssl": {"package": "OpenSSL", ...}`), and `<package>_SOURCE_DIR` is where it lands.
2. Add it in [dependencies.cmake](../../cmake/dependencies.cmake), under the conditions that need it:
   `AddJsonPackage(NAME <key> DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)` when the build compiles its sources into a
   target of its own, patches it or reads it as data, so no system package can stand in for it. One the system may give
   instead, which installs a CMake package `find_package` finds, gets a `min_version` and `AddJsonPackage(<key>)`,
   with `"bundled": true` when the pinned one stays the default (and `snx_cpm_options(<key>)` before it, for
   CPM_USE_LOCAL_PACKAGES). A package that is one file (a header, a font, a test vector), which CPMUtil would try to
   extract, is added with `snx_package_file(<key>)`.
3. Its license: in the install (CMakeLists.txt, under `licenses`) when it ships, and in [CREDITS](../CREDITS.md).

A patch goes in `.patch/<key>/NNNN-<what>.patch`, named in the entry's `patches` in the order they apply. CPMUtil
applies them with `patch -p1` and no fuzz to the release it extracts, does it again on a fresh copy when one changes,
and stops the configure on one that does not apply. To make one, edit a copy of the pristine file and rebuild the
hunks with `diff -u --label a/<file> --label b/<file>`. `runtime_architecture` refuses a download outside CPMUtil
(CPM.cmake's calls, FetchContent, `file(DOWNLOAD)`), a name cpmfile.json does not pin, a pin nothing adds, a package
without a `min_version` added without `FORCE_BUNDLED_PACKAGE ON`, a patch named and missing or there and not named,
and a cpmfile.json out of that form.

## Build variants

SDL3, Vulkan and FFmpeg are mandatory for the NX runtime, including when running with `-H`.
There are no renderer or media-decoding disable options. Tools-only and GNUABI-only builds do not need them.

Persistent per-build-dir options (POSIX examples):

```sh
cmake --preset default -B build -DSNX_FFMPEG_ASM=OFF && cmake --build build
```

- `-DSNX_FFMPEG_ASM=OFF`: omit x86-64 NASM assembly; slower FFmpeg decoding.
- `-DSNX_LTO=ON`: link with ThinLTO ([cmake/lto.cmake](../../cmake/lto.cmake)). OFF by default, as the builds of
  development and of pull requests run many times a day. The nightlies and releases turn it on for Linux x86-64,
  Windows and Android's arm64-v8a. It is used in the Release, RelWithDebInfo and MinSizeRel builds, never in a Debug
  one, where CMake's `CheckIPOSupported` links a test project with it: Linux, FreeBSD, Windows (Clang with lld:
  MSYS2's CLANG64, llvm-mingw, the `windows-cross` preset) and Android. It does not work on macOS and iOS, where CMake
  has no IPO for the apps' Swift, and the Linux and NetBSD cross builds' check does not test their preset's linker. It
  stays off with `SNX_TEST_SANITIZERS` unless the linker is lld: ASan gives each object a common symbol, and GNU ld's LTO plugin
  then takes every archive member that has one, with references that nothing defines. The objects
  compile to LLVM bitcode, and the link optimizes across files: on a Steam Deck, the GPU thread's time for each frame
  fell by 6.3%. The links take longer, the test programs' too. On an 8-thread Linux desktop a clean build took 440 s,
  against 316 s without it. The link keeps each module's machine code in the build folder's `thinlto-cache/` (2 GB at
  most, with lld or GNU ld's gold plugin), so a build after an edit to one file of the renderer took 18 s, against
  50 s without the cache and 4 s without ThinLTO. The installed `scarletnx-recompiler` archive and the parts it is made
  of always build without it, as machine code ([Library](library.md)), and the game modules' runtime library is
  compiled with its own flags, which it does not change.
- `-DSDL3_FORCE_BUNDLED=ON`: build the pinned SDL3 instead of using a system SDL3 development package
  (`-DCPM_DOWNLOAD_SDL3=ON` before CPMUtil, read as this).
- `-DSNX_DEPLOY=OFF`: disable runtime deployment.
- `-DSNX_JEMALLOC=OFF`: keep the C library's malloc in `scarletnx` instead of the pinned jemalloc (default ON on Linux,
  FreeBSD, macOS and Android, where it goes into the driver's executable `libscarletnx-run.so` that runs recompiles;
  refused elsewhere and with `SNX_TEST_SANITIZERS`). A recompile's LLVM threads spend less time in malloc with
  jemalloc.
- `-DSNX_TRACY=ON`: build the runtime with the pinned Tracy profiler client (default OFF). For development builds
  only; not available on iOS. It turns `SNX_DEPLOY` off, since the profiling runtime must not be copied over a
  game; the `tracy` preset sets both. See [profiling](development.md#profiling).
- `-DSNX_RECOMPILED_DIR=/path`: override deployment dir (default: empty, the data folder's `recompiled/` the build
  tree's driver uses). `SNX_GAMES_DIR`, its name before, is still taken while it is empty, said as it configures. A
  tree whose value is the checkout's `games/`, the default before, is set back to empty.
- `-DCPM_SOURCE_CACHE=/path`: override source-download cache.
- `-Dlz4_FORCE_SYSTEM=ON`, `-DVulkan-Headers_FORCE_SYSTEM=ON`, `-DSPIRV-Tools_FORCE_SYSTEM=ON`,
  `-DCPMUTIL_FORCE_SYSTEM=ON`: take LZ4, the Vulkan headers, SPIRV-Tools' programs, or all of them and SDL3, from the
  system, which must have them at the pinned version or newer (default OFF: every package from its pinned source but
  SDL3). `-DCPM_USE_LOCAL_PACKAGES=ON` before CPMUtil, read as these where the system has them. See
  [Rebuild, deploy, cache](#rebuild-deploy-cache).
- `-D<package>_CUSTOM_DIR=/path`: a folder holding that package (as cpmfile.json names it) already, which the configure
  takes instead of downloading it; refused for a package cpmfile.json patches, which CPMUtil would take unpatched.
- `-DSNX_DEPS_CACHE=/path`: reuse and publish finished FFmpeg, OpenSSL, jemalloc and SPIRV-Tools builds there
  (default: the environment's `SNX_DEPS_CACHE`, else none).
- `-DSNX_BUILD_NX=OFF`: omit the Switch runtime (default ON).
- `-DSNX_BUILD_GNUABI=OFF`: omit the CPU-only Linux AArch64 runtime, see [GNU ABI](../platforms/gnuabi/README.md) (default ON).
- `-DSNX_BUILD_3DS=OFF`: omit the 3DS layer, see [3DS](../platforms/3ds/README.md) (default ON).
- `-DSNX_BUILD_GUI=OFF`: omit the SDL3 launcher. The default is ON when `SNX_BUILD_NX` is on and `SNX_TOOLS_ONLY`
  is not, OFF otherwise.
- `-DSNX_TOOLS_ONLY=ON`: build the extraction/recompilation driver without the runtime, see below (default OFF).
- `-DSNX_RECOMPILER_SHARED=ON`, `-DSNX_RECOMPILER_INSTALL=ON`: also build the recompiler as a shared library, and
  install the library, see [Library](library.md) (defaults OFF and the value of `SNX_TOOLS_ONLY`).
- `-DSNX_BUILD_TESTING=OFF`: leave the tests out whatever `BUILD_TESTING` says. ON by default when ScarletNX is the
  top-level project, OFF when another project adds it with CPM or `add_subdirectory` ([Library](library.md)).

Test options, read when `BUILD_TESTING` is on (CTest's default), all OFF by default:

- `-DSNX_TEST_SANITIZERS=ON`: build the native tests, and the programs and libraries of the repository they run or
  link, with ASan and UBSan (`runtime_architecture` names one left out). The runtime is one, so it is not deployed.
  CI's Linux x86-64 job (`Linux AppImage (x86_64)`) runs the tests this way, with
  `LSAN_OPTIONS=use_stacks=0:use_registers=0:suppressions=$PWD/tests/lsan.supp` so the leak check finds the same leaks
  on every machine, and none of the system's libraries' own (SDL's ALSA backend never frees ALSA's device hints).
- `-DSNX_TEST_VULKAN=ON`: add the offscreen Vulkan integration tests, see below.
- `-DSNX_TEST_ONLINE_LIVE=ON`: add `online_presets_live`, which asks each WFC service's DNS server over the
  internet (labels `external` and `network`): how a service that stopped answering is found
  ([src/common/online](../../src/common/online/README.md)).
- `-DSNX_TEST_EDENPROBE=/path/to/edenprobe.nro`: add `edenprobe_vk_memory_1` and `edenprobe_vk_memory_2`, which run
  edenprobe as CI does against its baseline, with guest buffers copied and read in place (docs/dev/edenprobe.md).
- `-DSNX_TEST_VP9_VECTORS=ON`: add the `nvdec_vp9_*` tests, which run NVDEC's VP9 jobs on libvpx's test vectors;
  configure downloads the 32 files from the WebM project into `CPM_SOURCE_CACHE` once. CI's Linux, ASan, Windows and
  macOS jobs run them.
- `-DSNX_TEST_AV1_VECTORS=ON`, with `-DSNX_TEST_VULKAN=ON`: add `vulkan_video_av1`, which runs the native video
  service's AV1 decoder on four of AOMedia's test vectors, with Vulkan Video where the device decodes AV1 and on the
  CPU elsewhere; configure downloads the 8 files into `CPM_SOURCE_CACHE` once. CI's headless Vulkan step runs it.
- `-DSNX_TEST_A64_FP_DIFF=ON`: add the AArch64 FP differential test, which needs Python unicorn 2.1.4 and a native
  Linux host.
- `-DSNX_TEST_ARM_AOT=ON`, `-DSNX_TEST_ARM_DIFF=ON`: the 3DS ARM11 witnesses and the A32 unicorn comparison, see
  [3DS](../platforms/3ds/README.md).

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
[Software Vulkan / lavapipe setup](../user/usage.md#headless-vulkan--lavapipe).

Offscreen integration test, with an installed Vulkan 1.3 ICD (lavapipe works):

```sh
cmake --preset default -B build/headless-vk -DSNX_BUILD_GUI=OFF \
  -DSNX_DEPLOY=OFF -DSNX_TEST_VULKAN=ON && cmake --build build/headless-vk
# Set VK_DRIVER_FILES to your lavapipe manifest to force CPU rendering.
ctest --test-dir build/headless-vk -R vulkan_headless --output-on-failure
# the same with the validation layer
(cd build/headless-vk && src/nx/snx-test-vulkan -H --vk-validation=1)
```

The test uses synthetic pixels, not a game. It checks offscreen submissions, frame recycling, cropping,
readback hashes and PPM contents without WSI. Validation requires the Khronos validation layer to be installed.
It takes the runtime's settings as `--KEY=VALUE` (`--vk-memory=2`, `--memory-traps=2`) and stops on any
other argument. `SNX_TEST_VULKAN` is off by default so ordinary unit tests do not require an ICD.

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
hold its settings, logs and caches ([Usage](../user/usage.md#folders)). `--data DIR` before the command names another data
folder, `--build-dir DIR` another build tree.
The menu (`SNX_BUILD_GUI`) needs Python 3 to build its languages (`scripts/build-languages.py`,
[GUI](../user/gui.md#languages)). Python ≥3.12 is optional for `tools/*.py` diagnostics.

## Install / AppImage

```sh
cmake --install build --prefix stage
# -> stage/bin/, stage/share/licenses/scarletnx/
```

AppImage: run inside pkgforge's Arch container with `quick-sharun` ([CI setup](../../.github/workflows/build.yml)), as
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
and with `SNX_SYSROOT` set it cross-builds for another architecture. `scripts/llvm-source.sh` downloads that version's
source for it, as for the Android and Apple LLVM scripts, and checks it against the SHA-256 `scripts/llvm-source.sha256`
pins: a version without a line there is refused, so a new LLVM in CI's image needs its line first, the hash taken from
the digest on the release's page. `SNX_A64_DISASM=OFF` leaves out `scarletnx --mcp`'s A64 disassembler, which is for
working on the runtime.

`scarletnx` is every driver the build has (`src/multicall`): each driver is a library with no `main` of its own,
and the program, whichever drivers are on (`SNX_BUILD_NX` among them), runs the one its name says, so LLVM, SDL and
the code the drivers share are in it once. `scarletnx-switch`, `scarletnx-3ds`, `scarletnx-vita`, `scarletnx-gnuabi`,
`scarletnx-agb` and `scarletnx-nds` are links to it on Linux and macOS, made in the build folder and installed. Windows,
whose zips keep no links, installs `scarletnx.exe` alone, which takes a driver's name as its first argument
(`scarletnx.exe scarletnx-3ds ...`, how the launcher runs them); its build folder has hard links of those names, for
the tests. Android's and iOS's apps start the program their own way; Android's menu runs a driver as Windows' does,
its name first, through `libscarletnx-run.so`.

[Nightly AppImages](https://github.com/scarletnx/nightly/releases/latest), x86_64 and aarch64 · [Launch](../user/usage.md).

A pull request's own builds are linked from a comment on it once CI has built them.

[Licenses/credits](../CREDITS.md) · [Architecture](architecture.md) · [Roadmap](roadmap.md).
