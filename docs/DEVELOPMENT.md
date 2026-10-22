<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Development

[Architecture](ARCHITECTURE.md) · [build/toolchains](BUILD.md) · [usage](USAGE.md) · [agent rules](../CLAUDE.md) · [contributing](../CONTRIBUTING.md).
Run from checkout. POSIX examples: Windows uses `python`, `.exe`, PowerShell env syntax.
The build tree's drivers work in the user's data folder, as an installed copy does ([Folders](USAGE.md#folders));
the examples name its folders on Linux, and `--data DIR` keeps a run's files in DIR instead:

```sh
games=~/.local/share/scarletnx/recompiled work=~/.cache/scarletnx/work
```

## Rebuild / checks

```sh
cmake --build build
./build/scarletnx -r "$work/Game" Game --jobs 8
ctest --test-dir build --output-on-failure
scripts/format.sh --check
python3 tests/run_filesystem_tree.py --cc clang --sanitize
clang -fsyntax-only -Wall -fno-strict-aliasing -fwrapv -I src src/common/cpu/aarch64/cpu_helpers.c
```

Runtime relink deploys to the data folder's `recompiled/` by default (`SNX_RECOMPILED_DIR`). Recompiler/`src/common/cpu/aarch64/` edits require `-r`.
Avoid builds during timing runs. Formatting: clang-format 23 + `uv`/pinned Ruff; omit `--check` to write.
FS tests use production tree/HLE + synthetic temp data; `--only tree|service` selects a suite.

CI: Linux/Windows builds, ABI syntax, native tool/FS regressions, optional Python diagnostics, the Linux AppImages
and the Windows zip (each then run as installed), the Android APK, and [edenprobe](EDENPROBE.md)'s probes on lavapipe
against the cases that passed before (`scripts/edenprobe.py`). `.github/workflows/publish.yml` publishes what
builds everywhere, each said on Discord (`scripts/publish-builds.py`): a version tag pushed (`v1.0.0`; `v1.0.0-rc1` a
prerelease) as this repository's release of it, the latest, its notes listing the changes since the version before;
each push to `main` as a release of [scarletnx/nightly](https://github.com/scarletnx/nightly/releases), the newest 30
kept (this repository's `nightly` prerelease shows the latest's notes); each pull request as a prerelease there,
`pr-<number>`. The notes and the messages link each download with its size and list what changed (a pull request's
commits). A fork's pull request is published once someone of the team labels it `publish builds`, after each
push. The Discord cards of each pull request, each issue and each push to `main` are ReiWimsy's, the bot of
[scarletnx/discord-bots](https://github.com/scarletnx/discord-bots), which asks GitHub what changed every 30 s: no job
of this repository draws them. The secrets: `NIGHTLY_TOKEN` (writes that repository's contents) and
`DISCORD_WEBHOOK_RELEASES` (a version's message). Formatting is checked locally (`scripts/format.sh --check`).
Verify affected scenes manually, repeat intermittent failures, record evidence/scope.
Test guest semantics + malformed IPC with synthetic cases.

## Run / diagnose

```sh
(cd "$games/<id>" && ./<id> --seconds=60 --script=15000:400,16000:1 --press_len=300 --shot=out)
python3 tools/run_game.py <id> --runs 5 --seconds 60 --dump "$work/Game" --script ""
python3 tools/symbols.py "$work/Game" cf34380
python3 tools/disasm.py "$work/Game" NvRmStreamFlushSync
python3 tools/disasm.py "$work/Game" cf34240 cf343c0
build/scarletnx --scan-log ~/.local/state/scarletnx/recompiled/<id>/<id>.log [--format markdown]
```

- `--scan-log` (`src/common/utilities/logscan.c`, also built as `build/snx-logscan`): a log, or the launcher's Share
  log file (the game log in it), as a summary: the build, title ID, game version and GPU from the log's header, how
  the run ended, the game function it is pinned on (for `svcBreak`, the first frame past the SDK's abort), the service
  commands the runtime doesn't implement, warnings, the frames over 50 ms (how many, the slowest, and what the
  slowest did, from the renderer's frame lines), and a signature that is the same for every log failing the same way.
  JSON (a line a log, versioned by `schema`) or a markdown summary for an issue. Paths are shortened to `~`, and the
  crash report's code and memory dumps are left out. The launcher's Share Log puts the same summary at the top of
  `scarletnx.log`. A log is read once, a line at a time, in fixed memory: a gigabyte takes about two seconds.
- `scarletnx --mcp [--data DIR]` (`src/nx/runtime/driver/mcp.c`): an MCP server over stdio for an agent looking into a
  failure on this machine, built into the program so a player's agent has it too. `.mcp.json` starts it for agents
  opened in the checkout (`build/scarletnx --mcp --data .`). Its tools: `scan_log` (the summary above), `read_log`
  (a log's lines holding a text, `|` between alternatives, with context, or a range, or its last lines, only under
  the data folder and its state, cache and settings folders), `find_game` (the extracted games in `work/` with their
  title IDs and versions), `symbolize` and `disassemble` (as `tools/symbols.py` and `tools/disasm.py`, A64 through
  LLVM's disassembler where LLVM has the AArch64 target). The last game loaded stays loaded for the next call. Its
  edenprobe tools turn a confirmed report into an edenprobe case ([EDENPROBE.md](EDENPROBE.md)). Its GDB tools
  (`mcp_gdb.c`, [USAGE.md](USAGE.md#gdb-through-the-mcp-server)) run one GDB over its machine interface (MI 3,
  resumes as `-exec-*` commands in the background). They find a guest thread's `KThread` through its host thread's
  `tls_thread`, by the runtime's debug information, and guest memory at `g_mem`. The game reports
  on GitHub come from GitHub's own MCP server or `scripts/game-reports.py index`.
- Game reports: `.github/workflows/game-reports.yml` builds `snx-logscan` and runs `scripts/game-reports.py triage` on
  each issue and comment. An issue with a log attached (`.log`, `.txt`, `.gz`, `.zip`) or pasted in a fenced block gets
  one comment with the summary of each, edited as logs are added, the labels `game report` and `log: <how it ended>`,
  and the other reports with the same signature named. `triage --issue N --dry-run --logscan build/snx-logscan` shows
  what it would post. `scripts/game-reports.py index [--title-id ID] [--signature TEXT]` lists the reports and their
  logs as JSON lines. A maintainer's `status: confirmed` label on a report gets one comment: ready for an edenprobe
  case, with the steps.

- Runtime: `--seconds=N` / `--frames=N`, counted from the game's start, after the shader loading screen
  (`prepare_shaders=2`, which the first run after a rebuild spends translating the game's shaders again);
  `--shot=out` → CWD `out.ppm`; `--shot_every=N` → every N frames.
  `--script=ms:hexbuttons,...` (or `Nf:hexbuttons` to press at present N), `--press_len=ms` (default 300).
  `--swkbd_text=TEXT`: the software keyboard answers TEXT instead of opening the host's dialog.
  `--pause=at:length,...` pauses the whole game at `at` ms of the script's clock for `length` ms, and logs whether
  guest memory and guest time stayed unchanged while it held (`pause:` lines).
  Masks: A/B/X/Y `1/2/4/8`, L/R `40/80`, ZL/ZR `100/200`, Plus/Minus `400/800`,
  left/up/right/down `1000/2000/4000/8000`.
- `run_game.py`: Catherine New Game sequence by default; **`--script ""` disables it**.
  `--out DIR` → `runN.log` + PNG (PPM without Pillow); default fresh temp dir.
  Summary: exit, PC/symbol (`--dump`), frames/FPS. Unknown args pass through, e.g. `--fps_limit=0`.
- Guest VAs: hex; disasm function queries: substrings. Disasm requires `llvm-objcopy` + `llvm-objdump`.
  `symbols.py` and `disasm.py` read the modules with the recompiler's loader, through
  `build/scarletnx --dump-modules PROGRAM DIR` (`SNX_SCARLETNX` names another binary), cached per game folder in
  `~/.cache/scarletnx/module-dumps`. The dump's format is described in `src/nx/loader/loader.h`.
  `--log=2`: verbose; Windows `--profile=1`: `profile.txt` every 10 s.

The game's `[Debug]` settings ([USAGE.md](USAGE.md#settings--input)) are for investigating. Each is also given for one run as
`--key=value` on the game's command line:

- `--gpu_trace_target=0x5acf62e00[:bytes]`: logs (`gpu-trace-target:`, at most 64 lines of each kind) the clears
  and draws that write a render target starting in that GPU address range, with its size, vertex or index count and
  shader addresses. A color clear lists every bound target in the range. Write the address with `0x`; without a size
  the range is one byte. It finds what produced a wrong intermediate target.
- `--nvn_trace=1`: logs each NVN function the game resolves (`nvn: NAME -> ADDRESS`), the number of GPFIFO
  submissions (by any thread) during each queue call, and at exit the call count of every function used
  (`nvn-used:`). Calls by direct branch are counted only in a game recompiled with `--hook-direct-calls`.
  [NVN_HLE.md](NVN_HLE.md) has what the traces have shown.

## Profiling

The profiler is not part of the ordinary runtime: `SNX_TRACY` defaults to OFF, so `scarletnx` carries the pinned
Tracy 0.14.1 client only in a build that asks for it — `cmake --preset tracy -B build/tracy && cmake --build
build/tracy`, or `-DSNX_TRACY=ON` in a build of your own (Android's app leaves it out, and iOS cannot have it).
The client starts only when profiling is requested: the launcher setting, `--tracy-capture`, `--tracy`, or `F7`.
Without one, a run does not connect.

A game runs profiled from the terminal, the runtime's own flags and nothing else:

```sh
games="${XDG_DATA_HOME:-$HOME/.local/share}/scarletnx/recompiled"
build/tracy/scarletnx --game="$games/<id>" --tracy-capture   # records the run, and opens its trace when it ends
build/tracy/scarletnx --game="$games/<id>" --tracy           # a run for the GUI to attach to
build/tracy/scarletnx --game="$games/<id>" --tracy-capture --script=script --shot=shot  # a test's run, profiled
```

The short way is the launcher: turn on `Settings > Debug > Tracy profiler` and start the game. The client comes up
with the run; connect the Tracy GUI to `127.0.0.1:8086` when it is up. Broadcast discovery is off, so the GUI's
*connect to the next instrumented process* does not find it. A run recorded to a file needs no GUI at all:
`Settings > Launch > Tracy capture` records it and opens the trace in the profiler when the game ends.

`--tracy` starts the client without recording a file. The client off, an emission is one atomic load; the client up
but nothing attached, a zone is a couple of loads on the emitting thread (a thread that is not attached asks at every
zone, so a thread that makes few — the frontend's one a frame, the audio clock's one a tick — is traced from its next
one; an attached one asks every few thousand) and the other emissions skip their call; attached, the cost is Tracy's
own.

The sampler, which says where the time went between the zones, waits to be asked for: `Settings > Debug > Tracy
sampling` (`tracy_sampling`) starts it at the first connection, so a run that leaves it off pays nothing and its
timeline reads as the instrumented zones alone (with JIT'd guest code and the drivers' own threads, the sample stacks
read as a wall of [unknown]). It samples at 1 kHz (`TRACY_SAMPLING_HZ` changes that) and stops with the client. The
zones take no
callstacks of their own; the wait stacks,
which walk the stack at every context switch, are off. Every runtime build keeps the frame pointers `-O2` drops,
as the sampler walks them. The sampler needs the kernel's perf events (a normal user has them) while the context
switches need the tracefs and so root.

- The server (the GUI, or `tracy-capture`) must be the same Tracy version as the client, 0.14.1 here.
- The GUI's Statistics window has an *Instrumentation* and a *Sampling* button; the sampling one holds the hot
  symbols and their call stacks.
- The GPU's and the CPU's clocks drift apart while the machine synchronizes its time; stopping the sync
  (`systemctl stop systemd-timesyncd`) keeps the GPU track lined up.
- Tracy keeps files open, which can pass the system limit (lower with `sudo`): raise it (`ulimit -n`) for a
  profiled run.
- `tracy-csvexport` writes a trace out as CSV, without the GUI.

In the launcher, `Settings > Launch > Tracy capture` turns on a capture for each Switch run. The runtime starts
`tracy-capture` (from `Settings > Debug > Tracy capture tool` (`tracy_capture_tool`), beside the game, or on `PATH`)
and writes `captures/tracy_<game>_<date>.tracy` beside the game when the run ends. It passes `Settings > Debug >
Tracy port` (`tracy_port`) on when the setting names one. With the setting off, the runtime does not start the
client; use `--tracy` for a
GUI-only run, or the launcher's `Settings > Debug > Tracy profiler`, which starts the run with the client up for
the GUI to attach to. `--tracy-capture` also needs Tracy 0.14.1's `tracy-capture`; a runtime built with
`-DSNX_TRACY=OFF` (Android, iOS) refuses both flags.
A capture that cannot start, or one whose handshake the loading game missed three times, leaves the run going
without a capture and says why in the log: a setting does not cost the game its launch. A capture gets a port of its
own, so an earlier capture (or a GUI) on 8086 is not in the way, and the run waits for the capture before it starts
the game: the handshake needs an answer, and a game loading a dump starves it.

To capture only a later part of a run, leave that setting off and press `F7` in the game: it starts the client and
a 60-second capture beside the game, and logs its path. Run it again after it finishes for another segment. A
connected GUI and `tracy-capture` are both viewers of the same one-connection client, so disconnect the GUI first
if it is open.

With `--tracy`, connect the Tracy 0.14.1 GUI to `127.0.0.1:8086`, or to `tracy_port`'s port when the setting names
one; broadcast discovery is off. `tracy-capture` records the same stream to a file without the GUI. It can start
before or after the game, and `-s` bounds the recording; it writes the trace when that ends or the game exits.
A normal exit (Esc, the game's own exit, or an unattended limit) ends the zones still open on the exiting thread,
waits a moment for the other threads' short zones, sends the client's queued events and closes the connection, so
the capture sees a clean end rather than a dropped socket. If a thread is still inside a zone after that wait, the
client is left for `_exit` to end: the socket still closes and `tracy-capture` still writes its file, instead of
being killed before it has written anything. The SVCs that wait (`SleepThread`, `WaitSynchronization`,
`ArbitrateLock`, `WaitProcessWideKeyAtomic`, `WaitForAddress`) and the GPU thread's idle wait are deliberately not
zones: one left open would keep the server from acknowledging the client's Terminate. Source code transfer is off,
so the GUI has file and line numbers but not the text of the source.

```sh
tracy-capture -o trace.tracy -f -s 60 &
build/tracy/scarletnx --game="$games/<id>" --tracy
wait
```

The timeline has the frontend tick,
presents, GPU jobs and idle waits, Vulkan record/land/release/submit/wait/present work, shader translation and
pipeline creation, texture uploads, the audio and host1x clocks, every returning SVC except the waits above (named
as in `hos.svc`) and every IPC command (the service and command number in the zone's text). Frames appear with the
`frame ms`, guest memory, GPU memory and budget, per-frame draws, shader translations, pipelines made, texture
uploads, and the pipeline cache's hits and the time making and waiting for pipelines. Zones that work on a count
carry it as their value, such as the command buffers in a submit, the GPU channel of a job, the words of a
translated shader, the records in a record pass, the bytes of an uploaded texture (with its dimensions in the
text), the frames an audio tick runs behind its schedule, and the voices an audio frame plays. A translated
shader's zone names its stage and hash, and a pipeline's its hash, so a draw's work follows from the shader it
made. The Vulkan lock records a `vk lock wait` zone only when the lock is contended, so a wait shows apart from the
work done under it and an uncontended acquisition adds no zone.

The GPU's own track comes with it: the rendering passes of each frame are zones on the GPU's clock, named by the
target they draw to, and the GPU clock is put on the CPU's timeline once a frame, so a pass that took the GPU long
shows against the CPU's work that waited for it. The timestamps come from the graphics queue's, which the device
must have; they are taken when the runtime is profiled (a GUI connected or a capture running) or when
`SNX_VK_GPU_PROFILE=1` asks for them, which also logs what the GPU's time went to every 10 s. The binary is
not deployed beside every game the way an ordinary build is, so a game it is not run through the profiled driver for
keeps the runtime it already had: run it through the profiled driver once, and launcher rows work from then.
`SNX_TRACY=OFF` leaves the client out; iOS has none. The Windows-only `profile=1` sampler is separate.

The guest's own file I/O is in the trace too: a read or a write is a zone with the file's name and the bytes, and
`io read MB/s` and `io write MB/s` are the rate. The game's assets are not read during a run, though: they are mapped
from the dump, and the dump is read once, by the driver, before the runtime starts.

The sampler covers every CPU and the threads that start later, so the guest's threads are sampled too. The names come
from the ELF tables of the loaded objects: the runtime's own code and the game's recompiled code beside the game
resolve to functions, a shared library to its exported ones, and the rest (a driver's, usually stripped) stays an
address, which the `tracy-update` utility can resolve later. How many samples get through is the kernel's call: each
one walks up to 127 frames, and a machine busy with a recompile drops most of them, so a trace can be coarse. Lowering
`kernel.perf_event_max_stack` (it needs root) makes the samples smaller and more of them get through, and a longer
run gives the statistics more to work with. `TRACY_SAMPLING_HZ` in the environment asks for another rate.

## Shaders

```sh
"$games/Game/Game" --shader_corpus="$PWD/corpus"
build/snx-shaderc corpus --validate --out translated
```

`--validate` needs `spirv-val`. `--out` writes SPIR-V modules for build comparisons.

The instruction decoder is generated at build time. `src/nx/gpu/shader/maxwell_opcodes.def` lists each encoding
(opcode name, display name, 16-bit pattern), and `tools/shaderdec/shaderdec.c` (strict C99, built for the build
machine in a cross build, as swipc is) checks it and writes the opcode enum and a table-driven matcher into the
build folder, so nothing is set up at run time. Two patterns may overlap only when one is strictly narrower, and
that one is tried first. A new opcode is a row there (its `O_` name then exists for the translator), and
`ctest -R 'shaderdec|shader_decode'` checks the spec and decodes every row back to itself.
`[Debug] shader_corpus=` in the game's INI does the same for every run. Where the game gets no command line
(Android), its INI's `[Debug] diag=1` keeps each shader's entry in `captures/shaders` beside the game, for the same `snx-shaderc`.

## SVCs

`src/nx/hle/svc/hos.svc` declares the kernel's SVCs as Eden's SVC generator does, and `tools/svcgen/svcgen.c`
(strict C99, built for the build machine in a cross build, as swipc is) lays out each SVC's registers in the
64-bit and 32-bit ABIs from the declaration. It writes `hle/svc/hos.svc.inc` into the build folder: the handlers'
prototypes, a wrapper per SVC and ABI, and the tables by number that both kinds of thread dispatch through. A new
SVC is implemented by dropping its `stub` mark in `hos.svc` and defining `svc_<Name>` in `svc.c`, with the
declaration's arguments. `build/snx-svcgen --layout src/nx/hle/svc/hos.svc` prints the registers, and
`ctest -R svcgen_layout` checks them against `tests/svcgen_layout.txt`. The NX modules call an SVC through
`RtAPI.svcs`, so a change to the tables' size or signature is an `SNX_ABI` change.

## Provenance

Code ported from another project keeps its copyright lines and gets a row in [credits](CREDITS.md), with its
license text installed when the binaries carry it (`CMakeLists.txt`). Exclude dumps/keys/generated
modules/dependency builds from Git.
