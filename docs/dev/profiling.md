<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Profiling

## Index

* [Before you profile](#before-you-profile)
  * [What a recompiled game is to a profiler](#what-a-recompiled-game-is-to-a-profiler)
  * [A build to profile with](#a-build-to-profile-with)
  * [A scene that repeats](#a-scene-that-repeats)
* [First look: the stats box and the log](#first-look-the-stats-box-and-the-log)
* [Tracy: the whole run on one timeline](#tracy-the-whole-run-on-one-timeline)

[Development](development.md) · [architecture](architecture.md) · [build](build.md) · [settings](../user/settings.md)
· [the CPU cost of a draw](draw-cost.md).

This page is a walkthrough: which tool answers which question about a recompiled game's speed, how to run each one
against a game, and how to read what it gives. The reference for the Tracy integration (every zone and plot, what the
client leaves out) stays in [Development: Profiling](development.md#profiling); this page links there rather than
repeating it. Examples are POSIX shell from the checkout; `build/` stands for your build folder (`build/rel`,
`build/tracy`, ...).

Game dumps, captures, traces, symbolized profiles and shader corpora are the game's: keep them local, never commit
them, and keep them in the gitignored `work/` (or a scratch folder) rather than in the repository.

## Before you profile

### What a recompiled game is to a profiler

A profiler sees an ordinary native process. Knowing its parts saves most of the confusion:

| Part | Where it is | What a profiler names it |
|---|---|---|
| The runtime (HLE kernel, services, NVN to Vulkan, audio, input) | `recompiled/<id>/<id>[.exe]`, a copy of `scarletnx` the driver put beside the game | its own symbols (C functions: `buf_get`, `stage_set`, `svc_*`, ...) |
| The game's recompiled code (AOT) | `recompiled/<id>/<id>_code.{so,dll}` | `F_<guest address in hex>`, from the module's `.symtab` (`ir_codegen.c`'s `snx_ir_guest_function`) |
| Code the JIT compiled for functions the module missed | anonymous executable memory in the JIT loader's arena | nothing: `[unknown]`, `//anon`, a bare address (The JIT) |
| The game's code under NCE (AArch64 Linux/Android, `nce=1`) | the guest's own pages, at the guest's own addresses (`g_mem` is 0) | nothing; the address *is* the guest address |
| The GPU driver, the Vulkan loader, libc, SDL | shared libraries | their exports; a stripped driver stays addresses |

Things that follow from this:

- A guest thread is a host thread (`hle/kernel/thread.c`), so per-thread views in every tool are per guest thread too.
  The GPU thread, the recording thread, the JIT's threads (`jit compile`, `jit miss`), the audio and host1x workers
  are host threads with names of their own.
- `F_7100123450` is the guest function at `0x7100123450`. Its real (demangled) name comes from the game's own symbol
  tables, through `tools/symbols.py` (Naming the game's code).
- Most of a typical profile is the runtime, not the game: memory copies, the renderer's per-draw work, the SVCs and
  IPC. [The CPU cost of a draw](draw-cost.md) is an example of where that leads.
- The generated module has a `.symtab` but no `.eh_frame` (`link_elf.c` writes neither it nor debug info), and its
  functions are not compiled to keep frame pointers. Unwinders can name a sample inside the game's code, but a call
  stack through it is cut or wrong. Read the game's code by self time; read call stacks for the runtime's code.

### A build to profile with

| Build | What it is for |
|---|---|
| The normal release build (`cmake --preset default`, or your `build/rel`) | Throughput numbers, A/B timing, flat CPU profiles |
| `cmake --preset tracy -B build/tracy && cmake --build build/tracy` | Tracy, and any sampling profiler that needs call stacks: `SNX_TRACY=ON` adds `-fno-omit-frame-pointer` to the runtime (`CMakeLists.txt`), which `-O2` otherwise drops |
| A build with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | `perf annotate` and source lines in the runtime's code |

- The Tracy preset turns `SNX_DEPLOY` off: the profiled runtime is not copied over every game. The driver still
  hands a game the runtime it was started with, so the first run through `build/tracy/scarletnx` puts that runtime
  beside the game, and the next run through a normal build puts the normal one back ([Build](build.md)).
- Never compare timings from a Debug or sanitizer build (`build/san`), or between a Tracy build and a normal one.
- The game's own code is compiled by the recompiler, whatever the runtime's build type. Its optimization is the
  recompile's and `jit_opt`'s (`2` by default), not CMake's.

### A scene that repeats

A profile is only as good as its scene. Make the same work happen in every run:

- **Get to the scene the same way.** `--state_load=at:slot` loads a save state at a fixed time; `--input_replay=N`
  plays a recorded input file (`--input_record=N` makes one, `<game folder>/tas/<id>.<N>.snxtas`); `--script=` presses
  buttons at fixed times. See [Run / diagnose](development.md#run--diagnose). `native_cond_var` and `native_os_sync`
  refuse save states; set them to `0` to take one, and keep them so for the comparison.
- **Bound the run.** `--seconds=N` or `--frames=N`, counted from the game's start after the shader loading screen.
- **Warm the caches first.** The first run after a rebuild translates the game's shaders again
  (`prepare_shaders=2`) and makes its pipelines; the first run of a new scene JIT-compiles what the module missed.
  Run the scene once, then profile the second run, unless the cold start is what you are measuring.
- **Remove the pace.** `--fps_limit=0 --vsync=0` lets a frame take as long as its work. With the game's pace on, a
  fast frame waits, and the wait shows up as idle time rather than as a higher frame rate.
- **Keep the machine quiet.** No build, no second game, no other GPU load. A desktop compositor shares the GPU.

```sh
games="${XDG_DATA_HOME:-$HOME/.local/share}/scarletnx/recompiled"
g="$games/<id>/<id>"
"$g" --state_load=2000:1 --input_replay=1 --frames=1800 --fps_limit=0 --vsync=0
```

`tools/run_game.py` runs the same thing unattended and prints the average FPS of the run; options it does not know
go to the runtime:

```sh
python3 tools/run_game.py <id> --script "" --seconds 60 --fps_limit=0 --vsync=0 --out work/prof/base
```

## First look: the stats box and the log

Before any profiler, look at what the runtime already measures.

- **The stats box** (`stats=1`, `F12`): frames a second, the last frame's time, the process's CPU and memory, the
  graphics queue's busy share and busy time a frame (from Vulkan timestamps), video memory against the driver's
  budget (`VK_EXT_memory_budget`), and a graph of the last 120 frame times with 60 and 30 FPS guides. A graph that
  is flat but high is steady work; one with spikes is a hitch to find (a JIT miss, a pipeline compile, a texture
  upload: Tracy names it).
- **`--log=2`** adds, each second, a `resources:` line (footprint, free memory, the renderer's device memory), every
  5 s a `threads:` line with the three threads that took the most CPU, and every 600 presents an `ipc:` line with the
  slowest service commands. That alone often says which thread to profile. `--log=2` costs time itself: do not time a
  run with it.
- **`--vk_buffer_stats=1`** logs what the renderer copies between guest memory and its buffers.
- **`F5`** dumps every thread's state (guest PC and symbol) to the log: a poor man's sample when a game hangs or
  crawls.

## Tracy: the whole run on one timeline

[Tracy](https://github.com/wolfpld/tracy) is the profiler the runtime is instrumented for: frames, every SVC and IPC
command, the GPU thread's jobs, Vulkan recording and submission, shader translation and pipeline creation, texture
uploads and encodes, the JIT, audio, input, and the GPU's own passes, on one timeline with plots of every counter the
runtime keeps. It is the right first tool for "why is this frame slow".

Build it (`cmake --preset tracy -B build/tracy && cmake --build build/tracy`) and get Tracy **0.15.0**'s GUI
(`tracy-profiler`) and `tracy-capture`: the server must be the client's version. Then either:

```sh
# record a run to a file, and open it in the GUI when the run ends
build/tracy/scarletnx --game="$games/<id>" --tracy-capture --seconds=60

# start with the client up, and connect the GUI to 127.0.0.1:8086 (discovery is off)
build/tracy/scarletnx --game="$games/<id>" --tracy

# record without the GUI, on another machine or over ssh
tracy-capture -o work/prof/trace.tracy -f -s 60 &
build/tracy/scarletnx --game="$games/<id>" --tracy
wait
```

or, while a game runs, press `F10` for a 60-second capture beside the game (the log names its file). The launcher
has the same as settings (`Settings > Debug > Tracy profiler`, `Settings > Launch > Tracy capture`).

A walk through a trace:

1. **Frames.** The frame bar on top; click the slow frames. `frame ms` and its neighbours (`jit miss ms`,
   `pipeline wait ms`, `texture uploads`, `shaders`) say at once whether a hitch was a compile.
2. **Threads.** Find the thread whose zones fill the slow frame: the GPU thread (`gpu job`, `vk record`, ...), a guest
   thread (SVC zones, IPC commands), the JIT. A thread with gaps is waiting; `vk lock wait` appears only when the
   Vulkan lock was contended.
3. **The GPU track.** The passes on the GPU's clock, aligned to the CPU's once a frame. A long pass with the CPU idle
   is GPU-bound; the CPU busy while the GPU idles (`gpu idle:` plots) is CPU-bound.
4. **Statistics.** *Instrumentation* sums the zones; with the sampler on, *Sampling* lists the hot functions with
   their call stacks.

Turn on the sampler (`--tracy_sampling=1`) for the time between the zones. It needs Linux perf events (a normal user
has them unless `kernel.perf_event_paranoid` forbids it), samples at 1 kHz (`TRACY_SAMPLING_HZ`), and names the
runtime, the game's `_code.so` and shared libraries' exports; the JIT's code stays `[unknown]`. Without it, Tracy shows
the instrumented zones alone, which costs much less.

Things to know (more in [Development](development.md#profiling)):

- Every plot and zone is listed by the question it answers in [What a trace answers](development.md#what-a-trace-answers).
- Raise the open file limit (`ulimit -n 65536`) for a profiled run.
- The GPU and CPU clocks drift while the system synchronizes its time; stop the time sync for long traces.
- `tracy-csvexport trace.tracy > zones.csv` gives the zones without the GUI, for scripts.
- Tracy's client is not in Android's app or on iOS. On FreeBSD the instrumentation works; the sampler does not
  (Tracy samples with Linux perf events or Windows ETW).
- The cost: off, an emission is one atomic load; connected, Tracy's own. Measure throughput without it.

