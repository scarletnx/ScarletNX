# ScarletNX (Linux fork)

A Linux-first fork of SwitchRecomp by MZ Labs, a static recompiler for Switch games: ARM64 code is translated to LLVM
IR (libgccjit on DEC Alpha) and compiled into a game module (`games/<id>/<id>_code.so`, by title ID), and the runtime
in `src/nx/` emulates the OS (kernel, IPC, services) and the GPU (Maxwell command buffers and shaders, drawn with
Vulkan). A change is checked against the games it affects. `README.md` has setup and commands, `docs/ROADMAP.md`
the plan and current state, `docs/ARCHITECTURE.md` how the parts fit together. This file holds the rules sessions
get wrong and points to the skills that carry the procedures.

## Rules

- **Nothing of the game goes into git or anywhere outside this machine.** Keys, dumps (XCI, NSP, NCA), `work/`,
  `games/` (code, data, saves, logs) and shader or texture dumps stay local. Commit messages and docs may quote
  addresses, symbol names and short disassembly, never extracted files. `.claude/hooks/guard.py` blocks the
  obvious ways.
- **A build is not a test.** A runtime change is verified by running the game (`snx-run` skill) through the scene
  it affects, and the commit says what was run. A change meant to fix a crash is run enough times to show the crash
  is gone. A single passing run is insufficient for an intermittent crash.
- **Find the mechanism.** A fix explains why the game does what it does: what the game or SDK code expects
  (`tools/disasm.py`), and what the runtime gave it instead. No timing tweaks, retries or special cases for a
  title ID. Syncpoints above 127 "worked" for a whole session while overwriting the game's heap.
- **Upstream code is ours to change, carefully.** `main` is MZ Labs' code plus our commits. Keep changes small and
  in the file's style, with a comment saying why. The `upstream` branch is only ever written by
  `scripts/import-upstream.sh`.
- **Licensing.** The fork is GPL-3.0-or-later (`LICENSE`, since 2026-10-11). MZ Labs' files keep their Apache-2.0
  header, with the line saying the fork's changes are GPL, and every copy keeps NOTICE (`LICENSE.apache` is their
  license). A new file starts with `SPDX-License-Identifier: GPL-3.0-or-later` and the fork's copyright line. Code
  under GPL-3.0, GPL-2.0-or-later (yuzu, suyu, sudachi, Citron, Eden), LGPL, MIT, BSD, ISC, zlib or Apache 2.0 can
  be ported, keeping its copyright lines and naming where it came from (Ryujinx is MIT). GPL-2.0-only code can't be.
  Switchbrew and the SDK's own behavior, read from the disassembly, are the primary references, with Atmosphère
  (Mesosphère's kernel, Stratosphère's services) as the reference for how Horizon behaves. It is GPL-2.0-only, so
  it is read, never ported.
- **Formatting:** `scripts/format.sh` before committing (clang-format 23, ruff). It only changes whitespace, and
  the whole history is formatted, so an unformatted file shows up as noise in every diff. Third-party code
  (the packages in `.cache/cpm`) is never formatted.
- Ask only real decisions. When the next step is obvious, take it.

## Environment

- The Bash tool runs **zsh**: `$t:s` is a zsh modifier, so write `"${t}:s"`. Scripts in the repo use bash shebangs.
- Temporary files go in the session scratchpad, never in the repo root. Long runs use `run_in_background`.
- Never `pkill`/`killall`, or a `kill $(pgrep -f PATTERN)` loop: they match the shell running the command, whose
  command line contains the pattern (twice that killed the session's own shell). List with `pgrep -a NAME` (or
  `pgrep -x cc1`, by process name), then `kill PID` with the numbers in a separate command.
