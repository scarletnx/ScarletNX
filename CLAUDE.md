# ScarletNX

ScarletNX is a static recompiler for Switch games, for Linux first. It translates ARM64 code to LLVM IR and compiles
it into a game module (`recompiled/<id>/<id>_code.so`, by title ID). The runtime in `src/nx/` emulates the OS (kernel,
IPC, services) and the GPU (Maxwell command buffers and shaders, drawn with Vulkan).

Check a change against the games that it affects. `README.md` gives the setup and the commands. `docs/ROADMAP.md`
gives the plan and the current state. `docs/ARCHITECTURE.md` tells how the parts fit together. This file holds the
rules that sessions get wrong, and it points to the skills that hold the procedures.

## Rules

- **Claude files and agent messages use ASD-STE100.** CLAUDE.md, the Markdown files in `.claude/` and the memory
  files use Simplified Technical English. The prompts to subagents and the final reports of subagents also use it.
  The `ste100` skill gives the rules. Its checker runs in hooks on each edit, prompt and report, and in the ctest
  `claude_ste100`.
- **Nothing of the game goes into git or to a place outside this machine.** Keys, dumps (XCI, NSP, NCA), `work/`,
  `recompiled/` (code, data, saves, logs) and shader or texture dumps stay local. Commit messages and docs can quote
  addresses, symbol names and short disassembly, but never extracted files. `.claude/hooks/guard.py` blocks the
  obvious ways.
- **A build is not a test.** To verify a runtime change, run the game (`snx-run` skill) through the scene that the
  change affects. The commit tells what you ran. If a change fixes a crash, run the game enough times to show that
  the game does not crash again. One good run does not prove the fix of an intermittent crash.
- **Find the mechanism.** A fix explains why the game does what it does. It tells what the game or SDK code expects
  (`tools/disasm.py`), and what the runtime gave it instead. Do not tune delays, add retries or add special cases for
  a title ID. Syncpoints above 127 "worked" for a whole session while they overwrote the heap of the game.
- **Licenses.** ScarletNX is GPL-3.0-or-later (`LICENSE`). A new file starts with
  `SPDX-License-Identifier: GPL-3.0-or-later` and the copyright line of the project.

  You can port code under GPL-3.0, GPL-2.0-or-later (yuzu, suyu, sudachi, Citron, Eden), LGPL, MIT, BSD, ISC, zlib
  or Apache 2.0. Keep its copyright lines. Name where it came from (Ryujinx is MIT). List it in `docs/CREDITS.md`.
  You cannot port GPL-2.0-only code.

  The primary references are Switchbrew and the behavior of the SDK, which you read from the disassembly. Atmosphère
  (the kernel of Mesosphère, the services of Stratosphère) is the reference for how Horizon behaves. It is
  GPL-2.0-only, so read it, but never port it.
- **GPU work stays on the GPU.** Compute shaders do the swizzles, deswizzles, format conversions and decodes of
  surfaces (`docs/BLOCKLINEAR.md`), with no CPU fallback. The CPU copies guest memory to and from staging in whole
  ranges and does nothing more. Never read GPU or PCIe-mapped memory from the CPU. A readback is the last resort, for
  bytes that the CPU of the guest must see. It reads host-cached memory that the GPU copied into.
- **Strict aliasing, no `-fno-strict-aliasing`.** The configure step stops on that flag in all the targets of
  ScarletNX (`cmake/banned_flags.cmake`). Access an object only through one of these:
  - its own type
  - a character type
  - `memcpy` (`common/utilities/endian.h` for guest and wire bytes)
  - a union member
  - a local that you copy in and out.
- **Signed overflow is a bug, no `-fwrapv`.** The same check stops on `-fwrapv` and `-fno-strict-overflow`, also in
  the runtime library that the game modules link. Do arithmetic that wraps in unsigned types, and then convert it
  back. Give a wider intermediate to a product that can overflow.

  `-DSNX_TRAPV=ON` builds our code and that library with `-ftrapv`. It also adds a trap on left shifts of negative
  values. (`-fwrapv` also defined those shifts, and `-ftrapv` misses them.) Use it to find the overflows that remain.
  It is a build for the tests and a game run, never a build to ship.
