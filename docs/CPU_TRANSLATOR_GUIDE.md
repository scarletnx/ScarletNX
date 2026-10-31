<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Working on the CPU implementation plan

This guide is for whoever implements the [CPU implementation plan](CPU_TRANSLATOR_PLAN.md). The plan
says what to build. This guide says how to work in this repository: what to set up and what to read, which project
rules apply, and how each item is proven and handed back.

The plan and this guide were checked against the code at commit `05324b57` on `main` (2026-10-29). If the code has
moved since, check an item's "Now" against the current code before you start it.

## The job

* Start with [First CPU milestone: one measured SIMD improvement](CPU_SIMD_MILESTONE.md). Profile the real cost, choose
  one group, prove an exact vector replacement, and measure the same scene before and after. This is the current scope.
* Use the main plan as a supporting backlog. Choose later items from the milestone's evidence, rather than treating
  the entire phase table as a required implementation sequence.
* An item is done when its proof passes and the checklist under [Definition of done](#definition-of-done) holds.
* Add only the measurements and safety tests the first milestone needs. The complete T1 through T6 program does not gate it.
  I2, I3, I6 and I13 still require the block fuzzer (T5) before their larger ABI/register or atomic-loop changes.
* The maintainer answers the questions under "Decisions for the maintainers" in the plan. Until an answer comes,
  follow the recommendation written there.
* Items marked as an experiment or as research (M3, I5, R2) end in a short written report with measurements. Code
  follows only if the numbers justify it.

Read the plan's "OS cooperation and the Horizon boundary" section before changing runtime entry or state visibility.
ScarletNX must provide Horizon's observable behavior through its HLE runtime and the planned SDK translation layer.
SVC argument declarations alone do not prove a reduced state-save contract safe.

Maintainer feedback settles A1 through A5: implement A1 and normal A2 coverage, measure A3's optimization tradeoff,
keep A4 temporary pending the linked SDK layer, and leave A5 profile collection and use opt-in. Today's final linker
consumes native objects. Adding LTO requires retained IR and an IR link/import stage, with dependency-aware cache keys.
A2 also searches validated custom RTTI and associated method tables for otherwise-unreferenced functions. Reuse and
deduplicate existing relocation seeds. Confirm the custom layout before treating a table entry as a method address.

Tiered compilation (J1) is deferred. Standalone PLT binding (L1) is retired from active work because the maintainer plans
to expand hooking by Saturday, 2026-10-31. Coordinate SDK entry replacement with that system instead of adding another
binder. Preserve hook precedence in direct calls, JIT publication and invalidation. Unhooked calls keep normal dispatch.
The date records the maintainer's plan, not a verified delivery or a claim of complete import coverage.

For vector work, I7 only reduces register synchronization. Follow I15 to remove serial lane calls. First inspect LLVM's
optimized IR and host assembly to establish what it already vectorizes. Prove guest results and FPSR independently
of the instruction counts. The compiler experiments in I15 establish a lowering gap, not an end-to-end speedup.
The first milestone includes a conditional performance estimate. Treat its 5 to 18 percent scenario as arithmetic
under explicit assumptions, not a promised gain. Keep Rosetta observations distinct from proposed ScarletNX mechanisms.

## What you need

* **A Linux x86-64 machine** for most of the work. An AArch64 Linux machine helps for I12, M1 and R2. PowerPC64,
  RISC-V and LoongArch items can be proven under QEMU where no hardware is at hand.
* **The toolchain** from the "Prerequisites" of [Build](BUILD.md): Clang (CI uses 23), LLVM's development files
  (`llvm-config`), CMake 3.25 or newer, Ninja, Python 3, nasm and pkg-config. The configure step stops on any compiler
  other than Clang.
* **For the QEMU differential tests:** `qemu-aarch64`, Clang's `aarch64-linux-gnu` target and `ld.lld`.
  `cmake/qemu_ref.cmake` turns the QEMU reference on only when all three are present.
* **A Vulkan 1.3 driver** to run games.
* **Your own Switch keys and game dumps** for game runs. They must come from your own console, and nobody can send them
  to you. Never commit or share them, or anything extracted from them.
* **For speed work:** `perf`, and `llvm-mca` from LLVM.

## Get the code and build it

```sh
git clone https://github.com/scarletnx/ScarletNX.git
cd ScarletNX
cmake --preset default -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The first build downloads the third-party libraries once into `.cache/cpm`. Every build deploys the runtime into each
game in the data folder (`~/.local/share/scarletnx/recompiled/` on Linux), so do not build while a timing-sensitive test
or game run is going.

The commands you will use most, from the table in `CLAUDE.md`:

| Task | Command |
| --- | --- |
| Extract a dump | `build/scarletnx -x "Game.xci" --name Game` |
| Recompile a game | `build/scarletnx -r work/Game` |
| Run a game unattended | `cd ~/.local/share/scarletnx/recompiled/<id> && ./<id> --seconds=60 --shot=/path/out --script=...` |
| Name addresses, disassemble one function | `tools/symbols.py work/Game ADDR...`, `tools/disasm.py work/Game FUNC` |
| All tests | `ctest --test-dir build --output-on-failure` |
| The QEMU differential test | `ctest --test-dir build -R llvm_qemu_differential` |
| The module contract test | `ctest --test-dir build -R module_contract` |

`tools/run_game.py` wraps an unattended run with scripted input and screenshots. The procedure is in
`.claude/skills/snx-run/SKILL.md`.

## What to read, in order

1. [README](../README.md): what the project is, and the hosts it builds for.
2. [Architecture](ARCHITECTURE.md), the sections "Recompiler + module ABI" and "Guest exec": the pipeline, the contract
   between generated code and the runtime, the `SNX_ABI` rules, and how patched code is handled.
3. [The first SIMD milestone](CPU_SIMD_MILESTONE.md), then the relevant entries in [the plan](CPU_TRANSLATOR_PLAN.md).
4. `AGENTS.md` at the root of the repository: the current project rules and source map. Follow applicable local
   instructions when work starts, and use `CLAUDE.md` as additional context where it still applies.
5. [Build](BUILD.md) and [Development](DEVELOPMENT.md): build variants, tests and profiling.
6. [Usage](USAGE.md): extract, recompile and run, the folders, and the settings.
7. When an item needs them:
   * [GNU ABI](GNUABI.md) for T1, R1 and S1.
   * [AOT packs](AOT_PACKS.md), [ldr:ro](LDR_RO.md) and [NRO files](NRO.md) for the J items and L4.
   * [Cortex-A57 divergences](DIVERGENCES.md) for the FP items.
   * [The recompiler as a library](LIBRARY.md).
   * [macOS and iOS](APPLE.md), [Android](ANDROID.md), [PowerPC64](PPC64.md) and
     [RISC-V and LoongArch](RISCV-LOONGARCH.md) for the host notes.
   * [Licenses and credits](CREDITS.md) before you port anything.
8. The procedures in `.claude/skills/`: `snx-build`, `snx-run`, `snx-code-quality` and `snx-crash`. They are written
   for Claude Code but read well as checklists.

If you work with Claude Code in this repository, it loads `CLAUDE.md` and the skills by itself. Point it at this guide
and at the plan.

## Where the translator lives

| Area | Files |
| --- | --- |
| Finding functions | `src/common/cpu/recompiler/codegen.c`, `src/nx/loader/loader.c`, `src/nx/loader/codegen_view.c` |
| The instruction emitters, ported from QEMU's `translate-a64.c` | `ir_int.c`, `ir_ldst.c`, `ir_fpu.c`, `ir_vec.c`, `ir_vec_fp.c`, `ir_vec_crypto.c` in `src/common/cpu/recompiler/llvm/` |
| A guest function's IR: calls, returns, polls, stale checks | `ir_codegen.c` |
| Guest state, flags and memory accesses | `ir_state.c` |
| Guest registers as locals, and the call sites | `ir_promote.c` |
| Helpers, `snx_lookup`, calls into the runtime | `ir_helpers.c`, `ir_emit.c` |
| The pass pipeline and the target CPU | `ir.c` |
| The branch profile | `ir_profile.c` |
| Units, the unit cache, the module descriptor | `src/nx/loader/module_llvm.c`, `src/nx/loader/unit_cache.c`, `src/nx/loader/module_abi.h` |
| Linkers and the code loader | `link_elf.c`, `link_coff.c`, `link_macho.c`, `code_loader.c`, `elf_object.c` |
| The runtime JIT and its cache | `jit_service.c`, `jit_cache.c`, `src/nx/runtime/guest_jit.c`, `src/nx/runtime/rtapi.c` |
| The contract with generated code | `src/common/cpu/aarch64/cpu.h` (`SNX_ABI`, `CPU`, `snx_lookup`), `cpu_helpers.c`, `helpers/`, `rtlib.c` |
| The decoder | `src/common/cpu/aarch64/decode/` (`a64.decode`, `a64.profile`), generated by `tools/a64dec` |
| SVCs | `src/nx/hle/svc/hos.svc`, `tools/svcgen` |
| Guest memory | `src/nx/memory/mem.c`, `src/nx/memory/own.c` |
| Guest threads | `src/nx/hle/kernel/thread.c` |
| The GNU ABI runtime | `src/gnuabi/` |
| Translator tests | `tests/llvm_qemu_differential.c`, `tests/llvm_qemu_expected.txt`, `tests/a64_ported.c`, `tests/llvm_module.c`, `tests/llvm_bench.c`, `tests/module_contract.c`, `tests/qemu_ref/` |

## Rules that apply to this work

These come from `CLAUDE.md`. The ones below are those this work runs into most.

**Licenses.** ScarletNX is GPL-3.0-or-later. A new file starts with the SPDX line and the project's copyright line, as
this guide does. You may port code under GPL-3.0, GPL-2.0-or-later, LGPL, MIT, BSD, ISC, zlib or Apache 2.0. Keep its
copyright lines, say where it came from, and list it in `docs/CREDITS.md`. Never port GPL-2.0-only code. Atmosphère is
GPL-2.0-only: read it, never port it. Never take code from the attesor repository that the plan mentions, because it
holds decompiled Apple code.

**Find the mechanism.** A fix explains why the guest does what it does, from what the game or SDK code expects
(`tools/disasm.py`) and what the runtime gave it instead. Do not tune delays, add retries or add special cases for a
title ID.

**A build is not a test.** Verify a runtime change by running a game through the scene the change affects, and say in
the commit what you ran. For a crash fix, run enough times to show the crash is gone, since one good run proves nothing
about an intermittent crash.

**C.** Clang only, strict ISO C99 (`.claude/skills/snx-code-quality/SKILL.md`).

* No `-fno-strict-aliasing`. Access an object through its own type, a character type, `memcpy` or
  `common/utilities/endian.h`, a union member, or a local copied in and out.
* Signed overflow is a bug, and there is no `-fwrapv`. Do wrapping arithmetic in unsigned types. `-DSNX_TRAPV=ON` builds
  with `-ftrapv` to find overflows.
* Declare a local where it first gets its value. Take small helpers from `src/common/utilities/` (`ascii.h`, `bits.h`,
  `endian.h`, `common.h`), never from a local copy.
* A function or global another file uses is declared in the header beside the file that defines it, and everything
  else is static. The build enforces this with `-Werror=missing-prototypes`.
* Code only tests use goes in `tests/`, and CPU references for tests go in `tests/support/`.

**The contract with generated code.** A change to `codegen.c`, the decoder, `loader.c`, the LLVM backend or anything in
`src/common/cpu/aarch64/` means games must be recompiled to get it. A change to the contract itself raises `SNX_ABI` in
`cpu.h`, updates `tests/module_contract.c` and adds a row to the table in `docs/ARCHITECTURE.md`. Every bump forces a
full recompile of every game, so group bumps where the plan allows it, such as I3 with I4.

**Emitters.** Check every emitter or decoder change against QEMU with `ctest -R llvm_qemu_differential`, and give the
test binary `--sweep` for more words and states. A group that newly differs fails the test, and so does a group in
`tests/llvm_qemu_expected.txt` that no longer differs. Update that file in the same change.

**Settings.** A new setting goes in `src/nx/runtime/settings.txt`, from which `tools/settingsgen` generates the code
that reads it. A setting that changes generated code must also join the JIT cache key in `jit_cache.c`, as
`volatile_memory` does. A mode that changes results stays a setting whose default keeps the exact behavior.

**Game data.** Keys, dumps, `work/`, `recompiled/`, game logs, and shader or texture dumps stay on your machine. Commit
messages and docs may quote addresses, symbol names and short disassembly, but never extracted files. They never name
the game or its title ID either. Write "one game's menus" or "another game's save".

**Processes.** Never use `pkill`, `killall` or `kill $(pgrep -f PATTERN)`, because they can match the shell that runs
them. List the processes with `pgrep -a NAME` and kill by number. Game logs grow to megabytes, so read them with
`grep` and `tail`.

## How to hand work back

* Use one branch and one pull request per item, or per group of items that must land together. Put the item ID in the
  title.
* A commit subject starts with an area in brackets, such as `[cpu]` or `[build]`, followed by an imperative sentence.
  The body says why the change was made and how you verified it, and every claim in it matches the staged diff.
* Commit messages and docs are plain prose: no em dash between clauses, and no semicolon between sentences.
* Each pull request states:
  * the mechanism before and after
  * the proof the plan asks for, with the commands you ran and their results
  * for a speed item, the numbers before and after (T1 or `llvm_bench`), as a median with its spread over repeated
    runs, and the host CPU
  * for a runtime change, the scenes you ran and how many times, without naming the game
  * whether `SNX_ABI` changed
* Push only your own branches. Never force-push a branch you already pushed. The maintainer merges.
* Update the docs the change makes wrong. In the plan, mark the item done and record what it measured.

## Definition of done

For the first milestone, use its acceptance table and record actual host coverage. Preserve an exact fallback on hosts
without runtime validation. The broader host requirements below apply as later backlog items are implemented.

* The item's proof passes on x86-64 Linux, and on every other host the item names (under QEMU where no hardware exists).
* `ctest --test-dir build` passes, with no new group in `tests/llvm_qemu_expected.txt`.
* No guest-visible result changes, unless it sits behind a setting whose default keeps the exact behavior.
* `SNX_ABI` rose if and only if the contract changed, and `tests/module_contract.c` and `docs/ARCHITECTURE.md` follow
  it.
* The pull request records the measurements and the runs.
* The docs match the code.

## Questions

Send questions to the maintainer who gave you this guide, along with the item ID and what you found in the code. When
the code and a doc disagree, the code is right. Report the difference so the doc can be fixed.
