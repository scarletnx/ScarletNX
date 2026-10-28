<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# macOS and iOS

Both build on a Mac with Xcode, for Apple silicon (arm64), with Xcode's Clang, and the CI builds both on every push
(`.github/workflows/build.yml`). They are new. On a Mac, one game's module loads and runs as far as its title
background, without the logo or the menu. On an iPhone, one game has been prepared on the device and played with JIT on.
The rest is under [What is missing](#what-is-missing).

## The frontend

The menu and the runtime are SDL's window, as on Android. The parts the system does are a thin frontend over it, in the
same shape as Android's `MainActivity` and `src/gui/android.c`:

- `src/gui/apple.m` (Objective-C) is the menu's side: the data folder, the game the menu plays, and the SDL window's
  `UIWindow` or `NSWindow` for the Swift to present over.
- `dist/apple/Frontend.swift` (Swift) is the system's side, called from `apple.m` through `@_cdecl` functions: on iOS,
  the document picker SDL lacks there and the share sheet; on macOS, the Finder showing the saved log.

### macOS

`ScarletNX.app` is the desktop program in a bundle: `scarletnx` in `Contents/MacOS`, the themes and the
licenses in `Contents/share`, where an installed copy looks for them. It works as on Linux. The menu extracts and
recompiles through the driver as a child process, with SDL's own file and folder dialogs. Its data folder is
`~/Library/Application Support/scarletnx`. *Share Log* saves the log and selects it in a Finder window. The app is
signed ad hoc, as Apple silicon runs no unsigned code. A copy from a download may need *Open* from the Finder's menu
the first time.

The Vulkan driver is MoltenVK, Ryujinx's build of it ([V380-Ori/Ryujinx.MoltenVK](https://github.com/V380-Ori/Ryujinx.MoltenVK)),
which carries the patches an emulator's pipelines need and mainline MoltenVK lacks. Both apps carry it in their
`Frameworks`, and a game's folder gets that library beside its copy of the runtime (`snx_deploy_runtime`), a second name
where the disk allows one and a copy where it does not, as a read-only app image or one macOS translocated leaves. A
game plays as `<data folder>/recompiled/<id>/<id>`, outside the bundle, where SDL's own search of
`@executable_path/../Frameworks` reaches nothing. The runtime hands SDL the library its own folder holds
(`backend/sdl3.c`), and a run with no window loads the same file without SDL's video. The bare `scarletnx` of a
checkout has no library to deploy and no bundle around it, so it uses the system's Vulkan loader (Homebrew's
`molten-vk` and `vulkan-loader`).

### iOS

An iOS app runs one process and may not start another, so the iOS app works like Android's game activity without its
menu's driver:

- **The data folder** is the app's Documents, which the Files app (*On My iPhone > ScarletNX*) and the Finder show.
  It holds `dumps/` (a game's `.nsp` or `.xci`), `keys/` (`prod.keys`), `recompiled/` (what the app makes, each game as
  `recompiled/<id>/<id>_code.snxobj` with `_data/`) and `logs/`. The menu's settings, state and caches are in the app's
  `Library/scarletnx/`, which the Files app does not show ([Running a game on iOS](#running-a-game-on-ios)).
- **The menu** lists those games. *Load* and the folder settings open the system's document picker. Preparing a dump
  runs the driver on a thread of the app, since an app may not start a process. A dump picked outside `dumps/` is
  copied into it first. The thread has a 64 MiB stack, which LLVM's passes need.
- **A game** runs in the app's process once the menu closes (`src/nx/runtime/entry.c`): the runtime starts once per
  process, so the app ends with the game.
- **Share Log** opens the share sheet with `scarletnx.log`.
- **Logs.** Everything the app prints goes to the day it starts on's `logs/scarletnx-console-<date>.log` in its
  Documents (the 14 newest days kept), after a banner with the
  device, the system and the limits the app has, with the menu's own lines ([GUI](GUI.md)), and a fatal error, the
  menu's or a game's, is also written to `logs/scarletnx-error.log`. A phone shows no console, so these are what to read
  when a run fails; Share Log sends both.

The IPA is unsigned: AltStore, SideStore or a developer account signs it for the device (a Mac can too:
[Installing from a Mac](#installing-from-a-mac)), with
`dist/apple/ScarletNX.entitlements` (the larger address space and memory limit a game needs). It holds MoltenVK's
framework.

Code compiled while a game runs on macOS, 32-bit games' blocks and the runtime JIT's functions, goes through the
runtime's own loader (`src/common/cpu/recompiler/llvm/code_loader.c`). LLVM writes it as ELF objects with Darwin's
calling convention, and the loader places them in `MAP_JIT` memory, which each thread sees either writable or
executable. The app is signed without the hardened runtime, so `MAP_JIT` needs no entitlement.

### Running a game on iOS

The iPhone prepares the game itself: it extracts the dump and recompiles it with its own LLVM. Super Meat Boy takes
about two minutes on an iPhone 14 Pro, with the app in the foreground and the screen on.

You do not need to put files in the right folders. The app copies the `prod.keys` you pick into `keys/` and a dump you
pick into `dumps/`, then makes the game in `recompiled/`. Pick them from wherever they are (Files, Downloads, iCloud).

1. **Install the app.** Take `ScarletNX-ios-arm64-<commit>.ipa` from a CI run, or build it (`cmake --preset ios`,
   [Building](#building)), and install it with a sideloading tool that signs it with an Apple ID: iloader or
   SideStore. A free account signs an app for 7 days. iOS lets a device hold three apps with a free profile at a time,
   whichever Apple IDs signed them, so a second account adds no room. The app, SideStore and StikDebug fill them, and a
   fourth install fails with "maximum number of installed apps using a free developer profile".
2. **Set up JIT once.** iOS makes memory executable only for an app with a debugger attached. Install StikDebug and
   LocalDevVPN, give StikDebug the pairing file (SideStore keeps one: *Storage Explorer*,
   `ALTPairingFile.mobiledevicepairing`), hold ScarletNX in StikDebug's list and *Assign Script* `universal.js`. The
   runtime checks for the debugger first and says what to enable when there is none. Reinstalling StikDebug removes its data,
   assignments included, so assign the script again.
3. **Add your keys.** Open ScarletNX, *Install Keys*, and pick the `prod.keys` of your Switch.
4. **Prepare the game.** In *Games*, pick the `.nsp` or `.xci` and prepare it. The app extracts it and writes
   `recompiled/<id>/` with `<id>_code.snxobj` and `<id>_data/`. 64-bit games only. Keep the app in the foreground
   until it ends: iOS stops its GPU and its threads in the background.
5. **Play.** *Enable JIT* for ScarletNX in StikDebug, open ScarletNX, *Games*, and pick the game. Do this before each
   run, and do not leave the app while the game runs. Keep StikDebug open too: when it is closed, the debugger goes
   with it and the game loses JIT.

The folders the app makes in *On My iPhone > ScarletNX* are `dumps/` (your games' dumps), `keys/`, `recompiled/` (the
games it made) and `logs/` (what to send when a run fails). Reinstalling the app can remove them with some tools.

A Mac still prepares a game the same way (`scarletnx -x "Game.nsp" --name Game`, then
`scarletnx -r work/Game --target ios --outdir recompiled`), and its `recompiled/<id>/` can be copied into the app.

Limits found on an iPhone 14 Pro (iOS 26.6.1): without the paid extended-virtual-addressing entitlement the app has
about 13 GiB of address space and about 3 GB of memory, so the runtime takes the largest free run it can (128 GiB
down to 5 GiB) and lays the guest out to fit, with no alias region below 12 GiB.

### Installing from a Mac

A Mac with Xcode can sign the IPA and install it, with no sideloading tool. `scripts/apple-resign.sh` does it:

```
scripts/apple-resign.sh ScarletNX-ios-arm64-<commit>.ipa --team TEAM --bundle org.scarletnx.TEAM \
    --identity "Apple Development: you@example.com (ABCDE12345)" --device UDID
```

TEAM is the account's team ID, the identity one of its certificates (`security find-identity -v -p codesigning`) and
UDID the iPhone's (`xcrun devicectl list devices`). The script:

1. **Makes the provisioning profile.** It builds a stub app with the bundle ID and the team, with
   `-allowProvisioningUpdates`, and Xcode makes the profile in the account it is signed in to.
2. **Signs the app.** It puts that profile in the app, sets the bundle ID, takes the entitlements from the profile and
   signs the frameworks and then the app with the certificate.
3. **Installs it** with `xcrun devicectl device install app`. The same bundle ID replaces the app and keeps its
   `dumps/`, `keys/` and `recompiled/`. A sideloading tool adds the team to the bundle ID, so pass the one the
   installed app has (`org.scarletnx.TEAM`), as another counts as a new app.

Run it from Terminal.app on the Mac's own screen. Over ssh the keychain is locked and codesign fails with
`errSecInternalComponent`. A free account's profile lasts 7 days and covers three apps on a device in all, whichever
tool installed them. A free profile lacks the paid entitlements of `dist/apple/ScarletNX.entitlements`, so the app
is signed with the ones the profile has. Without `--device` the signed `ScarletNX.app` is left in the folder of
`--out`.

## What is missing

Apple silicon's 16 KiB pages no longer stop the runtime: it commits guest memory in whole host pages, as on
PowerPC64 ([PowerPC64](./PPC64.md)), and views of memory are copies there, as Windows has them. What still keeps
games from running:

- **A game's frame on a Mac.** The recompiler links an arm64 Mach-O dylib
  (`src/common/cpu/recompiler/llvm/link_macho.c`) with the ad hoc signature ld64 gives one, which Apple silicon
  needs to run it. A Mac loads one and the game runs, but the frame stops at the title background. Which part of the
  renderer on MoltenVK drops the rest is not known yet.
- **Game modules on iOS, one game checked.** iOS loads no code it did not ship with, so a game's code is a pack of
  objects (`scarletnx -r Game --target ios` writes `<id>_code.snxobj`, 64-bit games only) that the app loads into
  memory a debugger made executable (`load_pack` in `src/nx/runtime/main.c`, the JIT loader's iOS arena in
  `code_loader.c`). The app makes it itself, or a Mac does.
- **32-bit games on iOS.** Their blocks are compiled at run time (`src/nx/runtime/arm.c`,
  `src/common/cpu/arm/exec.c`), which needs memory that becomes executable. iOS gives that only to an app that may JIT.
- **Preparing on Windows or Linux.** Their x86 builds carry x86's code generator alone, to keep their size, so
  `--target ios` fails there. The iPhone prepares games itself now.
- **JIT without StikDebug.** The app still needs a debugger attached, which StikDebug gives ([#598](https://github.com/scarletnx/ScarletNX/issues/598)).
- **The 3DS, Vita and Linux drivers** are not built for iOS (`SNX_BUILD_3DS`, `SNX_BUILD_VITA`, `SNX_BUILD_GNUABI`),
  nor the Linux one for macOS.

## Building

LLVM's libraries first, once for each system, with AArch64's code generator alone, the one the recompiler uses
(about 10 minutes on an M1). `scripts/apple-llvm.sh` builds them, and the CI caches them:

```sh
scripts/apple-llvm.sh macos ~/apple-llvm/macos
scripts/apple-llvm.sh ios ~/apple-llvm/ios
```

macOS, then the app:

```sh
cmake --preset macos -DLLVM_DIR=$HOME/apple-llvm/macos/lib/cmake/llvm -DSNX_MOLTENVK=$HOME/moltenvk-macos
cmake --build --preset macos
cpack --config build/macos/CPackConfig.cmake -B out   # out/ScarletNX-macos-arm64.zip, ScarletNX.app in it
```

`SNX_MOLTENVK` is the release's `MoltenVK-macos.tar`, extracted (`MoltenVK-ios.tar` for iOS). CPack installs the app
into its own folder, signs it ad hoc and zips it (`-G DragNDrop` makes a disk image instead).
`cmake --install build/macos --prefix out/ScarletNX.app/Contents` makes the same app without the zip. The CI pins the release and
its SHA-256 (`.github/workflows/build.yml`). Homebrew's LLVM works too, with every code generator:
`-DSNX_LLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config"` in place of `LLVM_DIR`.

iOS, then the IPA:

```sh
cmake --preset ios -DLLVM_DIR=$HOME/apple-llvm/ios/lib/cmake/llvm -DSNX_MOLTENVK=$HOME/moltenvk-ios
cmake --build --preset ios
cpack --config build/ios/CPackConfig.cmake -B out     # out/ScarletNX-ios-arm64.ipa
```

`MACOS_VERSION` (default 13.0) and `IOS_VERSION` (default 16.0) are the oldest systems `apple-llvm.sh` builds for,
as the presets' `CMAKE_OSX_DEPLOYMENT_TARGET` are the apps'. FFmpeg, OpenSSL and the recompiler's runtime library are built outside
CMake's compiles and get the SDK, architecture and oldest version from `cmake/apple.cmake`.