- **The language before library calls, and one copy of each helper.** Declare a local where it first gets its
  value, not in a block at the top. Zero a value with `= {0}` or `*p = (T){0}`. Copy a struct by assignment.

  Use `memset` and `memcpy` only where padding bytes matter, or where the types differ. Padding bytes matter when the
  code hashes them, compares them or writes them out. Small helpers (ASCII classes, hex, align, clamp, FNV, endian
  loads) come from `common/utilities/` (`ascii.h`, `bits.h`, `endian.h`, `common.h`), never from a local copy. The
  `snx-code-quality` skill gives the details.
- **Code for tests stays in `tests/`.** `src/` holds what the programs of ScarletNX build and ship. A CPU reference
  for the output of the GPU goes in `tests/support/`. A decoder or a helper that only tests compile also goes there.

  Examples are the CPU decoders of ASTC, ETC2 and BCn, the `*_ref.c` engine references and the NVN decoders. Put
  their declarations in a header beside them, never in a runtime header. Tests include it as `"support/name.h"`.
- Ask the user only about real decisions. When the next step is obvious, do it.

## Environment

- The Bash tool runs **zsh**. `$t:s` is a zsh modifier, so write `"${t}:s"`. The scripts in the repo use bash
  shebangs.
- Put temporary files in the session scratchpad, never in the repo root. Use `run_in_background` for long runs.
- Never use `pkill`, `killall` or a `kill $(pgrep -f PATTERN)` loop. They match the shell that runs the command,
  because its command line contains the pattern. Two times, that killed the shell of the session.

  List the processes with `pgrep -a NAME` (or `pgrep -x cc1`, by process name). Then run `kill PID` with the numbers
  in a separate command. The same applies to a wait: `until ! pgrep -f PATTERN` matches its own shell and never
  ends. Wait for a file that the job writes, or use `pgrep -x NAME`.
- Do not build while a test that is sensitive to time runs. The build deploys a new runtime into `recompiled/` of
  the data folder, and it loads the CPU.
- Do not use `sudo` or install system packages. The user installs them.
- The context is small. Never print a whole log, a whole disassembly or a ninja log. A game log (`<id>.log`) grows to
  megabytes. Use `grep -n`, `tail`, or `tools/disasm.py` on one function.

## Build and run

| Task | Command |
| --- | --- |
| Build the runtime (with hactool in its driver) and the third-party libraries. The build deploys to each game in `recompiled/` of the data folder (`~/.local/share/scarletnx/` on Linux, `SNX_RECOMPILED_DIR`). | `cmake --preset default -B build && cmake --build build`. On Windows, run `scripts\bootstrap.ps1` one time, and `Set-ExecutionPolicy -Scope Process Bypass` and `. scripts\env.ps1` in each window. |
| Cross-build for Windows (llvm-mingw on PATH) | `cmake --preset windows-cross && cmake --build --preset windows-cross` |
| Extract a dump | `build/scarletnx -x "Game.xci" --name Game [--update u.nsp]` |
| Recompile a game into `recompiled/<id>/` of the data folder. The folder has the title ID as its name, or the name that you give after the folder. | `build/scarletnx -r work/Game` |
| Run with a script of inputs and a screenshot | `cd ~/.local/share/scarletnx/recompiled/<id> && ./<id> --seconds=60 --shot=/path/out --script=ms:hexbuttons,...` |
| Name addresses or disassemble game code | `tools/symbols.py work/Game ADDR...` / `tools/disasm.py work/Game FUNC` |
| Summarize a log (also the log of a player), or the game reports on GitHub | `build/scarletnx --scan-log LOG [--format markdown]` / `scripts/game-reports.py index [--title-id ID]`. The `scarletnx` MCP server (`build/scarletnx --mcp`, `.mcp.json`) has both, and the code of the game. Its GDB tools (`mcp_gdb.c`) debug a game that starts with `-S` or `-s`. |
| Run the tests | `ctest --test-dir build` (`-R swipc` for the IPC generator) |

