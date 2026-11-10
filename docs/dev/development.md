<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Development

## Index

* [Rebuild / checks](#rebuild--checks)
* [Run / diagnose](#run--diagnose)
* [CPU sampling without perf](#cpu-sampling-without-perf)
* [Profiling](#profiling)
* [Shaders](#shaders)
* [Vertex input](#vertex-input)
* [Driver bans](#driver-bans)
* [SVCs](#svcs)
* [Provenance](#provenance)

[Architecture](architecture.md) · [build/toolchains](build.md) · [usage](../user/usage.md) · [agent rules](../../CLAUDE.md) · [contributing](../../CONTRIBUTING.md).
Run from checkout. POSIX examples: Windows uses `python`, `.exe`, PowerShell env syntax.
The build tree's drivers work in the user's data folder, as an installed copy does ([Folders](../user/usage.md#folders));
the examples name its folders on Linux, and `--data DIR` keeps a run's files in DIR instead:

```sh
games=~/.local/share/scarletnx/recompiled work=~/.cache/scarletnx/work
```

## Rebuild / checks

```sh
cmake --build build
./build/scarletnx -r "$work/Game" Game --jobs 8
ctest --test-dir build --output-on-failure
python3 tests/run_filesystem_tree.py --cc clang --sanitize
clang -fsyntax-only -Wall -I src src/common/cpu/aarch64/cpu_helpers.c
```

Runtime relink deploys to the data folder's `recompiled/` by default (`SNX_RECOMPILED_DIR`). Recompiler/`src/common/cpu/aarch64/` edits require `-r`.
Avoid builds during timing runs. Formatting isn't checked: `git clang-format` (clang-format 23) on the changed lines.
FS tests use production tree/HLE + synthetic temp data; `--only tree|service` selects a suite.

CI: Linux/Windows builds, ABI syntax, native tool/FS regressions, optional Python diagnostics, the Linux AppImages
and the Windows zip (each then run as installed), the Android APK, and [edenprobe](edenprobe.md)'s probes on lavapipe
against the cases that passed before (`scripts/edenprobe.py`). A push to `main` cancels the build of the one before it
still running, as a push to a pull request does that pull request's. `.github/workflows/publish.yml` publishes what
builds everywhere, each said on Discord (`scripts/publish-builds.py`): a version tag pushed (`v1.0.0`; `v1.0.0-rc1` a
prerelease) as this repository's release of it, the latest, its notes listing the changes since the version before;
each push to `main` as a release of [scarletnx/nightly](https://github.com/scarletnx/nightly/releases), the newest 30
kept (this repository's `nightly` prerelease shows the latest's notes); each pull request as a prerelease there,
`pr-<number>`. The notes and the messages link each download with its size and list what changed (a pull request's
commits). A fork's pull request is published once someone of the team labels it `publish builds`, after each
push. The Discord cards of each pull request, each issue and each push to `main` are ReiWimsy's, the bot of
[scarletnx/wimsy-core](https://github.com/scarletnx/wimsy-core), which asks GitHub what changed every 30 s: no job
of this repository draws them. The secrets: `NIGHTLY_TOKEN` (writes that repository's contents) and
`DISCORD_WEBHOOK_RELEASES` (a version's message).
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
  log file (the game log in it), as a summary: the build (with the day its commit was made, when the log says it),
  title ID, game version and GPU from the log's header, how the run ended, the game function it is pinned on (for
  `svcBreak`, the first frame past the SDK's abort), the service commands the runtime doesn't implement, warnings, the
  frames over 50 ms (how many, the slowest, and what the slowest did, from the renderer's frame lines), and a signature
  that is the same for every log failing the same way.
  JSON (a line a log, versioned by `schema`) or a markdown summary for an issue. Paths are shortened to `~`, and the
  crash report's code and memory dumps are left out. The launcher's Share Log puts the same summary at the top of
  `scarletnx.log`. A log is read once, a line at a time, in fixed memory: a gigabyte takes about two seconds.
- `scarletnx --mcp [--data DIR]` (`src/nx/runtime/driver/mcp.c`): an MCP server over stdio for an agent looking into a
  failure on this machine, built into the program so a player's agent has it too. `.mcp.json` starts it for agents
  opened in the checkout (`build/scarletnx --mcp --data .`). Its tools: `scan_log` (the summary above), `read_log`
  (a log's lines holding a text, `|` between alternatives, with context, or a range, or its last lines, only under
  the data folder and its state, cache and settings folders), `find_game` (the extracted games in `work/` with their
  title IDs and versions), `state_info` (a recompiled game's save states: slot, program, when, play time, the build
  and stamp that saved each, and why this build can't load one), `symbolize` and `disassemble` (as `tools/symbols.py` and `tools/disasm.py`, A64 through
  LLVM's disassembler where LLVM has the AArch64 target). The last game loaded stays loaded for the next call. Its
  edenprobe tools turn a confirmed report into an edenprobe case ([edenprobe](edenprobe.md)). Its GDB tools
  (`mcp_gdb.c`, [Usage](../user/usage.md#gdb-through-the-mcp-server)) run one GDB over its machine interface (MI 3,
  resumes as `-exec-*` commands in the background). They find a guest thread's `KThread` through its host thread's
  `t_hle.thread.cur`, by the runtime's debug information, and guest memory at `g_mem`. The game reports
  on GitHub come from GitHub's own MCP server or `scripts/game-reports.py index`.
- Game reports: `.github/workflows/game-reports.yml` builds `snx-logscan` and runs `scripts/game-reports.py triage` on
  each issue and comment. An issue with a log attached (`.log`, `.txt`, `.gz`, `.zip`) or pasted in a fenced block gets
  one comment with the summary of each, edited as logs are added, the labels `game report` and `log: <how it ended>`,
  and the other reports with the same signature named. `triage --issue N --dry-run --logscan build/snx-logscan` shows
  what it would post, and `gh workflow run game-reports.yml -f issue=N` runs the workflow for that issue (one whose run
  failed), failing on a pull request or an issue without a log. `scripts/game-reports.py index [--title-id ID]
  [--signature TEXT]` lists the reports and their logs as JSON lines. A maintainer's `status: confirmed` label on a
  report gets one comment: ready for an edenprobe case, with the steps. The ctest `game_reports_workflow` runs the
  workflow's build step on the files its sparse checkout lists alone, so a file the log summary comes to include fails a
  pull request until the list has it.

- Runtime: `--seconds=N` / `--frames=N`, counted from the game's start, after the shader loading screen
  (`prepare_shaders=2`, which the first run after a rebuild spends translating the game's shaders again);
  `--shot=out` → CWD `out.ppm`; `--shot_every=N` → every N frames.
  `--script=ms:hexbuttons,...` (or `Nf:hexbuttons` to press at present N), `--press_len=ms` (default 300).
  `--input_record=N` records player 1's input as the game takes it (the controls', the script's and the on-screen
  controls' together) to the game's slot N, 1 to 9: `<game folder>/tas/<id>.<N>.snxtas`, as save states are
  `states/<id>.<slot>.snxstate`. What the slot held is replaced, and the log says how many frames that was. The file
  has a line `frame:buttons:lx,ly,rx,ry` each present the input changed, at most one a present: `frame` the presents
  since the game's start, `buttons` hex as the script's masks, each stick axis -32767 to 32767. `--input_replay=N`
  plays slot N's recording as player 1's input: each present takes the last line at or before its frame, neutral
  before the first, and the controls come back at the present after the last line's. `#` lines and empty ones are
  left for notes; a line not in that form, an axis out of range or a frame not past the one before ends the run with
  the slot, its line number and its text, and so do a slot outside 0 to 9 and one slot both recorded and played. Both
  on, on two slots, record what the replay gave. A paused game takes no input, so neither writes nor plays there;
  touch and motion are not recorded. The run still ends at `--seconds` or `--frames`. The replay goes by presents:
  where one build takes more presents than another to load a scene, the route slides from there on.
  `--swkbd_text=TEXT`: the software keyboard answers TEXT instead of showing its box over the game.
  `--pause=at:length,...` pauses the whole game at `at` ms of the script's clock for `length` ms, and logs whether
  guest memory and guest time stayed unchanged while it held (`pause:` lines).
  `--state_save=at:slot,...` and `--state_load=at:slot,...` save the game to a save state slot and load one at those
  times (`state:` lines); the files are `<game folder>/states/<id>.<slot>.snxstate`.
  Masks: A/B/X/Y `1/2/4/8`, L/R `40/80`, ZL/ZR `100/200`, Plus/Minus `400/800`,
  left/up/right/down `1000/2000/4000/8000`.
- `run_game.py`: by default, the input sequence that starts a new game in the game it was written for.
  **`--script ""` disables it**.
  `--out DIR` → `runN.log` + PNG (PPM without Pillow); default fresh temp dir.
  Summary: exit, PC/symbol (`--dump`), frames/FPS. Unknown args pass through, e.g. `--fps_limit=0`.
- Guest VAs: hex; disasm function queries: substrings. Disasm requires `llvm-objcopy` + `llvm-objdump`.
  `symbols.py` and `disasm.py` read the modules with the recompiler's loader, through
  `build/scarletnx --dump-modules PROGRAM DIR` (`SNX_SCARLETNX` names another binary), cached per game folder in
  `~/.cache/scarletnx/module-dumps`. The dump's format is described in `src/nx/loader/loader.h`.
  `--log=2`: verbose; Windows `--profile=1`: `profile.txt` every 10 s.

The game's `[Debug]` settings ([Settings](../user/settings.md#debug)) are for investigating. Each is also given for one run as
`--key=value` on the game's command line. One not at its default, and the GDB stub, is said at the top of the game's
picture while it runs (`Debug on: log=2, gdbstub`, a text setting by its key alone), in the log's header
(`debug: log=2 (default 1), gdbstub`), and in the launcher, as `(Debug on)` on the game's row and its Play and the list
beside it:

- `--log_max=MiB` (1 to 1024, default 16): the game log's size. Past it the log keeps the run's start (its first
  64 KiB) and its last lines, half of `log_max` in all, with a line saying how much went between them
  (`log: reached log_max=16 MiB: 7.4 MiB of lines between the run's start and here cut`), and the first time the
  game's picture says so for 3 s. `--log=2` with a long run is what fills it.
- `--log=2` also logs the process's resources, alike on every system. Each second a `resources:` line: the
  footprint, what the system still gives the process (`?` where it tells none) and the renderer's VMA blocks; on iOS
  Metal's memory, the thermal state and low power mode too. Every 5 s a `threads:` line: the three threads with the
  most time on the processors, by the names the runtime gives them (Linux and Android keep 15 bytes of a name,
  FreeBSD 19). A `memory: under N MiB free` line, with each GPU heap, the first time what the system still gives
  falls under 1024, 512, 256 and 128 MiB: on iOS the app's own room, elsewhere the system's free memory.
- `--log=2` also counts the IPC requests the services serve, by service and command, with the time each took from
  its arrival to its reply (the clock's, so a command that waits in its handler counts the wait). At the renderer's
  first present and every 600 after, an `ipc:` line gives the requests since the line before, their time, and the 8
  commands that took the most (`ISystemClock cmd 0 600 in 0.412 ms`). Up to 1024 commands count apart; the line
  gives the rest as one sum.
- On every log level, a warning of low memory from the system logs a `memory:` line with the same values and each
  GPU heap, 6 in a run at most: the memory pressure of iOS and macOS, SDL's low memory event (iOS; Android up to 13,
  and from 14 only as the app goes to the background), Linux's pressure stall trigger (tasks waiting for memory
  0.2 s of 2 s; a kernel from 6.5 with PSI, else a line says why there is none) and Windows' low memory
  notification. FreeBSD tells a process none. A Vulkan out-of-memory logs each heap and the descriptor pools, at
  the first 3.
- `--gpu_trace_target=0x5acf62e00[:bytes]`: logs (`gpu-trace-target:`, at most 64 lines of each kind) the clears
  and draws that write a render target starting in that GPU address range, with its size, vertex or index count and
  shader addresses. A color clear lists every bound target in the range. Write the address with `0x`; without a size
  the range is one byte. It finds what produced a wrong intermediate target.
- The log names each NVN function the game resolves that the runtime does not implement (`nvn: the game resolved
  NAME, which nvn doesn't implement`), and the game stops with its name when it calls one. [NVN](../layers/gpu/nvn.md)
  has what the SDK does.

## CPU sampling without perf

On Linux x86-64 and AArch64, `tools/profile_cpu.py` can sample a process without perf permissions. It builds a
local preload library and uses each thread's CPU clock. Sleeping threads generate no samples. The signal handler
writes a per-thread ring, and one collector writes the trace. No sampler code enters the ordinary runtime.

```sh
python3 tools/profile_cpu.py tune --out /local/scratch/sample-search
python3 tools/profile_cpu.py run --config /local/scratch/sample-search/winner.json \
    --out /local/scratch/capture -- /absolute/path/to/program arguments
python3 tests/cpu_sample.py
```

The search starts at one slot and doubles capacity until calibration passes. It then measures shuffled paired
rounds and uses bootstrap confidence intervals to select the smallest aligned footprint within two percent of the
fastest candidate. An overflow restarts the measurement batch with a larger buffer. A small native workload cannot
establish a sufficient buffer for every game. Check `samples.bin.json` after each real capture and increase capacity
if it reports drops. Keep the CPU interval separate from the collector's polling period.

`samples.bin` contains little-endian records: monotonic timestamp and host PC as two 64-bit integers, then TID and
sample weight as two 32-bit integers. Weight includes coalesced timer expirations. Report timer overruns separately
from ring overflow. Weighted samples estimate CPU cost, not instruction counts or exact guest PCs. Resolve PCs
against `capture/maps.txt` and the exact matching binaries. The wrapper also records per-thread scheduler CPU time,
run-queue wait and context-switch counts in `sched.jsonl`.

The parent recovers pending records after process exit, including `_exit`, and deduplicates a collector write whose
tail publication was interrupted. Failed and timed-out commands retain their trace, and `capture.json` records the
exit code. The wrapper still returns failure for those commands. One unpublished signal attempt per exiting thread
is reported separately. Recovery tests cover partial writes, duplicate recovery and invalid accounting. The sampler
uses SIGPROF and is intended for programs that do not install their own SIGPROF handler. Keep game captures and symbolized profiles local. Measure
throughput with unprofiled runs of the same scene.

## Profiling

The GPU benchmarks and the search for faster GPU settings (`tools/bench_nvn_draws.py`,
`tools/bench_gpu_descriptors.py`, `tools/bench_gpu_hotpaths.py`, `tools/tune_gpu.py`) are described with the
measurements they made, in [GPU preparation benchmarks](../history/gpu-hotpath-bench.md) and
[Graphics descriptor measurements](../history/gpu-descriptor-bench.md).

The profiler is not part of the ordinary runtime: `SNX_TRACY` defaults to OFF, so `scarletnx` carries the pinned
Tracy 0.14.1 client only in a build that asks for it — `cmake --preset tracy -B build/tracy && cmake --build
build/tracy`, or `-DSNX_TRACY=ON` in a build of your own (Android's app leaves it out, and iOS cannot have it).
The client starts only when profiling is requested: the launcher setting, `--tracy-capture`, `--tracy`, or `F10`.
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

To capture only a later part of a run, leave that setting off and press `F10` in the game: it starts the client and
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

## Vertex input

A pipeline is made per key (`src/nx/gpu/vk/pipeline.c`, `PipeKey`), and the vertex layout a program is drawn with,
its attributes' formats and offsets and its bindings' strides and divisors, is part of the key unless it is dynamic
state. Where the GPU's driver has `VK_EXT_vertex_input_dynamic_state` and its `vertexInputDynamicState` feature,
each draw sets its layout in the command buffer instead (`vkCmdSetVertexInputEXT`), and one pipeline serves every layout
of a program: the pipelines kept across runs of one game, 1984 with the layout in the key, come to 1218. The setting
is `vertex_input_dynamic_state` ([Settings](../user/settings.md#graphics)): `auto`, `on` or `off`, with the drivers known to
draw it wrong ([the driver bans](#driver-bans)) taking the layout in the key under `auto`. The log's
`vulkan: vertex_input_dynamic_state=` line says which a run takes, with the device, its driver and version.

A draw sets the layout when it differs from the one its command buffer has. A new command buffer, a pipeline that
holds its own layout (the multisample resolves, the on-screen controls) and the renderer's own draws (a clear of some
color components, which reads no vertices) leave the next draw to set it again. The pipelines kept for the next run
(`<id>.<driver>.vkshaders`) are made again when the choice changes: the file's key holds it.

```sh
"$games/Game/Game" --vertex_input_dynamic_state=off   # a pipeline per layout, to compare
ctest --test-dir build -R 'vk_vertex_input|vulkan_headless'
```

`vk_vertex_input` checks the setting's values, its row read from a game's `.ini` written before it, the table's bans
against stand-in devices and the log line of each case. `vulkan_headless` (`-DSNX_TEST_VULKAN=ON`) watches what every draw
of its checks finds in its command buffer: a draw with a dynamic layout and none set fails it. Its vertex input check
draws one program with three layouts in one command buffer, and `vulkan_headless_vertex_input_off` runs every check
again with the layout in the key.

## Driver bans

`src/nx/gpu/vk/extension_bans.txt` is the table of drivers known to break a Vulkan extension or feature: a line per
ban, with the extension or feature, `off` or `warn`, the driver (its `VkDriverId` by name), the versions in that
driver's own numbering, the system, the GPUs, what breaks, where that is known from, the case that tests it again, the
date it was last tested and the setting that turns it on anyway. Its head explains each field. It is compiled in, and
the device reads it before it turns anything on (`src/nx/gpu/vk/extension_bans.c`): an `off` line takes the extension
out of the device's list, or the feature off where the renderer can do without it, and a `warn` line is only said.
Each line that holds is said in the log, with the device, its driver and version and the table's line:

```
vulkan: VK_KHR_present_id2 is off on NVIDIA GeForce RTX 3060 (NVIDIA, 590.48.01, driver version 590.48.01): NVIDIA's driver before 595.0.0 hangs games at their first frames with present_id2 and present_wait2 (590.48, with Smooth Motion on) (DXVK dxvk_device_info.cpp:610, issue 5507; extension_bans.txt:66)
```

A ban joins the table with its source; one that holds for something the renderer does not use is said as such.
`vulkan_ban_ignore` ([Settings](../user/settings.md#graphics)) goes over the bans of the keys it lists for one run, to test a
driver again: `--vulkan_ban_ignore=VK_KHR_present_id2` on the command line, `--setting vulkan_ban_ignore=...` to
`scripts/edenprobe.py`, the row in a phone's `.ini`. Each key is said, as a warning with the ban it goes over where
one holds; a key the table has not stops the game with the nearest ones it has. A case that passes with the ban gone
over, held against the console's or Eden's, is what lifts it. To see which lines hold for this machine's drivers, or
for a driver given by hand:

```sh
build/tests/snx-test-vk-bans --devices                              # each Vulkan device and the lines that hold
build/tests/snx-test-vk-bans --match qualcomm 512.800.48 android    # a phone's driver
build/tests/snx-test-vk-bans --check src/nx/gpu/vk/extension_bans.txt   # a table being edited: its first error
ctest --test-dir build -R 'vk_extension_bans|vk_vertex_input'
```

Each extension the renderer can use also has a setting, `vk_ext_<name>`
([Settings](../user/settings.md#vulkan-extensions)), `auto` by default: `auto` leaves the extension to this table, `on` goes over every line on it, `off` takes it out of
the device's list (`vk_extension_settings`, beside the table in `extension_bans.c`). The list is the `table
g_vk_extensions_ini vulkan` of `src/nx/runtime/settings.txt`, a `setting @VK_...` line per extension `device.c`
names: `tools/settingsgen` checks each against the Khronos registry (the build's Vulkan-Headers' `registry/vk.xml`)
and makes its key and help. A new extension the renderer reads is a line there. The log says each one's value in
effect and why after the bans:

```
vulkan: VK_EXT_vertex_input_dynamic_state: on (vk_ext_vertex_input_dynamic_state=on goes over the ban: ...)
```

`vk_extension_bans` checks the table's errors, each driver's numbering at every bound's edge, the filter and the
features asked for against stand-in devices, the table the build carries, `vulkan_ban_ignore` read from a `.ini`
with a key the table has and with one it has not, and the extension settings' values and log lines.

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

Code ported from another project keeps its copyright lines and gets a row in [credits](../CREDITS.md), with its
license text installed when the binaries carry it (`CMakeLists.txt`). Exclude dumps/keys/generated
modules/dependency builds from Git.
