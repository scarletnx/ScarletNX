<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Working on the CPU implementation plan

## Index

* [The job](#the-job)
* [What you need](#what-you-need)
* [Get the code and build it](#get-the-code-and-build-it)
* [What to read, in order](#what-to-read-in-order)
* [Where the translator lives](#where-the-translator-lives)
* [Rules that apply to this work](#rules-that-apply-to-this-work)
* [How to hand work back](#how-to-hand-work-back)
* [Definition of done](#definition-of-done)
* [First milestone: one measured SIMD improvement](#first-milestone-one-measured-simd-improvement)
  * [Performance hypothesis](#performance-hypothesis)
  * [1. Find the cost](#1-find-the-cost)
  * [2. Check LLVM before changing the emitter](#2-check-llvm-before-changing-the-emitter)
  * [3. Replace one group exactly](#3-replace-one-group-exactly)
  * [4. Prove results and measure the same scene](#4-prove-results-and-measure-the-same-scene)
  * [Acceptance and handoff](#acceptance-and-handoff)
* [Questions](#questions)

This guide is for whoever implements the [CPU implementation plan](plan.md). The plan
says what to build. This guide says how to work in this repository: what to set up and what to read, which project
rules apply, and how each item is proven and handed back.

The plan and this guide were checked against the code at commit `05324b57` on `main`. If the code has
moved since, check an item's "Now" against the current code before you start it.

## The job

* Start with [First CPU milestone: one measured SIMD improvement](#first-milestone-one-measured-simd-improvement). Profile the real cost, choose
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
* **The toolchain** from the "Prerequisites" of [Build](../../dev/build.md): Clang (CI uses 23), LLVM's development files
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
| What each SIMD, FP and crypto op becomes, per emitter path | `ctest --test-dir build -R llvm_lowering` |
| The module contract test | `ctest --test-dir build -R module_contract` |

`tools/run_game.py` wraps an unattended run with scripted input and screenshots. The procedure is in
`.claude/skills/snx-run/SKILL.md`.

## What to read, in order

1. [README](../../../README.md): what the project is, and the hosts it builds for.
2. [Architecture](../../dev/architecture.md), the sections "Recompiler + module ABI" and "Guest exec": the pipeline, the contract
   between generated code and the runtime, the `SNX_ABI` rules, and how patched code is handled.
3. [The first SIMD milestone](#first-milestone-one-measured-simd-improvement), then the relevant entries in [the plan](plan.md).
4. `AGENTS.md` at the root of the repository: the current project rules and source map. Follow applicable local
   instructions when work starts, and use `CLAUDE.md` as additional context where it still applies.
5. [Build](../../dev/build.md) and [Development](../../dev/development.md): build variants, tests and profiling.
6. [Usage](../../user/usage.md): extract, recompile and run, the folders, and the settings.
7. When an item needs them:
   * [GNU ABI](../../platforms/gnuabi/README.md) for T1, R1 and S1.
   * [AOT packs](../../user/aot-packs.md), [ldr:ro](../../platforms/nx/nro.md#loading-nros-at-run-time-ldrro) and [NRO files](../../platforms/nx/nro.md) for the J items and L4.
   * [Cortex-A57 divergences](../../history/divergences.md) for the FP items.
   * [The recompiler as a library](../../dev/library.md).
   * [macOS and iOS](../../hosts/apple.md), [Android](../../hosts/android-app.md), [PowerPC64](../../hosts/ppc64.md) and
     [RISC-V and LoongArch](../../hosts/riscv-loongarch.md) for the host notes.
   * [Licenses and credits](../../CREDITS.md) before you port anything.
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
| The decoder | `src/common/cpu/aarch64/decode/` (`a64.decode`, `a64.profile`), generated by `tools/decodetree` |
| SVCs | `src/nx/hle/svc/hos.svc`, `tools/svcgen` |
| Guest memory | `src/nx/memory/mem.c`, `src/nx/memory/own.c` |
| Guest threads | `src/nx/hle/kernel/thread.c` |
| The GNU ABI runtime | `src/gnuabi/` |
| Translator tests | `tests/llvm_qemu_differential.c`, `tests/llvm_qemu_expected.txt`, `tests/llvm_lowering.c`, `tests/llvm_lowering_expected.txt`, `tests/a64_ported.c`, `tests/llvm_module.c`, `tests/llvm_bench.c`, `tests/module_contract.c`, `tests/qemu_ref/` |

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
`cpu.h`, updates `tests/module_contract.c` and adds a row to the table in `docs/dev/architecture.md`. Every bump forces a
full recompile of every game, so group bumps where the plan allows it, such as I3 with I4.

**Emitters.** Check every emitter or decoder change against QEMU with `ctest -R llvm_qemu_differential`, and give the
test binary `--sweep` for more words and states. A group that newly differs fails the test, and so does a group in
`tests/llvm_qemu_expected.txt` that no longer differs. Update that file in the same change. A change that moves an op
between inline, guarded and helper on an emitter path fails `llvm_lowering` until
`build/tests/snx-test-llvm-lowering tests/llvm_lowering_expected.txt --update` rewrites its summary in the same change.
`--objects DIR --counts FILE` adds each op's target object counts, which `--path NAME --triple TRIPLE` narrows to one
path on another target: instructions, calls, vector instructions, shuffles and GPR to vector moves, static code with
the cold paths included, which is code inspection and never a speed claim. An emitter that chooses by a host feature
runs both ways on one machine: `llvm_qemu_differential_avx2` (`--without-avx512`) compiles for the host as one
without AVX-512, and the choices table in `tests/llvm_lowering.c` checks the instruction each x86 path picks where
the class does not show it.

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
* `SNX_ABI` rose if and only if the contract changed, and `tests/module_contract.c` and `docs/dev/architecture.md` follow
  it.
* The pull request records the measurements and the runs.
* The docs match the code.

## First milestone: one measured SIMD improvement

Profile lane-helper cost in a repeatable game scene, replace one hot instruction group with exact vector code, and
measure that scene again. Deliver one bounded change with evidence. The [CPU implementation plan](plan.md)
is the supporting backlog. This is the active scope.

Use one available host for the first measured result. Preserve other hosts through the existing exact path and check
lowering for affected targets. Record which hosts actually ran. The full tooling program is not a prerequisite.
Tiered compilation stays deferred, standalone PLT binding stays retired, and SDK replacement follows the hook system.

### Performance hypothesis

An illustrative CPU-limited scenario gives about **5 to 18 percent higher throughput** if affected work occupies
10 to 20 percent of baseline elapsed time and becomes 2 to 4 times faster. These are unmeasured assumptions. Use
`S = 1 / ((1 - p) + p / s)` and the main plan's [scenario table](plan.md#simd-performance-scenarios).
At 40 percent affected work, the same assumptions give 25 to 43 percent. This is not an expected result or a Rosetta
benchmark. A profile must establish the affected fraction, and repeated timings must establish the actual speedup.

Report the prediction and measurements separately. GPU limits, parallel execution and frame caps can hide a CPU gain.
Percentages here describe throughput, not time saved. Removing four calls does not prove a 4-times improvement.

### 1. Find the cost

Use a runnable game, a fixed input script and the same scene duration. Record the host CPU, LLVM version, revision and
recompile settings. Warm shader preparation and code compilation before timing. Keep dumps and profiles local.

Sample CPU execution first. Add diagnostic counters only where needed to distinguish instruction groups or guard
failures. Start at `map`, `binary`, `split_begin` and `split_end` in
`src/common/cpu/recompiler/llvm/ir_vec_fp.c`, and `ir_vec_call` in `ir_vec.c` beside it.

Rank groups by their contribution to CPU time, with call counts and fast/slow entries as supporting evidence. Shared
helper names alone do not identify a guest operation: correlate sites with the guest PC and instruction word locally.
Measure whether one exceptional lane sends the whole vector to scalar helpers. `FABD`, estimates and conversions are
candidates, not predetermined winners. If no material cost appears, profile another relevant scene. If no game can be
profiled, record that blocker rather than claiming a synthetic benchmark completes this milestone.

### 2. Check LLVM before changing the emitter

Save real emitter IR before and after its actual optimization pipeline and inspect host assembly with the same target
features. Count helper calls, lane moves, state synchronization and spills. Model controls explain the result, but do
not substitute for checking the real emitted function.

LLVM 23.1.4 model checks already found:

| Input | Result |
| --- | --- |
| Opaque per-lane FP helpers | Four float-lane calls or two double-lane calls remain, with SLP off and on. |
| Scalar `fadd` lanes | LLVM already combines them into vector additions, even with SLP off. |
| Four scalar `llvm.fma.f32` operations | They combine with SLP on, but remain scalar with it off. |
| Vector saturating float-to-int intrinsic | The checked x86 lowering still uses scalar conversions. Cortex-A57 gets a vector conversion. |

Keep code LLVM already lowers well. Use a target-specific intrinsic only for a demonstrated gap in portable vector IR.
Do not enable SLP globally while its compiler issue remains unresolved. See I15 for the detailed compiler evidence.

### 3. Replace one group exactly

Apply Rosetta's documented SIMD-to-SIMD principle: translate the guest vector operation into a short host-vector
sequence. Prefer whole-vector LLVM IR, with proven lane masks and selects for guest semantic differences. Apple FP
hardware modes cannot be assumed on another host.

Use the main plan's [Rosetta evidence and adaptations](plan.md#i15-whole-vector-operations-instead-of-serial-lane-helpers)
to guide the sequence: preserve SIMD width, keep vector intermediates in SSA, and measure code size and spills as
well as call removal. The observed Rosetta register convention is background evidence, not a requirement to redesign
ScarletNX's guest-call ABI before this milestone. Its exact exceptional-lane implementation is not established by
these sources. The proposed masks, FPSR reductions and batched fallback must pass ScarletNX's own exactness tests.

Define the selected group's result bits and FPSR effects from Arm semantics and the existing exact helper. Preserve
NaN payload selection, signed zero, rounding and flush behavior where relevant. Reduce exception masks across lanes
and combine them with sticky FPSR state once. Sanitize unsafe shift counts and conversion inputs before evaluation:
a select does not suppress operand evaluation or remove LLVM poison. Keep the default exact FPSR mode.

Keep an exact fallback for cases not proven. Prefer one audited helper entry per vector instruction where software
remains necessary. A scalar loop inside it is an intermediate gain, not completion of the vector path. Retain the old
exact path on unsupported hosts. Inspect assembly to establish that the intended path uses vector instructions.

If I7's helper-state classification is needed, measure it separately: original baseline, I7 alone, then vector code.
Do not attribute fewer state saves to vectorization. Audit helper effects before changing attributes. Review `SNX_ABI`
and cache invalidation if the generated-code or helper contract changes.

### 4. Prove results and measure the same scene

Add focused tests under `tests/` and a kernel in `tests/llvm_bench.c` where needed. Run the QEMU differential test,
including `--sweep` with its required runner arguments, plus directed cases. Compare full result bits and relevant
flags for ordinary lanes, each position of one exceptional lane, multiple exceptional lanes, relevant FPCR modes and
initial sticky FPSR states. Include short consuming instruction sequences where needed to expose dependencies.
Verify affected big-endian lane order, or retain its old path until tested. Run affected module tests and the relevant
suite. Do not hide a new mismatch in `tests/llvm_qemu_expected.txt`.

Recompile guest modules for each emitter version. Time the focused benchmark and the same game scene with the same
hook configuration, settings and cache preparation. Keep diagnostic counters out of timed builds unless their cost
is equivalent. Do not build during timing. Follow the [implementer guide](guide.md) and applicable run
procedures.

Use at least five measured runs per version, alternate their order, and report individual results with median and
spread. Include frame-time median and tail, plus CPU samples or time where available. Record GPU limits, frame caps
or pacing that could hide a CPU gain. A hidden or inconclusive game result does not establish a frame-rate gain.

### Acceptance and handoff

| Deliverable | Acceptance condition |
| --- | --- |
| Selection | A real profile ties the chosen group to meaningful CPU cost. |
| Lowering | The intended path uses vector instructions and removes the selected lane calls. Remaining fallback paths are documented. |
| Correctness | Directed and differential tests preserve result bits and flags, with no new expected mismatch. |
| Focused speed | Repeated timings show a gain beyond observed run-to-run variation. |
| Game comparison | The same scene runs before and after. Report improvement, no measurable change or regression. |
| Performance model | Report measured assumptions and observed results separately. Explain a discrepancy with the conditional model. |
| Patch report | State the host coverage, ABI/cache effects, commands and measurements. Keep game-derived artifacts local. |

Deliver one patch, or an independently measured prerequisite plus that patch. Recommend retaining, revising or dropping
it from the evidence. If the focused sequence improves but the scene does not, say so and judge whether the change
justifies its complexity. If exactness cannot be proven, retain the original implementation and record the failed
hypothesis. Choose later backlog work from these results.

The full block fuzzer does not gate this bounded milestone when focused tests cover its mechanism. The larger ABI,
register-passing and atomic-loop redesigns retain their separate test requirements.

Source: Dougall Johnson, [Why is Rosetta 2 fast?, SSE2 discussion](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/).

## Questions

Send questions to the maintainer who gave you this guide, along with the item ID and what you found in the code. When
the code and a doc disagree, the code is right. Report the difference so the doc can be fixed.
