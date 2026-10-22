<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Usage

## Index

* [Extract → AOT → run](#extract--aot--run)
  * [Updates and DLC](#updates-and-dlc)
  * [NRO applications](#nro-applications)
* [GDB (Linux)](#gdb-linux)
  * [GDB through the MCP server](#gdb-through-the-mcp-server)
* [Headless Vulkan / lavapipe](#headless-vulkan--lavapipe)
* [AppImage / display](#appimage--display)
* [Folders](#folders)
* [Users](#users)
* [Setup](#setup)
* [Settings / input](#settings--input)
  * [On-screen controls](#on-screen-controls)

Setup: [BUILD](BUILD.md). Checkout CWD; own dumps/keys.
For the keyboard-controlled SDL3 menu and file drops, see [GUI](GUI.md).

## Extract → AOT → run

Pass a single XCI, NSP, DXCI or DNSP to recompile and play:

```sh
./build/scarletnx "Game.xci"
```

The driver runs each step as a child process and starts the game with its folder, `recompiled/<id>/`, as its working
directory. The game is named by its title ID, read from the dump's NCA headers, an NRO by its name (a dump without a
title ID of its own by its stem); `recompiled/` is the data folder's, from a build tree too ([Folders](#folders)).

Nothing of the dump is extracted by default. The game's `.ini` records the dump's path as `source=` under `[Game]`, and
the recompile reads the ExeFS's modules from the dump itself. When the game runs, the runtime maps the dump read-only
from that path and reads its ExeFS and RomFS out of it on the fly, decrypting what is encrypted, with the keys
where the driver finds them (the data folder `recompiled/` is in, else `~/.switch`). No copy or cache of those files is
kept anywhere: `recompiled/<id>/<id>_data/` holds only the game's `control.nacp` and its saves. An update embedded in an
NSP is applied over the base as it is read. Moving or deleting the dump breaks the game until it is back, or until
`source=` names its new place, and the run says so. A game made by an older version has `<id>.source` in its folder
instead, a link to the dump, which is still read when the `.ini` names no dump. Playing the dump again records it in
the `.ini`.

`--extract-container` (the launcher's **Extract dump**) extracts the dump instead, straight into the game's
folder, `recompiled/<id>/<id>_data/` (`exefs/`, `romfs/`, `control.nacp`), and the game is made from it there: there is no
second copy. `<id>.extracting` stays in the game's folder while an extraction is unfinished, and a stopped one is redone
on the next run, the game's saves kept. A game made from its dump is extracted on the first run with the option, with
no recompile, and a game run without it again has that extraction taken away, as long as the dump it records is there.
`--data DIR` and `--build-dir DIR` may precede the dump. Keys use the usual discovery paths below.
`--outdir DIR` puts the game's folder in DIR in place of the data folder's `recompiled/`, as `-r` takes it. With
`--extract-container`, `--out DIR` keeps the dump's extraction at DIR instead, as `-x` takes it, the game made from a
copy of it; an NRO has no extraction to name.
`--tmpfs` extracts to tmpfs instead, and implies `--extract-container`: `<temp>/scarletnx/work/<title ID>` (`$TMPDIR`,
else `/tmp`; the temporary folder on Windows), where the game reads its RomFS from while its ExeFS and saves stay in
`recompiled/<id>/<id>_data/`. The game's folder gets `<id>.tmpfs` while its RomFS is in tmpfs. A run extracts the RomFS again
from the dump the game records when the system emptied tmpfs, and moves it between tmpfs and the game's folder when
`--tmpfs` is given or left out. A game that records no dump says *Can't be extracted to tmpfs: the game's settings name
no dump it was made from*. `-r --tmpfs` on an extraction leaves its RomFS where it is.
`-H` (`--headless`) can precede or follow the dump and is passed to the game only.
Omitting it starts the game with a window.
`-x` means `--extract`, and `-r` means `--recompile` (`--recomp`, its older name, still works and says so once).
On UNIX, short flags can be combined. Windows accepts separate short flags only.

To prepare a dump without playing, use `./build/scarletnx -r "Game.xci"`.
This forces recompilation, even if a cached executable exists, and extracts nothing unless `--extract-container` is
given. `scarletnx -r --outdir games -- "Game.xci"` recompiles straight from a dump the same way, reading its ExeFS in
place. `-rx` (`-r -x` on Windows) prepares it extracted, as `-r --extract-container` does. `-x` alone only extracts.

A game made already plays the same way, named by its folder or its program: `./build/scarletnx recompiled/<id>` (or
`recompiled/<id>/<id>`) runs it, recompiled first where it is when a build of another ABI made it, from what its `<id>_data/`
keeps (the ExeFS or the NRO: no dump, keys or extraction), or from the dump its `.ini` records when it was
made without extracting it. `-r` recompiles it without playing. `--outdir` and `--out`
name a dump's folders and are refused for it. `--game=recompiled/<id>` runs it as it is, whichever build made it.
Pass arguments to the game executable after `--`, for example:

```sh
./build/scarletnx "Game.xci" -- some arg
```

The argument boundary is retained when launching the deployed game, so game arguments
cannot trigger launcher actions or overwrite runtime settings. This does not add a
homebrew-loader guest argv block.
`-f` (or `--fullscreen`, `--fullscreen=1`) can also precede or follow the dump and is forwarded to the game,
which then starts fullscreen (not in timed test runs). `--fullscreen=0` starts it windowed. Without either, the
game's `.ini` decides (`fullscreen=1` starts fullscreen).
`--touch=0|1|2` is forwarded to the game too: its touch screen takes nothing, the fingers, or the fingers and the
mouse's left button (the default). The game's menu sets it too; the game's `.ini` keeps its own `touch`.
`--controls=0|1` likewise hides or shows the [on-screen controls](#on-screen-controls) (on by default on Android);
the game's `.ini` keeps `controls`. Any other `--key=value` of the game's
`.ini` is forwarded as well, for that run (`scarletnx --settings` lists them all), and so are the timed-run ones
(`--seconds=`, `--shot=`...); any other key stops the run with `unknown option`.
The game's own executable also takes `-v`, the same as `--log=2`.
LLVM is the only backend. `--backend=llvm`, which older scripts give, is accepted and changes nothing, and any
other `--backend` stops with an error.
Use the separate commands for other compiler options.

DXCI and DNSP with decrypted content need no keys. Containers are identified by their headers,
including renamed files. A matching bundled update is applied automatically. DLC-only and update-only dumps cannot
be used as a base game.

```powershell
.\build\scarletnx.exe "C:\Games\Game.dxci"
.\build\scarletnx.exe --extract "C:\Games\Game.dnsp" --update "C:\Games\Update.dnsp"
```

### Updates and DLC

An update or a DLC is recorded beside its game, in the game's folder, and checked in the game's list of updates and
DLCs, `addons=` in the game's `.ini` (their title IDs, a comma between two). The game plays with what that list
checks: any number of DLCs and one update at most. Give their dumps with the game's, or alone once the game is in
`recompiled/`:

```sh
./build/scarletnx Game.xci --update Update.nsp --update Dlc.nsp   # the game made first if it isn't yet
./build/scarletnx Dlc.nsp                                          # a game made already: its DLC checked too
```

Each dump given is checked along with what the list checks already: a DLC is added, and an update, older or newer,
takes the place of the one checked before, as installing an older update means playing it. The launcher lists a
game's updates and DLCs under it: opening a game that has some shows its updates as a choice of one (each version
installed, the newest first, by its version from its own control data, or No update) and its DLCs as boxes to check
(`DLC 1`, numbered from the base ID the game's control data gives). Save keeps the choice in `addons=` without
playing, Play keeps it and plays the game, and Back leaves the list as it was.

- **Programs:** an update is recompiled from its own ExeFS into a program beside the game,
  `recompiled/<id>/<id>_<its title ID>v<its version>`, which runs whenever it is checked. Every version of a game's update
  has the same title ID, so each version installed keeps a program of its own, its version its content meta's. A DLC runs its game's code and has no program,
  only its record: its own `.ini`, which holds `source=` (the path of its dump), and its data folder. A DLC program
  an older version made is taken away the next time its dump is played.
- **Settings and saves:** an update runs with its game's `.ini` and its game's data folder, so its saves are the
  game's.
- **Files:** the game's own program never reads an update's files. Without Extract dump, an update reads
  its ExeFS and RomFS from its dump, its RomFS patch read over the game's dump (the game's `.ini` must name it, which
  playing the game's dump records). Each checked DLC's RomFS image is read from its dump and served as installed. A
  checked DLC whose dump is not there is left out, and the log says so. With Extract dump, an update is
  extracted into its own data folder with its RomFS applied, and a DLC's image goes to
  `system/archives/<its title ID>.romfs` in its own. Those folders lie over the game's: an update's ExeFS, RomFS and
  control data replace the game's, a DLC's `system/` is added to the game's, and nothing else of theirs is used.

`-x` keeps extracting an update over its game into one folder (`-x Game.nsp --update Update.nsp`), and extracts a
DLC's dump given alone as its RomFS image.

### Mods

A game's mods go in the `mods` folder of its data folder, one folder each, named as you like:
`recompiled/<id>/<id>_data/mods/<mod name>/exefs/` and `.../<mod name>/romfs/`. A mod lies over the game the way an update
does, and over the update too when one is checked. A file in its `romfs/` replaces the RomFS file at the same path, or
is added to the RomFS when the game has none there. A file in its `exefs/` (`main`, `subsdk0` and the other modules,
or `main.npdm`) replaces the program's file of that name. Mods are laid in the order of their names, so a later one
wins over an earlier one where both have the same file.

Every mod in the folder is played until it is turned off. Opening a game in the launcher lists its mods under its
updates and DLCs, each with a box to check, and Open Mods Folder opens the folder in the file manager (it is made
first when it isn't there). M on Games opens that box for the game in focus, one with nothing beside it too. The mods
turned off are kept in `mods_off=` in the game's `.ini`, their folder names with a `/` between two.

Add Mod, in the same box, opens the system's folder picker and copies the folder picked, whole, into the mods folder
under its own name, the original left where it is; the mod shows checked at once, and the message says where it went.
The copy goes into a hidden `.<name>.adding` first and takes the mod's name once every file is in, so a copy that
stops never shows as a mod. It refuses, naming the folder: a name the mods folder has already, a folder inside the
mods folder or one that holds it, one with neither `exefs/` nor `romfs/`, a name that starts with a dot (the mods list
hides those), a link inside the folder, and on a phone a folder of an app or a cloud that no path reaches; a file that
cannot be copied is named with the system's reason. On Android and iOS the box has Add Mod and no Open Mods Folder or
Open Cheats Folder, as a phone has no file manager to show a folder in.

A mod's `exefs/` changes the game's code, which ScarletNX compiles ahead of time. The game's stamp (`<id>.abi`) records
the mods' ExeFS files it was recompiled with, and playing it through the driver or the launcher recompiles it when the
mods played have other ones. Run directly, a game made with other mods' files runs the ExeFS it was made from, and the
log says so.

A mod's `exefs/` can also hold patches, each made for one build of a module: an IPS or IPS32 file named after the
module's build ID (`exefs/<build id>.ips`), and IPSwitch text (`exefs/<name>.pchtxt`, the build ID after its
`@nsobid-`), read as Eden reads them. Each is written over the module it is for when the game is recompiled, and over
that module's segments as the game loads them. A patch that does not fit its module (a record past its end, a line
the format has no meaning for) stops the recompile, which names the file, the line and the value. Patches count in
the stamp as the mods' ExeFS files do: turning a mod with one on or off, or changing one, recompiles the game. Cheats
in `exefs/` are not applied.

A game's cheats go in the `cheats` folder of its data folder, in the Atmosphere format, a file named after the main
module's build ID: `recompiled/<id>/<id>_data/cheats/<build id>.txt`, each cheat a `[name]` line and its codes under it.
Every cheat in it runs once per frame shown until it is turned off. Opening a game in the launcher lists its cheats
under its mods, each with a box to check, and Open Cheats Folder opens the folder (made first when it isn't there).
The cheats turned off are kept in `cheats_off=` in the game's `.ini`, their names with a `|` between two; a cheat with
a `|` in its name always runs, and the launcher says so.

Add Cheats, in the same box, opens the system's file picker for a `.txt` and copies it into the cheats folder as
`<build id>.txt`, the only name the runtime reads; the original stays where it is, and its cheats show checked at once.
A file already named by a build ID (16 hex digits) keeps that name, in capitals, and is refused when the game plays
with another, the message naming both. Another takes the build ID of the main module the game plays with as the box has
it: a played mod's `exefs/main`, else the checked update's own (from its extraction: one read from its dump has none
the launcher can read), else the game's `exefs/main` or `main.nro`; with none to read, the file is refused, the message
naming the module it looked for. It refuses too a name the cheats folder has already (files are never merged), a file
that is not a `.txt`, one over 1 MiB, one with no `[name]` line, and on a phone a document of an app or a cloud that no
path reaches.

Run the same command again to retry after a failure or interruption:

- `work/Game/` skips extraction. Automatic extraction publishes this directory only after success,
  using `work/Game.extracting/` while running. An incomplete staging directory is discarded on retry.
- A complete `recompiled/<id>/<id>` (`<id>.exe` on Windows), code module and `<id>.abi` stamp naming this build's
  ABI skip recompilation. The stamp holds `SNX_ABI`: a game a build of another ABI made is recompiled
  automatically, as is one with a missing or invalid stamp, and that recompile takes from the unit cache (below) every
  code unit whose code is the same. One another build of the same ABI made keeps its code,
  and its `recompiled/<id>/<id>` is replaced by this build's runtime when the two differ, so a newer build's fixes reach
  it. Failed builds never publish a valid ABI checkpoint. A change to the recompiler that keeps `SNX_ABI` isn't
  seen: there `-r` recompiles.
- Changing directory and running the game happen on every launch. A game failure never invalidates the build.

Failures stop the workflow and print the failed stage and exit status or signal. On POSIX, stopping a
child also aborts that stage so it can be retried, except for intentional debugger stops. Uncatchable termination of the driver itself, such as
`SIGKILL`, cannot print a diagnostic. Do not run concurrent workflows for the same output name.
Existing manually extracted directories are assumed complete. Remove stale checkpoints or use the
explicit commands when changing a dump or rebuilding game code.

### NRO applications

Pass an uncompressed NRO directly to recompile and play, without keys or extraction:

```sh
./build/scarletnx "app.nro" -H
```

The `.nro` extension is case-insensitive. NRO launches always recompile and restage the input,
so a same-named cached executable or extraction directory cannot silently select an older program.
Embedded RomFS assets are staged by the recompiler. Compressed NROs are not supported.
This uses ScarletNX's existing guest runtime, not a complete homebrew-loader environment.
Compatibility still depends on the application's runtime and service requirements.
See [NRO implementation notes](NRO.md) for the reference and validation policy.

To run the steps separately, here in folders of the current one (`--out` and `--outdir`; without them, the data
folder's `work/` and `recompiled/`):

```sh
./build/scarletnx -x "Game.xci" --out work/Game
./build/scarletnx -r work/Game Game --outdir recompiled
recompiled/Game/Game
```

```powershell
.\build\scarletnx.exe -x "Game.xci" --out work/Game
.\build\scarletnx.exe -r work/Game Game --outdir games
.\games\Game\Game.exe
```

Remaining examples: POSIX.

```sh
# NSP + same-title update
./build/scarletnx -x Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys

./build/scarletnx -r work/Game Game --outdir games --jobs 8
./build/scarletnx -r app.nro --name App --outdir games
./build/scarletnx -r -h
```

- **Keys:** encrypted content uses `--keys`/`--title-keys` flags, then the data folder ([Folders](#folders)) → `~/.switch`
  (`%USERPROFILE%\.switch` on Windows).
  Common NSP tickets supplement title keys. Fully decrypted content needs neither key file.
- **Extraction:** defaults to the data folder's `work/<dump stem>`; `--name`/`--out DIR` override. Refuses existing
  `exefs`. hactool runs inside the driver and reads each NCA where it lies in the XCI or NSP, so the ExeFS and RomFS
  are written straight into the output and nothing is unpacked beside it
  ([key filtering and cleanup](../src/nx/runtime/driver/extract.c)).
  Sparse and compressed NCAs, updates with large relocation tables and gamecard dumps that start with the card's key
  area all extract, through what [`storage.c`](../src/nx/runtime/driver/hactool/storage.c) ports from LibHac.
- **Recompile:** AOT → module (LLVM) →
  final runtime deployment, for this machine's CPU. Shaders compile directly to SPIR-V at runtime.
  `-O 0..3,s,z` (`--opt=`) sets the optimization level (2 by default), and `--jobs N` (`--aot_threads=N`) the threads
  compiling (0 or none, one for each core). Every pass of LLVM's pipelines can be turned off for the game's code,
  `--llvm_pass_<pass>=0` (`--llvm_pass_instcombine=0`), and each option LLVM's C API sets on the pass builder and the
  target machine set, `--llvm_<option>=N` (`--llvm_inliner_threshold=50`). `scarletnx --settings=recompile` lists
  them, as the launcher's LLVM Internals page shows them: the passes are those of the pipelines the build's LLVM prints
  for each level, and a build without an `opt` of that LLVM has none. A dump played with these is recompiled with them
  when it has to be, and a game made already keeps its code until `-r` recompiles it. A value a setting does not take
  (`--aot_threads=abc`) stops the recompile. The code the runtime compiles while the game runs keeps LLVM's own, but
  for the SLP vectorizer, which `slp_vectorizer` turns on or off for both.
  `--target auto|linux|win64` takes only this host's own system: modules are built for the machine that builds
  them. `--target ios` writes `<id>_code.snxobj` instead, a pack of the code's objects for the iOS app to load into JIT
  memory (64-bit games only, [APPLE](APPLE.md)); it makes no runtime copy. `--runtime FILE` deploys that runtime instead of the running one, or of the one a tools-only build builds in
  its checkout's `build/native-recomp` (`build/win64-recomp` on Windows). `-q` (`--quiet`) prints no progress.
  `--keep-c` is the deprecated spelling of `--keep-ir`.
- **Codegen:** `--no-build` stages data/INI only; `--jit-only` omits AOT functions.
  `--chunk N` sets instructions per code unit. Each unit's object is kept in `units/` of the cache folder
  ([Folders](#folders)), which every game's recompile shares, under the SHA-256 of the unit's LLVM IR, the LLVM, the
  CPU and its features, the optimization level and the LLVM Internals: a unit found there is not compiled again (a
  recompile after an `SNX_ABI` change that leaves the unit's IR alone, or of a game whose code changed in a few units),
  and the last line says how many were (`unit cache: 241 of 259 units reused (310.2 MB) from ...`). The module
  descriptor and the link are made each time. A kept object that is not whole is compiled again, and said. The cache
  always leaves a tenth of its disk free: when a new unit would take that, the units reused longest ago are removed
  for it, and a unit there is still no room for is not cached (said once, with the disk's free bytes, and how many
  units at the end; the units removed for room are summed there too). `--unit_cache_gb=N`
  (0, no limit, by default) also holds the folder to N GB after each recompile, the units reused longest ago removed
  first. A cache folder that cannot be made stops the recompile, saying why.
  `--keep-ir` saves the generated code (LLVM IR) in `<outdir>/<NAME>/<NAME>_build/`.
  `--hook-direct-calls` sends calls and branches between guest functions through the dispatch table too, so
  `nvn_trace=1` also counts calls made by direct branch. The game runs slower, so use it only for investigating.
- **Data updates:** ExeFS refreshes; `<id>_data/romfs` is reused.
  Replace cached RomFS after asset changes; preserve `<id>_data/save/`.

With `--out work/Game` and `--outdir games` as above (without them, see [Folders](#folders)):

| Output | Contents |
| --- | --- |
| `work/Game/{exefs,romfs}` | Extracted input |
| `recompiled/<id>/<id>[.exe]`, `<id>_code.so/.dll` | Runtime + AOT module |
| `recompiled/<id>/<id>_data/`, `<id>.ini` | Data/saves (and `control.nacp`) + settings |
| `recompiled/<id>/<id>_build/` | Generated code, only with `--keep-ir` |
| `recompiled/<id>/shader_cache/vk_pipelines.bin` | Vulkan pipeline cache |
| `recompiled/<id>/shader_cache/<id>.vkshaders` | Translated shaders and the pipelines made, made again at the next start |
| `recompiled/<id>/shader_cache/<id>.vkshipped` | The shaders found in the game's own files and executables, prepared ahead of time (`prepare_shaders`), each read when the game first draws with it. It can be large: one game's 2,700 shaders took 245 MB, another's 18,500 took 2 GB |

## GDB (Linux)

Install `gdbserver` on PATH (and `gdb` for the MCP tools below). `-S` enables the native-code stub on loopback port **1234**
(the QEMU default port). The game runs until a debugger connects.
`-s` enables the stub and waits before runtime initialization. It implies `-S`.

```sh
./build/scarletnx -s "Game.xci"
# Or: recompiled/<id>/<id> -Hs
```

In GDB, load the deployed executable's symbols and connect:

```gdb
file recompiled/Game/Game
tar rem :1234
continue
```

The launcher prints a connection hint before waiting. A separate helper keeps accepting
connections while the game is stopped, then delegates the remote protocol to `gdbserver`.
The stub debugs host/native registers and code, not an emulated ARM64 CPU. It supports one
connection per run. Local ptrace permissions must allow attachment. Binding failures and
missing `gdbserver` are reported instead of silently running without a debugger.
No debugger flags or combined short flags are supported on Windows.

The recompiled game module is a shared library with a symbol for each function, so GDB names game
frames as it loads. A 64-bit game's functions are `F_<address>`, and a 32-bit game's blocks are
`A_<address>` (A32) and `T_<address>` (Thumb). Code the JIT compiles at run time lives in an
anonymous code arena. With `--gdb_jit=1` (the `[Debug]` setting `gdb_jit`) and the stub (`-S` or
`-s`), the runtime hands each compile to GDB through its JIT interface, with its symbols. A 32-bit
game's are `a32_<address>_<thumb>`. GDB stops the whole game to read each compile while it is
attached, and again for each at the game's end. That slows a 32-bit game (its blocks compile one
at a time) to a crawl, so it is off by default. Without the stub the setting does nothing.

The runtime handles SIGSEGV and SIGBUS itself: memory it moves while a thread writes to it stops
that thread on a fault, and a crash's fault writes the crash report. Tell GDB
`handle SIGSEGV SIGBUS nostop noprint pass` and break on `crash_report` to stop only at a crash.
A SIGSEGV or SIGBUS another process sends (`kill -SEGV`, or GDB's `signal SIGSEGV`) is not a
crash. The runtime prints a line saying so and runs on.

### GDB through the MCP server

An agent can drive GDB through the MCP server (`scarletnx --mcp`). It needs `gdb` (9 or later)
on PATH as well as `gdbserver`, and a runtime with its debug information (the default
RelWithDebInfo build, not one built with `-g0`).

- `gdb_attach` connects to a game started with `-S` or `-s` and stops it. It sets the `handle`
  line above and a breakpoint on `crash_report`. Given the game (a folder, a game of `work/` or a
  title ID, of the same version), the other tools name guest addresses and the symbols above by
  the game's own symbols. The stub takes one connection per run, so a second attach is refused.
- `gdb_command` runs one GDB command from a list of reading, breakpoint and execution commands
  (`backtrace`, `info`, `print`, `x`, `frame`, `thread`, `break`, `watch`, `set var` and the
  like), by GDB's names, aliases and unambiguous prefixes. Shells, scripts, file writes, command
  bodies (`commands`, `define`, `if`, `while`) and session changes are refused. `print` and
  `call` still run the game's own functions. A resume (`continue`, `next`, `step`, `finish` and
  the rest) runs in the background and waits up to `wait` seconds for the game to stop at a
  breakpoint or a crash, then interrupts it. With `wait` 0 the game runs on, and only
  `interrupt` is taken until it stops.
- `gdb_threads` lists each host thread, and each guest thread's id, its SDK name, whether it is
  in an SVC and its guest pc and lr. It finds a guest thread through its host thread's TLS, which
  GDB reads with the C library's `libthread_db`. Without it (a musl build) every thread is listed
  as the runtime's.
- `gdb_guest` shows a guest thread's registers and its guest backtrace from its frame records.
- `gdb_memory` reads guest memory. A page the GPU holds can't be read until the game's CPU
  touches it.
- `gdb_detach` lets the game run on without the debugger.

## Headless Vulkan / lavapipe

The normal SDL3 build can run without initializing SDL video or opening a window:

```sh
./build/scarletnx --game=recompiled/Game -H --seconds=30 --shot=/tmp/frame
# Or extract, recompile and launch a dump without a window:
./build/scarletnx "Game.xci" -H
```

`-H` enables it. Omit the flag for the normal window. `--headless` is the long spelling
and accepts no value (`--headless=0` and `--headless=1` are errors).
This is a startup option, not a live toggle or an INI setting. SDL3 is required by every NX runtime
build, but headless runs do not initialize its video subsystem or require a display server.

Rendering still uses Vulkan 1.3, including uploads, draws, compute and screenshot readback. Headless runs
create no window, `VkSurfaceKHR` or swapchain and request no WSI extensions, including `VK_EXT_headless_surface`.
Frames still submit and recycle their resources. `--seconds`, `--frames`, `--script` and `--shot` work as before.
The time and frame limits and the script count from the game's start, after the shaders made before it
(`prepare_shaders`).
FFmpeg decoding is always included in the NX runtime, including headless builds, for video and hwopus audio.

To use software rendering, select Mesa's **lavapipe Vulkan ICD**, rather than the hardware driver. Mesa may
report this device as `llvmpipe` in the Vulkan log. Selecting a CPU ICD is independent of `-H`:

```sh
# Linux example. Substitute the actual lvp ICD manifest installed on your machine.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/scarletnx --game=recompiled/Game -H --seconds=30 --shot=/tmp/frame
```

Manifest names vary (for example `lvp_icd.json`). FreeBSD packages normally install manifests under
`/usr/local/share/vulkan/icd.d/`. Older Vulkan loaders use `VK_ICD_FILENAMES` instead of `VK_DRIVER_FILES`.
`LIBGL_ALWAYS_SOFTWARE` is an OpenGL setting and is not how this Vulkan renderer selects lavapipe.
No display server is required. Check the `vulkan:` device line in the log to confirm the chosen ICD.

The NX runtime includes SDL3 even when running headless; headless mode skips video initialization.

## AppImage / display

```sh
chmod +x ScarletNX-linux-x86_64.AppImage
./ScarletNX-linux-x86_64.AppImage --game=recompiled/Game
SDL_VIDEO_DRIVER=x11 recompiled/Game/Game
```

On an ARM64 machine the image is `ScarletNX-linux-aarch64.AppImage`, on PowerPC64 LE `ScarletNX-linux-ppc64le.AppImage`
([PPC64](PPC64.md)).
`--game` selects the game's folder, its `Game_code.so` and `Game_data/`; or name the image `recompiled/Game/Game.AppImage`
to load the files beside it.
The AppImage also prepares dumps: `./ScarletNX-linux-x86_64.AppImage Game.nsp`, in the folder [Folders](#folders) names,
and each game gets a copy of the AppImage (on Windows, of `scarletnx.exe` and the DLLs next to it).
X11 bypasses Wayland. [Licenses](CREDITS.md).

## Folders

The driver works in one folder, its data folder, the first of these that applies:

1. the one `--data DIR` names (the menu names its own);
2. a portable copy's: `.scarletnx/` beside the AppImage, or beside the Windows zip's programs, when it is there;
3. the user's data folder: `~/.local/share/scarletnx` on Linux, `%APPDATA%\scarletnx` on Windows,
   `~/Library/Application Support/scarletnx` on macOS. On Android the app names its own, `.scarletnx/` in its own
   storage ([ANDROID](ANDROID.md)).

A driver run from its build tree works in the same folder as an installed one: the checkout it was built from is
not a data folder. `--data DIR`, or `--outdir DIR` for a recompile, keeps a run's files in one folder of your choice.

Each game is a folder of `recompiled/` named by its title ID, `recompiled/0100DA801624E000/` and its files
`0100DA801624E000*` in it; a program without an ID of its own (an NRO, an ExeFS without NPDM) by its name, and
`--name` names any. The game is shown by the name in its `control.nacp` (the window, the recompile, the menu's
list). The user's data folder and a portable copy's keep each kind of file apart: on Linux the user's in the
[XDG](https://specifications.freedesktop.org/basedir/latest/) folder of its kind, elsewhere in its own `config/`,
`state/` and `cache/`. Any other folder `--data` or `--outdir` names keeps everything in its games' folders.

| What | Another `--data`, an `--outdir` | Linux, the user's data folder | A portable copy's, the user's elsewhere |
| --- | --- | --- | --- |
| a game: its program, `<id>_code.so`, `<id>_data/` (saves, `control.nacp`), `captures/` | `recompiled/<id>/` | `~/.local/share/scarletnx/recompiled/<id>/` | `recompiled/<id>/` |
| its settings, `<id>.ini` | `recompiled/<id>/` | `~/.config/scarletnx/recompiled/<id>/` | `config/recompiled/<id>/` |
| its `<id>.log` and `threads.txt` | `recompiled/<id>/` | `~/.local/state/scarletnx/recompiled/<id>/` | `state/recompiled/<id>/` |
| its pipeline cache | `recompiled/<id>/shader_cache/` | `~/.cache/scarletnx/recompiled/<id>/shader_cache/` | `cache/recompiled/<id>/shader_cache/` |
| extractions (`3ds/` for the 3DS driver) | `work/` | `~/.cache/scarletnx/work/` | `cache/work/` |
| the recompile's code units, `<key>.o`, every game's | `units/` of the user's cache folder | `~/.cache/scarletnx/units/` | `cache/units/` |
| the menu's themes, `<name>/theme.ini` | - | `~/.local/share/scarletnx/themes/` | `themes/` |
| the menu's `settings.ini`, `app.ini`, `folders.txt`, `gamecontrollerdb.txt` | - | `~/.config/scarletnx/` | `config/` |
| the setup's `setup.ini` ([Setup](#setup)) | - | `~/.config/scarletnx/` | `config/` |
| the menu's `recent.txt`, `last-run.log` | - | `~/.local/state/scarletnx/` | `state/` |
| the menu's Share Log, `scarletnx.log` | - | `~/.local/state/scarletnx/` | `state/` |
| `prod.keys`, read before `~/.switch` | the folder | `~/.local/share/scarletnx/` | the folder |
| the console's users, `users.ini` and `<uid>.jpg` ([Users](#users)), and `online.ini`, an Eden room's password (`room`) | `users/` of the user's data folder | `~/.local/share/scarletnx/users/` | `users/` |

The menu works in the user's data folder or a portable copy's, unless its Settings > App > Folders picks a data
folder, a games folder or an extractions folder ([GUI](GUI.md)): it passes them to the driver as `--data`, `--outdir`
and `--out` (an extractions folder picked keeps a Switch dump's extraction there, the game made from a copy; to the
3DS driver as `--data` and `--output`), and its own files stay where they are. Its Logs and Caches move the state and
cache folders of the table: `[folders] state =` and `cache =` in `app.ini`, full paths, which the menu, the drivers
and the games read (a game's `.folders` below names them), its files going to the same place under them
(`recompiled/<id>/<id>.log` of the state folder); a value that is not a full path is said once and the default kept.
The config folder holds `app.ini`, so it moves only with `$XDG_CONFIG_HOME` or a portable copy. On Android they
are in its data folder's `config/` and `state/`, in the app's own storage, which needs no permission. `~/.local/share`, `~/.config`, `~/.local/state` and `~/.cache` are the
defaults of `$XDG_DATA_HOME`, `$XDG_CONFIG_HOME`, `$XDG_STATE_HOME` and `$XDG_CACHE_HOME` (a relative one is ignored).
Each time the driver makes or starts a game of the user's data folder, it names the folders of the game's settings, log
and caches in `recompiled/<id>/<id>.folders`, where the game reads them: started with other variables (its copy of an
AppImage run with `--appimage-portable-config` or `--appimage-portable-home`, a shortcut), it finds them there.

An older version made its games in the data folder's `games/` (`recompiled/` now, the dumps in `dumps/` beside it), and
kept a game's files side by side in `games/` (`games/Game`, `Game_code.so`, `Game_data/`,
`Game.ini`), the menu's in the data folder's `gui/`, outside Linux the rest in the data folder itself, a portable
copy's in a `games/` beside the program, and a build tree's `games/`, `work/` and keys in its checkout. This one moves
none of it by itself: as long as it is there, the menu lists each folder and where it goes now, the driver says so on
each run, and an older game runs where it is, its run saying where its folder goes. Asked, it moves them:

```
./build/scarletnx --migrate       # each entry a line and where it would go; nothing is touched
./build/scarletnx --migrate=yes   # moved, the same lines; Settings > App > Move Old Files in the menu
```

Each entry of an older folder goes into the folder this version keeps it in, the data folder's `games/` with its games'
settings, logs and caches in the config, state and cache folders; one already there stays, and one on another disk is
not moved, each said with both paths, the status 1. A game laid out beside the others is not moved by name: it moves
as [DEPRECATIONS](DEPRECATIONS.md) says.

## Users

The console's users are in `users/users.ini` of the data folder ([Folders](#folders)), every game's: up to 8, each a
section in the console's order. Without the file there is one, `Player`. User 1 has the uid users.ini gives it; with
none it has the one it always had, `0123456789abcdeffedcba9876543210`, so the saves made before there were others stay
its own. Each other one needs a uid of 32 hex digits of its own (not zero) and a nickname. A nickname is 4 to 20
letters, digits, spaces, `.`, `_` or `-`, the names Eden's rooms take; any other stops the game and says why:

```ini
[user1]
nickname=Aogo
[user2]
uid=5f3c0d9e8a7b41c2963e0f1a2b3c4d5e
nickname=Xuji
```

`uid` and `nickname` are the keys; any other key, a section out of order or twice, or a line that is no key stops the
game and says why. The launcher's Settings > User renames a user (Enter types its nickname, the rule above checked) and
picks the one the games play as; the file's other lines stay as they are. A user's picture is `users/<uid>.jpg`, a JPEG of at most 128 KiB (256x256 as the console's), read
when the game starts; without one it is Wimsy. The game plays as the user its `.ini`'s `user` names (`--user=N` for one
run): with one user, the game picks it on its own; with more, the game's user selector gets that user. Each user's
Account saves are `save/user.<uid>` in the game's `<id>_data/`; those an older version left in `save/user`, of the uid
`0123456789abcdeffedcba9876543210`, are played there until you move them ([DEPRECATIONS](DEPRECATIONS.md)). The game's
menu (Home, F2) shows the user playing in a box at its head's right, their nickname and picture, and the launcher the
one its Settings' `user` names, where a click picks another ([GUI](GUI.md)); a users.ini that lists no users is said in
the launcher's foot.

## Setup

The first start goes through a setup: in the launcher's window ([GUI](GUI.md#setup)), or in the terminal a dump is
first run from. It says first what older versions did that this one does otherwise ([DEPRECATIONS](DEPRECATIONS.md):
only said, nothing is moved), then asks:

1. **Appearance**: the launcher's language (app.ini's `language`), starting on the one it has (the system's, until
   one is picked), and in the launcher's setup its theme and UI scale beside it; the terminal's asks the language
   alone.
2. **Install type**: installed, in the user's folders ([Folders](#folders)), or portable, everything in
   `.scarletnx/` beside the program (or the AppImage), which it makes. An app (Android, iOS) has no other.
3. **Folders**, the launcher's: where its recompiled games go and the folders of dumps it lists (Settings > App >
   Folders).
4. **Keys**: `prod.keys` and `title.keys` where the driver reads them, or installed from a folder that has them.
5. **First user**: a `users/users.ini` there is kept, or overwritten, the one before kept as `users.ini.old`. A new one
   has one user, the nickname given (`Player` by default), its uid random, of a v4 UUID's form as Ryujinx's are; or
   `0123456789abcdeffedcba9876543210`, the uid user 1 has had, while a game in `recompiled/` has saves of it.

In the launcher the first page also has Skip, which takes each step as its default, what Enter on each would take
(the nickname `Player`), and writes `setup.ini`.

`setup.ini`, in the config folder, keeps how far it went:

```ini
[setup]
version = 1
notices = 9
```

`version` is the steps asked, and a newer version's step is asked alone; `notices` is how many of the notices it has
shown, and one a newer version adds shows alone, with OK, before anything else.

```sh
./build/scarletnx --setup                  # all of it again, in the terminal
printf '\n\n\n\n\nAogo\n' | ./build/scarletnx --setup  # its answers a line each, Enter the one in brackets
./build/scarletnx --no-setup "Game.xci"    # no setup this run, nothing written
```

A run whose stdin or stdout is no terminal (a pipe, the launcher's runs) asks nothing. With `--no-setup` nothing is
written, so a data folder with no users.ini plays as `Player`, uid `0123456789abcdeffedcba9876543210`.

## Settings / input

`Game.ini`: help rewritten at startup, most settings hot-reload. `--key=value` overrides one run.
Restart for `docked`, `fullscreen`, `language`, `resolution`, `anisotropy`, `memory_gb`, `heap_max_mb`, `airplane_mode`,
`ldn`, `room`, `pipeline_threads`, `prepare_shaders`, `jit_threads`, and the `[Debug]` settings other than `log`, `auto_stub_ipc` and `diag`.
`scarletnx --settings` prints each setting the launcher's Settings offer, one a line: its section, key, default,
type, range and help, tab-separated. Every setting, the recompiler's too, is described once in
`src/nx/runtime/settings.txt`, which the build generates the tables and `g_settings` from.

| Key | Default | Effect |
| --- | --- | --- |
| `source` | none | The dump the game was made from, which the recompile writes. Without `--extract-container` the game reads its ExeFS and RomFS from it. Left out, the `<id>.source` link older versions made is read instead |
| `docked` | `1` | `1` is TV mode, `0` handheld mode (1280x720) |
| `fullscreen` | `0` | `1` starts fullscreen, except in timed test runs (`--seconds`, `--frames`). `-f` or `--fullscreen=0` sets it for one run. F11 or Alt+Enter switches without changing it |
| `language` | `1` | The game's language, English (US). The `.ini` lists the others, `0` to `17`. One the runtime doesn't know is English (US) |
| `showcase` | `0` | `1` draws player 1's buttons and sticks, as the game gets them, and the frames a second in a corner of the window, for a video. F6 turns it on and off |
| `showcase_corner` | `br` | The corner `showcase` draws in: `tl` top left, `tr` top right, `bl` bottom left, `br` bottom right |
| `resolution` | `1` | Internal 3D scale, `0.25` to `4`: `1` native, `2` double, `0.75` and `0.5` faster |
| `shadows` | `1` | `0` skips shadow maps |
| `vsync` | `1` | Images are shown only on the display's refresh, so nothing tears. `0` doesn't wait for it: the newest image is shown on the next refresh where the display can do that, else at once (which may tear). A display that can do neither waits for the refresh as `1` does |
| `anisotropy` | `0` | Texture filtering as the game asks. `2` to `16` sharpens surfaces seen at an angle |
| `fps_limit` | `game` | The pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30. `0` is unlimited, and `10` to `1000` is that many FPS |
| `memory_gb` | `0` | Memory the game sees. Below `6` it is the console's 3.2 GB, `6` gives 4.8 GB and `8` gives 6.8 GB. Games that run out of memory on 3.2 GB need `6` |
| `heap_max_mb` | `0` | The most heap the game can allocate, in MB. `0` is as much as the memory allows |
| `pipeline_threads` | `0` | Threads that make the GPU's new pipelines while the game goes on, and those of earlier runs before it starts with `prepare_shaders=2`. `0` uses all the computer's threads but two, two to twelve. `1` to `32` is that many. More make a first run's new pipelines sooner, with less CPU left for the game |
| `async_pipelines` | `0` | `1` skips what a new shader draws while its pipeline is made, where the game would otherwise pause until it is ready. The game no longer stops where a new shader first appears, and the things that shader draws show up a few frames later. `0` waits for every pipeline |
| `prepare_shaders` | `2` | Prepares the shaders the game ships in its files and executables ahead of time, so the game's draws needn't make them and it stutters less. They are prepared again after ScarletNX, the GPU or its driver is updated. `0` (Off) makes each shader only when the game first draws with it. `1` (While playing) prepares them in the background, and the game uses them once all are ready. `2` (Before starting) prepares them behind a loading screen in the launcher theme's look, which also waits for the shader cache of earlier sessions, so an opening movie doesn't stutter. A key that types, an arrow or the keypad, a gamepad button other than Home, or a tap starts the game at once, and the rest is prepared while it runs. Esc quits (on Android, back opens the menu, whose Exit quits). F2 or a gamepad's Home opens the menu, and F11 or Alt+Enter switches fullscreen, without starting the game. The screen shows only when the wait passes a quarter second, its words are in English, and a timed run (`--seconds`) starts its clock after it |
| `jit_threads` | `0` | Threads compiling the game code the recompiler missed, which a thread waits for when it reaches it. `0` uses every CPU the game may run on but two, at most 8. They run below the game's threads |
| `network` | `0` | `0` reports offline and lets the game reach only loopback, which is not a network sandbox. `1` uses the computer's connection, and the resolver blocks known console service domains. Local wireless works either way (`ldn`) |
| `airplane_mode` | `0` | `1` is the console in airplane mode: offline whatever `network` says, no local wireless, as the game hears it, and in no Eden room |
| `ldn` | `lan` | Local wireless. `lan` is over the LAN with ldn_mitm's protocol, with another ScarletNX, a Ryujinx or a console running ldn_mitm: with `network=1` the console is the computer's address, with `0` it is `127.0.0.N`, one per ScarletNX on the machine, so two of them play together through loopback. `room` is through the Eden room `room` names, with the Edens and ScarletNXs in it. `off` is local wireless disabled, as the game hears it. Another word stops the game |
| `room` | empty | The Eden room `ldn=room` joins at start: `host` or `host:port` (`24872` without one); empty with `ldn=room`, or with a space in it, stops the game. The console is there as the user playing, by its nickname ([Users](#users)), at the address the room gives it (`192.168.x.y`), which nifm gives the game too, and the game's datagrams go through the room. A room with a password takes it from `users/online.ini` ([Folders](#folders)), a file only you may read (`chmod 600`, not checked on Windows) that holds `room_password=...` alone: another key or a section stops the game. A room that refuses stops the game and says why |
| `user` | `1` | The console's user the game plays as, `1` to `8`, as [Users](#users) lists them. A number with no user stops the game |
| `touch` | `2` | The game's touch screen: `2` fingers and the mouse's left button, `1` fingers, `0` off |
| `controls` | `1` on Android, else `0` | The [on-screen controls](#on-screen-controls) shown |
| `controls_snap`, `controls_scale`, `controls_opacity`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics` | `0`, `50`, `100`, `1`, `1`, `1` | As [On-screen controls](#on-screen-controls) describes them. `controls_scale` runs from `0` to `100` for 50% to 150% |
| `gamepad_swap_ab` | `0` | `1` swaps a gamepad's A and B. `0` sends A, B, X and Y where the console's are |
| `gamepad_map` | none | The player's map over that, below |
| `gamepad_deadzone` | `20` | The sticks' dead zone, in percent of their travel (`0` to `90`) |
| `gamepad_rumble` | `100` | How strongly gamepads vibrate when the game asks, in percent. `0` is never |
| `gamepad_background` | `0` | `1` keeps gamepads playing while the window is not focused |
| `gamepad_joycon_pair` | `1` | `1` plays a left and right Joy-Con together, `0` separately. Restart the game after changing it |
| `gamepad_p<N>_device` (N from 1 to 8) | empty | Player N's controller: empty takes the next to connect, else its SDL GUID as the log names it, `/2` after it for the second of two alike |
| `gamepad_p<N>_connected` | `1` | `0`: the game never sees player N. It always sees player 1, which the keyboard drives |
| `gamepad_p<N>_type` | `0` | The controller the game sees for player N: `0` as it is, `1` Pro Controller, `2` two Joy-Cons, `3` a left Joy-Con, `4` a right one |
| `gamepad_p<N>_swap_ab`, `gamepad_p<N>_rumble`, `gamepad_p<N>_map` | `-1`, `-10`, empty | Player N's own `gamepad_swap_ab`, `gamepad_rumble` and `gamepad_map`; at these values, every player's |
| `log` | `1` | Log detail. `2` logs everything, which adds runtime overhead |
| `auto_stub_ipc` | `1` | A service command the runtime doesn't know answers success, logged as `[undeclared]`, and a service it doesn't have answers every command that way. `0` answers them with the console's UnknownCommandId instead, which shows the gap where the game makes the call |
| `diag` | `0` | `1` saves the data of each new compute shader to `captures/compute`, and each new shader's snx-shaderc corpus entry to `captures/shaders`, for debugging |
| `vk_validation` | `0` | `1` turns on the Vulkan validation layer (`VK_LAYER_KHRONOS_validation`, from the Vulkan SDK or the distribution's validation layers package), which logs each misuse of Vulkan it finds. It slows the game down a lot. Without the layer installed the run only warns |
| `slp_vectorizer` | `0` | `1` runs LLVM's SLP vectorizer, which packs scalar instructions into SIMD ones, in the recompile and the JIT; the next recompile and the JIT's next run take it. Off while LLVM 23 can break a module with it and stop the recompile (llvm-project issue 231525). LLVM Internals' `llvm_slp_vectorization` leaves it running |
| `gdb_jit` | `0` | `1`, with the GDB stub (`-S` or `-s`), hands the code the JIT compiles to GDB ([GDB](#gdb-linux)). GDB stops the game for each compile while it is attached, and each compile's object stays in memory for the run |
| `profile` | `0` | `1` writes `profile.txt` every 10 seconds. The CPU profiler is Windows only |
| `nvn_trace` | `0` | `1` logs each NVN function the game resolves and, at exit, how often each was called ([DEVELOPMENT.md](DEVELOPMENT.md)). The game runs slower |
| `shader_corpus` | empty | A folder that keeps each translated shader for `snx-shaderc`. With it every shader is translated, and none is kept for the next run |
| `gpu_trace_target` | empty | `address[:bytes]`: logs the clears and draws that write a render target starting in that GPU address range |
| `vk_gpu_profile` | `0` | `1` logs the GPU's time per rendering pass every 10 seconds |
| `vk_buffer_stats` | `0` | `1` logs what the renderer copies between guest memory and its buffers |
| `vk_record` | `1` | `0` records Vulkan commands on the GPU thread instead of a thread of their own |
| `compute` | `1` | `0` skips every compute shader dispatch |
| `vk_max_allocation_mb`, `vk_budget_mb` | `0`, `0` | A smaller cap on each Vulkan allocation and on the GPU memory the caches keep, in MiB, for testing. `0` is the device's |
| `mme_hle`, `mme_jit`, `mme_verify` | `1`, `1`, `0` | The GPU macros run natively, compiled, and checked against the interpreter. `0`, `0` interprets them all |
| `renderdoc`, `renderdoc_frames` | empty | RenderDoc captures to that path's stem of the presents listed (`500,800`, or `45s` for the first after 45 seconds) |

A gamepad's motion sensors, a Joy-Con's among them, go to the game when SDL exposes them. No setting turns them off.

`audout` PCM playback and the audio renderer's device sink (`audren`) use SDL3's system-default output on
Windows, Linux and supported Unix systems. Choose the output in your operating system's sound settings. The
renderer does not render splitters, voice biquad filters, or the delay, reverb and biquad effects yet.

SDL gamepads send A, B, X and Y where the console's are (A right, B bottom, X top, Y left), as emulators do;
`gamepad_swap_ab=1` swaps A and B. `gamepad_map` sets what any control sends, a control by where it sits and the
button it sends, as `south:B east:A lt:ZL`; the ones it leaves out send the button in their place. The sticks'
dead zone in `gamepad_deadzone` and the game's vibration as strong as `gamepad_rumble` (both in percent), and with
`gamepad_background=1` still play while the window is not focused; the game's menu (Home, F2) sets them under
*Controllers*, which names the pads connected; its *Player* row shows every player's settings (*All*) or one
player's own Swap A/B, Vibration and Button map. Each connected gamepad uses its own player slot, up to eight.
Keyboard and on-screen controls also drive player one.
`gamepad_joycon_pair=1` makes SDL combine a left and right Joy-Con. Set it to `0` to expose them separately and
restart the game after changing it.

For a joystick that SDL detects but does not recognize as a gamepad, put SDL mapping lines in
`gamecontrollerdb.txt` beside the launcher's `settings.ini` ([Folders](#folders)). A game in the launcher's
`recompiled/` reads the same file; any other game, the user's (on Android every game, the launcher's). To use a file
elsewhere, set
`SDL_GAMECONTROLLERCONFIG_FILE` to its path before starting the launcher or game. A mapping line starts with the
joystick GUID shown in the game log or the launcher's Controllers page, which names the file too, followed by a name
and SDL button and axis bindings. For example:

```text
GUID,My Controller,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,
```

Replace `GUID` and each binding with values for the device, and `Linux` with `Windows`, `FreeBSD` or `Android`
where it runs: SDL takes a line of the file only with its own platform. This SDL mapping selects physical joystick
controls; `gamepad_map` instead selects which Switch buttons the mapped controls send. The game log reports every
detected joystick, whether it has an SDL mapping, and any gamepad open error. On Linux, installing
`dist/72-scarletnx-input.rules` in `/etc/udev/rules.d/` grants desktop seat access to joystick event nodes and to
the hidraw nodes of the console's controllers, for Joy-Con features. Reload udev rules and reconnect the controller
after installation. Other HIDAPI controllers may need their vendor's own hidraw rule.

| Keyboard | Switch |
| --- | --- |
| Arrows; WASD / IJKL | D-pad; L/R sticks |
| X or Space / Z / C / V | A / B / X / Y |
| Q / E / 1 / 3 | L / R / ZL / ZR |
| Enter / Backspace | Plus / Minus |

F1 help; F2 the game's menu; F3 screenshot; F4 FPS limit off and on; F5 thread dump; F6 showcase on and off;
F7 60-second Tracy capture (SNX_TRACY builds); F11/Alt+Enter fullscreen; Pause pauses the whole game and resumes it
(the window's title says "(paused)"); Escape quit. A window shows at most one image per refresh and drops the rest
unless images tear, and the game keeps the pace `fps_limit` sets.

In the game's menu the arrows, Enter and Escape move, select and go back; a gamepad's D-pad or left stick, A and
B do the same, the on-screen controls' too, and a direction held goes on after a moment.

A game that stops on an error shows a panel with **Share Log**, **Copy path** and **Close**. Share Log saves
`scarletnx.log` (the build, the error and the game's log) beside the game's log, as the menu's does, and on Android
sends it to the app you pick; Copy path copies that file's path once it is saved, else the game's log's. The panel says
what each did. Up and Down move between the items, Enter or A does the one in focus, and any other key or button
closes it. On a desktop the game's log path is
underlined once the log exists: a click opens it in the system's viewer, the panel staying open.

### On-screen controls

A pad drawn over the game, with rEden's buttons and sizes, laid out as the console's two controllers hold them:
sticks, D-pad, A B X Y, L R ZL ZR, plus and minus, the stick buttons, Home (the game's menu) and Capture (a
screenshot). Fingers press them, the mouse's left button too; a finger that lands off them goes to the game's
touch screen. They are on by default on Android and off elsewhere.

The game's menu (Back on Android, Home on a gamepad, F2) has their page: show them, *Edit layout* (drag a control;
tap one and size it with the bar on top; *Done* or Back ends it), snap to a grid, scale and opacity, the buttons
shown, a stick's middle where the finger lands, a D-pad a finger slides on, a buzz on each press (Android), the touch
screen, and *Reset layout*. The game's `.ini` keeps them: `controls`, `controls_scale`, `controls_opacity`,
`controls_snap`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics`, a control's place once it
was moved (`controls_a=0.930 0.470 100`: its middle across the screen and its size in percent), and whether it is
shown once that changed (`controls_a_shown=0`). The launcher's Settings have the same, but the layout.
Beside the executable: `Game.log`, `threads.txt` (15 s refresh), `captures/` (screenshots); an installed copy on Linux
keeps the first two in its state folder ([Folders](#folders)).
Scripts/disasm/shaders: [DEVELOPMENT](DEVELOPMENT.md).
