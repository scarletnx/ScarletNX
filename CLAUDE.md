# ScarletNX

ScarletNX is a Linux-first static recompiler for Switch games: ARM64 code is translated to LLVM
IR and compiled into a game module (`recompiled/<id>/<id>_code.so`, by title ID), and the runtime
in `src/nx/` emulates the OS (kernel, IPC, services) and the GPU (Maxwell command buffers and shaders, drawn with
Vulkan). A change is checked against the games it affects. `README.md` has setup and commands, `docs/ROADMAP.md`
the plan and current state, `docs/ARCHITECTURE.md` how the parts fit together. This file holds the rules sessions
get wrong and points to the skills that carry the procedures.

## Rules

- **Nothing of the game goes into git or anywhere outside this machine.** Keys, dumps (XCI, NSP, NCA), `work/`,
  `recompiled/` (code, data, saves, logs) and shader or texture dumps stay local. Commit messages and docs may quote
  addresses, symbol names and short disassembly, never extracted files. `.claude/hooks/guard.py` blocks the
  obvious ways.
- **A build is not a test.** A runtime change is verified by running the game (`snx-run` skill) through the scene
  it affects, and the commit says what was run. A change meant to fix a crash is run enough times to show the crash
  is gone. A single passing run is insufficient for an intermittent crash.
- **Find the mechanism.** A fix explains why the game does what it does: what the game or SDK code expects
  (`tools/disasm.py`), and what the runtime gave it instead. No timing tweaks, retries or special cases for a
  title ID. Syncpoints above 127 "worked" for a whole session while overwriting the game's heap.
- **A driver's bug gets a workaround, scoped to that driver, and only once it is shown to be the driver's.** Some
  frames are wrong because the driver is: Turnip, MoltenVK, and the Mali/MediaTek and Adreno drivers have real
  defects and gaps. Establish that first (the state the runtime submitted and the SPIR-V are right, the same content
  is right on another driver or as the specification says the call behaves), because most wrong output is still
  ours. Where it is the driver and a workaround is the best option, it is the fix rather than a feature dropped.
  Work around the defect that was seen, never a driver that might misbehave. Check the known practice first (the
  specification, the validation layer, another emulator's solution) and keep the workaround to that driver: gated on
  the driver (`driverID`) or the extension or feature it lacks, with a comment naming the driver, its version and
  what it does wrong, so every other driver takes the normal path. `g_vk.has_dynamic_stride` and
  `g_vk_flush_denorm` in `vk/device.c` are the pattern. Check both sides where possible: the driver that needs it,
  and lavapipe or a desktop driver that does not. When the driver is not to hand, the commit says which side was
  actually run. The special case is the driver's, never a title ID's.
- **Licensing.** ScarletNX is GPL-3.0-or-later (`LICENSE`). A new file starts with
  `SPDX-License-Identifier: GPL-3.0-or-later` and the project's copyright line. Code under GPL-3.0,
  GPL-2.0-or-later (yuzu, suyu, sudachi, Citron, Eden), LGPL, MIT, BSD, ISC, zlib or Apache 2.0 can be ported,
  keeping its copyright lines and naming where it came from (Ryujinx is MIT), and is listed in `docs/CREDITS.md`.
  GPL-2.0-only code can't be. Switchbrew and the SDK's own behavior, read from the disassembly, are the primary
  references, with Atmosphère (Mesosphère's kernel, Stratosphère's services) as the reference for how Horizon
  behaves. It is GPL-2.0-only, so it is read, never ported.
- **GPU work stays on the GPU.** Swizzling, deswizzling, format conversion and decoding of surfaces run as compute
  shaders (`docs/BLOCKLINEAR.md`), with no CPU fallback. The CPU copies guest memory to and from staging in whole
  ranges and nothing more. Never read GPU or PCIe-mapped memory from the CPU: a readback is the last resort, for
  bytes the guest's CPU must see, and it reads host-cached memory the GPU copied into.
- **Strict aliasing, no `-fno-strict-aliasing`.** The configure stops on that flag anywhere in ScarletNX's own targets
  (`cmake/banned_flags.cmake`). An object is accessed through its own type, a character type or `memcpy`
  (`common/utilities/endian.h` for guest and wire bytes), a union member, or a local copied in and out.
- **Signed overflow is a bug, no `-fwrapv`.** The same check stops on `-fwrapv` and `-fno-strict-overflow`, the
  runtime library the game modules link included. Arithmetic that wraps is done in unsigned types and converted back,
  a product that can overflow gets a wider intermediate. `-DSNX_TRAPV=ON` builds our code and that library with
  `-ftrapv` and a trap on left shifts of negative values (which `-fwrapv` also defined and `-ftrapv` misses) to find
  the ones left (a build for the tests and a game run, never one to ship).
- **Test-only code lives in `tests/`.** `src/` holds what ScarletNX's programs build and ship. A CPU reference the
  GPU's output is checked against, a decoder or a helper that only tests compile goes in `tests/support/` (the CPU
  decoders of ASTC, ETC2 and BCn, the `*_ref.c` engine references, the NVN decoders), with its declarations in a
  header beside it, never in a runtime header. Tests include it as `"support/name.h"`.