Clang is the only compiler. The configure step stops on any other compiler. The recompiler builds game code with LLVM,
its only backend, and the build must have LLVM. The third-party libraries are CPM packages, with their versions in
`cmake/dependencies.cmake`. The build downloads them one time into `.cache/cpm`, and builds them in the same tree as
the runtime.

The runtime finds its game module by its own file name. For this reason, each game has its own copy of the runtime,
with the name of the game. The deploy step of the build makes these copies. `tools/a64dec` generates the A64 decoder
from `a64.decode` of QEMU and `a64.profile` of ScarletNX (`src/common/cpu/aarch64/decode/`). The LLVM emitters
(`src/common/cpu/recompiler/llvm/ir_int.c`, `ir_ldst.c`, `ir_fpu.c`, `ir_vec*.c`) come from `translate-a64.c` of
QEMU.

Check a change to the decoder or the emitters against `qemu-aarch64` with
`ctest --test-dir build -R llvm_qemu_differential`. Give `--sweep` to the binary for more words and states.
`tests/llvm_qemu_expected.txt` lists the groups that still differ. A new group that differs makes the test fail. A
group in the list that no longer differs also makes the test fail.

Recompile the game code (`_code.so`) only when one of these changes. Together they make the ABI between the generated
code and the runtime:

- `src/common/cpu/recompiler/codegen.c`
- the decoder (`src/common/cpu/aarch64/decode/`, `a64_decoder.c`)
- `loader.c`
- the backend (`src/common/cpu/recompiler/llvm/`, `src/nx/loader/module_*.c`)
- anything in `src/common/cpu/aarch64/`.

If a change there keeps `SNX_ABI` (`cpu.h`), the runtime loads and runs the old code and gives no warning. If you
increase `SNX_ABI`, the runtime refuses each module that a build made before the change. Then you must recompile
each game.

## Where things are

