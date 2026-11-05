<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# macOS and iOS

## Index

* [The frontend](#the-frontend)
  * [macOS](#macos)
  * [KosmicKrisp (experimental)](#kosmickrisp-experimental)
  * [iOS](#ios)
  * [Running a game on iOS](#running-a-game-on-ios)
  * [Installing from a Mac](#installing-from-a-mac)
* [What is missing](#what-is-missing)
* [A GPU lockup on MoltenVK](#a-gpu-lockup-on-moltenvk)
* [Building](#building)

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
  the document picker SDL lacks there.

### macOS

`ScarletNX.app` is the desktop program in a bundle: `scarletnx` in `Contents/MacOS`, the themes and the
licenses in `Contents/share`, where an installed copy looks for them. It works as on Linux. The menu extracts and
recompiles through the driver as a child process, with SDL's own file and folder dialogs. Its data folder is
`~/Library/Application Support/scarletnx`. *Share Log* saves the logs' zip in the Logs folder's `share/` and opens it in
the Finder. The app is signed ad hoc, as Apple silicon runs no unsigned code. A copy from a download may need *Open*
from the Finder's menu the first time.

The Vulkan driver is MoltenVK, Ryujinx's build of it ([V380-Ori/Ryujinx.MoltenVK](https://github.com/V380-Ori/Ryujinx.MoltenVK)),
which carries the patches an emulator's pipelines need and mainline MoltenVK lacks. Both apps carry it in their
`Frameworks`, and a game's folder gets that library beside its copy of the runtime (`snx_deploy_runtime`), a second name
where the disk allows one and a copy where it does not, as a read-only app image or one macOS translocated leaves. A
game plays as `<data folder>/recompiled/<id>/<id>`, outside the bundle, where SDL's own search of
`@executable_path/../Frameworks` reaches nothing. The runtime hands SDL the library its own folder holds
(`backend/sdl3.c`), and a run with no window loads the same file without SDL's video. The bare `scarletnx` of a
checkout has no library to deploy and no bundle around it, so it uses the system's Vulkan loader (Homebrew's
`molten-vk` and `vulkan-loader`).

The 3DS and the Vita's windows open through `common/gpu/vk/window.c`, which names the same library to SDL and takes
its own entry points from it. One process must hold one copy of MoltenVK: a second copy's `MVKInstance` is a
different type, so the surface call hands one copy's instance to the other's `vkGetInstanceProcAddr` and the process
segfaults. That is what a Mac with Homebrew's `molten-vk` in the loader's ICD folders saw when volk took the system
loader and SDL took the bundle's library.

### KosmicKrisp (experimental)

A game can run on [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html) instead, Mesa's Vulkan driver on
Metal. Set `vulkan_driver = kosmickrisp` in the game's `.ini`, or start it with `--vulkan_driver=kosmickrisp`. The
option is in Graphics in the launcher's Settings and in the game's menu. The setting also takes `moltenvk`, and `auto`
keeps the MoltenVK the app carries.

The support is experimental. A game may draw wrong or stop on KosmicKrisp, and MoltenVK stays the default until the
support is stable. After that, KosmicKrisp becomes the default on a Mac and MoltenVK the fallback.

Where KosmicKrisp lacks an extension or a feature that the renderer asks for, the runtime takes the path it has for a
driver without it, as it does for the features MoltenVK lacks. The game keeps running on KosmicKrisp, and a run does not
change driver half way. What that costs today: transform feedback is not captured, a sampler with a border colour of its
own takes the nearest of black and white, wireframe draws solid, and the frame pacing falls back to its timer. One game's
title screen has run on it, at the frames a second the game caps itself to.

The app's default run loads its MoltenVK directly, as before, so `VK_DRIVER_FILES` has no effect on it. A checkout's run
uses the system loader, where `VK_DRIVER_FILES` alone selects KosmicKrisp. The setting is the app's way to do the same:
it gives SDL the Vulkan loader and sets the ICD. SDL and volk take that one library.

KosmicKrisp needs an Apple silicon Mac with macOS 26 or later, and the Vulkan loader (`VK_DRIVER_FILES`).
The runtime finds it:

- beside the program, from an app built with `SNX_KOSMICKRISP` (its own folder holds the driver, its loader and the
  ICD JSON, and a game's folder gets the same three from the deploy), else
- in Homebrew's `mesa` (`/opt/homebrew/share/vulkan/icd.d`, or `/usr/local` on Intel), else
- in a LunarG Vulkan SDK under `~/VulkanSDK`.

The loader comes from the same folders, or from `/opt/homebrew/lib/libvulkan.1.dylib` or `/usr/local/lib` when the
driver and the loader are installed apart. LunarG's build is self-contained, and Homebrew's links the prefix's
libraries. Homebrew's build can also need a newer macOS than the SDK's, so the SDK's driver is the one to carry. Do not
put Homebrew's driver in the app. A driver the setting names and the Mac does not have is said in the log, and the
other driver plays instead, so a game still starts.

The setting is the app's, for its Switch games. The 3DS and Vita frontends take the driver through the Vulkan
loader, and a Mac without `VK_DRIVER_FILES` gives them MoltenVK. Their own settings do not name a driver yet.

The app carries KosmicKrisp when the build gets the SDK's macOS folder:

```sh
cmake --preset macos -DLLVM_DIR=$HOME/apple-llvm/macos/lib/cmake/llvm \
    -DSNX_MOLTENVK=$HOME/moltenvk-macos -DSNX_KOSMICKRISP=$HOME/VulkanSDK/1.4.363.0/macOS
```

The app then carries both drivers. The build takes the SDK's `lib/libvulkan_kosmickrisp.dylib`, its
`lib/libvulkan.1.dylib` and the API version of its ICD JSON, and writes an ICD JSON whose library path is relative.

The Vulkan tests run on the driver that the Vulkan loader picks. To run them on KosmicKrisp, name its ICD:

```sh
VK_DRIVER_FILES=/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
    ctest --test-dir build/macos -R '^(3ds_vk_.*|vulkan_.*|vk_.*)$' --output-on-failure
```

They pass on both drivers on a Mac that has KosmicKrisp. The `vulkan_driver` test makes a device through the ICD where
the Mac has one, and checks nothing where it has none, as a machine without mesa or the SDK does.

### iOS

An iOS app runs one process and may not start another, so the iOS app works like Android's game activity without its
menu's driver:

- **The data folder** is the app's Documents, which the Files app (*On My iPhone > ScarletNX*) and the Finder show.
  It holds `dumps/` (a game's `.nsp` or `.xci`), `keys/` (`prod.keys`), `recompiled/` (what the app makes, each game as
  `recompiled/<id>/<id>_code.snxobj` with `_data/`) and `logs/`. The menu's settings, state and caches are in the app's
  `Library/scarletnx/`, which the Files app does not show ([Running a game on iOS](#running-a-game-on-ios)).
- **The menu** lists those games. *Load* and the folder settings open the system's document picker. Under
  LiveContainer, enable its Fix File Picker for ScarletNX: without it, the picker shows the files, but Open gives none
  back. The banner says `host:` when another app runs ScarletNX. Preparing a dump
  runs the driver on a thread of the app, since an app may not start a process. A dump picked outside `dumps/` is
  copied into it first. The thread has a 64 MiB stack, which LLVM's passes need.
- **A game** runs in the app's process once the menu closes (`src/nx/runtime/entry.c`): the runtime starts once per
  process, so the app ends with the game.
- **Share Log** asks how, then saves `scarletnx-logs-<date>.zip` or `scarletnx.log` in `logs/share/` of its Documents
  and opens that folder in the Files app, or opens `logs/`.
- **Logs.** Everything the app prints goes to the day it starts on's `logs/scarletnx-console-<date>.log` in its
  Documents (the 14 newest days kept), after a banner with the
  device, the system and the limits the app has, with the menu's own lines ([GUI](GUI.md)), and a fatal error, the
  menu's or a game's, is also written to `logs/scarletnx-error.log`. A phone shows no console, so these are what to read
  when a run fails; Share Log sends both. The banner also gives the disk space free for the app, and the menu's start
  says if the last start ended without an exit of the menu or of its game: a crash, the memory limit, or a close from
  the app switcher. `logs/.running-<pid>` is the mark ([GUI](GUI.md)).
- **More logs.** Debug's *Log level* at 2 adds these lines to the launcher log, on iOS and on macOS where the system
  has them. `picker:` is each step of the document picker: its open, its answer, the item and its security scope. An
  open with no answer after it is a pick that iOS did not end. `lifecycle:` is the app going to the background and
  back, and the low memory warnings, on iOS, from the menu's start to the end of the process. `run:` and `recompile:`
  are each start and end, with the status, the time and the disk space free, and on iOS the app's memory. On iOS a
  game gets a start line only: it runs in the app's process once the menu closes, and its end ends that process, so
  the next start's banner tells if it ended with an exit. `state:` is each change of
  the thermal state and of the low power mode. `folders:` lists `dumps/`, `keys/` and `recompiled/` at the start, by
  name and size, and the folders the player added. `dialog:` is the answer of each picker, or the error of SDL. The
  banner always gives the app's bundle and the program of its process. When the program is not in the bundle, a host
  such as LiveContainer runs the app, and the banner says so in a `host:` line. In the lines of this level and in the
  bundle and program of the banner, a path in the home folder starts with `~`.
- **MetricKit.** The app subscribes to MetricKit at its start. Each payload that the system gives (a diagnostic
  payload with crashes, hangs, CPU and disk write exceptions, or a metric payload with the counts of exits for memory)
  goes as JSON to `logs/metrickit-<date>_<time>.json`, named by the end of the time that it covers. The console log
  gets one line for each payload, with its file (or the file and why it was not written) and what it holds. The
  payloads that the system gave before this start are read again at each start, and a file that already holds one is
  not written again.

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
about 13 GiB of address space and about 3 GB of memory, so the runtime takes the largest free run it can (512 GiB
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
  needs to run it. A Mac loads one and a game runs; MoltenVK still loses the GPU in some scenes (see
  [A GPU lockup on MoltenVK](#a-gpu-lockup-on-moltenvk)).
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

## A GPU lockup on MoltenVK

Super Mario Bros. Wonder's opening cutscene sometimes stops the GPU on macOS, and the device is lost. The screen stays
black with the loading flower, and Metal reports `VK_TIMEOUT ... Caused GPU Hang Error`, or
`VK_ERROR_OUT_OF_DEVICE_MEMORY`. The `gpuEvent` report that macOS writes for the process gives the mechanism:
`restart_reason_desc` is `firmware-detected lockup`, and `guilty_dm` is 2 or 3. It is not a page fault, and it is not
a shader that fails to compile.

The route from a fresh save repeats it: L+R at the title, A at *Select a User*, A for one player, A on the character,
then A to start. The game reads `OpeningDemo_*` and `Env_Grassland.bwav`, uploads about 80 MB of texture, and the
first frame of the cutscene follows. The fault depends on timing, so a rate needs many runs:

| build (M5, macOS 27.2, MoltenVK 1.4.1, headless, 45 s, same route) | runs that lost the device |
| --- | --- |
| `main` | 6 of 12 |
| a set of 16 textures a stage | 1 of 8 |
| that, and compute's set too | 2 of 8 |
| a descriptor set for each shader, graphics and compute | 1 of 2 |
| the same, with `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` | 7 of 8 |
| KosmicKrisp (Mesa 26.2.6) | 0 of 4 |

The frames render correctly in every build above: no draw was skipped, and the screenshot shows the cutscene. The
smaller sets only change the timing.

What these measurements rule out:

- **A set past Metal's limits, and MoltenVK's Metal argument buffers.** A set of only the shader's own bindings does
  not help, and argument buffers off does not help once no draw is skipped. The earlier run with argument buffers off
  that lost nothing was the one where about 2 800 draws were skipped: their pipelines could not be made with a set
  past the limit.
- **Guest compute.** `compute=0` still lost the device (4 of 8).
- **The recording thread.** `vk_record=0` still lost it (#820).
- **A memory barrier alone.** A full barrier after every draw, one at the end of every pass, and a wait at the present
  all failed (#820).
- **Memory pressure and fewer command buffers in flight** (#820).

The evidence points at the order between draws, but no barrier test has removed the fault yet. #820 gives the likely
place: the renderer's barriers name the `Surf` a descriptor came from, and a later draw can read or write the same
image through a crop, another view or a guest alias, which is a different Metal resource than the one the barrier
named. Metal then runs the two together. The missing order is not yet found.

PR #1206 adds `vulkan_driver=kosmickrisp`, so the same route can be run on Mesa's KosmicKrisp, whose Vulkan side has
none of these limits. It is the driver to compare against while this is open.

A device loss from the recording thread also used to hang the process: `submit_run` called `fatal()`, and the window
release waits for the recording thread in `vk_rec_sync`, so the thread waited for itself and the process spun at full
speed instead of showing the error. `vk_rec_sync` now returns on the recording thread
(`src/nx/gpu/vk/record.c`).

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