- Ask only real decisions. When the next step is obvious, take it.

## Environment

- The Bash tool runs **zsh**: `$t:s` is a zsh modifier, so write `"${t}:s"`. Scripts in the repo use bash shebangs.
- Temporary files go in the session scratchpad, never in the repo root. Long runs use `run_in_background`.
- Never `pkill`/`killall`, or a `kill $(pgrep -f PATTERN)` loop: they match the shell running the command, whose
  command line contains the pattern (twice that killed the session's own shell). List with `pgrep -a NAME` (or
  `pgrep -x cc1`, by process name), then `kill PID` with the numbers in a separate command. The same goes for waiting:
  `until ! pgrep -f PATTERN` matches its own shell and never ends. Wait on a file the job writes, or `pgrep -x NAME`.
- Don't build while a timing-sensitive test runs: the build deploys a new runtime into the data folder's `recompiled/` and loads the CPU.
- No `sudo` and no system packages: the user installs those.
- Context is scarce. Never print a whole log (a game's `<id>.log` grows to megabytes), a whole disassembly or a
  ninja log. Use `grep -n`, `tail`, `tools/disasm.py` on one function.

## Build and run

| Task | Command |
| --- | --- |
| Build the runtime (hactool inside its driver) and the third-party libraries (deploys to every game in the data folder's `recompiled/`, `~/.local/share/scarletnx/` on Linux: `SNX_RECOMPILED_DIR`) | `cmake --preset default -B build && cmake --build build` (Windows: `scripts\bootstrap.ps1` once, `Set-ExecutionPolicy -Scope Process Bypass` and `. scripts\env.ps1` per window) |
| Cross-build for Windows (llvm-mingw on PATH) | `cmake --preset windows-cross && cmake --build --preset windows-cross` |
| Extract a dump | `build/scarletnx -x "Game.xci" --name Game [--update u.nsp]` |
| Recompile a game (into the data folder's `recompiled/<id>/`, named by its title ID, or by a name given after the folder) | `build/scarletnx -r work/Game` |
| Run with scripted input and a screenshot | `cd ~/.local/share/scarletnx/recompiled/<id> && ./<id> --seconds=60 --shot=/path/out --script=ms:hexbuttons,...` |
| Name addresses / disassemble game code | `tools/symbols.py work/Game ADDR...` / `tools/disasm.py work/Game FUNC` |
| Summarize a log (a player's too) / the game reports on GitHub | `build/scarletnx --scan-log LOG [--format markdown]` / `scripts/game-reports.py index [--title-id ID]`; the `scarletnx` MCP server (`build/scarletnx --mcp`, `.mcp.json`) has both and the game's code, and its GDB tools (`mcp_gdb.c`) debug a game started with `-S` or `-s` |
| Run the tests | `ctest --test-dir build` (`-R swipc` for the IPC generator) |

Clang is the only compiler: the configure stops on any other; the recompiler builds game code with LLVM (required), its
only backend. The third-party libraries are CPM packages pinned in `cmake/dependencies.cmake`, downloaded once into `.cache/cpm`, and
built in the same tree as the runtime.

The runtime finds its game module by its own file name, so each game has a copy of the runtime named after it, made
by the build's deploy step. The A64 decoder is generated by `tools/a64dec` from QEMU's `a64.decode` and ScarletNX's
`a64.profile` (`src/common/cpu/aarch64/decode/`), and the LLVM emitters (`src/common/cpu/recompiler/llvm/ir_int.c`,
`ir_ldst.c`, `ir_fpu.c`, `ir_vec*.c`) are ported from QEMU's `translate-a64.c`. A change to either is checked against
`qemu-aarch64` with `ctest --test-dir build -R llvm_qemu_differential` (`--sweep` on the binary for more words and
states). The groups still known to differ are in `tests/llvm_qemu_expected.txt`: a new one fails the test, and so
does a listed one that no longer differs. Game code (`_code.so`) only needs recompiling when `src/common/cpu/recompiler/codegen.c`, the decoder (`src/common/cpu/aarch64/decode/`, `a64_decoder.c`),
`loader.c`, the backend (`src/common/cpu/recompiler/llvm/`, `src/nx/loader/module_*.c`) or anything in
`src/common/cpu/aarch64/` changes (the
ABI between generated code and runtime). A change there that keeps `SNX_ABI` (`cpu.h`) loads and runs the old code
silently. Bumping `SNX_ABI` makes the runtime refuse every module built before it, so every game needs recompiling.

## Where things are

| Area | Files |
| --- | --- |
| C99 host driver (`src/nx/runtime/driver/`) | `driver.c` (dispatch), `extract.c` (XCI/NSP), `recompile.c` (AOT workflow/data), `toolchain.c` (runtime deploy build); `src/nx/loader/loader.c` (NSO/NRO, metadata), `src/common/cpu/recompiler/codegen.c` (functions) + `src/common/cpu/aarch64/a64_decoder.c` (the generated decoder, from `decode/`), `src/common/utilities/common.c` (host APIs); the LLVM backend: `src/common/cpu/recompiler/llvm/` (IR emitters ported from QEMU, ELF/COFF/Mach-O linkers, `rtlib.c`, `code_loader.c` loading objects into an in-process code arena, `gdb_jit.c` handing them to GDB under the `gdb_jit` setting, `jit_service.c` compiling missed guest code at run time) + `src/nx/loader/module_llvm.c` |
| Generated-code ABI | `src/common/cpu/aarch64/`: `cpu.h` (with `SNX_ABI`), `cpu_helpers.c` (compiled into each game module, with the topic files it includes from `helpers/`: `fp.c`, `fp_arithmetic.c`, `integer.c`, `crypto.c`, `vector.c`) |
| The recompiler as a library (`docs/LIBRARY.md`, #171) | `src/common/cpu/recompiler/snx_recompiler.h` (the interface), `library.c` (code in memory through the LLVM backend and `llvm/link.c`, a small descriptor instead of the NX `SnxModule`, loaded from memory), `CMakeLists.txt` (`scarletnx-recompiler`, one archive of the backend's objects, installed with a CMake package), `tests/recompiler_library.c` |
| 32-bit games (A32/Thumb) | `src/common/cpu/arm/` (the block compiler, `README.md`; `rt_helpers.c`, what blocks leave to the backend), `src/nx/loader/module_arm.c` (block discovery, the `SnxArmModule`), `src/nx/runtime/arm.c` (thread loop, block tables, run-time compiles and hints, the NX `SnxArmRt`), `snx_svc32` in `hle/svc/svc.c` (the 32-bit SVC registers, generated from `hos.svc`), `mem_layout32` in `memory/mem.c`; `tests/nx_arm32_smoke.py` runs a synthetic one |
| Startup | `src/nx/runtime/`: `main.c` (startup, loading the game module, the shader loading screen before the game's first thread: `prepare_wait`), `options.c` (the .ini), `settings.txt` (every setting, the game's, the launcher's and the recompiler's: `tools/settingsgen` generates `snx_settings.h`, `struct Settings` and `g_settings`, member names from the settings' names, and their tables, with LLVM's passes and options filled in from the LLVM built against; `cmake/settings.cmake`), `ini.c` (reading and writing a table's settings), `clock.c` (guest time), `rtapi.c` (what the game module calls), `crash.c`, `present.c` (vi's present and the frame pacing), `loader.c` (with `loader_segments`, the executables' segments for the shader pass), `entry.c`, `startup.c` (switches parsed before loading), `gdbstub.c`, `guest_jit.c` (the runtime JIT's NX side); `src/multicall/`: the program itself and its only `main`, every driver the build has (each a library with no `main`), run by the name it is started under (links on Linux and macOS: `scarletnx-switch`, `scarletnx-3ds`, `scarletnx-vita`, `scarletnx-gnuabi`, `scarletnx-agb`) or by that name as its first argument (Windows, and its launcher) |
| Kernel and memory | `src/nx/hle/kernel/`: `kernel.c` (handles, waits, events), `thread.c` (threads, the per-core lock), `sync.c` (mutexes, condition variables, address arbitration, WaitSynchronization), `debug.c` (backtraces, dumps), `ipc.c` (HIPC/CMIF parsing and replies, sessions, domains, the service registry); `src/nx/hle/svc/`: `svc.c` (the SVC handlers, `svc_<Name>`), `hos.svc` (Eden's SVC declarations: `tools/svcgen` generates each SVC's registers in both ABIs and the tables by number, which the NX modules call through `RtAPI.svcs`), `svc_mem.c` (memory SVCs, GetInfo); `src/nx/memory/mem.c`, `own.c` (guest memory the GPU owns, trapped until the CPU touches it: `docs/GPU_OWNERSHIP.md`) |
| IPC interfaces | Every service interface is declared in a `.swipc` file beside its C file (SwIPC's language, `docs/IPC.md`). `tools/swipc/swipc.c` (C99) generates the dispatchers into the build folder (`snx_swipc` in `cmake/swipc.cmake`, the list in `src/nx/CMakeLists.txt`). A service's C file includes its generated `.swipc.inc` and defines one static handler per command that isn't a `@stub`, `nn_ns_IFoo_Command(SvcObj* self, args..., outs...)`. A new command goes in the `.swipc` file first |
| Services | `src/nx/hle/services/`: one file per service family (`sm.c`, `am.c`, `set.c`, `time.c`, `acc.c`, `audio.c` (hwopus decodes with FFmpeg), `misc.c` for the small ones, `services.c` registers them), `audren.c` (the audio renderer's update), `pl.c` (shared fonts), `hid.c`, `vi.c`, `ldr.c`, `network/` (bsd, nifm, dns), `nvdrv.c` (nvhost devices, syncpoints); `src/nx/hle/filesystem/` (`fs.c`, `fs_tree.c`) |
| GPU | `src/nx/gpu/`: `gpu.c` (GPFIFO, the GPU thread), `maxwell/` (`engines.c` 3D engine methods, `mme.c` macros, `compute.c`, `fermi2d.c`, `dma.c` (both hand their copies to the renderer; their CPU references, which the tests hold the GPU to, are `tests/support/fermi2d_ref.c` and `dma_ref.c`), `maxwell3d.c`, `write_pages.c`: the last GPU-side write to each page), `renderer.h` (the `Renderer` table for Vulkan, `g_renderer`), `renderer.c` (shared renderer state), `shader/` (Maxwell to SPIR-V: `shader.c` handles program hashing and flow, `decode.c` the decoder, its matcher generated from `maxwell_opcodes.def` by `tools/shaderdec`, `spirv_translate.c` and `spirv_compute.c` emit stages through `src/common/utilities/spirv_emit.c` (the SPIR-V writer the 3DS's `vsgen.c` and `fsgen.c` share), `corpus.c` records shaders, `shipped.c` finds the programs a game ships in its files and executables), `shipped_pass.c` (the walk over those files and executables that hands each program to the renderer), `texture/` (`depth.c`: packed depth/stencil texels; `blocklinear.comp` deswizzles and swizzles on the GPU, `texconv.comp` converts formats the host can't take, `docs/BLOCKLINEAR.md`), `nvn_trace.c` (`nvn_trace`: the NVN functions a game resolves and calls); `tests/support/nvn_control.c` and `nvn_command_gap.c` decode NVN's macro calls and Maxwell methods' write effects, built into their tests (`docs/NVN_HLE.md`) |
| Vulkan renderer | `src/nx/gpu/vk/`: `vk.h` (lists the parts), `device.c` (instance, device, queues, swapchain, VMA), `cmd.c` (frames in flight, staging), `surface.c` and `format.c` (render targets, formats), `texture.c` (textures, samplers), `astc.c` (ASTC compute uploads), `blocklinear.c` and `texconv.c` (the GPU's deswizzles, swizzles and format conversions), `buffer.c` (guest memory as buffers), `pipeline.c` (shader modules, pipelines, the shaders kept across runs and those the game ships), `pipeline_file.c` (the files they are kept in, `.vkshaders` and `.vkshipped`), `render.c` (rendering passes, clears, draws), `compute.c` (compute on the compute queue, results landing in guest memory), `owned.c` (results kept on the GPU, published over guest memory, landed when the CPU touches it), `dma.c`, `blit.c`, `nvdec.c` and `vic.c` (the DMA engine, the 2D engine's blits from memory, NVDEC pictures, with Vulkan Video under the experimental `vulkan_video` setting, and VIC, all on the GPU), `renderer.c` (its side of the renderer interface, presenting), `vma.cpp` (VMA's implementation); Vulkan-Headers, volk and VMA are CPM packages (`cmake/dependencies.cmake`) |
| Shader corpus | `--shader_corpus=dir` (the `[Debug]` setting `shader_corpus`) records each translated shader; `build/snx-shaderc dir --validate` translates it directly to SPIR-V and validates it with `spirv-val`, which must be on PATH (`--out DIR` from two builds, diffed, shows a translator change kept the output) |
| Video | `src/nx/gpu/video/`: `host1x.c` (IOVA, ioctls, a channel per nvdrv NVDEC or VIC fd, each NVDEC one with its own decoder), `nvdec.c` (H.264 and VP9 with FFmpeg's libavcodec, the picture laid out in the surface its job names on the GPU), `vic.c` (the jobs VIC composes on the GPU, `vic_compose.glsl`); their CPU references for the tests are `tests/support/nvdec_ref.c` and `vic_ref.c` |
| Platform | `src/nx/runtime/platform/`: `plat.c` (logging, fatal), `plat_sdl.c` (clocks, sleeping, libraries and path queries through SDL3), `plat_posix.c` and `plat_win32.c` (threads, condition variables, memory, file maps, the crash handler per OS), `profiler_win32.c`, `unattended.c` (scripted input, timed exit, screenshots, shared by both backends), `overlay.c` (the in-game menu, drawn with the renderer's UI quads), `prepare_screen.c` (the shader loading screen before the game, drawn the same way), `backend/sdl3.c` (Linux and Windows, windowed or headless), `backend/gamepad.c` (SDL gamepads, each player's mapping and settings); `src/common/platform/runtime.rc` and `runtime.manifest`, every Windows program's resources (the UTF-8 code page, the icon) |

The NX runtime components are CMake object libraries in `src/nx/CMakeLists.txt`: `core`, `kernel`, `hle`, `video`,
`gpu`, `platform`, `shader` and `vk`. Game modules compile the ABI helpers separately. Includes are written
from `src/` (`#include "nx/gpu/vk/vk.h"`). A function or global that another file uses is
declared once, in the header next to the file that defines it (`hle/kernel/kernel.c` and `hle/kernel/kernel.h`), and
everything else is static. The build enforces it: `-Werror=missing-prototypes` and `missing-variable-declarations`.

## Skills and agents

Skills in `.claude/skills/`:
- `snx-build`: building the runtime and games, the deploy step, the Clang presets, and what fails silently.
- `snx-run`: running the game unattended: scripted input, screenshots, logs, repeat runs for intermittent bugs.
- `snx-crash`: from a crash report or `svcBreak` to the mechanism, with the symbol and disassembly tools.
- `snx-service`: implementing or fixing an HLE service against what the game's SDK code expects.
- `snx-code-quality`: RFC #142's C rules (strict ISO C99, no redundant state, one allocation per owner) for C in `src/`.
- `snx-gpu-shader`: writing and optimizing our compute shaders (constant tables, bit twiddling, uniform branches,
  fewer fetches, workgroup structure) and proving they give the CPU reference's bytes.
- `record-lesson`: where a lesson goes so the next session finds it.
- `code-audit`: the multi-pass audit-and-fix loop, with the `snx-*-reviewer` agents in `.claude/agents/review/`.

Agents in `.claude/agents/`, to keep disassembly and logs out of the main context:
- `crash-triage` (read-only): symbolizes and disassembles a crash, and reports the mechanism with its evidence.
- `sdk-scout` (read-only): works out what a game or SDK function does and expects from the runtime.

Domain experts in `.claude/agents/experts/`, for consulting, design or implementation (the prompt says which, and
whether the agent may build and run: only one at a time can):
- `snx-c-expert`: ISO C, undefined behavior, `__atomic` and the memory model, C99 as #142 requires, LLP64.
- `snx-kernel-expert`: the Horizon kernel (SVCs, threads, sync, memory) and the Linux and Windows kernels under it.
- `snx-arm64-expert`: exact ARMv8-A userspace (EL0) semantics for the recompiler's decoder profile and IR emitters.
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
- Commit messages, like comments, docs and test data, say what a game did, not which game it was: "one game's menus",
  "another's save load", "a player's log". A title ID is no better than a name.
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
- Nothing in a draw's setup should submit or wait for the GPU (`vk_finish`, a readback like `vk_rt_flush`). A frame's
  descriptor pools and staging live until its next present now, but a wait lands storage results into guest memory
  the setup may already have read, so the draw starts its setup over when `vk_submit_number()` changed. Copy on the
  GPU instead.
- A render target with a wrong guest extent is a memory corruptor, not a local bug. A 3D color target taken as an
  array (its volume bit read from the wrong register) spanned 63 MB: its write-backs replaced a game's movie frames
  and its temporal-upscaler history, and its flushes cost 24 GPU waits a frame. When rendering breaks in places that
  share nothing and the frame rate collapses with it, log rt_bytes() of the targets being flushed first.
- Submodule pins went wrong for a week of commits (a `git submodule add` records the default branch's tip, not the
  tag checked out afterwards). The libraries are CPM packages now (`cmake/dependencies.cmake`), each a release URL with
  its SHA-256: a bump changes both, and the hash is checked against the download. CPM's `OPTIONS` become cache
  variables of the whole build.
