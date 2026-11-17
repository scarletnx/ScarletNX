<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Profiling

## Index

* [Before you profile](#before-you-profile)
  * [What a recompiled game is to a profiler](#what-a-recompiled-game-is-to-a-profiler)
  * [A build to profile with](#a-build-to-profile-with)
  * [A scene that repeats](#a-scene-that-repeats)
  * [Which tool for which question](#which-tool-for-which-question)
* [First look: the stats box and the log](#first-look-the-stats-box-and-the-log)
* [Tracy: the whole run on one timeline](#tracy-the-whole-run-on-one-timeline)
* [CPU profiling](#cpu-profiling)
  * [Naming the game's code](#naming-the-games-code)
  * [perf (Linux)](#perf-linux)
  * [profile_cpu.py: sampling without perf](#profile_cpupy-sampling-without-perf)
  * [pmcstat and DTrace (FreeBSD)](#pmcstat-and-dtrace-freebsd)
  * [Windows, macOS and Android](#windows-macos-and-android)
  * [The JIT](#the-jit)
  * [Feeding a profile back to the recompiler](#feeding-a-profile-back-to-the-recompiler)
  * [Micro-benchmarks for CPU work](#micro-benchmarks-for-cpu-work)
* [GPU profiling](#gpu-profiling)
  * [CPU-bound or GPU-bound](#cpu-bound-or-gpu-bound)
  * [vk_gpu_profile: where the GPU's time goes](#vk_gpu_profile-where-the-gpus-time-goes)
  * [The GPU track in Tracy](#the-gpu-track-in-tracy)
  * [RenderDoc](#renderdoc)
  * [Vendor GPU profilers](#vendor-gpu-profilers)
  * [One shader: snx-shaderperf](#one-shader-snx-shaderperf)
  * [The renderer's CPU side](#the-renderers-cpu-side)
  * [GPU memory](#gpu-memory)
* [Comparing two builds or two settings](#comparing-two-builds-or-two-settings)
* [Pitfalls](#pitfalls)

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
| Code the JIT compiled for functions the module missed | anonymous executable memory in the JIT loader's arena | nothing: `[unknown]`, `//anon`, a bare address ([The JIT](#the-jit)) |
| The game's code under NCE (AArch64 Linux/Android, `nce=1`) | the guest's own pages, at the guest's own addresses (`g_mem` is 0) | nothing; the address *is* the guest address |
| The GPU driver, the Vulkan loader, libc, SDL | shared libraries | their exports; a stripped driver stays addresses |

Things that follow from this:

- A guest thread is a host thread (`hle/kernel/thread.c`), so per-thread views in every tool are per guest thread too.
  The GPU thread, the recording thread, the JIT's threads (`jit compile`, `jit miss`), the audio and host1x workers
  are host threads with names of their own.
- `F_7100123450` is the guest function at `0x7100123450`. Its real (demangled) name comes from the game's own symbol
  tables, through `tools/symbols.py` ([Naming the game's code](#naming-the-games-code)).
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

- **Get to the scene the same way.** `--state-load=at:slot` loads a save state at a fixed time; `--input-replay=N`
  plays a recorded input file (`--input-record=N` makes one, `<game folder>/tas/<id>.<N>.snxtas`); `--script=` presses
  buttons at fixed times. See [Run / diagnose](development.md#run--diagnose). `native_cond_var` and `native_os_sync`
  refuse save states; set them to `0` to take one, and keep them so for the comparison.
- **Bound the run.** `--seconds=N` or `--frames=N`, counted from the game's start after the shader loading screen.
- **Warm the caches first.** The first run after a rebuild translates the game's shaders again
  (`prepare_shaders=2`) and makes its pipelines; the first run of a new scene JIT-compiles what the module missed.
  Run the scene once, then profile the second run, unless the cold start is what you are measuring.
- **Remove the pace.** `--fps-limit=0 --vsync=0` lets a frame take as long as its work. With the game's pace on, a
  fast frame waits, and the wait shows up as idle time rather than as a higher frame rate.
- **Keep the machine quiet.** No build, no second game, no other GPU load. A desktop compositor shares the GPU.

```sh
games="${XDG_DATA_HOME:-$HOME/.local/share}/scarletnx/recompiled"
g="$games/<id>/<id>"
"$g" --state-load=2000:1 --input-replay=1 --frames=1800 --fps-limit=0 --vsync=0
```

`tools/run_game.py` runs the same thing unattended and prints the average FPS of the run; options it does not know
go to the runtime:

```sh
python3 tools/run_game.py <id> --script "" --seconds 60 --fps-limit=0 --vsync=0 --out work/prof/base
```

### Which tool for which question

| The question | Start with |
|---|---|
| How fast is it, and is it smooth? | the stats box (`F12`), `--seconds`' average FPS, `tools/run_game.py` |
| Which thread, and what is it doing frame by frame? | [Tracy](#tracy-the-whole-run-on-one-timeline) |
| Which functions burn the CPU? | [perf](#perf-linux), [pmcstat](#pmcstat-and-dtrace-freebsd), [profile_cpu.py](#profile_cpupy-sampling-without-perf), Tracy's sampler |
| Is the frame rate bound by the CPU or the GPU? | [CPU-bound or GPU-bound](#cpu-bound-or-gpu-bound) |
| Which render pass costs the GPU? | [`vk_gpu_profile=1`](#vk_gpu_profile-where-the-gpus-time-goes), Tracy's GPU track |
| Which draw of that pass, and why? | `vk_gpu_draw_profile=1`, then [RenderDoc](#renderdoc) and a [vendor profiler](#vendor-gpu-profilers) |
| Is this shader translation faster than that one? | [snx-shaderperf](#one-shader-snx-shaderperf) |
| Did my change make the game faster? | [`tools/bench_nvn_game.py`](#comparing-two-builds-or-two-settings) |

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
- **`--vk-buffer-stats=1`** logs what the renderer copies between guest memory and its buffers.
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

Turn on the sampler (`--tracy-sampling=1`) for the time between the zones. It needs Linux perf events (a normal user
has them unless `kernel.perf_event_paranoid` forbids it), samples at 1 kHz (`TRACY_SAMPLING_HZ`), and names the
runtime, the game's `_code.so` and shared libraries' exports; the JIT's code stays `[unknown]`. Without it, Tracy shows
the instrumented zones alone, which costs much less.

Turn on `tracy_context_switches` (`--tracy-context-switches=1`) for the OS's context switches on the threads'
timelines. A guest thread's wait and preemption then show against the game's zones. On Linux this needs permission to
read perf events and tracefs (`kernel.perf_event_paranoid`, and a high `ulimit -n`), and Tracy turns it off with a
debug line when it cannot. Tracy couples it to its sampler, so the sample stacks come on with it.

Each guest thread's wait also appears as a **fiber**, named for what the thread blocked on: an object, a mutex, a
condition variable, an address, a pause, a service request or a game pause. The Fibers pane lists them with their
durations, so a thread's idle time can be sorted by why it waited without reading the log.

Tracy 0.15.0 has no call that writes a trace from the app. A fatal outside a signal marks the trace's message pane
with the reason and closes the client, so a capture writes the events up to it. A fatal from the crash handler exits
before the client can close. Its capture keeps the events already sent, which may not include the reason. Start with
`--tracy-capture` to keep a trace of a crash, and read the game's log for the reason. The runtime's own crash handler
stays in charge of the signal.

Things to know (more in [Development](development.md#profiling)):

- Every plot and zone is listed by the question it answers in [What a trace answers](development.md#what-a-trace-answers).
- Raise the open file limit (`ulimit -n 65536`) for a profiled run.
- The GPU and CPU clocks drift while the system synchronizes its time; stop the time sync for long traces.
- `tracy-csvexport trace.tracy > zones.csv` gives the zones without the GUI, for scripts.
- Tracy's client is not in Android's app or on iOS. On FreeBSD the instrumentation works; the sampler does not
  (Tracy samples with Linux perf events or Windows ETW).
- The cost: off, an emission is one atomic load; connected, Tracy's own. Measure throughput without it.

## CPU profiling

### Naming the game's code

A sample in the game's code is named `F_<hex>` (AOT), or not named at all (JIT, NCE). The hex is the guest address,
and the modules are laid out at the same bases as in the runtime, so the game's own symbol tables name it:

```sh
python3 tools/symbols.py work/Game 7100123450 71001a2b3c     # demangled names, as the game's modules have them
python3 tools/disasm.py  work/Game 7100123450 71001234f0     # the guest code of a range
python3 tools/disasm.py  work/Game nn::gfx::detail::Foo       # a whole function, by (sub)name
```

`work/Game` is the extracted dump (`exefs/` + `romfs/`) or an NRO. Both read the modules through
`scarletnx --dump-modules` (`SNX_SCARLETNX` names another binary) and cache them in `~/.cache/scarletnx/module-dumps`.
`disasm.py` needs `llvm-objcopy` and `llvm-objdump`. Many retail modules are stripped: then a function only has its
address, and the guest disassembly (or the host code, `perf annotate`) is what tells it apart.

To see the host code the recompiler made for a function, disassemble the module itself:

```sh
llvm-objdump -d --disassemble-symbols=F_7100123450 "$games/<id>/<id>_code.so"
```

### perf (Linux)

`perf` is the general tool on Linux: no rebuild, every thread, hardware counters.

**Who can sample.** `kernel.perf_event_paranoid` of 2 (the usual default) lets a user sample their own processes'
user-space code; that is enough. `-1` (root) adds kernel stacks and system-wide sampling.

**A flat profile of a scene** (the most useful first view, as the game's code has no reliable stacks):

```sh
"$g" --state-load=2000:1 --seconds=90 --fps-limit=0 --vsync=0 &
sleep 30                                                  # past loading, into the scene
perf record -F 999 -e cycles:u -p $! -o work/prof/perf.data -- sleep 30
perf report -i work/prof/perf.data --no-children --sort comm,dso,sym
```

- `--sort comm,dso,sym` splits by thread name, then module: the `_code.so` rows are the game's code, the `<id>` rows
  the runtime, `[unknown]`/`//anon` the JIT.
- `--sort tid` or `perf report --tid=...` isolates one thread (the GPU thread for the renderer, a game thread for the
  game's own work).
- `perf top -p PID --sort dso,sym` gives the same live.

**Call stacks for the runtime's code.** Use the Tracy build (frame pointers) and `--call-graph fp`:

```sh
perf record -F 999 -e cycles:u --call-graph fp -p PID -o work/prof/fp.data -- sleep 20
perf report -i work/prof/fp.data --children --sort sym
```

Stacks stop at the game's code: a runtime function called from the game (an SVC wrapper, an NVN handler, a hooked
`memcpy`) shows its runtime callers up to the generated function, then breaks. On Intel, last-branch-record call
stacks do not need frame pointers and see through the game's code too, a few calls deep:

```sh
perf record -F 999 -e cycles:u --call-graph lbr -p PID -o work/prof/lbr.data -- sleep 20
```

`--call-graph dwarf` copies the stack with each sample and unwinds with `.eh_frame`; it works for the runtime and the
libraries, not through the module, which has none, and is heavy (keep the sample rate low).

**Flame graphs.** `perf script -i work/prof/fp.data | stackcollapse-perf.pl | flamegraph.pl > work/prof/fg.svg`
([FlameGraph](https://github.com/brendangregg/FlameGraph)), or open the data in
[hotspot](https://github.com/KDAB/hotspot), which also has a per-thread timeline.

**Counters.** `perf stat` says why code is slow rather than where:

```sh
perf stat -e task-clock,cycles,instructions,branch-misses,cache-misses,context-switches,cpu-migrations \
    -p PID -- sleep 20
perf stat --per-thread -e task-clock -p PID -- sleep 10        # CPU time per guest and runtime thread
```

Low instructions per cycle in the game's code points at memory (guest memory accesses are plain loads from `g_mem`,
`GP(addr)`); many context switches point at the guest's synchronization (try `native_os_sync`/`native_cond_var`, and
look at the SVC zones in Tracy).

**Annotate.** `perf annotate -i work/prof/perf.data -s F_7100123450` shows the hot instructions of a generated
function; set it beside `tools/disasm.py` of the same address to read the guest code each host instruction came from.

### profile_cpu.py: sampling without perf

Where perf is not allowed (a locked-down machine, a container), `tools/profile_cpu.py` samples a process on Linux
x86-64 and AArch64 with a preload library (`tools/cpu_sample.c`) and each thread's own CPU clock (SIGPROF). Sleeping
threads give no samples, so it measures CPU cost, not waits.

```sh
python3 tools/profile_cpu.py tune --out work/prof/ring-search               # once: size the rings
python3 tools/profile_cpu.py run --config work/prof/ring-search/winner.json \
    --out work/prof/capture -- "$g" --seconds=60 --fps-limit=0
```

The capture holds `samples.bin` (timestamp, host PC, TID, weight), `maps.txt` to resolve the PCs against, and
`sched.jsonl` (per-thread CPU time, run-queue wait and context switches). Check `samples.bin.json` for drops after
every real capture and raise the capacity if it reports any. The format and the recovery rules are in
[CPU sampling without perf](development.md#cpu-sampling-without-perf).

### pmcstat and DTrace (FreeBSD)

FreeBSD has no `perf`; it has `hwpmc(4)` with `pmcstat(8)`, and DTrace. Both need root (or a user `security.bsd`
settings allow), and the module loaded:

```sh
sudo kldload hwpmc
pmcstat -L | head                      # the events this CPU has
```

**A flat profile** of a running game (the PID is the `<id>` process):

```sh
sudo pmcstat -P cpu_clk_unhalted.thread_p -t PID -O work/prof/game.pmc    # Intel; AMD: ls_not_halted_cyc
# ... let the scene run, then Ctrl-C
pmcstat -R work/prof/game.pmc -g -D work/prof/gprof                        # gprof-style files per module
pmcstat -R work/prof/game.pmc -G work/prof/callgraph.txt                   # callers and callees, as text
```

`-P` samples the process's own threads (`-S` would sample the whole system). `-t` attaches to a PID; giving the
command after the options starts it instead. `pmcstat -R ... -T` gives a live, `top`-like view of a log or of a live
session (`pmcstat -P event -t PID -T`).

Call chains come from the frame pointers, so the Tracy build is again the one to use, and they stop at the game's
code. For a flame graph, `stackcollapse-pmc.pl` from FlameGraph reads `-G` output.

**DTrace** samples with the `profile` provider and walks user stacks with frame pointers too:

```sh
sudo dtrace -p PID -n 'profile-997 /pid == $target/ { @[ufunc(uregs[R_PC])] = count(); } tick-30s { exit(0); }' \
    -o work/prof/funcs.txt
sudo dtrace -p PID -n 'profile-997 /pid == $target/ { @[ustack(32)] = count(); } tick-30s { exit(0); }' \
    -o work/prof/stacks.txt
stackcollapse.pl work/prof/stacks.txt | flamegraph.pl > work/prof/fg.svg
```

DTrace is also good at questions a sampler is not: how often a syscall is made and how long it takes
(`syscall:::entry /pid == $target/ { @[probefunc] = count(); }`), or which thread waits on what
(`sched:::off-cpu`).

Notes for FreeBSD:

- The host has no userfaultfd: `write_tracking` and the GPU's memory traps use page protection there, and a
  profile shows the fault handling as `SIGSEGV`/`SIGBUS` handler time (`vm_fault` in the kernel when sampling it). Read
  that as the cost of the trap model ([GPU ownership](../layers/gpu/ownership.md)), not as a bug.
- Tracy's sampler does not run; use Tracy for the instrumented zones and `pmcstat` for where the time went between
  them.
- `top -H -p PID` (or `top -HSP`) shows the CPU of each thread by name: the runtime's names are cut to 19 bytes.

### Windows, macOS and Android

- **Windows.** `--profile=1` (`profile=1` in the game's `.ini`) is a built-in sampler
  (`src/nx/runtime/platform/profiler_win32.c`): it samples each runtime thread's PC every millisecond, skips threads
  whose cycle count did not move (waiting), and writes `profile.txt` beside the game every 10 s, by thread. An address
  outside the runtime is given as its module and offset, with the first runtime address found above it on the stack.
  It is the zero-setup option. Tracy builds and samples on Windows too (its sampler uses ETW and needs the program run
  as Administrator). General tools that read PDBs and ETW (Windows Performance Analyzer, Superluminal, VTune) work on
  the runtime; the game's `_code.dll` is named by its COFF symbol table, without unwind data.
- **macOS.** Instruments' Time Profiler or `xctrace record --template 'Time Profiler' --attach PID`. The module is a
  Mach-O dylib whose `__eh_frame` and `__compact_unwind` the linker drops (`link_macho.c`), so its stacks are cut as on
  Linux. Vulkan runs over MoltenVK; Xcode's Metal tools see Metal, not Vulkan.
- **Android.** `simpleperf` (from the NDK) on the app's process; Tracy is not built into the app. Under NCE the game's
  code runs from its own addresses: the sampled PC is the guest address, and `tools/symbols.py` names it as it is.
  The GPU vendors' tools for phones are below.

### The JIT

The recompile compiles what its analysis finds; a function it missed is compiled at run time by the JIT, on the
`jit compile` threads (background) and a `jit miss` thread (what a game thread is waiting for). Two costs follow:

1. **Compile time.** A game thread that reaches a missed function waits for its compile. Tracy's `jit miss ms`,
   `jit compile ms`, `jit queue depth` and `jit batch size` show it; a hitch whose frame holds a `jit miss ms` spike is
   one. The settings `jit_threads`, `jit_region_instructions`, `jit_live_budget` and `jit_opt` trade compile time
   against code quality ([Settings](../user/settings.md#performance)). `tools/tune_jit_latency.py` compares
   compile strategies offline on private snapshots with `snx-bench-jit-latency`.
2. **Unnamed code.** The JIT's code is in anonymous memory and the runtime writes no perf map, so external
   profilers cannot name it. To profile it, make it AOT: the JIT records what it compiled, and an
   [AOT pack](../user/aot-packs.md) links those functions into the game's module, where they have `F_` names. A profile
   of the pack's run names them. Under the GDB stub, `gdb_jit=1` registers each JIT object with GDB (`gdb_jit.c`), so
   a debugger can name and unwind them; it stops the game at every compile, so it is for inspection, not timing.

A profile in which `[unknown]` is large is usually JIT code: check its share before reading the rest.

### Feeding a profile back to the recompiler

Two settings take a profile as input. Both change how the code is laid out or optimized, not what it does:

- **Function order** (`-r --layout`). `tools/layout.py` records a scene's taken branches with LBR
  (`perf record -j any,u`; Intel LBR, AMD BRS or LBRv2), clusters each function after its most frequent caller (C3, as
  lld's `CallGraphSort.cpp`) and writes an order the recompile puts the hot code in. ELF modules for this host only.

  ```sh
  python3 tools/layout.py record PID 30 work/prof/branches.data      # in the scene to make fast
  python3 tools/layout.py order work/prof/branches.data "$games/<id>/<id>_code.so" work/prof/game.layout
  build/scarletnx -r work/Game --layout work/prof/game.layout
  ```

- **Branch odds** (`ir_branch_profile`): a file of branch counts, a line each (the branch's address and instruction
  word in hex, then taken and not taken). The recompiler gives LLVM those odds for each branch whose address and word
  match; the next recompile and the JIT's next run take it ([Settings](../user/settings.md)).

Measure either with [`tools/bench_nvn_game.py`](#comparing-two-builds-or-two-settings) against the same game without it.

### Micro-benchmarks for CPU work

A change to a hot path is easier to measure outside the game, then confirmed in it:

- `tests/llvm_bench.c` times the recompiler's code on assembly kernels.
- `tools/cpu_plan.py` and `tools/tune_cpu_sequences.py` compare candidate instruction sequences for an emitter after
  exactness checks, with quiet serial timings ([CPU guide](../layers/cpu/guide.md); `llvm-mca` for a static estimate).
- `tools/bench_gpu_hotpaths.py` times the renderer's CPU-side preparation on synthetic workloads, and
  `tools/bench_gpu_descriptors.py` sets, descriptor buffers and heaps on the real driver
  ([GPU preparation benchmarks](../history/gpu-hotpath-bench.md),
  [Graphics descriptor measurements](../history/gpu-descriptor-bench.md)).

These measure a named workload, not a game's frame rate.

## GPU profiling

The GPU side of a recompiled game is the runtime's Vulkan renderer replaying the game's NVN (or Maxwell) commands:
the shaders are the game's, translated to SPIR-V; the passes, barriers and copies are the renderer's. GPU profiling
asks two questions: is the GPU the bound at all, and if so, which pass, draw or shader.

### CPU-bound or GPU-bound

- **Stats box:** the graphics queue's busy share. Near 100% with the frame rate under target: GPU-bound. Low with the
  frame rate under target: the CPU (very often the GPU thread's per-draw work) is the bound.
- **Resolution test:** lower the internal resolution (`resolution`). A frame rate that rises with it was GPU-bound;
  one that does not move was CPU-bound.
- **Tracy:** the GPU track beside the GPU thread. `gpu idle: waited/frame/recording/queued` splits the queue's idle
  time by its cause; `gpu gap: dispatches/copies/barriers/nothing` what was between passes.
- **System tools:** `nvidia-smi dmon -s puc` (NVIDIA: utilization and clocks), `radeontop` (AMD), `intel_gpu_top`
  (Intel) give the device's busy share independently of the runtime. A GPU that does not reach its top clocks under
  load is often idle more than it looks.

### vk_gpu_profile: where the GPU's time goes

`--vk-gpu-profile=1` (`src/nx/gpu/vk/gpuprof.c`) times every rendering pass of each frame on the GPU, with its first
color (or depth) target, its draw count, and what was recorded between it and the pass before (copies by kind and
bytes, barriers, dispatches). Every 10 s the log gets the frame's span, the time in passes, the time in the gaps by
what they held, and the targets that took the most. It also times each command buffer, and splits the queue's idle
time into "before the CPU submitted the next one" (the GPU had nothing to do: a round trip, between frames, or the
recording was slower than the GPU) and "after" (a semaphore wait). It needs timestamps on the graphics queue
(`timestampComputeAndGraphics`) and, for the CPU alignment, calibrated timestamps; the log says when a device runs
it as `0`.

```sh
"$g" --state-load=2000:1 --seconds=60 --fps-limit=0 --vsync=0 --vk-gpu-profile=1 --log=1
grep 'vulkan: GPU' ~/.local/state/scarletnx/recompiled/<id>/<id>.log   # the 10-second summaries
```

It works in every build, with no external tool, on every platform with timestamps (Android included). Off, it costs
a branch per pass. It is the first GPU tool to reach for: a pass that owns most of the frame, or a gap full of copies
(that is how 955 MB of buffer copies a frame were found), says where to look next.

### The GPU track in Tracy

In a Tracy build, a connected run takes the same timestamps and puts each pass on the GPU's track, named by its
target, with each compute batch's dispatches; the GPU clock is aligned to the CPU's once a frame, so a long pass
shows against the CPU work that waited for it. `--vk_gpu_draw_profile=1` adds a zone on every draw and dispatch
(named by pipeline, topology and count), capped at 256 draws a frame and 128 per batch (`gpu untimed draws` counts the
rest). A timestamp between draws keeps the GPU from overlapping them on many devices, so the draw zones inflate the
frame: use them to rank draws inside a pass, not to time the frame. See
[Development: Profiling](development.md#profiling).

### RenderDoc

[RenderDoc](https://renderdoc.org/) captures one frame's Vulkan calls and replays them: every pass, draw, resource,
pipeline state and shader (with SPIR-V disassembly and, for a translated shader, the GLSL-like decompilation), and
per-draw GPU durations in its Event Browser (`Time` column, via the clock button). The runtime loads it itself:

```sh
# capture presents 500 and 800, to work/prof/rd_<frame>.rdc
"$g" --renderdoc=work/prof/rd --renderdoc-frames=500,800
# or the first present after 45 seconds
"$g" --renderdoc=work/prof/rd --renderdoc-frames=45s
```

With `renderdoc` set, `Ctrl+Shift+F12` (`hotkey_capture`) or the game menu's **Capture frame** captures the frame after
the next present; a game started from RenderDoc's own UI (`qrenderdoc`, *Launch Application* on `<id>`) works the
same. RenderDoc writes the capture inside the present that ends it and the game stops meanwhile (up to a minute for a
frame of 1.4 GB); the log and a notice say when it is written.

- On Wayland, run under XWayland (`SDL_VIDEO_DRIVER=x11`): RenderDoc has no Wayland surfaces.
- `vk_checkpoints=0` (the default) keeps `VK_NV_device_diagnostic_checkpoints` off, so a capture replays on a driver
  without it.
- The renderer sets no debug labels or object names: passes and resources appear by their Vulkan handles and order.
  Match them to Tracy's or `vk_gpu_profile`'s pass names (the target's guest address and size), and to
  `--gpu-trace-target=0x<address>`, which logs every clear and draw into a target.
- `qrenderdoc --python script.py` scripts a capture (save each pass's targets, list draws by duration).
- No RenderDoc on NetBSD; on FreeBSD it is not packaged, so capture on Linux or Windows.

RenderDoc's timings are the replay's, one draw at a time; they rank draws well and frame times poorly.

### Vendor GPU profilers

For counters below the API (occupancy, cache hit rates, stalls by unit), use the GPU vendor's profiler on the
runtime like on any Vulkan application. Launch `"$games/<id>/<id>"` with the options above as the application:

| GPU | Tool | Notes |
|---|---|---|
| NVIDIA | Nsight Graphics (Frame Debugger, GPU Trace) | Linux and Windows; GPU Trace gives per-unit throughput across a frame range |
| AMD | Radeon GPU Profiler | AMD's driver, or RADV with `MESA_VK_TRACE=rgp` (trigger with `MESA_VK_TRACE_TRIGGER=<file>` and `touch <file>`) |
| Intel | Mesa's `INTEL_MEASURE`, GPA | `INTEL_MEASURE` logs per-draw/per-renderpass GPU time to CSV |
| Mesa, any | `MESA_VK_TRACE`, `perfetto` data sources where the driver has them | names depend on the driver and version |
| Arm Mali, Qualcomm Adreno | Arm Streamline / Performance Studio, Snapdragon Profiler, Android GPU Inspector | for the Android app |

`VK_KHR_performance_query` counters, where a driver has them, are reachable through `snx-shaderperf --counters` for a
single shader. NVIDIA's driver has none.

### One shader: snx-shaderperf

A pass is slow because of one shader: take it out of the game and time it alone.

1. Keep the game's shaders: `--shader-corpus=$PWD/work/corpus` (every shader is translated, none kept for the next
   run), or `diag=1` where the game takes no command line (Android: `captures/shaders` beside the game).
2. Translate them to SPIR-V: `build/snx-shaderc work/corpus --validate --out work/translated` (`--validate` needs
   `spirv-val`).
3. Find the slow one: its hash is in Tracy's shader and pipeline zones, and in RenderDoc's pipeline state.
4. Time it, and a variant of it, in one run:

   ```sh
   build/snx-shaderperf --reflect work/translated/<hash>.spv
   build/snx-shaderperf work/translated/<hash>.spv work/variant.spv --size 1920x1080 --check --target-rse 0.005 \
       --json=work/prof/shader.json
   ```

   It interleaves the shaders' submissions, rejects outliers, and gives a ratio of medians with a bootstrap interval
   and a verdict; on NVIDIA it also reports registers, binary size and shared memory per stage
   (`VK_KHR_pipeline_executable_properties`). `--check` hashes everything the shaders write, so a faster variant that
   computes something else is caught. Compare only shaders measured in the same run.

The tool's options are in [`tools/shaderperf/README.md`](../../tools/shaderperf/README.md).

### The renderer's CPU side

In many games the GPU idles while the GPU thread records the frame: then a "GPU problem" is a CPU profile of one
thread. Profile the GPU thread with perf/pmcstat (`--tid`, or `--sort comm` and its name), and look for the functions
[The CPU cost of a draw](draw-cost.md) names (`buf_get`, `cb_get`, `tex_resolve`, `stage_set`, `vk_shader_hash`,
`vk_mirror_harvest`, libc copies). Tracy's `draws`, `dispatches`, `pipeline make ms`, `pipeline wait ms` and
`texture upload MB/s` give the counts to divide by. Settings that move this work are worth one A/B run each:
`vk_record` (a recording thread of its own), `vk_descriptors_mode`, `vertex_input_dynamic_state`, `vk_memory`,
`vk_buffer_pages` ([Settings](../user/settings.md#debug)); `tools/tune_gpu.py` searches them for a game.

Pipeline compiles are the other CPU-side GPU cost: the first frames of a new scene make pipelines (`pipeline make
ms`); the next run loads them from `<id>.<driver>.vkshaders`. `tools/prepare_pipeline_probe.py` with
`tools/bench_pipeline_compile.py` times the captured pipelines' compiles in fresh processes, with the drivers' disk
caches off.

### GPU memory

Running out of video memory looks like a GPU slowdown (evictions, uploads, textures over the bus) and sometimes as a
lost device. Watch the stats box's video memory against the budget, Tracy's GPU memory and budget plots and
`texture cache misses/s`, and the `resources:`/`memory:` lines of `--log=2`. A Vulkan out-of-memory logs each heap
and the descriptor pools. `vk_budget_mb` and `vk_max_allocation_mb` impose a smaller budget, to reproduce a small
GPU's behaviour on a large one; `astc_mode` changes what ASTC textures cost on a GPU without ASTC
([Settings](../user/settings.md#graphics)).

## Comparing two builds or two settings

A profile says where the time goes; whether a change helped is a separate, paired measurement of unprofiled runs.

```sh
python3 tools/bench_nvn_game.py --game "$games/<id>/<id>_code.so" \
    --baseline work/build-before/scarletnx --candidate build/rel/scarletnx \
    --frames 600 --script work/scene.snxtas --rounds 3 --timeout 300 --uncapped --out work/prof/ab
```

It runs baseline and candidate alternately, each from an isolated copy of the same saves, settings and empty caches,
replays a frame-based input file, rejects runs that crash, time out or end on a blank screen, and keeps each run's
final screenshot to inspect. Its metric includes startup and shader preparation; for steady-state frame time, use a
long `--frames` or compare Tracy's `frame ms` over the same scene.

- `tools/tune_gpu.py GAME --state SLOT --warmup 20 --seconds 60 --out work/prof/tune` searches GPU settings and
  retests the fastest against the baseline; it applies nothing.
- `tools/analyze_gpu_budget.py` puts measured component costs side by side without turning them into a promised FPS.
- Keep the build type, the driver version, the power profile and the clocks the same. Run several rounds and report
  the spread, not one number.

## Pitfalls

- **Profiling changes the run.** Tracy's sampler, `--log=2`, `vk_gpu_draw_profile`, `tracy_frame_images` and DWARF
  call graphs all cost time. Find hot spots with them; measure speed without them.
- **The pace hides the work.** With `fps_limit` and `vsync` on, a faster frame only waits longer. Profile uncapped,
  or read frame *work* time rather than frame rate.
- **Cold caches.** Shader translation, pipeline creation and JIT compiles dominate a first run. Profile a warm one
  unless the start is the subject.
- **`[unknown]` is not noise.** It is usually JIT code (or NCE code): see [The JIT](#the-jit).
- **Stacks through the game's code are not trustworthy.** Self time is; use LBR on Intel for callers.
- **A per-draw timestamp serializes draws.** Draw-level GPU timings rank draws inside a pass; they do not add up to
  the frame.
- **Clocks.** CPU frequency scaling and GPU boost move between runs. Pin the governor (`cpupower frequency-set -g
  performance` on Linux, `powerd` off or `sysctl dev.cpu.0.freq` on FreeBSD) and warm up before measuring; compare
  within one run where a tool allows it.
- **Two viewers of Tracy.** The client takes one connection: a capture and the GUI cannot both have a run.
- **Data stays local.** Traces, captures, corpora and symbolized profiles contain the game's code and assets.
