<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Development

## Index

* [Rebuild / checks](#rebuild--checks)
* [Run / diagnose](#run--diagnose)
* [CPU sampling without perf](#cpu-sampling-without-perf)
* [Profiling](#profiling)
  * [What a trace answers](#what-a-trace-answers)
  * [What the profiler's client leaves out](#what-the-profilers-client-leaves-out)
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
cmake build -DBUILD_TESTING=ON && cmake --build build
./build/scarletnx -r "$work/Game" Game --jobs 8
ctest --test-dir build --output-on-failure
python3 tests/run_filesystem_tree.py --cc clang --sanitize
clang -std=gnu23 -fsyntax-only -Wall -I src src/common/cpu/aarch64/cpu_helpers.c
cmake --build build --target cppcheck
```

Runtime relink deploys to the data folder's `recompiled/` by default (`SNX_RECOMPILED_DIR`). Recompiler/`src/common/cpu/aarch64/` edits require `-r`.
Avoid builds during timing runs. Formatting isn't checked: `git clang-format` (clang-format 23) on the changed lines.
The `cppcheck` target runs cppcheck over every file of `src/` and `tests/` the tree compiles, after a build, and shows
every finding: `cmake/cppcheck.cfg` says what our headers cannot (`snx_alloc` never returns NULL). A finding fails the
target unless its check is in `cmake/cppcheck-not-zero.txt`, the checks that still have findings; a check brought to
zero leaves that list, and fails the target from then on.
FS tests use production tree/HLE + synthetic temp data; `--only tree|service` selects a suite.

CI (`.github/workflows/build.yml`) has one job for each platform and architecture, which builds its package, then
tests and checks it with the same tree and compiler cache: each Linux AppImage (x86-64 and AArch64) with ABI syntax,
native tool/FS regressions, headless Vulkan on lavapipe and optional Python diagnostics, x86-64's also under ASan and
UBSan and with [edenprobe](edenprobe.md)'s probes on lavapipe against the cases that passed before
(`scripts/edenprobe.py`); the cross-built AppImages, the Windows zips (x86-64 and AArch64), the Android APK and the
macOS and iOS apps. On `main` the AppImages and the zips also run as installed, at the end of their jobs. A push to
`main` cancels the build of the one before it still running, as a push to a pull request does that pull request's.
`.github/workflows/publish.yml` publishes what builds everywhere, each said on Discord (`scripts/publish-builds.py`): a
version tag pushed (`v1.0.0`; `v1.0.0-rc1` a prerelease) as this repository's release of it, the latest, its notes
listing the changes since the version before;
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
(cd "$games/<id>" && ./<id> --seconds=60 --script=15000:400,16000:1 --press-len=300 --shot=out)
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
  that is the same for every log failing the same way. An iOS day log holds the launcher's starts one after another: the
  summary is of the last one that ran a game or failed (its outcome, build and counts alone), with how many starts the
  log has and how the earlier ones ended.
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
  `--shot=out` → CWD `out.ppm`; `--shot-every=N` → every N frames.
  `--script=ms:hexbuttons,...` (or `Nf:hexbuttons` to press at present N), `--press-len=ms` (default 300).
  `--input-record=N` records player 1's input as the game takes it (the controls', the script's and the on-screen
  controls' together) to the game's slot N, 1 to 9: `<game folder>/tas/<id>.<N>.snxtas`, as save states are
  `states/<id>.<slot>.snxstate`. What the slot held is replaced, and the log says how many frames that was. The file
  has a line `frame:buttons:lx,ly,rx,ry` each present the input changed, at most one a present: `frame` the presents
  since the game's start, `buttons` hex as the script's masks, each stick axis -32767 to 32767. `--input-replay=N`
  plays slot N's recording as player 1's input: each present takes the last line at or before its frame, neutral
  before the first, and the controls come back at the present after the last line's. `#` lines and empty ones are
  left for notes; a line not in that form, an axis out of range or a frame not past the one before ends the run with
  the slot, its line number and its text, and so do a slot outside 0 to 9 and one slot both recorded and played. Both
  on, on two slots, record what the replay gave. A paused game takes no input, so neither writes nor plays there;
  touch and motion are not recorded. The run still ends at `--seconds` or `--frames`. The replay goes by presents:
  where one build takes more presents than another to load a scene, the route slides from there on.
  `--swkbd-text=TEXT`: the software keyboard answers TEXT instead of showing its box over the game.
  `--pause=at:length,...` pauses the whole game at `at` ms of the script's clock for `length` ms, and logs whether
  guest memory and guest time stayed unchanged while it held (`pause:` lines).
  `--state-save=at:slot,...` and `--state-load=at:slot,...` save the game to a save state slot and load one at those
  times (`state:` lines); the files are `<game folder>/states/<id>.<slot>.snxstate`.
  Masks: A/B/X/Y `1/2/4/8`, L/R `40/80`, ZL/ZR `100/200`, Plus/Minus `400/800`,
  left/up/right/down `1000/2000/4000/8000`.
- `run_game.py`: by default, the input sequence that starts a new game in the game it was written for.
  **`--script ""` disables it**.
  `--out DIR` → `runN.log` + PNG (PPM without Pillow); default fresh temp dir.
  Summary: exit, PC/symbol (`--dump`), frames/FPS. Unknown args pass through, e.g. `--fps-limit=0`.
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

- `--log-max=MiB` (1 to 1024, default 16): the game log's size. Past it the log keeps the run's start (its first
  64 KiB) and its last lines, half of `log_max` in all, with a line saying how much went between them
  (`log: reached log_max=16 MiB: 7.4 MiB of lines between the run's start and here cut`), and the first time the
  game's picture says so for 3 s. `--log=2` with a long run is what fills it.
- `--log=2` also logs the process's resources, alike on every system. Each second a `resources:` line: the
  footprint, what the system still gives the process (`?` where it tells none) and the renderer's blocks of device memory; on iOS
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
- `--gpu-trace-target=0x5acf62e00[:bytes]`: logs (`gpu-trace-target:`, at most 64 lines of each kind) the clears
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

[Profiling](profiling.md) walks through every profiler, CPU and GPU, against a recompiled game; this section is the
reference of the Tracy integration it uses.

The GPU benchmarks and the search for faster GPU settings (`tools/bench_nvn_draws.py`,
`tools/bench_gpu_descriptors.py`, `tools/bench_gpu_hotpaths.py`, `tools/tune_gpu.py`) are described with the
measurements they made, in [GPU preparation benchmarks](../history/gpu-hotpath-bench.md) and
[Graphics descriptor measurements](../history/gpu-descriptor-bench.md).

The profiler is not part of the ordinary runtime: `SNX_TRACY` defaults to OFF, so `scarletnx` carries the pinned
Tracy 0.15.0 client only in a build that asks for it — `cmake --preset tracy -B build/tracy && cmake --build
build/tracy`, or `-DSNX_TRACY=ON` in a build of your own (Android's app leaves it out, and iOS cannot have it).
The client starts only when profiling is requested: the Settings' Debug rows (`tracy profiler`, `tracy capture`),
`--tracy-capture`, `--tracy`, or `F10`.
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
read as a wall of [unknown]). It samples at 1 kHz (the build-time `TRACY_SAMPLING_HZ` define changes that) and stops with the client. The
zones written with `SNX_ZONE_CS` ask for a call stack when `tracy_zone_callstack` is set: `pipeline make compile
callstack`, `shader translate maxwell`, `kern core switch`, `kern core handoff`, `kern mutex handoff`,
`kern svc unknown`, `svc hid cmd`, `state memory check`, `state memory load` and `state compress`.
The wait stacks, which walk the stack at every context switch, are a build option. The default build defines
`TRACY_NO_WAIT_STACKS` and leaves them off. Configure with `-DSNX_TRACY_WAIT_STACKS=ON` to remove that define and build
them in. Either `tracy_sampling` or `tracy_context_switches` starts them; the latter needs the tracefs and so root on Linux.
A Tracy build keeps the frame pointers `-O2` drops, as the sampler walks them. The sampler needs the kernel's perf events, which
a normal user has.

- The server (the GUI, or `tracy-capture`) must be the same Tracy version as the client, 0.15.0 here.
- The GUI's Statistics window has an *Instrumentation* and a *Sampling* button; the sampling one holds the hot
  symbols and their call stacks.
- The GPU's and the CPU's clocks drift apart while the machine synchronizes its time; stopping the sync
  (`systemctl stop systemd-timesyncd`) keeps the GPU track lined up.
- Tracy keeps files open, which can pass the system limit (lower with `sudo`): raise it (`ulimit -n`) for a
  profiled run.
- `tracy-csvexport` writes a trace out as CSV, without the GUI.

The instrumentation also has `SNX_SECTION_SCOPED` for phases on Tracy's section track, `SNX_ZONE_COLOR` to color an
open zone, `SNX_MEM_DISCARD` to mark a pool cleared as a whole and `SNX_LOCK_MARK` to put the source line on a named
lock. Lock announcements now cover more of the runtime's mutexes and show both outcomes of a try lock.
[Profiling](profiling.md#sections-lock-marks-and-callstacks) says what each shows and what it costs.

In the launcher, `Settings > Launch > Tracy capture` turns on a capture for each Switch run. The runtime starts
`tracy-capture` (from `Settings > Debug > Tracy capture tool` (`tracy_capture_tool`), beside the game, or on `PATH`)
and writes `captures/tracy_<game>_<date>.tracy` beside the game when the run ends. It passes `Settings > Debug >
Tracy port` (`tracy_port`) on when the setting names one. With the setting off, the runtime does not start the
client; use `--tracy` for a
GUI-only run, or the launcher's `Settings > Debug > Tracy profiler`, which starts the run with the client up for
the GUI to attach to. `--tracy-capture` also needs Tracy 0.15.0's `tracy-capture`; a runtime built with
`-DSNX_TRACY=OFF` (Android, iOS) refuses both flags.
A capture that cannot start, or one whose handshake the loading game missed three times, leaves the run going
without a capture and says why in the log: a setting does not cost the game its launch. A capture gets a port of its
own, so an earlier capture (or a GUI) on 8086 is not in the way, and the run waits for the capture before it starts
the game: the handshake needs an answer, and a game loading a dump starves it.

To capture only a later part of a run, leave that setting off and press `F10` in the game: it starts the client and
a capture beside the game that runs for `tracy_capture_seconds` (60 s by default), and logs its path. Run it again
after it finishes for another segment. A connected GUI and `tracy-capture` are both viewers of the same
one-connection client, so disconnect the GUI first if it is open.

With `--tracy`, connect the Tracy 0.15.0 GUI to `127.0.0.1:8086`, or to `tracy_port`'s port when the setting names
one; broadcast discovery is off. `tracy-capture` records the same stream to a file without the GUI. It can start
before or after the game, and `-s` bounds the recording; it writes the trace when that ends or the game exits.
A normal exit (Esc, the game's own exit, or an unattended limit) ends the zones still open on the exiting thread,
waits a moment for the other threads' short zones, sends the client's queued events and closes the connection, so
the capture sees a clean end rather than a dropped socket. If a thread is still inside a zone after that wait, the
client is left for `_exit` to end: the socket still closes and `tracy-capture` still writes its file, instead of
being killed before it has written anything. The SVCs that wait (`SleepThread`, `WaitSynchronization`,
`ArbitrateLock`, `WaitProcessWideKeyAtomic`, `WaitForAddress`) and the GPU thread's idle wait are deliberately not
zones: one left open would keep the server from acknowledging the client's Terminate.

```sh
tracy-capture -o trace.tracy -f -s 60 &
build/tracy/scarletnx --game="$games/<id>" --tracy
wait
```

The timeline has the frontend tick,
presents, GPU jobs and idle waits, Vulkan record/land/release/submit/wait/present work, shader translation and
pipeline creation, texture uploads, the replay of an NVN submission, compute dispatches, the publishes of surfaces
over guest memory, the BC7 encodes between frames, the landings of the guest memory the GPU owns,
the input tick, the audio and host1x clocks, every returning SVC except the waits above (named
as in `hos.svc`) and every IPC command (the service and command number in the zone's text). Frames appear with the
`frame ms`, guest memory, GPU memory and budget, per-frame draws, shader translations, pipelines made, texture
uploads, and the pipeline cache's hits and the time making and waiting for pipelines. The texture cache's rates
(`texture cache hits/s`, `texture cache misses/s`, `texture upload MB/s`, `texture frees/s`) and the memory the GPU
owns (`own traps/s`, `own land MB/s`) go on the frontend tick, beside `guest memory` and the io rates, so a rate is
read over the same second of wall time as the others. Zones that work on a count
carry it as their value, such as the command buffers in a submit, the GPU channel of a job, the words of a
translated shader, the records in a record pass, the bytes of an uploaded texture (with its dimensions in the
text), the frames an audio tick runs behind its schedule, and the voices an audio frame plays. A translated
shader's zone names its stage and hash, and a pipeline's its hash, so a draw's work follows from the shader it
made. The Vulkan lock records a `vk lock wait` zone only when the lock is contended, so a wait shows apart from the
work done under it and an uncontended acquisition adds no zone.

The GPU's own track comes with it: the rendering passes of each frame are zones on the GPU's clock, named by the
target they draw to, and each dispatch a compute batch holds. With a client attached, a zone is put on every draw and
dispatch as well, named by pipeline, topology and count, so the pass that is unexpectedly expensive says which of its
draws it was: a timestamp between draws leaves the queue waiting for the one before it on many devices, so the zones
come only with an attached client. The pool is bounded (`GP_QUERIES` 16384 timestamps and `GP_DISP` 4096 dispatches a
frame slot), and the log says how many passes went untimed. Each zone is written outside the predication it draws under,
whose region writes no timestamp. The GPU clock is put
on the CPU's timeline once a frame, so a pass that took the GPU long shows against the CPU's work that waited for it. The timestamps come from the graphics queue's, which the
device must have; they are taken when the runtime is profiled (a GUI connected or a capture running) or when
`vk_gpu_profile=1` asks for them, which also logs what the GPU's time went to every 10 s. The binary is
not deployed beside every game the way an ordinary build is, so a game it is not run through the profiled driver for
keeps the runtime it already had: run it through the profiled driver once, and launcher rows work from then.
`SNX_TRACY=OFF` leaves the client out; iOS has none. The Windows-only `profile=1` sampler is separate.

The guest's own file I/O is in the trace too: a read or a write is a zone with the file's name and the bytes, and
`io read MB/s` and `io write MB/s` are the rate. The game's assets are not read during a run, though: they are mapped
from the dump, and the dump is read once, by the driver, before the runtime starts.

### What a trace answers

Every number a run collects is a plot on the same timeline as the zones, so a question has a plot rather than a
second tool. The cost of each is one atomic load per emission, and each goes out only while a viewer is attached.

| The question | The plot or zone |
|---|---|
| Why is this frame slow? | `frame ms`, `jit compile ms` and `jit miss ms` (a compile the game waited for), `pipeline make ms` and `pipeline wait ms`, `shader translate ms`, `texture uploads` (bytes) and `texture upload count`, `render targets made`, `vk finish ms`, `texture pipeline count` and `texture pipeline ms`, `shaders` |
| Is the GPU or the CPU the bound? | the GPU track's passes and `gpu gap:` plots beside `frame ms` and the CPU's own zones |
| Where does the GPU's time go? | the pass zones, `gpu gap: dispatches/copies/barriers/nothing`, `gpu idle: waited/frame/recording/queued`, `gpu busy` (a percentage), `gpu submissions/frame`, `copy MB`; `gpu round:` splits the CPU's waits by kind, `gpu hold:` the busy time by what it held, `gpu submit:` the submissions by reason |
| Which draw of that pass? | the GPU track's draw zones in a connected Tracy run; the log says how many passes went untimed |
| What does the guest cost the host? | `guest cpu` and `process cpu` (of one processor), `guest threads`, `guest top thread ms`, `top svc 0/s` to `top svc 5/s` and `top hook 0/s` to `top hook 4/s` (a message names each rank) |
| What does the game ask of the kernel? | the SVC zones with their result on error, and `svc/s`, `svc miss/s` |
| Which IPC commands cost the most? | `ipc req/s`, `ipc handler ms`, `ipc commands`, `ipc other req/s`, and `ipc top 0 ms` to `ipc top 7 ms` (a message names each rank). The counters are kept while a profiler is attached as well as at `log=2` |
| What does the JIT do? | `jit compile ms`, `jit batch size`, `jit batch functions`, `jit instructions`, `jit code KiB`, `jit compiled pcs`, `jit batches`, `jit cache loads`, `jit queue depth`, `jit variant hits` and `jit variant misses`, `jit failures`, `jit dropped batches` |
| What does NCE do? | the `nce hook` zones and `nce outcalls/s`, `nce hooks/s`, `nce kicks/s`, `nce svc/s`, `nce trap/s`, `nce ticks/s`, `nce run/s` |
| Is the audio behind or clipping? | `audio queued ms`, `audio underruns`, `audio frame ms`, and the `audio voice` zones |
| What does host1x's worker do? | the `host1x job` zones, with the engine, the class and the syncpoint value |
| Is the host running out of memory? | `host heap` (the allocator's live bytes), `host rss`, `host free` |
| How much of a frame does the GPU's hold on guest memory cost, and was a stall a landing? | the `own land` zones (the bytes and the ranges each lands), `own bytes`, `own traps/s`, `own land MB/s` |
| Does the texture cache serve a level, or is the same texture encoded and uploaded again? | the `texture BC7 encode` zones, `texture cache hits/s`, `texture cache misses/s`, `texture upload MB/s`, `texture frees/s` |
| Which wait held a thread back, a syncpoint, a present's fence or the display's pace? | `syncpt wait ms`, `present fence wait ms`, `present pace ms`, `syncpt waits/s`, `syncpt timeouts/s`, and a message names a syncpoint that waited more than a millisecond |
| Does the input tick cost anything, and which voice or part of the audio frame is its cost? | the `hid update` zone with the states it wrote, `hid updates/s`, the `audio voice` zones with their microseconds, `audio mix ms` beside `audio frame ms` |
| Is the input and output keeping up? | `io read MB/s`, `io write MB/s`, `guest memory` |
| Is a frame being dropped or a queue growing? | `submits pending`, `compute pending`, `draws`, `dispatches` |
| What CPU work does a resource take? | `vk resource:` for image requirements, allocation and destruction, target setup and reload, surface reclaim, and buffer maintenance and cache build, each in ms per frame |
| Which GPU wait held a present back? | `gpu wait: finish/recycle/submit ms` per frame |
| Compute: why did the CPU wait for a compute batch? | the `vk compute wait` zone with `vk pipe compute wait pending ms per wait`, `vk pipe compute wait land ms per wait` and `vk pipe compute wait game ms per wait` |
| Hooks: what does a hooked copy or a native wait cost? | the `hook mem copy` and `hook mem set` zones (the bytes and the library function in the text), `hook os sleep ms`, `hook os cs contention ms`, `hook os cv wait ms`, `hook mutex contention ms`, `hook cv wait ms`, `hook cv timed wait ms` and `hook sleep ms`. `hook os sleep skip`, `hook os cv no wait`, `hook mutex try` and `hook recursive try` mark a rare decision, and `hook os cv timeout`, `hook cv already expired`, `hook cv timed out` and `hook sleep none` a rare outcome |
| Kernel: which core handed a thread off, and what kind of wait held it? | the `kern core handoff`, `kern core switch`, `kern core yield`, `kern mutex handoff` and `kern cv take mutex` zones (the core and the waiter in the text), `kern core wait ms`, `kern wait sleep ms`, `kern wait objects ms`, `kern wait mutex ms`, `kern wait cv ms`, `kern wait addr ms`, `kern wait pause ms` and `kern preempt late ms` |
| SVCs: what did a memory SVC decide? | the `kern set perm`, `kern set attr`, `kern gpu attr`, `kern map memory`, `kern unmap memory`, `kern map process` and `kern unmap process` zones, with the bytes as the value and a refusal in the zone's text. `kern set heap` carries MiB, not bytes, and `kern code memory` carries the operation code, not a size |
| NVN: which command cost the most to record, and why was a draw skipped? | the `nvn cmd copy commands`, `nvn cmd dlss`, `nvn cmd spill segment`, `nvn cmd out of memory`, `nvn cmd tex snap missing` and `nvn queue finish` zones with `gpu nvn cmd records`, `gpu nvn cmd bytes` and `gpu nvn replay records`. The `nvn draw arrays`, `nvn draw mesh`, `nvn draw elements`, `nvn clear texture`, `nvn dispatch`, `nvn draw texture`, `nvn dma copy` and `nvn buffer texture copy` zones carry the skip reason as the value, and `nvn copy record` names the kind of copy |
| Shaders: which phase of a translation cost the time? | the `shader translate sm86` and `shader translate maxwell` zones with `shader sm86 translate us` and `shader maxwell translate us`; the `shader decode`, `shader cbuf fold`, `shader dataflow`, `shader control stack`, `shader structure`, `shader alloc` and `shader emission` zones, with `shader instructions`, `shader branches`, `shader blocks`, `shader cb fold hit`, `shader cb fold miss`, `shader flow fallback`, `shader sample-invariant` and `shader translate failures`. `shader translate maxwell` takes a call stack when `tracy_zone_callstack` is set |
| Descriptors: what did a descriptor write do? | the `vk descriptor writes`, `vk descriptor reap`, `vk descriptor border acquire`, `vk descriptor object insert`, `vk descriptor object shared` and `vk descriptor buffer write` zones with `vk pipe desc reaped`, `vk pipe desc frame cache hit per lookup`, `vk pipe desc lasting lookup`, `vk pipe desc write words` and `vk pipe desc object found` |
| Pipelines: which stage of a build cost the time? | the `pipeline make`, `pipeline make compile`, `pipeline make compile callstack`, `pipeline whole make`, `pipeline library make`, `pipeline optimize`, `pipeline entry lookup`, `shader cache records`, `vk pipe libraries choice` and `vk pipe stats write` zones with `vk pipe make ms`, `vk pipe make cache hit`, `vk pipe whole make ms`, `vk pipe whole cache hit`, `vk pipe lib make ms`, `vk pipe lib cache hit`, `vk pipe cached lookup ms`, `vk pipe cached hit`, `vk pipe fast link ms`, `vk pipe fast link cache hit`, `vk pipe optimize ms`, `vk pipe optimize cache hit`, `vk pipe cache read bytes`, `vk pipe shader cache hit bytes`, `vk pipe shader cache miss`, `vk pipe shader cache read bytes` and `vk pipe shader cache write bytes`. `pipeline make compile callstack` takes a call stack when `tracy_zone_callstack` is set |
| ASTC: how many uploads were transcoded, and how much memory did the results take? | the `vk mem astc bc7`, `vk mem astc bcn` and `vk mem astc rgba8` zones (the block dimensions in the text) with `vk mem astc uploads` and `vk mem astc MB` per frame |
| The allocator: when did a pool or a driver allocation grow? | the `vk mem block new`, `vk mem block find`, `vk mem BL scratch trim`, `vk mem budget cap`, `vk mem space take` and `vk mem space give` zones with `vk mem blocks made`, `vk mem blocks MB`, `vk mem driver allocs`, `vk mem driver MB`, `vk mem driver ms` and the `vk mem block table grown` message |
| Save states: which stage of a save or a load cost the time? | the `state gather`, `state section read`, `state memory check`, `state memory load`, `state shmem`, `state compress`, `state save refused`, `state write refused` and `state load refused` zones with `rt state gather bytes`, `rt state save ms`, `rt state load bytes`, `rt state load check ms`, `rt state load ms`, `rt state write copied`, `rt state write file` and `rt state read copied` |
| NCE: what did patching do, and which pattern fired or got rejected? | the `nce patch`, `nce scan`, `nce hook`, `nce hook added`, `nce hook duplicate`, `nce hook rejected`, `nce refused` and `nce net none` zones with `rt nce patch ms`, `rt nce patch sites`, and the `rt nce pat none fired` to `rt nce pat hook fired` and `rt nce pat none rejected` to `rt nce pat hook rejected` plots, one for each of the nine pattern kinds |
| Present: which wait held a present back? | the `present schedule`, `present refresh`, `present delayed`, `present free` and `present dropped` zones and the `vk pres gpu wait`, `vk pres gpu trace` and `vk pres hdr off` zones with `rt present sleep ms`, `vk pres gpu stall ms`, `vk pres gpu stalls`, `vk pres display wait ms`, `vk pres display latency ms`, `vk pres display polls`, `vk pres display let go`, `vk pres acquire outcome`, `vk pres finish ms` and `vk pres maintenance ms` |
| Video: what did a decode job cost, and why was a slot or job skipped? | the `video nvdec job`, `video nvdec ffmpeg`, `video nvdec rewrite bits`, `video nvdec decoder full`, `video nvdec codec change`, `video vic job`, `video vp9 uncompressed header`, `video vp9 compressed header` and `video vp9 rebuild` zones with `video vic job ms`, `video nvdec job ms`, `video host1x waits`, `video host1x queue waits`, `video pictures decoded`, `video pictures total`, `video nvdec failures`, `video vic slot skipped`, `video vic slots` and `video vic outcome` |
| Audio: which stage of a tick or an update cost the time? | the `audio sinks`, `audio sink open`, `audio sink underrun`, `audio sink full`, `audio no sink`, `audio answer buffer small`, `audio voice inactive`, `audio out append`, `audio out no release`, `audio update failed`, `audio denoise` and `audio downmix 5.1` zones with `audio tick late us`, `audio render us`, `audio out tick us`, `audio sort us`, `audio voices us`, `audio sinks us`, `audio out releases`, `audio update in us`, `audio update dsp us`, `audio update out us`, `audio denoise us`, `audio downmix us`, `audio underruns` and `audio voice starve` |
| Platform: what did input, the overlay and the platform's own work cost? | the `plat frontend frame`, `plat SDL event pump`, `plat gamepad scan`, `plat gamepad open`, `plat gamepad close`, `plat input record open`, `plat scripted input`, `plat connect join`, `plat connect tick`, `plat hud draw`, `plat overlay toast lay`, `plat overlay menu lay`, `plat overlay draw`, `plat vpad draw` and `plat vpad showcase draw` zones, with `plat SDL events`, `plat script held bits`, `plat hud quads`, `plat overlay quads`, `plat vpad quads` and `plat vpad showcase quads`. The platform's own work adds the `plat screenshot`, `plat log cut`, `plat page protect`, `plat ww open`, `plat trap open`, `plat trap arm`, `plat trap continue`, `plat trap fill`, `plat vm commit` and `plat profile write` zones, with `plat log cut bytes`, `plat pages protected`, `plat pages opened`, `plat pages armed`, `plat trap fill bytes`, `plat vm commit KB` and `plat profile samples` |
| Audio: how long did opening an output sink take? | `audio sink open us` |
| GPU: how long did a channel park, and how many methods did a job run? | `gpu park wait us`, `gpu job methods`, the `gpu engine 3d methods`, `gpu engine compute methods`, `gpu engine i2m methods`, `gpu engine 2d methods` and `gpu engine dma methods` plots, and the `gpu wait met` and `gpu wait timeout` zones |
| Network: which call waited, how much data moved, and how long did a DNS lookup, TLS step, room join or LAN connect take? | `net accept wait ms`, `net poll wait ms`, `net select wait ms`, `net recv wait ms`, `net recv bytes`, `net send wait ms`, `net send bytes`, `net dns ms`, `net ssl poll wait ms`, `net tls handshake us`, `net tls record us`, `net room join ms`, `net ldn connect ms`, `net nifm online` |
| nn: how much paced file I/O and codec output did a call ask for? | `nn fs pace ms`, `nn fs read bytes`, `nn fs write bytes`, `nn codec out bytes` |
| NVN: how long did a sync or a surface acquire wait? | `nvn sync wait us`, `nvn acquire wait us` |
| Runtime stats: how often did the guest's busiest SVC, count or hook change? | `rt gstat top change/s` |
| SVCs: which service call waited or took time? | `svc am keyboard wait ms`, `svc ldr nro us`, `svc nvideo decode us`, `svc nvideo submit bytes`, `svc nvideo vt wait ms`, `svc vi dequeue wait ms`, `svc vi transact us` |
| Vulkan pipelines: which draws or passes were skipped, how many commands were recorded, and how were shader objects made? | `vk pipe draw skipped`, `vk pipe pass skipped`, `vk pipe record commands`, `vk pipe shader object ms`, `vk pipe shader object from binary`, `vk pipe desc frame cache hit per lookup` |
| Present: how many bytes moved, and what did mirroring, HDR and query reads cost? | `vk pres blit bytes`, `vk pres dma bytes`, `vk pres capture tile bytes`, `vk pres nvdec bytes`, `vk pres vic bytes`, `vk pres writeback bytes`, `vk pres hdr`, `vk pres mirror harvest bytes`, `vk pres mirror harvest left pages`, `vk pres mirror harvest wait ms`, `vk pres mirror land ms`, `vk pres mirror land pages`, `vk pres mirror mem need ms`, `vk pres query read stall ms`, `vk pres query read stalls` |

Many zones carry a small code as their value and name the reason in the zone's text or in a message. A skipped draw's value says why it was skipped, a core handoff's text says which core took which waiter, and a memory SVC's text says what it refused. Group a zone by its value to rank the reasons, then read the zone's text or the message for the reason a given frame took. Some rare decisions have a zone or a message of their own, so they show as their own rows in the Statistics window rather than inside a count.

The two GPU-queue idle plots are apart on purpose: `gpu thread idle` is the GPU thread's own job-queue wait, and
`gpu idle:` are the graphics queue's execution idle, which the frame's own work and its waits between them give.

### What the profiler's client leaves out

Where a thread waits stays out of the default build: the wait stacks walk the stack at every context switch, which
Linux gives only to root with tracefs, and the build defines `TRACY_NO_WAIT_STACKS` to leave them off. Configure with
`-DSNX_TRACY_WAIT_STACKS=ON` to build them in, and a run with `tracy_context_switches=1`, root and tracefs collects
them. A zone's own source (`TRACY_NO_CODE_TRANSFER` off) and the client's
zone verification (`TRACY_NO_VERIFY` off, so a zone begun and not ended is caught) are in the build, and both cost
nothing until a GUI asks. A frame's picture is a readback and a downsample of the window's image, which takes
`tracy_frame_images`' seconds.

The sampler covers every CPU and the threads that start later, so the guest's threads are sampled too. The names come
from the ELF tables of the loaded objects: the runtime's own code and the game's recompiled code beside the game
resolve to functions, a shared library to its exported ones, and the rest (a driver's, usually stripped) stays an
address, which the `tracy-update` utility can resolve later. How many samples get through is the kernel's call: each
one walks up to 127 frames, and a machine busy with a recompile drops most of them, so a trace can be coarse. Lowering
`kernel.perf_event_max_stack` (it needs root) makes the samples smaller and more of them get through, and a longer
run gives the statistics more to work with. The build-time `TRACY_SAMPLING_HZ` define sets another rate.

## Shaders

```sh
"$games/Game/Game" --shader-corpus="$PWD/corpus"
build/snx-shaderc corpus --validate --out translated
```

`--validate` needs `spirv-val`. `--out` writes SPIR-V modules for build comparisons.

The instruction decoder is generated at build time. `src/nx/gpu/shader/sm53.decode` lists each encoding (a
16-bit pattern of the word's top bits), and `tools/decodetree` (strict ISO C23, built for the build machine in a cross
build) checks it and writes a switch-based decoder into the build folder, so nothing is set up at run time.
Patterns that overlap must be in a `{ }` group, which tries them in order: the narrower first. A new opcode is a
pattern there and an entry of `SM53_OPS` in `translator.h` (its `O_` name and display name), from which `decode.c`
makes its `trans_` function. `ctest -R shader_decode` checks that every opcode decodes.
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
"$games/Game/Game" --vertex-input-dynamic-state=off   # a pipeline per layout, to compare
ctest --test-dir build -R 'vk_vertex_input|vulkan_headless'
```

`vk_vertex_input` checks the setting's values, its row read from a game's `.ini` written before it, the table's bans
against stand-in devices and the log line of each case. `vulkan_headless` (`-DSNX_TEST_VULKAN=ON`) watches what every draw
of its checks finds in its command buffer: a draw with a dynamic layout and none set fails it. Its vertex input check
draws one program with three layouts in one command buffer, and `vulkan_headless_vertex_input_off` runs every check
again with the layout in the key.

## Driver bans

`src/common/gpu/vk/extension_bans.txt` is the table of drivers known to break a Vulkan extension or feature: a line per
ban, with the extension or feature, `off`, `warn` or `prefer`, the driver (its `VkDriverId` by name, or `*` for each
driver, whose versions are then `*`), the versions in that
driver's own numbering, the system, the GPUs, what breaks, where that is known from, the case that tests it again, the
date it was last tested and the setting that turns it on anyway. Its head explains each field. It is compiled in, and
the device reads it before it turns anything on (`src/common/gpu/vk/extension_bans.c`): an `off` line takes the extension
out of the device's list, or the feature off where the renderer can do without it, and a `warn` line is only said.
A `prefer` line is the other way round: the way its key names is taken on the drivers it lists and on no other
(`vk_prefer`), with the measure that showed it. The renderer compares no driver or vendor itself:
`runtime_architecture` refuses one outside the table's reader.
Each line that holds is said in the log, with the device, its driver and version and the table's line:

```
vulkan: VK_KHR_present_id2 is off on NVIDIA GeForce RTX 3060 (NVIDIA, 590.48.01, driver version 590.48.01): NVIDIA's driver before 595.0.0 hangs games at their first frames with present_id2 and present_wait2 (590.48, with Smooth Motion on) (DXVK dxvk_device_info.cpp:610, issue 5507; extension_bans.txt:66)
```

A ban joins the table with its source; one that holds for something the renderer does not use is said as such.
[Driver quirks](../layers/gpu/driver-quirks.md) shows every line by driver, beside the quirks the code works around by
itself: `scripts/check-docs.py --bans` writes those rows from the table after a change to it, and `docs_check` fails
while they differ.
`vulkan_ban_ignore` ([Settings](../user/settings.md#graphics)) goes over the bans of the keys it lists for one run, to test a
driver again: `--vulkan-ban-ignore=VK_KHR_present_id2` on the command line, `--setting vulkan_ban_ignore=...` to
`scripts/edenprobe.py`, the row in a phone's `.ini`. Each key is said, as a warning with the ban it goes over where
one holds; a key the table has not stops the game with the nearest ones it has. A case that passes with the ban gone
over, held against the console's or Eden's, is what lifts it. To see which lines hold for this machine's drivers, or
for a driver given by hand:

```sh
build/tests/snx-test-vk-bans --devices                              # each Vulkan device and the lines that hold
build/tests/snx-test-vk-bans --match qualcomm 512.800.48 android    # a phone's driver
build/tests/snx-test-vk-bans --check src/common/gpu/vk/extension_bans.txt   # a table being edited: its first error
ctest --test-dir build -R 'vk_extension_bans|vk_vertex_input'
```

Each extension the renderer can use also has a setting, `vk_ext_<name>`
([Settings](../user/settings.md#vulkan-extensions)), `auto` by default: `auto` leaves the extension to this table, `on` goes over every line on it, `off` takes it out of
the device's list (`vk_extension_settings`, beside the table in `extension_bans.c`). The list is the `table
g_vk_extensions_ini vulkan` of `src/nx/runtime/settings.txt`, a `setting @VK_...` line per extension a renderer
turns on: `tools/settingsgen` checks each against the Khronos registry (the build's Vulkan-Headers' `registry/vk.xml`)
and makes its key and help. A new extension the renderer reads is a line there. The log says each one's value in
effect and why after the bans:

```
vulkan: VK_EXT_vertex_input_dynamic_state: on (vk_ext_vertex_input_dynamic_state=on goes over the ban: ...)
```

`vk_extension_bans` checks the table's errors, each driver's numbering at every bound's edge, the filter and the
features asked for against stand-in devices, the table the build carries, `vulkan_ban_ignore` read from a `.ini`
with a key the table has and with one it has not, and the extension settings' values and log lines.

An extension stays on only with its commands. Once the device is made, each command each of its extensions requires
is asked of the driver (`vkGetDeviceProcAddr`, `snx_vk_commands_lacking` in `src/common/gpu/vk/extension_commands.c`).
The table of them is generated from the same registry by `tools/vkcmdgen`: every device extension, with the commands
of its `<require>` blocks that a device answers for, each block's `depends` kept and checked against the device (a
version up to the one the renderer runs it at, an extension it is made with; any other name, an instance
extension's, does not hold). A driver that lists an extension without one of them (NVIDIA's
`VK_EXT_depth_clamp_control` without `vkCmdSetDepthClampRangeEXT`) would have the renderer call a null pointer: the
extension goes off as an `off` line takes one out, its features and their structures with it, and the device is made
again without it, once. Every renderer's device takes that check, in the one way they make it (below). One the device
can't go without
(a window's swapchain, what 1.3 took in on a 1.2 device) fails the start, saying so, and so does a device made again
that still lacks a command. The log says each, with the driver and its version (here a command
`vk_debug_hide_commands` hid, on lavapipe):

```
vulkan: llvmpipe (LLVM 22.1.8, 256 bits)'s driver lists VK_EXT_conditional_rendering without vkCmdBeginConditionalRenderingEXT, which it requires (vk_debug_hide_commands hides it from the check): the device is made again without it
vulkan: VK_EXT_conditional_rendering is off on llvmpipe (LLVM 22.1.8, 256 bits) (llvmpipe, Mesa 26.2.6 (LLVM 22.1.8), driver version 26.2.6): its driver lists it without vkCmdBeginConditionalRenderingEXT, which it requires (vk_debug_hide_commands hides from the check what the driver has)
```

`vk_debug_hide_commands` ([Settings](../user/settings.md#debug)) has the check take the commands it lists as missing, to
test that path on any driver; a 3DS or Vita game takes it too. `vulkan_headless_hidden_commands` hides
`vkCmdSetDepthClampRangeEXT` and `vkCmdBeginConditionalRenderingEXT` and runs every check of `vulkan_headless` on the
device made again without the extensions of the two that the driver has on; `3ds_vk_hidden_commands` makes the 3DS's
device again without `VK_EXT_shader_object`, and `vita_vk_device` the Vita's without
`VK_KHR_dynamic_rendering_local_read`, by the check and by its setting; `vk_extension_commands` checks the table, the
`depends`, a stand-in device's check and the hide list; `vkcmdgen` the generator on registries of its own and on the
build's; and `vk_extension_settings` that every device extension a renderer can turn on has its `setting @VK_...` line.

```sh
ctest --test-dir build -R 'vk_extension_commands|vk_extension_settings|vkcmdgen|vk_extension_bans'
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --test-dir build -R 'hidden_commands'
```

Every renderer makes its device one way, `snx_vk_device_make` (`src/common/gpu/vk/device.h`): the Switch's, the
3DS's, the Vita's, the NDS's, the GBA's and the Android apps'. It reads the table, `vulkan_ban_ignore` and
`vk_debug_hide_commands`; on each physical device it applies the `vk_ext_*` settings, then the bans, to the device's
list, and gives what is left to the renderer's `choose`. That turns an extension on only through
`snx_vk_choice_enable`, which refuses, naming it, one no `setting @VK_...` line names (the Android apps' program reads
no settings, so takes the bans and the check alone), and makes the device through `snx_vk_choice_create`; the check of
the commands and the device made again follow. The log says the bans and the setting of each extension the renderer
asked about, each device passed over and why, and the device made:

```
nds gpu: the device is AMD Radeon RX 6700 XT (RADV NAVI22), driver radv Mesa 26.2.6, Vulkan 1.3, queue family 0, made with no extension
```

`runtime_architecture` fails a source of `src/` but `common/gpu/vk/device.c` that calls `vkCreateDevice` or
`volkLoadDevice`, and `vk_device_make` checks the way with stand-in drivers: the settings before the bans, an extension
no setting names refused, one whose driver lacks a command left out of the device made again, the devices passed over.

## SVCs

`src/nx/hle/svc/hos.svc` declares the kernel's SVCs as Eden's SVC generator does, and `tools/svcgen/svcgen.c`
(strict ISO C23, built for the build machine in a cross build, as swipc is) lays out each SVC's registers in the
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