| Area | Files |
| --- | --- |
| C99 host driver (`src/nx/runtime/driver/`) | `driver.c` (dispatch), `extract.c` (XCI and NSP), `recompile.c` (the AOT workflow and data), `toolchain.c` (the build that deploys the runtime). `src/nx/loader/loader.c` (NSO and NRO, metadata). `src/common/cpu/recompiler/codegen.c` (functions) and `src/common/cpu/aarch64/a64_decoder.c` (the generated decoder, from `decode/`). `src/common/utilities/common.c` (host APIs). The LLVM backend is `src/common/cpu/recompiler/llvm/` and `src/nx/loader/module_llvm.c`. In `llvm/`: the IR emitters from QEMU, the ELF, COFF and Mach-O linkers, and `rtlib.c`. `code_loader.c` loads objects into a code arena in the process. `gdb_jit.c` gives them to GDB when the `gdb_jit` setting is on. `jit_service.c` compiles the guest code that the AOT build missed, at run time. |
| Generated-code ABI | `src/common/cpu/aarch64/`: `cpu.h` (with `SNX_ABI`) and `cpu_helpers.c`. Each game module compiles `cpu_helpers.c`, with the topic files that it includes from `helpers/`: `fp.c`, `fp_arithmetic.c`, `integer.c`, `crypto.c`, `vector.c`. |
| The recompiler as a library (`docs/LIBRARY.md`, #171) | `src/common/cpu/recompiler/snx_recompiler.h` (the interface). `library.c` compiles code in memory through the LLVM backend and `llvm/link.c`. It makes a small descriptor in place of the NX `SnxModule`, and loads it from memory. `CMakeLists.txt` makes `scarletnx-recompiler`, one archive of the objects of the backend, and installs it with a CMake package. The test is `tests/recompiler_library.c`. |
| 32-bit games (A32 and Thumb) | `src/common/cpu/arm/`: the block compiler (`README.md`). `rt_helpers.c` has what the blocks leave to the backend. `src/nx/loader/module_arm.c`: block discovery and the `SnxArmModule`. `src/nx/runtime/arm.c`: the thread loop, the block tables, the compiles and hints at run time, and the NX `SnxArmRt`. `snx_svc32` in `hle/svc/svc.c`: the 32-bit SVC registers, which the build generates from `hos.svc`. `mem_layout32` in `memory/mem.c`. `tests/nx_arm32_smoke.py` runs a synthetic 32-bit game. |
| Startup | In `src/nx/runtime/`: `main.c` has the startup, the load of the game module, and the shader screen before the first thread of the game (`prepare_wait`). `options.c` reads the .ini file. `settings.txt` lists each setting of the game, the launcher and the recompiler. From it, `tools/settingsgen` generates `snx_settings.h`, `struct Settings` and `g_settings`, with member names from the setting names, and their tables. It fills in the passes and options of the LLVM that the build uses (`cmake/settings.cmake`). `ini.c` reads and writes the settings of a table. `clock.c`: guest time. `rtapi.c`: what the game module calls. `crash.c`. `present.c`: the present of vi, and the frame pace. `loader.c`: with `loader_segments`, the segments of the executables for the shader pass. `entry.c`. `startup.c`: the switches that the runtime reads before the load. `gdbstub.c`. `guest_jit.c`: the NX side of the runtime JIT. `src/multicall/` is the program itself, with its only `main`. It holds each driver of the build, each a library with no `main`. It runs the driver by the name that started it (links on Linux and macOS: `scarletnx-switch`, `scarletnx-3ds`, `scarletnx-vita`, `scarletnx-gnuabi`, `scarletnx-agb`). On Windows and in its launcher, the name is the first argument. |
| Kernel and memory | `src/nx/hle/kernel/`: `kernel.c` (handles, waits, events) and `thread.c` (threads, the lock for each core). `sync.c`: mutexes, condition variables, address arbitration and WaitSynchronization. `debug.c`: backtraces and dumps. `ipc.c`: the HIPC and CMIF parser, replies, sessions, domains and the service registry. `src/nx/hle/svc/`: `svc.c` (the SVC handlers, `svc_<Name>`) and `svc_mem.c` (the memory SVCs, GetInfo). `hos.svc` has the SVC declarations of Eden. From it, `tools/svcgen` generates the registers of each SVC in the two ABIs, and the tables by number. The NX modules call them through `RtAPI.svcs`. `src/nx/memory/mem.c`, and `own.c` for the guest memory that the GPU owns. The runtime traps that memory until the CPU touches it (`docs/GPU_OWNERSHIP.md`). |
| IPC interfaces | A `.swipc` file beside its C file declares each service interface, in the language of SwIPC (`docs/IPC.md`). `tools/swipc/swipc.c` (C99) generates the dispatchers into the build folder (`snx_swipc` in `cmake/swipc.cmake`, the list in `src/nx/CMakeLists.txt`). The C file of a service includes its generated `.swipc.inc`. It defines one static handler for each command that is not a `@stub`: `nn_ns_IFoo_Command(SvcObj* self, args..., outs...)`. Put a new command in the `.swipc` file first. |
| Services | `src/nx/hle/services/` has one file for each service family: `sm.c`, `am.c`, `set.c`, `time.c`, `acc.c`, `audio.c` (hwopus decodes with FFmpeg), and `misc.c` for the small services. `services.c` registers them. `audren.c`: the update of the audio renderer. `pl.c`: shared fonts. `hid.c`, `vi.c`, `ldr.c`. `network/`: bsd, nifm, dns. `nvdrv.c`: the nvhost devices and syncpoints. The file system is in `src/nx/hle/filesystem/` (`fs.c`, `fs_tree.c`). `src/nx/hle/hook/` has the exported functions of the game that the runtime runs natively. `hooks.hook` declares them, and `tools/hookgen` generates their adapters (`docs/HOOKS.md`). |
| GPU | In `src/nx/gpu/`: `gpu.c` (GPFIFO, the GPU thread). In `maxwell/`: `engines.c` (the methods of the 3D engine), `mme.c` (macros), `compute.c`, `fermi2d.c` and `dma.c`. `fermi2d.c` and `dma.c` give their copies to the renderer. The tests compare the GPU with their CPU references, `tests/support/fermi2d_ref.c` and `dma_ref.c`. `maxwell3d.c`. `write_pages.c` has the last write of the GPU side to each page. `renderer.h`: the `Renderer` table for Vulkan, `g_renderer`. `renderer.c`: the shared state of the renderer. `shader/` translates Maxwell to SPIR-V. In it, `shader.c` does the program hashes and flow, and `decode.c` is the decoder. `tools/shaderdec` generates the matcher of the decoder from `maxwell_opcodes.def`. `spirv_translate.c` and `spirv_compute.c` emit stages through `src/common/utilities/spirv_emit.c`. `vsgen.c` and `fsgen.c` of the 3DS use the same SPIR-V writer. `corpus.c` records shaders. `shipped.c` finds the programs that a game ships in its files and executables. `shipped_pass.c` walks over those files and executables, and gives each program to the renderer. In `texture/`: `depth.c` for packed depth and stencil texels. `blocklinear.comp` deswizzles and swizzles on the GPU, and `texconv.comp` converts the formats that the host cannot use (`docs/BLOCKLINEAR.md`). `nvn_trace.c` (`nvn_trace`): the NVN functions that a game resolves and calls. `tests/support/nvn_control.c` and `nvn_command_gap.c` decode the macro calls of NVN and the write effects of Maxwell methods. Their tests compile them in (`docs/NVN_HLE.md`). |
| Vulkan renderer | In `src/nx/gpu/vk/`: `vk.h` lists the parts. `device.c`: instance, device, queues, swapchain, VMA. `cmd.c`: frames in flight, staging. `surface.c` and `format.c`: render targets and formats. `texture.c`: textures and samplers. `astc.c`: ASTC uploads with compute. `blocklinear.c` and `texconv.c`: the deswizzles, swizzles and format conversions of the GPU. `buffer.c`: guest memory as buffers. `pipeline.c`: shader modules, pipelines, the shaders that the runtime keeps between runs, and the shaders that the game ships. `pipeline_file.c`: the files that keep them, `.vkshaders` and `.vkshipped`. `render.c`: render passes, clears, draws. `compute.c`: compute on the compute queue, with the results in guest memory. `owned.c`: results that the GPU keeps. The runtime publishes them over guest memory, and lands them when the CPU touches that memory. `dma.c`, `blit.c`, `nvdec.c` and `vic.c` do these on the GPU: the DMA engine, the blits of the 2D engine from memory, NVDEC pictures and VIC. NVDEC uses Vulkan Video when the experimental `vulkan_video` setting is on. `renderer.c`: the renderer side of the interface, and the present. `vma.cpp`: the implementation of VMA. Vulkan-Headers, volk and VMA are CPM packages (`cmake/dependencies.cmake`). |
| Shader corpus | `--shader_corpus=dir` (the `[Debug]` setting `shader_corpus`) records each translated shader. `build/snx-shaderc dir --validate` translates the corpus directly to SPIR-V and validates it with `spirv-val`, which must be on PATH. To show that a change to the translator keeps the output, compare the `--out DIR` of two builds. |
| Video | In `src/nx/gpu/video/`: `host1x.c` has the IOVA, the ioctls, and one channel for each NVDEC or VIC fd of nvdrv. Each NVDEC channel has its own decoder. `nvdec.c` decodes H.264 and VP9 with libavcodec of FFmpeg. It puts the picture on the GPU, in the surface that its job names. `vic.c`: the jobs that VIC composes on the GPU (`vic_compose.glsl`). The CPU references for their tests are `tests/support/nvdec_ref.c` and `vic_ref.c`. |
| Shared Vulkan setup | `src/common/gpu/vk/` (`snx_vk_common`) has what the renderers of the Switch, the 3DS and the Vita share. `vk_common.c`: the instance, the queries of layers and extensions, the choice of device and queue family, feature chains, memory types and VkResult names. `ring.c`: the submission ring of the 3DS and the Vita, with fences, command buffers, serials and descriptor sets. `swapchain.c`: the format, extent and image count, and the functions that make and free it. `window.c`: the SDL window, instance and surface. A frontend keeps only its own features, queues and memory. |
| Shared helpers | `src/common/utilities/` has these files. `ascii.h`: character classes with no locale, hex digits, path separators. `bits.h`: align, clamp, min and max, bit fields, FNV-1a, qsort comparators. `endian.h`: loads and stores by byte address. `common.h`: paths, files, libraries, the game folder. Look here before you write a small helper. |
| Platform | In `src/nx/runtime/platform/`: `plat.c` (the log, fatal errors). `plat_sdl.c`: clocks, sleep, libraries and path queries through SDL3. `plat_posix.c` and `plat_win32.c`: threads, condition variables, memory, file maps, and the crash handler of each OS. `profiler_win32.c`. `unattended.c`: scripted input, the timed exit and screenshots, which the two backends share. `overlay.c`: the menu in the game, which the UI quads of the renderer draw. `prepare_screen.c`: the shader screen before the game, which they draw the same way. `backend/sdl3.c`: Linux and Windows, in a window or headless. `backend/gamepad.c`: SDL gamepads, with the mapping and the settings of each player. `src/common/platform/runtime.rc` and `runtime.manifest`: the resources of each Windows program (the UTF-8 code page, the icon). |

The components of the NX runtime are CMake object libraries in `src/nx/CMakeLists.txt`: `core`, `kernel`, `hle`,
`video`, `gpu`, `platform`, `shader` and `vk`. Game modules compile the ABI helpers separately. Write includes from
`src/` (`#include "nx/gpu/vk/vk.h"`).

Declare a function or a global that another file uses one time. Put the declaration in the header next to the file
that defines it (`hle/kernel/kernel.c` and `hle/kernel/kernel.h`). Make everything else static. The build enforces
this with `-Werror=missing-prototypes` and `missing-variable-declarations`.

## Skills and agents

The skills in `.claude/skills/`:

- `snx-build`: how to build the runtime and the games, the deploy step, the Clang presets, and what fails with no
  message.
- `snx-run`: how to run the game with no user: scripted input, screenshots, logs, and repeat runs for intermittent
  bugs.
- `snx-crash`: from a crash report or `svcBreak` to the mechanism, with the symbol and disassembly tools.
- `snx-service`: how to implement or fix an HLE service for what the SDK code of the game expects.
- `snx-code-quality`: the C rules of RFC #142 (strict ISO C99, no redundant state, one allocation for each owner)
  for C in `src/`.
- `snx-gpu-shader`: how to write and optimize our compute shaders (constant tables, bit operations, uniform branches,
  fewer fetches, workgroup structure). It also tells how to prove that they give the bytes of the CPU reference.
- `record-lesson`: where a lesson goes, so that the next session finds it.
- `code-audit`: the audit and fix loop in many passes, with the `snx-*-reviewer` agents in `.claude/agents/review/`.
- `ste100`: the ASD-STE100 rules for the Claude files and for agent prompts and reports, and the checker that the
  hooks run.

The agents in `.claude/agents/` keep disassembly and logs out of the main context:

- `crash-triage` (read-only): symbolizes and disassembles a crash, and reports the mechanism with its evidence.
- `sdk-scout` (read-only): finds what a function of a game or of the SDK does, and what it expects from the runtime.

The domain experts in `.claude/agents/experts/` give advice, design or implementation. The prompt says which, and if
the agent can build and run. Only one agent at a time can build and run.

- `snx-c-expert`: ISO C, undefined behavior, `__atomic` and the memory model, C99 as #142 requires, LLP64.
- `snx-kernel-expert`: the Horizon kernel (SVCs, threads, sync, memory) and the Linux and Windows kernels under it.
- `snx-arm64-expert`: the exact semantics of ARMv8-A userspace (EL0) for the decoder profile and the IR emitters of
  the recompiler.
- `snx-emu-expert`: how Ryujinx, the yuzu family, Citra, Cemu and others solved a problem, with the license of the
  code.
- `snx-vulkan-expert`: Vulkan 1.3 and SPIR-V, synchronization, lifetimes, and how Maxwell state maps onto Vulkan.
- `snx-perf-expert`: how to profile and judge optimizations with perf, bpftrace, heaptrack and llvm-mca.
- `snx-gamemath-expert`: math, algorithms and physics in the runtime, the HLE services, the shader translator and
  the renderer. Examples are numerics, linear algebra, DSP, sensors, color and time. It also knows the poor
  algorithms to reject there.

## Git

- `origin` is `git@github.com:scarletnx/ScarletNX.git` (public). Push only when the user asks. Never force-push a
  pushed branch.
- A commit subject starts with an area prefix in brackets (for example, `[gpu]` or `[build]`), then an imperative
  sentence. The body says why, and how you verified the change. Check each claim in the message against the staged
  diff.
- A commit message says what a game did, not which game it was. Comments, docs and test data do the same. Examples
  are "one game's menus", "another's save load" and "a player's log". A title ID is no better than a name.
- Stage files by name. Run `git diff --cached --stat` just before the commit. A commit takes all the staged changes,
  also a `git mv` or `git rm` from earlier in the session. A commit ends with the `Co-Authored-By` line that the
  instructions of the session give.

## Text for users

README, ROADMAP, commit messages, and the messages that the runtime prints for players are plain prose. They have no
em dash between clauses, no semicolon between sentences, and no spaced hyphen as a dash. They have no rule-of-three
flourishes. This rule does not apply to code comments. The Claude files follow ASD-STE100 (the first rule above).

## Lessons that cost a session

- The stream library of NVIDIA has tables of 128 entries, one entry for each syncpoint
  (`NvRmChannelNumSyncPoints` is a stub that fails). Each syncpoint comes from `syncpt_alloc()` in
  `hle/services/nvdrv.c`.
- The SDK asserts (`svcBreak`) when a service answers in a way that real hardware never does. An example is an npad
  that reports two styles. Look for the condition of the assert in the caller before you change anything else.
- A service that "does nothing" can still stop a game. The movie player of CRI waits until the audio service
  releases the audio buffers.
- In NVN dual vertex programs, VertexA (slot 0) writes the position, and VertexB writes the varyings. Without
  VertexA, the screen is black, and each draw call succeeds.
- The old runtime build of the recompiler dropped the H.264 decoder (OpenH264 at that time) with no message. This
  occurred when the library was not there, and movies stayed black. Now only CMake builds the runtime, and it fetches
  FFmpeg with the other libraries.
- SPIR-V lets the driver assume that a shader has no infinities or NaNs, unless the shader declares
  SignedZeroInfNanPreserve. Maxwell shaders use infinities and NaNs (the compiler of NVIDIA makes -inf with
  `MUFU.LG2` of zero). Without the declaration, the lighting showed NaN blocks. The direct SPIR-V emitter declares the
  float controls that `spirv_translate.c` requests for supported shader modules.
- The setup of a draw must not submit to the GPU or wait for it (`vk_finish`, or a readback like `vk_rt_flush`). The
  descriptor pools and staging of a frame now live until its next present. But a wait lands storage results into
  guest memory that the setup possibly read before. So the draw starts its setup again when `vk_submit_number()`
  changed. Copy on the GPU instead.
- A render target with a wrong guest extent corrupts memory. It is not a local bug. The renderer took a 3D color
  target as an array, because it read the volume bit from the wrong register. The target spanned 63 MB. Its
  write-backs replaced the movie frames of a game and the history of its temporal upscaler. Its flushes cost 24 GPU
  waits in each frame.

  Look for this when the image breaks in places that share nothing, and the frame rate falls too. Then first log
  `rt_bytes()` of the targets that the renderer flushes.
- The submodule pins were wrong for a week of commits. A `git submodule add` records the tip of the default branch,
  not the tag that you check out after it. The libraries are now CPM packages (`cmake/dependencies.cmake`), each a
  release URL with its SHA-256. A bump changes both, and the build checks the hash against the download. The
  `OPTIONS` of CPM become cache variables of the whole build.