- Don't build while a timing-sensitive test runs: the build deploys a new runtime into `games/` and loads the CPU.
- No `sudo` and no system packages: the user installs those. `uvx` fetches ruff on its own.
- Context is scarce. Never print a whole log (a game's `<id>.log` grows to megabytes), a whole disassembly or a
  ninja log. Use `grep -n`, `tail`, `tools/disasm.py` on one function.

## Build and run

| Task | Command |
| --- | --- |
| Build the runtime, hactool and the third-party libraries (deploys to every game in `games/`) | `cmake --preset default -B build && cmake --build build` (Windows: `scripts\bootstrap.ps1` once, `Set-ExecutionPolicy -Scope Process Bypass` and `. scripts\env.ps1` per window) |
| Cross-build for Windows (llvm-mingw on PATH) | `cmake --preset windows-cross && cmake --build --preset windows-cross` |
| Extract a dump | `build/scarletnx -x "Game.xci" --name Game [--update u.nsp]` |
| Recompile a game (into `games/<id>/`, named by its title ID, or by a name given after the folder) | `build/scarletnx -r work/Game` |
| Run with scripted input and a screenshot | `cd games/<id> && ./<id> --seconds=60 --shot=/path/out --script=ms:hexbuttons,...` |
| Name addresses / disassemble game code | `tools/symbols.py work/Game ADDR...` / `tools/disasm.py work/Game FUNC` |
| Run the tests | `ctest --test-dir build` (`-R swipc` for the IPC generator) |
| Format / check | `scripts/format.sh` / `scripts/format.sh --check` |

Clang is the only compiler: the configure stops on any other; the recompiler builds game code with LLVM (required) or
libgccjit (optional, for DEC Alpha EV67). The
third-party libraries are CPM packages pinned in `cmake/dependencies.cmake`, downloaded once into `.cache/cpm`, and
built in the same tree as the runtime.

The runtime finds its game module by its own file name, so each game has a copy of the runtime named after it, made
by the build's deploy step. The backends' emitters (`src/common/cpu/recompiler/llvm/ir_*.c`, `gccjit/jit_*.c`) mirror `a64_templates.inc` case by case: a template change needs the same change in both, checked with `snx-test-llvm-differential clang . [--gccjit]`. Game code (`_code.so`) only needs recompiling when `src/common/cpu/recompiler/codegen.c`, `a64.c`,
`loader.c`, the backends (`src/common/cpu/recompiler/llvm/`, `gccjit/`, `src/nx/loader/module_*.c`) or anything in
`src/common/cpu/aarch64/` changes (the
ABI between generated code and runtime). A change there that keeps `SNX_ABI` (`cpu.h`) loads and runs the old code
silently. Bumping `SNX_ABI` makes the runtime refuse every module built before it, so every game needs recompiling.

## Where things are

| Area | Files |
| --- | --- |
| C90 host driver (`src/nx/runtime/driver/`) | `driver.c` (dispatch), `extract.c` (XCI/NSP), `recompile.c` (AOT workflow/data), `toolchain.c` (runtime deploy build, shader prebuild); `src/nx/loader/loader.c` (NSO/NRO, metadata), `src/common/cpu/recompiler/codegen.c` (functions, and their C for the interpreter and gnuabi) + `src/common/cpu/aarch64/a64.c` + `a64_templates.inc` (the instructions' C), `src/common/utilities/common.c` (host APIs); `--backend=llvm` (default): `src/common/cpu/recompiler/llvm/` (IR emitters, ELF/COFF linkers, `rtlib.c`, `code_loader.c` loading objects into an in-process code arena, `jit_service.c` compiling missed guest code at run time) + `src/nx/loader/module_llvm.c`; `--backend=gcc` (optional, DEC Alpha EV67): `src/common/cpu/recompiler/gccjit/` + `src/nx/loader/module_gccjit.c` |
| Generated-code ABI | `src/common/cpu/aarch64/`: `cpu.h` (with `SNX_ABI`), `cpu_helpers.c` (compiled into each game module, with the topic files it includes from `helpers/`: `fp.c`, `integer.c`, `simd.c`, `crypto.c`, `simd_mem.c`) |
| The recompiler as a library (`docs/LIBRARY.md`, #171) | `src/common/cpu/recompiler/snx_recompiler.h` (the interface), `library.c` (code in memory through the backends and `llvm/link.c`, a small descriptor instead of the NX `SnxModule`, loaded from memory), `CMakeLists.txt` (`scarletnx-recompiler`, one archive of the backends' objects, installed with a CMake package), `tests/recompiler_library.c` |
| 32-bit games (A32/Thumb) | `src/common/cpu/arm/` (the block compiler, `README.md`; `rt_helpers.c`, what blocks leave to the backend), `src/nx/loader/module_arm.c` (block discovery, the `SnxArmModule`), `src/nx/runtime/arm.c` (thread loop, block tables, run-time compiles and hints, the NX `SnxArmRt`), `snx_svc32` in `hle/svc/svc.c` (the 32-bit SVC registers), `mem_layout32` in `memory/mem.c`; `tests/nx_arm32_smoke.py` runs a synthetic one |
| Startup | `src/nx/runtime/`: `main.c` (startup, loading the game module), `options.c` (the .ini), `ini.c` (the settings tables each module describes its keys with), `clock.c` (guest time), `rtapi.c` (what the game module calls), `crash.c`, `present.c` (vi's present and the frame pacing), `loader.c`, `entry.c`, `startup.c` (switches parsed before loading), `gdbstub.c`, `guest_jit.c` (the runtime JIT's NX side) |
| Kernel and memory | `src/nx/hle/kernel/`: `kernel.c` (handles, waits, events), `thread.c` (threads, the per-core lock), `sync.c` (mutexes, condition variables, address arbitration, WaitSynchronization), `debug.c` (backtraces, dumps), `ipc.c` (HIPC/CMIF parsing and replies, sessions, domains, the service registry); `src/nx/hle/svc/`: `svc.c` (the SVC dispatch), `svc_mem.c` (memory SVCs, GetInfo); `src/nx/memory/mem.c` |
| IPC interfaces | Every service interface is declared in a `.swipc` file beside its C file (SwIPC's language, `docs/IPC.md`). `tools/swipc/swipc.c` (C90) generates the dispatchers into the build folder (`snx_swipc` in `cmake/swipc.cmake`, the list in `src/nx/CMakeLists.txt`). A service's C file includes its generated `.swipc.inc` and defines one static handler per command that isn't a `@stub`, `nn_ns_IFoo_Command(SvcObj* self, args..., outs...)`. A new command goes in the `.swipc` file first |
| Services | `src/nx/hle/services/`: one file per service family (`sm.c`, `am.c`, `set.c`, `time.c`, `acc.c`, `audio.c` (hwopus decodes with FFmpeg), `misc.c` for the small ones, `services.c` registers them), `audren.c` (the audio renderer's update), `pl.c` (shared fonts), `hid.c`, `vi.c`, `ldr.c`, `network/` (bsd, nifm, dns), `nvdrv.c` (nvhost devices, syncpoints); `src/nx/hle/filesystem/` (`fs.c`, `fs_tree.c`) |
| GPU | `src/nx/gpu/`: `gpu.c` (GPFIFO, the GPU thread), `maxwell/` (`engines.c` 3D engine methods, `mme.c` macros, `compute.c`, `fermi2d.c`, `dma.c`, `maxwell3d.c`), `renderer.h` (the `Renderer` table for Vulkan, `g_renderer`), `renderer.c` (shared renderer state), `shader/` (Maxwell to SPIR-V: `shader.c` handles program hashing and flow, `decode.c` the instruction table, `spirv_translate.c` and `spirv_compute.c` emit stages through `spirv_emit.c`, `corpus.c` records shaders), `texture/` (`astc.c` CPU reference for tests, `etc2.c`) |
| Vulkan renderer | `src/nx/gpu/vk/`: `vk.h` (lists the parts), `device.c` (instance, device, queues, swapchain, VMA), `cmd.c` (frames in flight, staging), `surface.c` and `format.c` (render targets, formats), `texture.c` (textures, samplers), `astc.c` (ASTC compute uploads), `buffer.c` (guest memory as buffers), `pipeline.c` (shader modules, pipelines), `render.c` (rendering passes, clears, draws), `compute.c` (compute on the compute queue, results landing in guest memory), `renderer.c` (its side of the renderer interface, presenting), `vma.cpp` (VMA's implementation); Vulkan-Headers, volk and VMA are CPM packages (`cmake/dependencies.cmake`) |
| Shader corpus | `SNX_SHADER_CORPUS=dir` records each translated shader; `build/snx-shaderc dir --validate` translates it directly to SPIR-V and validates it with `spirv-val`, which must be on PATH (`--out DIR` from two builds, diffed, shows a translator change kept the output) |
| Video | `src/nx/gpu/video/`: `host1x.c` (IOVA, ioctls, a channel per nvdrv NVDEC or VIC fd, each NVDEC one with its own decoder), `nvdec.c` (H.264 with FFmpeg's libavcodec, the picture written into the surface its job names), `vic.c` (composes from those surfaces in guest memory) |
| Platform | `src/nx/runtime/platform/`: `plat.c` (logging, fatal), `plat_posix.c` and `plat_win32.c` (threads, locks, time, memory, files per OS), `profiler_win32.c`, `runtime.rc` and `runtime.manifest` (the Windows UTF-8 code page), `unattended.c` (scripted input, timed exit, screenshots, shared by both backends), `backend/sdl3.c` (Linux and Windows, windowed or headless), `backend/gamepad.c` (SDL gamepads, each player's mapping and settings) |

The NX runtime components are CMake object libraries in `src/nx/CMakeLists.txt`: `core`, `kernel`, `hle`, `video`,
`gpu`, `platform`, `shader` and `vk`. Game modules compile the ABI helpers separately. Includes are written
from `src/nx/` (`#include "gpu/vk/vk.h"`). A function or global that another file uses is
declared once, in the header next to the file that defines it (`hle/kernel/kernel.c` and `hle/kernel/kernel.h`), and
everything else is static. The build enforces it: `-Werror=missing-prototypes` and `missing-variable-declarations`.

## Skills and agents

Skills in `.claude/skills/`:
- `snx-build`: building the runtime and games, the deploy step, the Clang presets, and what fails silently.
- `snx-run`: running the game unattended: scripted input, screenshots, logs, repeat runs for intermittent bugs.
- `snx-crash`: from a crash report or `svcBreak` to the mechanism, with the symbol and disassembly tools.
- `snx-service`: implementing or fixing an HLE service against what the game's SDK code expects.
- `snx-code-quality`: RFC #142's C rules (strict ANSI C, no redundant state, one allocation per owner) for C in `src/`.
- `import-upstream`: bringing a new MZ Labs release into `main`.
- `record-lesson`: where a lesson goes so the next session finds it.
- `code-audit`: the multi-pass audit-and-fix loop, with the `snx-*-reviewer` agents in `.claude/agents/review/`.

Agents in `.claude/agents/`, to keep disassembly and logs out of the main context:
- `crash-triage` (read-only): symbolizes and disassembles a crash, and reports the mechanism with its evidence.
- `sdk-scout` (read-only): works out what a game or SDK function does and expects from the runtime.

Domain experts in `.claude/agents/experts/`, for consulting, design or implementation (the prompt says which, and
whether the agent may build and run: only one at a time can):
- `snx-c-expert`: ISO C, undefined behavior, `__atomic` and the memory model, C90 as #142 requires, LLP64.
- `snx-kernel-expert`: the Horizon kernel (SVCs, threads, sync, memory) and the Linux and Windows kernels under it.
- `snx-arm64-expert`: exact ARMv8-A userspace (EL0) semantics for the recompiler's templates and IR emitters.
- `snx-emu-expert`: how Ryujinx, the yuzu family, Citra, Cemu and others solved a problem, with the license of the code.
- `snx-vulkan-expert`: Vulkan 1.3 and SPIR-V, synchronization, lifetimes, Maxwell state onto Vulkan.
- `snx-perf-expert`: profiling and judging optimizations with perf, bpftrace, heaptrack and llvm-mca.
- `snx-gamemath-expert`: math, algorithms and physics in the runtime, HLE services, shader translator and renderer
  (numerics, linear algebra, DSP, sensors, color, time), and the poor algorithms to reject there.

## Git

- `origin` is `git@github.com:scarletnx/ScarletNX.git` (public). Push only when asked. Never force-push a
  pushed branch.
- Commit subjects use a bracketed area prefix (for example, `[gpu]` or `[build]`) followed by an imperative sentence.
  The body says why, and how it was verified. Check
  every claim in the message against the staged diff.
- Stage files by name, and check `git diff --cached --stat` right before committing: a commit takes everything
  staged, including a `git mv` or `git rm` from earlier in the session. Commits end with the `Co-Authored-By` line
  the session's instructions give.

## User-facing text

README, ROADMAP, commit messages, and messages the runtime prints for players read as plain prose: no em-dash
splicing clauses, no semicolons joining sentences, no spaced hyphen as a dash, no rule-of-three flourishes. Code
comments and developer docs under `.claude/` are out of scope.

## Lessons that cost a session

- NVIDIA's stream library sizes its per-syncpoint tables at 128 (`NvRmChannelNumSyncPoints` is a stub that fails).
  Every syncpoint comes from `syncpt_alloc()` in `hle/services/nvdrv.c`.
- The SDK asserts (`svcBreak`) when a service answers in a way real hardware never does, like an npad reporting two
  styles. Look for the assert's condition in the caller before changing anything else.
- Services that "do nothing" can still stall a game: CRI's movie player waits for released audio buffers.
- NVN dual vertex programs: VertexA (slot 0) writes the position, VertexB the varyings. Missing A means a black
  screen with every draw call succeeding.
- The recompiler's old runtime build dropped the H.264 decoder (then OpenH264) silently when the library was missing,
  and movies stayed black. The runtime is only built by CMake now, which fetches FFmpeg with the other libraries.
- SPIR-V lets the driver assume a shader has no infinities or NaNs unless it declares SignedZeroInfNanPreserve, and
  Maxwell shaders rely on them (NVIDIA's compiler makes -inf with `MUFU.LG2` of zero). Without it, lighting came out
  as NaN blocks. The direct SPIR-V emitter declares the float controls requested by `spirv_translate.c` for
  supported shader modules.
- Nothing in a draw's setup may submit or wait for the GPU (`vk_finish`, a readback like `vk_rt_flush`): the frame's
  descriptor pools and staging reset after it, and the draw's sets already allocated turn to garbage. Copy on the GPU
  instead.
- Submodule pins went wrong for a week of commits (a `git submodule add` records the default branch's tip, not the
  tag checked out afterwards). The libraries are CPM packages now (`cmake/dependencies.cmake`), each a release URL with
  its SHA-256: a bump changes both, and the hash is checked against the download. CPM's `OPTIONS` become cache
  variables of the whole build.
