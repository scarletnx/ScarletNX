<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# CPU implementation plan

**Start here:** [First CPU milestone: one measured SIMD improvement](CPU_SIMD_MILESTONE.md). Profile lane-helper cost,
replace one hot instruction group with exact vector code, and compare the same game scene before and after. That is
the active implementation scope. The sections below are the supporting backlog, and their phase labels do not require
finishing every tool or redesign before the first measured change.

Rosetta 2 translates x86-64 code into arm64 code for one host family, Apple silicon. ScarletNX translates A64 code into
the code of every host it supports. The [README](../README.md) lists Linux, Windows, FreeBSD, Solaris and Android, with
builds for x86-64, AArch64, PowerPC64 (little and big endian), RISC-V and LoongArch, and macOS and iOS built in CI.
This plan takes each part of Rosetta 2's design, finds the code in ScarletNX that does the same job, and describes
how to adapt the technique to each host and to the Horizon behavior that ScarletNX must provide.
Whoever implements it should start with the [guide](CPU_TRANSLATOR_GUIDE.md): setup, rules and how work is handed back.

Much of Rosetta's shape is already here. `scarletnx -r` compiles a game ahead of time into a native module through
LLVM, a runtime JIT compiles what the recompile missed, and both results are kept between runs. The parts that still
fall short are the cost of guest state at guest calls, calls that go through the dispatch table, the wait on the first
run of missed code, and the lack of measurements that would rank these. Rosetta also leans on Apple hardware made to
behave like x86. ScarletNX's hosts offer no such features, except the AArch64 host, which shares the guest's own
architecture. The plan relies instead on what the recompiler can know ahead of time, such as the AArch64 calling
standard and the executables' relocations, and on profiles of real runs.

## Contents

* [The hosts](#the-hosts)
* [OS cooperation and the Horizon boundary](#os-cooperation-and-the-horizon-boundary)
* [What "as good as Rosetta 2" means](#what-as-good-as-rosetta-2-means)
  * [SIMD performance scenarios](#simd-performance-scenarios)
* [Rosetta 2 part by part, applied to ScarletNX](#rosetta-2-part-by-part-applied-to-scarletnx)
  * [1. Translation ahead of time, and its cache](#1-translation-ahead-of-time-and-its-cache)
  * [2. Links between translated code: stubs and indirect branches](#2-links-between-translated-code-stubs-and-indirect-branches)
  * [3. The JIT for code the AOT pass did not see](#3-the-jit-for-code-the-aot-pass-did-not-see)
  * [4. Instruction translation](#4-instruction-translation)
  * [5. Memory order](#5-memory-order)
  * [6. System calls](#6-system-calls)
  * [7. Runtime support: CPU features, TLS, signals and exceptions](#7-runtime-support-cpu-features-tls-signals-and-exceptions)
  * [8. Delivery: the daemon, signing and debugging](#8-delivery-the-daemon-signing-and-debugging)
  * [9. Debug switches and measurement](#9-debug-switches-and-measurement)
* [Order of work](#order-of-work)
* [Prior art](#prior-art)
* [How every change is proven](#how-every-change-is-proven)
* [Risks and non-goals](#risks-and-non-goals)
* [Decisions for the maintainers](#decisions-for-the-maintainers)
* [Docs and comments that are out of date](#docs-and-comments-that-are-out-of-date)
* [Sources](#sources)

## The hosts

Rosetta 2 targets one host whose hardware Apple extended for it. ScarletNX's hosts differ in what they give an A64
guest for free, and each part below notes where that matters.

| Host | Systems | Objects | Pages | Runtime JIT | General registers | Flags | Memory order against Arm's |
| --- | --- | --- | --- | --- | --- | --- | --- |
| x86-64 | Linux, Windows, FreeBSD, Solaris, Android | ELF, COFF on Windows | 4 KiB | Yes, on Windows from 10 1803 | 16 | Yes, with the carry of a subtraction inverted against Arm's C | Stronger (TSO) |
| AArch64 | Linux, Android, iOS, macOS | ELF, or a Mach-O dylib with an ad hoc signature on Apple systems | 4 KiB on most Linux and Android, 16 KiB on Apple silicon | Yes. macOS through `MAP_JIT`, iOS only with a debugger attached | 31, the guest's own | NZCV, the guest's own | The same |
| PowerPC64, little and big endian | Linux | ELF, ELFv1 or ELFv2 | 64 KiB on most ppc64le kernels | Yes | 32, less r1, r2 and r13 | Condition register fields and the carry in XER | Weaker: not multi-copy atomic |
| RISC-V 64 | Linux | ELF | 4 KiB | No | 32, less zero, sp, gp and tp | No integer flags | Close (RVWMO) |
| LoongArch 64 | Linux | ELF | Set by the kernel | No | 32, less zero, tp, sp and r21 | No integer flags | Weak, still to be compared with Arm's |

The object formats, page sizes and JIT support come from `docs/ARCHITECTURE.md`, `docs/APPLE.md`, `docs/PPC64.md`,
`docs/RISCV-LOONGARCH.md` and `HOST_MACHINE` in `src/common/cpu/recompiler/llvm/code_loader.c`. The README says the
PowerPC64, RISC-V and LoongArch builds are cross-built, that big-endian PowerPC64 has not run on a big-endian machine,
and that no game runs on macOS yet. `docs/RISCV-LOONGARCH.md` says no real RISC-V or LoongArch machine has run them.

Two rows matter most for this plan. x86-64 is the only host with 16 general registers, so it is the one where guest
registers cannot all live in host registers. RISC-V and LoongArch have no runtime JIT, so a function the recompile
missed cannot run there at all, and the game ends with the reason (`rt_miss` in `src/nx/runtime/rtapi.c`).

## What "as good as Rosetta 2" means

Rosetta 2 is the reference for four properties. Translated code runs at a large fraction of native speed. Translation
costs the user once and nothing after. Code that appears only at run time still works, through a JIT that runs rarely.
And the guest sees exact results, with a precise guest state at a fault. ScarletNX has the second and third properties
in a strong form already on hosts with a JIT. The first and fourth need work, and none of the four is measured today.

The targets below are proposals. The measurements of part 9 give the current values, and the targets should be revised
once those numbers exist. Each metric is measured on every host that can run the test.

| Metric | How it is measured | Proposed target |
| --- | --- | --- |
| Speed ratio | The same C kernels built natively for the host and as AArch64 Linux programs, the latter run through `scarletnx-gnuabi` with a warm cache (T1). The ratio is native time over translated time, as a geometric mean. On an AArch64 host the native build is the guest program itself. | Reported in CI first. Then 0.7 or better for integer kernels on x86-64, and 0.9 or better on AArch64. |
| Call cost | Memory operations and host instructions for each guest `BL` and `RET` pair, from the recompile's statistics (T3) and the calls kernel of `tests/llvm_bench.c`. | Half of the memory operations measured at the start. |
| AOT coverage | The share of executed guest functions that came from the AOT module rather than the JIT, from the runtime counters (T2). | 99.9 percent after one recompile with hints, and no new hints after two sessions in the same scenes. On hosts with no JIT, no miss at all. |
| Miss latency | Time from `rt_miss` to the guest thread running again (T2). | Record the baseline and tail latency. No tiered-compilation target gates current work. J1 is deferred. |
| Recompile time | Wall time of `scarletnx -r` with an empty unit cache, and after new hints. `docs/ARCHITECTURE.md` gives 740 s cold and 24 to 28 s warm for a game of 255 units. | Half the cold time. Under 60 s for a recompile that only adds hints. |
| Exactness | `ctest -R llvm_qemu_differential`, the block-level fuzzer (T5), memory-order litmus tests (M1). | No new groups in `tests/llvm_qemu_expected.txt`. Every mode that changes results is a setting, and the exact behavior is the default. |
| Precise faults | A crash report names the guest pc of the faulting instruction. | Every fault in AOT and JIT code. |

### SIMD performance scenarios

For the first milestone, an illustrative scenario is about **5 to 18 percent higher throughput** if the selected
work accounts for 10 to 20 percent of baseline elapsed CPU-critical work and becomes 2 to 4 times faster. Neither
assumption has been measured in a game. This is a conditional calculation, not the expected gain from the full plan
or a performance number taken from Rosetta.

For a fixed CPU-limited workload, let `p` be the fraction of baseline elapsed time affected and `s` be the speedup of
that entire portion, including guards, fixups, spills and remaining fallback. With other work unchanged, Amdahl's law
gives `S = 1 / ((1 - p) + p / s)`. Throughput gain is `100 * (S - 1)` percent.

| Affected fraction of baseline elapsed time | Affected work 2 times faster | Affected work 4 times faster |
| --- | --- | --- |
| 10 percent | 5.3 percent higher throughput | 8.1 percent higher throughput |
| 20 percent | 11.1 percent higher throughput | 17.6 percent higher throughput |
| 40 percent | 25.0 percent higher throughput | 42.9 percent higher throughput |

These are speed increases, not equal percentages of time saved. At `p = 0.2` and `s = 2`, elapsed time falls by
10 percent, giving 11.1 percent more throughput. Helper call counts and summed samples across parallel threads do
not by themselves establish `p`. Identify work that delays completion of the measured workload. GPU overlap, waits
and frame caps can hide an execution improvement or invalidate this simple model for frame rate.

Estimate `p` from the scene profile and `s` from the complete before/after sequence, then report the model beside
observed scene timings. Record any gap rather than treating the calculation as a result. Do not add percentages from
several items: their affected work may overlap. A vector intrinsic or four removed lane calls does not establish a
2-times or 4-times speedup. No universal improvement or Rosetta-level speed is promised.

## OS cooperation and the Horizon boundary

Rosetta's macOS path has cooperation from the platform that it targets. Apple documents kernel involvement in loading
translated processes and a system service for AOT artifacts. This is an architectural advantage, not a service that
ScarletNX can assume from Linux or another host OS. See Apple's
[Rosetta 2 platform security documentation](https://support.apple.com/guide/security/rosetta-2-on-a-mac-with-apple-silicon-secebb113be1/web).

Horizon supplies its own kernel and service contracts. It provides no compatibility contract for substituting Linux,
Windows or another host. The practical requirement behind treating Horizon as uncooperative is to preserve its
observable behavior at each boundary. This plan makes no claim about the intent behind Horizon's design. Its SVCs
include handle operations, address arbitration and memory mapping, which cannot be replaced by matching host calls
on the basis of a name alone ([Switchbrew SVC reference](https://switchbrew.org/wiki/SVC)).

ScarletNX supplies this behavior in `src/nx/hle/` and `src/nx/memory/`. Its existing code shows why translation speed
alone is insufficient: `src/nx/hle/kernel/thread.c` manages guest cores and scheduling, `src/nx/hle/kernel/ipc.c`
implements IPC, and `src/nx/memory/own.c` coordinates guest accesses with GPU-owned pages. Generated code enters this
runtime through `RtAPI`, including `RtAPI.svcs` (`src/common/cpu/recompiler/llvm/ir_codegen.c`).

| Boundary | Contract required before an optimization |
| --- | --- |
| Guest calls and linked SDK code | Explicit register, stack and callback rules, plus a path for nonlocal returns and runtime re-entry. AAPCS64 alone does not describe an SVC or an arbitrary hand-written guest routine. |
| SVC and IPC | Horizon results, handle ownership, TLS buffers and blocking behavior must remain correct. Declared register arguments do not describe all memory or scheduling effects. |
| Threads and waits | Retain guest core, priority, wakeup and context rules through the host scheduler. |
| Memory and faults | Retain guest mappings and permissions, code invalidation, and GPU ownership. Host faults must be distinguished from guest faults and from deliberate ownership traps. |
| Shared SDK translation layer | Define entry points, guest layouts, TLS, callbacks, state visibility and version selection. Linking a layer does not remove the Horizon contracts below it. |

Every optimization that crosses one of these boundaries must name its preconditions, its state synchronization points
and a tested fallback. S1 cannot narrow state saves from `hos.svc` alone. R2 cannot run code directly merely because
the host is AArch64. The same rule applies to the planned SDK translation layer: it is a maintainer-stated direction,
not an implementation confirmed by this document. Keep the existing HLE path until replacement behavior is proven.

## Rosetta 2 part by part, applied to ScarletNX

The parts follow the outline that attesor's README gives Rosetta 2: translation ahead of time, the JIT, instruction set
translation, system call translation and runtime support, with its translation challenges (registers, memory order,
vector instructions and calling conventions). The facts about Rosetta itself come from Project Champollion and Dougall
Johnson's post, because attesor's own claims need a second source (see [Sources](#sources)).

Each work item has an ID from its part (A for translation ahead of time, L for links, J for the JIT, I for instruction
translation, M for memory, S for system calls, R for runtime support, D for delivery, T for tools). It gives the
change, where it goes, its effect on `SNX_ABI`, the proof it needs, a size (S, M, L or XL) and a priority (P0 first). A
change to the contract between generated code and the runtime raises `SNX_ABI` and recompiles every game
(`docs/ARCHITECTURE.md`, "Module ABI"). A change that only alters the recompiler's output for the same contract needs
no bump, and old modules keep running until they are recompiled. A new recompile setting must join the JIT cache key,
as `volatile_memory` does (`src/common/cpu/recompiler/llvm/jit_cache.c`).

### 1. Translation ahead of time, and its cache

**Rosetta 2.** Dougall Johnson describes Rosetta translating the entire text segment up front, before the program
runs. Champollion shows the mechanics (part 1, "Roles of oahd and oahd-helper", and part 2). At exec time the runtime
hands the file to the `oahd` daemon, which computes a SHA-256 over the file's contents and its path and looks for a
matching `.aot` file under `/var/db/oah`. On a miss, `oahd-helper` translates the whole binary into a Mach-O file with a
new `LC_AOT_METADATA` load command (number `0xcacaca01`). The x86-64 system libraries are translated once into a shared
`aot_shared_cache`, about 2.4 GB in Champollion's analysis, mapped into every process.

**ScarletNX now.** The launcher's **Load Switch NSP/XCI/NRO** extracts and recompiles a game before its first run, and
the driver recompiles a game whose `<id>.abi` stamp is stale when it is played. The unit cache
(`src/nx/loader/unit_cache.c`) keys each unit's object on its IR, which is close to Rosetta's content key. The module
records each executable's build ID and segment layout, and the runtime refuses an executable that changed
(`snx_module_mods` in `src/nx/loader/module_llvm.c`, `src/nx/runtime/loader.c`). Discovery
(`src/common/cpu/recompiler/codegen.c`, `src/nx/loader/loader.c`) starts from the dynamic symbols, the addends of
relative relocations, the `.eh_frame` ranges (swept word by word), hints from earlier runs and `ADR` or `ADRP`+`ADD`
address loads, then follows branches. Jump table cases are not followed. Units group sorted functions up to the
`unit_instructions` setting, 90000 instructions by default and 20000 on phones (`module_llvm.c`). A cold recompile of a
game of 255 units takes about 740 s.

Rosetta translates only what it can find too, but its JIT catches the rest on every host. ScarletNX has no JIT on
RISC-V, LoongArch or Intel Macs, so the completeness of discovery is what decides whether a game runs there.

#### A1 Recover jump tables

- **Now.** Address loads that feed a `BR` within six words count as a jump table base, but the case targets are not
  followed (`codegen.c`). Each case first reached at run time misses, and a `BR` inside a function goes to a switch over
  all the function's leaders (`ir_codegen.c`).
- **Change.** Match the patterns compilers emit (`ADRP`/`ADD` base, `LDRB`/`LDRH`/`LDRSW` of a scaled index, `ADD`,
  `BR`), bound the index with the earlier `CMP` and `B.HI`, add the targets as leaders, and emit an LLVM `switch` on the
  index. Unmatched forms keep today's path.
- **Where.** `codegen.c`, `ir_codegen.c`.
- **ABI.** None.
- **Proof.** A synthetic program with dense and sparse switches, and fewer hints in the T2 counters on game runs.
- **Size and priority.** M, P1, and the first item on hosts with no JIT.

#### A2 Sweep for functions nothing references

- **Change.** Before sweeping code gaps, discover method targets from validated custom RTTI and its associated method
  tables, as described below. Then decode gaps in `.text` between known functions and combine candidate entries with
  relocation, unwind and branch evidence. Fixed-width A64 decoding and
  a plausible prologue are clues, not proof: data can decode as instructions, and leaf functions can lack a prologue.
  Keep speculative candidates separate from proven function boundaries. Do not let them split an existing function
  or change its execution semantics without validation. Stop a candidate path at an invalid word.
- **Where.** `src/nx/loader/loader.c` for metadata and address resolution, and `codegen.c` for the discovery seeds and
  gap analysis.
- **ABI.** None.
- **Proof.** Coverage tests include custom RTTI with otherwise-unreferenced methods, embedded data, padding and leaf
  functions without prologues. Compare discovered targets with known fixtures, then use T2 to measure fewer misses.
  Validate newly reached code with differential tests.
- **Size and priority.** M, P1. This is expected AOT coverage work, especially on hosts with no JIT.

**Custom RTTI and method-table discovery.** Search loaded executable data for the runtime's custom type records and
follow their validated links to method tables. Establish each supported layout from its SDK/runtime access code or
documented format. Custom RTTI need not follow the standard C++ ABI, and a type record may contain no method addresses
itself. Do not assume every word in a record is a function pointer.

The current relative-relocation scan already seeds code pointers, including some vtable methods and callbacks
(`relptrs` in `loader.c`, consumed by `snx_codegen_analyze` in `codegen.c`). Extend that coverage with explicit metadata
discovery rather than duplicating it. Resolve entries using the format's actual absolute or relative addressing and
the owning module's load base. Validate record bounds, counts and links before following them, then require method
targets to be aligned and inside a loaded executable code range. Deduplicate them with existing symbol, relocation,
unwind and hint seeds, retaining their metadata origin for diagnostics.

Feed validated method addresses, including adjustment thunks, into the existing discovery worklist even when no direct
branch references them. Respect existing function ranges and internal-entry handling rather than splitting a function
from a table reference alone. Run this metadata pass before speculative code sweeping and apply it when NRO metadata
becomes available through its existing discovery path. Unknown formats retain normal discovery and dispatch.

Use synthetic fixtures for a method reachable only through RTTI, shared tables, relative entries where supported,
duplicate targets, thunks and malformed or data-only lookalikes. Report newly discovered methods separately from
relocation seeds and the scan cost. Compare cold AOT misses before and after. This is function discovery, not automatic
devirtualization or permission to bypass hooks. Extracted RTTI records stay local.

#### A3 Stable units

- **Now.** A new function shifts the boundaries of later units, so a recompile that only adds hints can miss in the
  unit cache for many units. Each unit is optimized independently, emitted as a machine-code object by
  `snx_ir_object` (`src/common/cpu/recompiler/llvm/ir.c`), and linked with the other objects in `module_llvm.c`.
  `unit_cache.c` hashes bitcode but retains native objects. The current final link does not perform LLVM LTO.
- **Change.** Partition units by fixed address ranges within each executable, so a new function changes only its own
  source unit. Measure the effect of the new boundaries on inlining and constant propagation, since moving callers
  and callees apart can lose optimizations that the old unit happened to allow. D1 builds on this.
- **Optimization path.** Separate stable discovery units from optimization units. First compare stable native-object
  units with today's output. If the loss matters, retain bitcode and evaluate ThinLTO or a selective IR merge before
  native-object emission. Full LTO is another measured option, with higher memory and rebuild costs. Passing an LTO
  flag to the existing object linker cannot recover discarded IR. ThinLTO needs a summary/import stage and backend
  cache keys that include imported definitions and optimization settings. A changed callee can legitimately rebuild
  other optimization units. See [LLVM's ThinLTO design](https://llvm.org/docs/ThinLTO.html).
- **Where.** `module_llvm.c`, `unit_cache.c`, `ir.c`, and a new IR link/import stage if justified.
- **ABI.** None if the generated-code contract stays the same. Version the cache format and invalidate affected keys.
- **Proof.** Measure rebuild scope, time, peak memory and execution speed. Include callers and callees across a unit
  boundary, and change a callee to confirm that imported copies are invalidated. Claim local rebuilds only for
  independent units without imported dependencies.
- **Size and priority.** M, P2 for stable units. LTO integration is a separate L-sized experiment within A3.

#### A4 Temporary compile-time tuning before the linked SDK layer

- **Direction.** The maintainer plans to link an SDK translation layer instead of recompiling the SDK for every game.
  Treat a translated-SDK object cache as temporary. Do not build a permanent cross-game SDK recompilation system for
  a path scheduled for replacement.
- **Change.** Use pass timing to identify cheap improvements in the current compiler. Keep only work that also helps
  application code or demonstrably pays for itself before the SDK layer arrives. When that layer is available, bind
  supported SDK entry points through its versioned interface, with the Horizon boundary contracts above. Retain a
  translated path for unsupported SDK versions, patched implementations and unknown entry points.
- **Where.** `src/common/cpu/recompiler/llvm/ir.c`, `module_llvm.c`, and the future SDK layer's interface. Its location
  and implementation are not specified by this plan.
- **ABI.** Timing and pass tuning need none. SDK integration needs an explicit ABI review, including callbacks and
  guest state, before its compatibility and invalidation rules can be decided.
- **Proof.** Report the short-term cost and benefit of tuning separately from SDK integration. Test SDK calls and
  callbacks against the existing path, including unsupported-version fallback, IPC and thread behavior.
- **Size and priority.** S, P3 for temporary tuning. Scope SDK integration with the layer's owner when its interface is
  available. The standalone shared translated-SDK cache proposal is retired.

#### A5 Optional profile-guided translation

Rosetta translates each instruction once with almost no optimization. ScarletNX runs LLVM, which can use a profile.

- **Now.** `ir_profile.c` reads branch counts (pc, word, taken, not taken) from the file in `SNX_LLVM_BRANCH_PROFILE`,
  and only `tests/llvm_perf/test_profile.py` writes one.
- **Change.** Add separate opt-in settings to collect a profile and to use it for a recompile. Keep both off by
  default, since gains are workload dependent and collection has a cost. Instrument guest branches and `BLR` targets
  and write the profile beside the game at exit. An explicitly enabled profile build can set branch weights, order
  functions by heat and split cold code. L2 may use the `BLR` targets when that option is enabled. Reject stale
  profiles using executable identity, instruction bytes and the profile schema. Missing profiles use the normal build.
- **Where.** `ir_profile.c`, `ir_codegen.c`, `module_llvm.c`, `src/nx/runtime/settings.txt`.
- **ABI.** None.
- **Proof.** Compare the normal build, the instrumented build and the optimized profile build. Measure execution time,
  collection overhead, recompile cost and code size across different scenes, including scenes absent from training.
  Report regressions as well as gains on each tested host.
- **Size and priority.** M, P2, optional. No universal speedup is assumed.

### 2. Links between translated code: stubs and indirect branches

**Rosetta 2.** A call to another image goes through `__stubs_sh`, which holds pairs of an x86 target and its translated
target (Champollion part 1, "X86_64 address resolution and lazy binding"). The stub compares the live lazy pointer with
the stored x86 target and branches straight to the translated code when they match. A mismatch goes to
`resolve_x64_addr`, which can call the JIT and then writes the new pair back. Other indirect targets resolve through a
lookup of function starts and otherwise unreferenced blocks, with the JIT behind it, for example for switch statements
(Dougall Johnson).

**ScarletNX now.** The lookup is a dense table with one slot for each guest instruction word (`CPU.disp`,
`src/common/cpu/aarch64/cpu.h`). It is constant-time, at a memory cost of twice the code size on a 64-bit host.
`snx_lookup` loads `code_lo`, `code_hi` and `disp` from the `CPU` structure, then the slot, in about twelve
instructions with two conditional branches (`build_lookup` in `src/common/cpu/recompiler/llvm/ir_helpers.c`). A game's
executables call each other through PLT stubs that load a GOT entry and `BR` to it. The loader does not read
`DT_JMPREL` (`loader.c`), so the recompiler does not know those targets, and every call to an import runs the stub's
`BR` through the lookup. Code a game maps at run time (NROs through `ldr:ro`) lies outside `CPU.disp`, so each call into
it from outside its unit goes through `rt->miss`, then `variant_resolve`, then the JIT's slot
(`src/nx/runtime/guest_jit.c`, `docs/LDR_RO.md`).

**On each host.** All of this is IR, so it applies to every host alike. Direct calls in a large module pass through the
veneers that AArch64 Mach-O modules get past 128 MiB and the stubs that PowerPC64 modules get past 32 MiB
(`docs/ARCHITECTURE.md`). Those still cost less than a lookup.

#### L1 Standalone PLT binding retired in favor of hook expansion

- **Status.** Removed from the active implementation plan. The maintainer plans to expand the hooking system by
  Saturday, 2026-10-31, making a separate PLT optimization a poor investment for the SDK calls that hooks will cover.
  This is planned work, not a claim that the expanded system already exists or covers every import.
- **Direction.** Put SDK entry replacement and call routing in the expanded hook system. Reuse that system's target
  identification, version checks and invalidation rather than adding a parallel PLT binder. Rosetta's useful idea is
  guarded target resolution with a correct fallback. Apply it inside the hook architecture where appropriate.
- **Current integration point.** `call` in `ir_codegen.c` already respects `hook_direct_calls` before emitting direct
  calls, and `module_llvm.c` propagates the option. Any future direct-call shortcut must preserve hook precedence for
  both AOT and JIT code. Do not bypass an active hook because an original guest target is known.
- **Fallback and proof.** Preserve normal dispatch for unhooked imports and unsupported SDK versions. Hook integration
  must test direct and indirect entries, callbacks, and target replacement or removal under its supported lifecycle.
  Check guest state and Horizon effects as well as whether the hook fires. Reconsider residual PLT work only if
  post-expansion measurements show an uncovered hot path.
- **ABI and scope.** No standalone implementation or estimate. Hook interface changes receive their own ABI review.

#### L2 Guarded direct calls for hot indirect targets

- **Change.** When profile use is explicitly enabled (A5), the next recompile compares against the hottest `BLR` target
  and emits a
  direct call to it, with the lookup as the fallback. This covers C++ virtual calls, and it is static, with no writable
  code. A runtime cache like `__stubs_sh` is a later option. Its entry must update atomically, for example by packing a
  32-bit target offset and a 32-bit function index into one 64-bit word.
  Resolve hook ownership first, and invalidate or bypass this shortcut when a hook changes the destination. Coordinate
  the representation with the expanded hook system rather than adding a second target cache.
- **Where.** `ir_codegen.c`, `ir_profile.c`.
- **ABI.** None.
- **Proof.** Differential tests, plus the T2 counters before and after.
- **Size and priority.** M, P2, optional with A5. Without a usable profile, keep the normal lookup path.

#### L3 A module that owns its dispatch table

- **Change.** Let the AOT module own the dispatch table of its own code, as a hidden zero-filled array, and point
  `CPU.disp` at it so the runtime keeps writing slots as it does now. The table's address and the code range become
  link-time constants, so a lookup from AOT code is one subtraction, one bound and alignment check, and one load. JIT
  units and NRO images keep the lookup through `CPU`.
- **Where.** `cpu.h`, `ir_helpers.c`, `src/nx/loader/module_abi.h`, `guest_jit.c`.
- **ABI.** Bump.
- **Proof.** Differential tests and the indirect part of `llvm_bench`.
- **Size and priority.** S, P2.

#### L4 Dispatch for NRO code

- **Change.** Give each added image its own dispatch region that `snx_lookup` checks, or one sparse table over the
  whole address space.
- **Where.** `cpu.h`, `guest_jit.c`, `src/nx/hle/services/ldr.c`.
- **ABI.** Bump.
- **Proof.** The NRO tests and a game run that loads NROs.
- **Size and priority.** M, P2.

### 3. The JIT for code the AOT pass did not see

**Rosetta 2.** The JIT translates code the AOT pass never saw, such as code a program writes at run time and switch
targets the lookup does not cover. Dougall Johnson notes that it runs relatively rarely. Champollion part 1 follows the
`translate` function in the runtime.

**ScarletNX now.** A pc with no code goes to `rt_miss` (`rtapi.c`). The thread leaves its core and waits until its
whole batch is compiled: up to 256 pcs plus a 20000-instruction discovery budget, at `jit_opt` (O2 by default, O0 on
phones), on up to four threads at nice 10 (`jit_service.c`, `jit_service.h`, `settings.txt`). There is no interpreter.
ScarletNX keeps what the JIT compiled in `<game>.jitcache` and records each missed pc as a hint for the next recompile,
which Rosetta's documents do not describe. Code a mod patches reaches the runtime through `IC IVAU`, and every AOT
function then checks a stale byte on entry and after each call returns (`stale_check` and `check_return` in
`ir_codegen.c`, and "Code the game patches" in `docs/ARCHITECTURE.md`).

**On each host.** x86-64 and AArch64 hosts have the JIT, and so does PowerPC64. iOS has it only with a debugger
attached (`docs/APPLE.md`), and RISC-V, LoongArch and Intel Macs have none (`HOST_MACHINE` in `code_loader.c`). Phones
compile at O0 because other levels break there (`guest_jit.c`).

#### J1 A fast first tier (deferred)

- **Change.** Compile the missed function alone at O0 first, publish it in the dispatch table, and resume the thread.
  Queue the O2 batch behind it and swap the slot when it is ready. Retain O0 code while any thread or direct reference
  can still reach it, then reclaim it only with a proven lifetime scheme. Phones already compile everything at O0, so
  for them this item only shrinks the batch a thread waits on. Publication must respect hook-owned slots.
- **Where.** `jit_service.c`, `guest_jit.c`.
- **ABI.** None.
- **Proof.** T2 shows the wait per miss. The JIT forms of the differential tests still pass. Game runs that reach new
  code show shorter stalls.
- **Size and priority.** M, P3, deferred. Tiered compilation is not a current priority and is not a prerequisite for
  SIMD, register or hook work. Revisit only if T2 still shows significant compilation stalls after coverage and
  execution improvements. The existing single-tier JIT remains the baseline.

#### J2 Priority for batches a thread waits on

- **Change.** Run a batch that a guest thread waits on at normal priority, and keep nice 10 (below normal on Windows)
  for warm batches.
- **Where.** `jit_service.c`.
- **ABI.** None.
- **Proof.** T2 wait times under load.
- **Size and priority.** S, P1.

#### J3 A runtime JIT on RISC-V and LoongArch

- **Now.** `HOST_MACHINE` is 0 on these hosts, so the loader refuses what the JIT would compile, and a miss ends the
  game (`code_loader.c`, `rtapi.c`).
- **Change.** Teach the code loader these hosts' relocations and calling conventions, starting from the object
  support the AOT linker already has for them (`elf_object.c`, `link_elf.c`), and set `HOST_MACHINE`.
- **Where.** `code_loader.c`, `elf_object.c`.
- **ABI.** None.
- **Proof.** The JIT form of the differential test under QEMU system emulation, then on real hardware when it exists.
- **Size and priority.** M, P2.

#### J4 Stale checks only where code can change

- **Change.** First measure their cost with T2 and T3, then audit code mutation and target replacement in the expanded
  hook system. Absence of ExeFS mods alone cannot establish immutability. Omit return checks only under an explicit
  runtime contract covering guest patches, hook changes and direct references to old code. Keep full checks where
  that contract is unavailable. A possible middle step is a shared invalidation epoch, if it preserves the hook and
  guest code lifecycle. Unexpected mutation must take the supported invalidation path or fail clearly, never run stale
  code. Do not use this optimization to restrict behavior the hook system needs.
- **Where.** `ir_codegen.c`, `guest_jit.c`, `src/nx/runtime/driver/recompile.c`.
- **ABI.** Bump, since the runtime must know whether a module has the checks.
- **Proof.** A run with a hooking mod still takes its hooks. A run without mods shows no check in the counters.
- **Size and priority.** S, P2.

### 4. Instruction translation

This is attesor's "instruction set translation" with three of its four translation challenges: registers, vector
instructions and calling conventions. Memory order has part 5.

#### One instruction at a time, or whole functions

**Rosetta 2.** Each x86 instruction becomes one or more ARM instructions, and the x86 state stays canonical at every
instruction boundary. That makes exceptions, sampling profilers and debuggers precise. Dougall Johnson notes that it
almost entirely prevents optimization between instructions, apart from removing dead flags and combining prologues and
epilogues. Rosetta also keeps maps from translated addresses back to x86 addresses (Dougall Johnson, and the per-image
metadata in Champollion part 2).

**ScarletNX now.** The opposite trade. LLVM optimizes whole functions at O2 by default (`ir.c`), and the `CPU`
structure holds the architectural state only as of the last call site (`src/common/cpu/recompiler/llvm/ir_promote.c`).
The code is faster for it, but a fault does not know its guest pc, and crash reports cannot name the faulting
instruction. The plan keeps LLVM's optimization on every host and adds the map.

##### I1 A guest pc for every host instruction

- **Change.** Emit a debug location for each guest instruction, with the guest pc as the line, in AOT and JIT objects.
  `crash.c`, `gdb_jit.c` and perf can then map a host pc to a guest pc. Rebuilding the registers at a fault would need
  LLVM stack maps at memory operations, which can come later if a use appears (R1 is one).
- **Where.** `ir_codegen.c`, `gdb_jit.c`, `src/nx/runtime/crash.c`.
- **ABI.** None while tools read the table. Bump if the module descriptor gains a pointer for `crash.c`.
- **Proof.** A synthetic fault reports its exact guest pc on each host.
- **Size and priority.** M, P1.

#### Registers

**Rosetta 2.** Champollion part 1 ("Analyzing the code in AOT files: calling conventions") shows translated code
keeping the 16 x86-64 general registers in `x0` to `x15`, starting with `RAX` in `x0`, and AOT code calling with those
registers in place. Guest state never goes to memory between instructions or across calls. Dougall Johnson points out
that this works because x86-64 has half as many registers as AArch64, and that the same approach would be a real
drawback for an A64 guest on x86-64.

**ScarletNX now.** Guest registers are fields of `CPU`. After emission, `ir_promote.c` turns them into SSA values for
the whole function. At each call site it stores the registers written since the last site and reloads the live ones
after the call. A call to a helper that touches only FPCR, FPSR and the exclusive monitor (`snx-noregs`) syncs only
those, and every other call syncs all (`call_site` in `ir_promote.c`).

**On each host.** A64 has 31 general registers, SP and 32 vector registers. x86-64 has 16 and 16 (32 with AVX-512), so
a full map is impossible there, and fewer stores and reloads at calls are the gain. AArch64 has the guest's own
registers. PowerPC64, RISC-V and LoongArch each have 32 general registers less three or four reserved ones, so a map
close to Rosetta's is possible on every host but x86-64.

##### I2 An ABI contract mode, with a checker

- **Change.** Compiled code follows the AArch64 procedure call standard (AAPCS64). A callee that returns normally keeps
  `X19` to `X29` and SP, and it cannot read `X9` to `X17` or NZCV as inputs. Add an `abi_contract` recompile setting
  that lets promotion use these rules at guest `BL` and `BLR` sites. Callee-saved registers then stay in host registers
  or host stack slots across the call with no store or reload. Caller-saved temporaries that are dead after the call
  are not stored. SVC sites, helper calls and the paths into the runtime keep today's rules. A checking mode records
  `X19` to `X29` and SP at each function entry and compares them at each normal return, and it logs the guest pc of any
  function that breaks the contract. Exceptions and `longjmp` return to another pc, and the mismatch path already
  reloads from the `CPU` structure.
- **Where.** `ir_promote.c`, `ir_codegen.c`, `settings.txt`, the JIT cache key in `jit_cache.c`.
- **ABI.** None. It is a recompile setting that changes the output for the same contract.
- **Proof.** The checking mode runs clean over the test programs and through the scenes of the games used for testing.
  T3 shows fewer stores and reloads per call, and `llvm_bench` shows the time.
- **Size and priority.** M, P1. The setting stays off by default until the checker has run clean on those games.

##### I3 Pass guest registers in host registers

- **Change.** Give guest functions LLVM's `preserve_none` convention where the target has it, with guest registers as
  arguments and results. On x86-64 that covers the hot ones: `X0` to `X7`, SP, `X30` and the next pc. On AArch64,
  PowerPC64, RISC-V and LoongArch it can cover nearly all of them, which comes close to Rosetta's fixed map. The `CPU`
  structure stays the home of architectural state for the runtime and the cold paths. Tail calls keep working because
  caller and callee share the convention. The thread loop and the miss path enter guest code through a small
  trampoline with Clang's `preserve_none` attribute. A host whose LLVM lacks the convention keeps the C convention. On
  macOS and Android, `X18` stays free, because the platform reserves it.
- **Where.** `cpu.h`, `ir_codegen.c`, `ir_promote.c`, `src/nx/hle/kernel/thread.c`, `rtapi.c`, `jit_service.c`.
- **ABI.** Bump.
- **Proof.** As I4, plus the module contract test (`ctest -R module_contract`).
- **Size and priority.** L, P1, after I2 and I4 show what is left to win.

#### Calls and returns

**Rosetta 2.** `CALL` and `RET` become `BL` and `RET`, so the host's return predictor works. Dougall Johnson describes a
separate stack of pairs, the expected x86 return address and its translated target, that each return checks before it
trusts the host return. Champollion's memory map shows a region named "Rosetta Return Stack" (part 1).

**ScarletNX now.** The equivalent, on every host. A guest `BL` is a host call and `RET` is a host return. The caller
then checks that `c->pc` equals its own `pc+4` and leaves for the dispatch loop when it does not (`check_return` in
`ir_codegen.c`). Every backward branch inside a function also reads `c->poll`, so a loop without SVCs yields to a
waiting thread of higher priority.

##### I4 Return the next guest pc in a register

- **Now.** A guest function has the type `void(CPU*, u64 entry)` (`cpu.h`). `RET` stores `c->pc` and returns, and the
  caller loads `c->pc` to compare it.
- **Change.** Make guest functions return the next guest pc as a value. The caller compares the returned register with
  `pc+4`, and only the mismatch path stores `c->pc`. The thread loop in `thread.c` takes the returned pc in place of
  reading `c->pc`.
- **Where.** `cpu.h`, `ir_codegen.c`, `thread.c`, `jit_service.c`, the AOT pack and variant paths.
- **ABI.** Bump. The function type changes, so every module is recompiled from new IR. Group it with I3.
- **Proof.** `tests/llvm_module.c`, the differential test in AOT and JIT forms, the calls kernel of `llvm_bench`, and
  game runs that exercise exceptions and `longjmp`, which return to a different pc.
- **Size and priority.** M, P1.

##### I5 Cheaper polls, if they cost

- **Change.** Only if T2 shows a cost: poll every few iterations through a counter, or move polls to loop exits for
  loops with a known trip count. The poll's cold call also keeps LLVM from vectorizing such loops.
- **Where.** `ir_codegen.c`.
- **ABI.** None.
- **Proof.** Scheduling tests and game runs, since such a loop must still yield.
- **Size and priority.** S, P3.

#### Flags and condition codes

**Rosetta 2.** Rosetta does not compute flags that every path overwrites before a read (Dougall Johnson). It uses
`CFINV`, `RMIF` and `SETF8`/`SETF16` from FEAT_FlagM, `AXFLAG`/`XAFLAG` from FEAT_FlagM2 for FP compares, and an
undocumented Apple extension that computes x86 PF and AF into bits 26 and 27 of NZCV. Without that extension, in Linux
VMs, PF costs five instructions per operation.

**ScarletNX now.** Each flag-setting instruction computes N, Z, C and V as four 32-bit values, and records which
compare produced them (`flags_origin` in `src/common/cpu/recompiler/llvm/ir_state.c`). A `B.cond` in the same block then
folds into one host compare and branch. After promotion LLVM drops the flags nothing reads. A flag origin does not reach
past its block.

**On each host.** AArch64 has NZCV itself. x86-64 has flags, with the carry of a subtraction inverted against Arm's C.
PowerPC64 has condition register fields and a carry bit in XER. RISC-V and LoongArch have no integer flags, so each NZCV
value costs instructions there, and folding compares into branches matters most on those two hosts.

##### I6 Flags across blocks

- **Change.** Keep the origin, the operation and its operands, as promotable state across blocks, so a `B.cond` in a
  later block can still fold into one host compare. Store the flags at sites as one packed word in the promoted form.
- **Where.** `ir_state.c`, `ir_promote.c`.
- **ABI.** None while the `CPU` structure keeps four fields. Packing them in `CPU` too would be a bump.
- **Proof.** The block fuzzer (T5) and the differential test.
- **Size and priority.** M, P2. P1 on RISC-V and LoongArch.

#### Vector instructions

**Rosetta 2.** SSE maps to reasonably optimized sequences of NEON operations (Dougall Johnson).

**ScarletNX now.** The AdvSIMD and crypto encodings are emitted as LLVM vector IR, and the module uses the host CPU's
own extensions (`snx_ir_host` in `ir.c`). On x86-64 the emitters use AES-NI, PCLMULQDQ, SSE4.2 CRC32C, VPTERNLOG,
PAVGB and GF2P8AFFINEQB when the host has them (`docs/ARCHITECTURE.md`). Some groups still call helpers. The
per-lane helpers of vector FP go through `ir_vec_call` without the `snx-noregs` marking. A call is a full promotion site
that stores dirty registers and reloads live registers unless an earlier scalar FP declaration marked the helper
(`ir_vec.c`, `ir_emit.c`, `ir_promote.c`).
The scalar forms of `FRECPE`, `FRSQRTE` and `FRECPX`, and the vector forms of the first two, call one helper per lane
(`unary_form` in `ir_vec_fp.c`). `SRSHL`, `URSHL`, `SQSHL`, `UQSHL`, `SQRSHL` and `UQRSHL` by register call
`snx_vec_shl`, and the helpers of `rtlib.c` are a separate object that LLVM cannot inline (`ir.c`).

**On each host.** LLVM lowers the same vector IR to SSE through AVX-512, NEON, VSX, the RISC-V V extension or LSX and
LASX. A RISC-V host without the V extension gets scalar code. Big-endian PowerPC64 needs care with lane order wherever
the emitters move a vector through memory.

##### I7 Vector FP helpers as FP-only sites

- **Change.** Mark the lane helpers with `ir_noregs` in `ir_vec_call`, after checking that each one touches only its
  arguments, FPCR and FPSR, so the result no longer depends on the order of declarations.
- **Where.** `ir_vec.c`, `ir_vec_fp.c`.
- **ABI.** None.
- **Proof.** The differential test, and the store and reload counts of T3.
- **Size and priority.** S, P0.

I7 reduces state synchronization but leaves one call per lane. It is a prerequisite, not the end of the vector work.

##### I15 Whole-vector operations instead of serial lane helpers

- **Now.** `map` in `ir_vec_fp.c` extracts lanes, calls `lane` through `ir_vec_call`, and inserts each result. Several
  operations first try whole-vector arithmetic, but a guard failure sends the vector to this scalar path. One
  exceptional lane can therefore cause calls for every lane. The helper declarations are opaque to LLVM and can
  update CPU state.
- **LLVM evidence.** Model IR experiments with LLVM 23.1.4 used
  `opt -S -passes='default<O2>,function(sink,simplifycfg)'` with `-vectorize-slp=false` and `true`. The helper model
  retained four calls for four float lanes and two calls for two double lanes in both modes. Controls with scalar
  `fadd` operations became vector additions in both modes. Four scalar `llvm.fma` calls combined into a vector FMA
  only with SLP enabled. These are compiler experiments, not game benchmarks or a proof of guest FP semantics.
- **Rosetta principle.** Apply the documented mapping of SSE operations to short NEON sequences in the other
  direction: represent a guest AdvSIMD operation as whole-vector IR, then correct semantic differences with lane
  masks and selects where exact behavior is proven. This does not require host OS cooperation. Do not assume Apple
  FP modes or hardware features exist on another host.
- **Change.** Inventory the always-scalar and guard-failure paths, starting with Cortex-A57 instructions. Prefer LLVM
  vector operations and intrinsics whose output is already good. Start with candidates such as guarded `FABD`
  (subtraction followed by absolute value), rounding and conversions, then the estimates of I8 and shifts of I9.
  Classify NaNs, zeros and denormals as vector masks. Preserve guest NaN selection and payloads, FZ behavior, signed
  zero, rounding and every observable FPSR flag. Reduce per-lane exception masks once and combine with existing
  FPSR flags. Keep an exact fallback wherever the vector proof is incomplete.
- **Fallback.** Where a rare case still needs software, use one audited helper entry per guest vector instruction,
  passing all operands and returning the vector result and exception flags. Internally scalar work may remain until
  a vector replacement is proven. Mark state effects accurately so the call need not synchronize unrelated registers.
  Verify how each host ABI passes vectors, including whether it uses memory. One call does not guarantee no spills.
- **LLVM check before implementation.** For each candidate, save representative IR before and after the actual
  compiler pipeline and inspect `llc` output for every target and relevant feature level. Count scalar helper calls,
  lane extraction, spills and vector instructions. A vector intrinsic may lower to scalar code or library calls.
  Keep existing code where LLVM already produces the proposed sequence. Add a target-specific lowering only for a
  measured gap, with a portable exact path for other hosts. SLP remains a separately tested option while its known
  compiler issue is unresolved (`slp_vectorizer` in `settings.txt`).
- **Where.** `ir_vec_fp.c`, `ir_vec.c`, `ir_fpu.c`, `ir_vec_crypto.c` where applicable, and `tests/` for compiler
  fixtures, exactness tests and benchmarks. New helper contracts also touch `cpu.h` and `helpers/`.
- **ABI.** Internal IR changes need no bump. New generated-code/runtime helper contracts require review and a bump
  when incompatible. Include affected helper definitions in cache invalidation.
- **Proof.** Check results and FPSR against QEMU and the existing exact helpers, with mixed ordinary and exceptional
  lanes, all rounding modes and relevant FPCR settings. Test lane order on big-endian hosts. Measure ordinary inputs,
  one exceptional lane and all exceptional lanes separately. Assembly proves vector lowering, differential tests prove
  semantics, and repeated timing proves a gain. Generic fast-math flags cannot replace these proofs. Preserve compiler
  reproductions in `tests/` when implementation starts, including both the opaque helper and pure scalar controls.
- **Size and priority.** L, P1 after I7 and the measurement tools. Land one proven instruction group at a time.

The [first SIMD milestone](CPU_SIMD_MILESTONE.md) turns these ideas into a bounded experiment. Use the
[conditional performance model](#simd-performance-scenarios), then compare its assumptions with the actual scene.

**How Rosetta informs the design.** The sources below describe observed Rosetta behavior. The final column is the
proposed adaptation for ScarletNX, which must be proven independently.

| Documented Rosetta behavior | Evidence | Proposed application to ScarletNX |
| --- | --- | --- |
| Common SSE2 operations map to short NEON sequences. | [Dougall Johnson, SSE2 discussion](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/) | Emit a whole-vector sequence for the selected AdvSIMD group rather than calling a scalar helper for every lane. |
| The analyzed AOT convention maps XMM0 through XMM15 to q0 through q15. | [Champollion, calling conventions](https://ffri.github.io/ProjectChampollion/part1/#analyzing-the-code-in-aot-files-calling-conventions) | Keep operands as vector SSA values within the generated function and inspect spills. Do not require the separate custom-ABI project for the first milestone. |
| Code expansion and instruction-cache cost influence translation choices. | [Dougall Johnson, one-to-one translation discussion](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/) | Compare instruction count and generated bytes as well as helper calls. Reject oversized inline fixups without a measured benefit. |
| Apple hardware supports alternative FP behavior used by Rosetta. | [Dougall Johnson, floating-point discussion](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/) | Use matching hardware only where guest bits and flags are proven. Otherwise use vector masks and exact fallback. Never assume a portable Arm-compatibility FP mode. |

These observations do not establish Rosetta's exact exceptional-lane algorithm. Batched helpers, lane masks and FPSR
reductions in this plan are ScarletNX proposals. Borrow the compact SIMD mapping principle without assuming access to
Apple's hardware or host OS integration.

The following comparison was rerun with LLVM 23.1.4. These small model functions isolate compiler behavior. They
establish which transformations LLVM performs, without measuring a complete emitted guest function or a game.

| Input IR shape | O2, SLP off | O2, SLP on | Consequence |
| --- | --- | --- | --- |
| Four float lanes calling the opaque FP helper | Four calls remain | Four calls remain | Replace the helper boundary to expose vector work. |
| Two double lanes calling the opaque FP helper | Two calls remain | Two calls remain | The same obstacle exists for double precision. |
| Extract, scalar `fadd`, insert, for four floats or two doubles | One vector `fadd` | One vector `fadd` | LLVM already handles this case. A manual rewrite needs a separate benefit. |
| Four scalar `llvm.fma.f32` operations | Four scalar intrinsics | One `llvm.fma.v4f32` | Emit the vector intrinsic directly where exact FMA is justified and the host supports it. |

The unknown helpers can update memory reachable through `CPU*`. Successive calls can therefore depend on earlier
state changes, including sticky FPSR updates. The extract/insert syntax alone is not the obstacle. I7's private
`snx-noregs` attribute helps ScarletNX's register promotion. It does not supply LLVM with a vector implementation of
the helper or make its memory effects disappear. LTO may expose helper bodies, but does not itself prove that the
resulting stateful operations can be combined. The optimized IR and assembly still have to demonstrate that.

Representative `llc -O2` checks of four-float vector intrinsics show why each target needs inspection:

| Operation | Baseline x86-64 | Zen 4 | Cortex-A57 | Required decision |
| --- | --- | --- | --- | --- |
| `llvm.sqrt.v4f32` | `sqrtps` | `vsqrtps` | `fsqrt v0.4s` | Use existing vector lowering wherever guest result and flags are proven. |
| `llvm.fma.v4f32` | Four `fmaf` library calls and spills | `vfmadd213ps` | Vector `fmla` plus a register move | Retain the host-FMA capability guard. Avoid introducing library calls on baseline x86. |
| `llvm.fptosi.sat.v4i32.v4f32` | Four scalar conversions plus fixups | Four scalar conversions plus fixups | `fcvtzs v0.4s` | Investigate an x86 vector conversion with exact masks. A vector intrinsic alone is insufficient here. |

These instruction forms describe lowering only. In particular, an intrinsic's numeric result does not provide Arm's
NaN payload rules or FPSR behavior. A conversion sequence with the right integer result still needs correct IOC and
IXC handling. Likewise, a vector `select` does not suppress evaluation of either operand. Sanitize lanes before unsafe
shifts or float-to-int operations, avoid LLVM poison, and account for host exceptions if either path can trap.

Implement I15 in this order:

1. **Expose the cost.** T2/T3 count helper calls and fast/slow path entries by instruction group. Benchmark all-normal
   vectors and vectors with exactly one exceptional lane. Verify which guard rejects that lane rather than assuming
   all scalar paths have the same cause.
2. **Reduce call overhead first.** Apply the audited I7 state classification and batch remaining scalar fallback work
   behind one vector-instruction helper entry. Measure spills and state synchronization as well as call counts.
   A scalar loop inside that helper is an intermediate step, not completion of SIMD translation.
3. **Remove always-scalar cases.** Begin with guarded `FABD`, then audit rounding and conversions against the target
   lowering table. Reuse existing vector arithmetic and add only the missing operations and exact fixups.
4. **Keep ordinary lanes in vectors.** Replace all-lane rejection where a proven vector mask and fixup can handle
   the exceptional lanes. Derive NaN, zero, overflow and underflow decisions from the guest pseudocode. Where the
   proof is incomplete, take the one-call exact fallback. Do not simply evaluate both existing paths and select:
   the scalar helper path changes FPSR and would still execute its serial calls.
5. **Finish estimates and shifts.** I8 uses the guest's exact estimate algorithm, not approximate host reciprocal
   instructions assumed to match. I9 uses whole-vector masks and safe counts. Prefer generic vector IR when LLVM
   already lowers it well, then feature-gated intrinsics for demonstrated gaps. Check every supported host family.
6. **Keep the evidence.** Add compiler fixtures under `tests/llvm_perf/` during implementation, with the opaque-call,
   scalar arithmetic and scalar-intrinsic controls above. Keep semantic tests separate from lowering checks. Compare
   helper calls, spills, code size and repeated timings after each instruction group, including mixed-lane inputs.

Rosetta's SIMD-to-SIMD mapping is documented in Dougall Johnson's
[SSE2 discussion](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/). Its Apple-specific FP hardware is
a separate advantage. The lane masks, batched helpers and FPSR reductions above are proposed ScarletNX adaptations,
not claims about Rosetta's undocumented implementation. They do not depend on a cooperative Horizon kernel.

##### I8 Inline FRECPE, FRSQRTE and FRECPX

- **Change.** Emit the Arm ARM's estimate tables and steps inline, for all lanes at once. The results are defined bit
  for bit, so the inline form must match the helper exactly. Games use these estimates in vector math.
- **Where.** `ir_vec_fp.c`, `ir_fpu.c`.
- **ABI.** None.
- **Proof.** The differential test with `--sweep` on these rows.
- **Size and priority.** M, P1.

##### I9 Inline shifts by register

- **Change.** Emit whole-vector shifts, count masks and saturation, following I15's lowering checks. Clamp or mask
  counts before LLVM shifts to avoid poison for out-of-range counts. Combine QC across lanes once. Keep an exact
  fallback where the target lacks an efficient vector form.
- **Where.** `ir_vec.c`.
- **ABI.** None.
- **Proof.** The differential test.
- **Size and priority.** M, P2.

##### I10 Inline the small helpers of the runtime library

- **Change.** Link the bitcode of small `rtlib.c` helpers into each unit before optimization, and keep slow paths as
  cold calls. The inline helpers of `cpu.h`, such as `ldx` and `stx`, are already IR in each module (`ir_helpers.c`).
- **Where.** `ir.c`, `rtlib.c`.
- **ABI.** None.
- **Proof.** Recompile time and module size stay acceptable, and `llvm_bench` improves.
- **Size and priority.** M, P2.

#### Floating point

**Rosetta 2.** x86 and Arm both follow IEEE 754 but differ in NaN propagation and in when tininess is detected. Dougall
Johnson writes that the M1 has a non-standard mode, close to the later FEAT_AFP of Armv8.7, that gives the x86 behavior
in hardware. x87 is a full but slow software emulation.

**ScarletNX now.** FP runs on the host's IEEE unit only behind guards that prove the result and the cumulative flags
equal the soft-float helper's, and the helper runs otherwise (`fast_binary`, `fast_sqrt` and `fast_fma` in
`src/common/cpu/recompiler/llvm/ir_fpu.c`). The helpers follow the Arm ARM for NaN propagation, the default NaN, flush
to zero and every rounding mode (`src/common/cpu/aarch64/helpers/fp.c`), and no FP group differs from QEMU
(`tests/llvm_qemu_expected.txt`). A directed rounding mode sends every operation to a helper. The FMA fast path waits
until FPSR.IXC is set and needs a host FMA. Halves, scalar `FRINT*` and `FJCVTZS` always call helpers. The
`fpsr_flags` setting (default 1) keeps FPSR exact.

**On each host.** The guards are host-generic, because each proves the host result equal to the helper's whatever the
host's own rules are. That covers RISC-V, which never propagates a NaN payload, since the fast paths never produce a
NaN. Only ties-away rounding (`FRINTA`, `FCVTA*`) has a fast path limited to AArch64 hosts, which have that rounding
instruction (`ir_fpu.c`). An AArch64 host can drop the guards altogether, as Rosetta's hardware mode lets it (I12).

##### I11 More guarded FP paths

- **Change.** Prove the inexact flag of a single FMA through a residual in double, and of a double FMA with an
  error-free residual. Use F16C for halves on x86-64 hosts that have it, and the `llvm.roundeven` family for scalar
  `FRINT*` as the vector form already does.
- **Where.** `ir_fpu.c`, `ir_vec_fp.c`.
- **ABI.** None.
- **Proof.** The differential test with `--sweep`, on each host.
- **Size and priority.** M, P2.

##### I12 Native FP on AArch64 hosts

- **Change.** Load the guest FPCR (rounding mode, FZ, DN, FZ16) into the host FPCR on entry to guest code, and restore
  the host's on every path into the runtime. Emit plain host FP with no guards, read the host FPSR at `MRS FPSR` and at
  exits, and keep `FPCR.AH` clear on hosts with FEAT_AFP. Mark functions with LLVM's dynamic denormal mode so constant
  folding does not assume the default environment.
- **Where.** `ir_fpu.c`, `ir_vec_fp.c`, `ir_codegen.c`, the thread loop and the SVC entry in the runtime.
- **ABI.** Bump.
- **Proof.** The differential test on an AArch64 host, with all FPCR modes.
- **Size and priority.** M, P1 for AArch64 hosts.

#### Exclusives and atomics

**Rosetta 2.** x86's locked instructions become Arm atomics. The direction is the reverse here.

**ScarletNX now.** The Switch's Cortex-A57 has no LSE atomics, so compiled code builds every atomic from `LDXR`/`STXR`
loops. `LDXR` arms four monitor fields in `CPU`. `STXR` loads them, does a locked compare and swap and clears the
monitor (`ir_ldst.c`, `ir_helpers.c`). The monitor compares values, so it cannot see ABA, as in Dynarmic.

**On each host.** LLVM lowers a compare and swap to `lock cmpxchg` on x86-64, to LSE or an exclusive loop on AArch64,
and to reservation loops on PowerPC64, RISC-V and LoongArch. A 16-byte compare and swap goes through `RtAPI.cas128` on
RISC-V and LoongArch (`docs/RISCV-LOONGARCH.md`).

##### I13 Exclusive loops as single host atomics

- **Change.** Recognize the loops compilers emit inside one block: load exclusive, one operation (`ADD`, `SUB`, `AND`,
  `ORR`, `EOR`, a plain swap, or a compare that branches out for a compare and swap), store exclusive, and `CBNZ` back.
  Emit one `atomicrmw` or `cmpxchg` with the loop's ordering. Leave the registers and flags as one successful pass of
  the loop would, with the status register zero and the monitor clear.
- **Where.** `ir_ldst.c`, `ir_codegen.c`.
- **ABI.** None.
- **Proof.** The differential and block tests, a multithreaded synthetic program that counts with atomics, and the T2
  helper counts.
- **Size and priority.** M, P1.

#### Native code for library routines (beyond Rosetta)

Rosetta translates the x86-64 system libraries too. Windows on Arm goes further with ARM64EC, which lets native code
run inside an x64 process, and Box64 runs whole native host libraries in place of translated ones.

##### I14 Native versions of hot SDK routines

- **Change.** If T4 shows them hot, recognize `memcpy`, `memset`, `memmove`, `memcmp` and `strlen` in the game's SDK
  by dynamic symbol name together with a hash of their code. Replace their bodies with host routines that work on guest
  memory through `c->mem`, in the module's runtime library, so each host gets its own best version.
  Register these replacements through the expanded hook system and coordinate with A4's planned SDK layer instead
  of maintaining duplicate replacements. Preserve guest memory
  permissions, GPU ownership traps and callback/state contracts at each supported entry point.
- **Where.** `codegen.c`, `rtlib.c`, `ir_codegen.c`.
- **ABI.** None while the routines live in the module.
- **Proof.** The results equal the guest routine's on randomized inputs, including overlapping and unaligned ranges.
  The replaced routine leaves the registers the guest routine would leave under AAPCS64.
- **Size and priority.** M, P1 if T4 shows them hot, else P3.

### 5. Memory order

**Rosetta 2.** Apple silicon has a per-thread TSO mode that gives ordinary loads and stores x86 ordering, so Rosetta
needs no barriers for them. Apple documents how Linux VMs can use the same mode for Rosetta (Virtualization framework,
"Accelerating the performance of Rosetta"). Dougall Johnson notes that NVIDIA's Denver and Carmel and Fujitsu's A64FX
also implement TSO.

**ScarletNX now.** Plain guest loads and stores are ordinary LLVM loads and stores at `c->mem + address`, volatile only
when the `volatile_memory` setting is on (default off). Ordered accesses are sequentially consistent LLVM atomics.
`DMB ISHLD` and `DMB ISHST` become acquire and release fences, and other barriers become full fences (`ir_ldst.c`,
`ir_int.c`, `ir_state.c`).

**On each host.** The guest's model is the weaker one only on x86-64, where `LDAR` costs a plain `mov`, `STLR` an
`xchg` and a full `DMB` an `mfence`. AArch64 has the guest's own model, and only `DMB ISHST` comes out stronger than it
needs (`dmb ish`). PowerPC64 is weaker than Arm, because it is not multi-copy atomic, and RISC-V's RVWMO is close to
Arm's. On every host but x86-64, the host CPU itself can reorder plain accesses, so an address dependency that LLVM
optimizes away could let a guest observe an order the console never shows. Fences cover the barriers, but only tests
can show what plain accesses do there.

#### M1 Litmus tests on every host, and the small fixes

- **Change.** Add litmus tests for the Arm model (message passing, store buffering, load buffering, IRIW, and message
  passing with an address dependency) as synthetic multithreaded programs, and run each many times on every host. Emit
  `dmb ishst` for `DMB ISHST` on AArch64 hosts. Keep `STLR` as a sequentially consistent store on x86-64, because a
  release store followed by `LDAR` must not reorder, and only revisit it if T2 shows `STLR` is hot. Examine whether
  LLVM can split, duplicate or reorder a plain guest access in a way an Arm thread could observe, and use unordered
  atomics for those accesses if it can.
- **Where.** `ir_ldst.c`, `ir_int.c`, `ir_state.c`, new tests.
- **ABI.** None.
- **Proof.** The litmus tests show only outcomes the Arm model allows.
- **Size and priority.** M, P2. P1 on PowerPC64, whose model is known to be weaker than Arm's, and on LoongArch until
  its model is compared.

#### M2 The whole guest address space, reserved

- **Now.** The host reserves at most 128 GiB, and the address space the guest is told about ends there. Generated code
  adds `c->mem` with no mask (`src/nx/memory/mem.c`, `ir_state.c`), so a wild guest pointer past the reservation reaches
  host memory instead of faulting as it would on the console.
- **Change.** Where the host has the address space, reserve the full 39-bit guest range plus a guard as inaccessible
  memory. Add a recompile setting that masks addresses to 39 bits for debugging, off by default.
- **Where.** `mem.c`, `ir_state.c`, `settings.txt`.
- **ABI.** None.
- **Proof.** A synthetic wild pointer faults cleanly. `llvm_bench` shows the mask's cost.
- **Size and priority.** S, P2.

#### M3 Free the base register (experiment)

Rosetta's translated code reaches guest data with no base register, because guest and host share one address space
(Dougall Johnson, on the memory layout). ScarletNX loads `c->mem` once per function and adds it to every guest address,
so one host register holds the base (`ir_codegen.c`, `ir_state.c`).

- **Change.** Map the guest space at the same host addresses, reserved with `MAP_FIXED_NOREPLACE` where the host's
  address space allows, so the base is the constant 0. On Linux x86-64 the GS segment, through LLVM address space 256,
  is a second way. Windows reserves GS for itself.
- **Where.** `mem.c`, `ir_state.c`.
- **ABI.** Bump, because the module is compiled for one model.
- **Proof.** `llvm_bench` and llvm-mca on hot loops first. Keep it only on hosts where the gain is clear. x86-64, with
  its 16 registers, is where it can matter.
- **Size and priority.** M, P3.

### 6. System calls

**Rosetta 2.** attesor's README lists system call translation among Rosetta's parts, with the work of keeping register
state across the call.

**ScarletNX now.** An SVC calls its own entry through `RtAPI.svcs[imm]` in a Switch module, with each SVC's registers in
the 64-bit and 32-bit ABIs generated by `tools/svcgen` from `src/nx/hle/svc/hos.svc`. The GNU ABI runtime accepts
`SVC #0` and reads the Linux syscall number from `x8` (`docs/ARCHITECTURE.md`). To promotion an SVC is a call like any
other with no attributes, so it stores every dirty guest register before it and reloads every live one after it
(`call_site` in `ir_promote.c`).

**On each host.** The SVC path is IR and runtime C, the same on every host.

#### S1 SVC sites that sync only the SVC's registers

- **Change.** Use `hos.svc` as the starting point for register arguments and results, then audit the entire handler
  path for context inspection, blocking, scheduler transitions, faults and callbacks. Encode an explicit effect
  contract for each audited SVC. Narrow stores and reloads only when that contract proves them safe. The argument
  list alone cannot prove this: a call with few arguments may expose the whole thread context while it blocks.
  Context-changing, debug and unaudited calls remain full sites. Any path that needs the complete `CPU` state must
  materialize it before runtime re-entry. In GNU ABI modules, use separate Linux effect contracts where `x8` is known.
- **Where.** `ir_codegen.c`, `ir_promote.c`, `tools/svcgen`.
- **ABI.** Bump, since the runtime may no longer read every guest register during an SVC.
- **Proof.** The SVC tests and a handler audit, followed by tests of blocking and wakeup, thread suspension and context
  inspection, IPC TLS buffers, and game runs. Check both entry state and state after resumption. An unproven effect
  keeps the full synchronization path.
- **Size and priority.** M, P2.

### 7. Runtime support: CPU features, TLS, signals and exceptions

**Rosetta 2.** attesor's README lists CPU feature detection, thread-local storage and signal and exception handling.

**ScarletNX now.**

- **CPU features.** The generated decoder takes each operation's feature from `a64.profile`, and the target's feature
  set (the Switch's Cortex-A57 by default) decides which words are UNDEF (`docs/ARCHITECTURE.md`). The guest sees the
  console's CPU on every host.
- **Thread-local storage.** A guest thread is a host thread with its own `CPU` and TLS (`docs/ARCHITECTURE.md`, "Guest
  exec").
- **Faults.** On Linux, guest memory that the GPU owns traps through `userfaultfd`, and the runtime lands the page and
  resumes the thread (`src/nx/memory/own.c`, `src/nx/runtime/platform/plat_posix.c`). A crash report has the registers
  of the last call site (I1 adds the pc).
- **Signals in the GNU ABI runtime.** Handlers are recorded but never entered, and a thread `clone` returns `ENOSYS`
  (`docs/GNUABI.md`).

#### R1 Signals for the GNU ABI runtime at safe points

- **Change.** Deliver an asynchronous signal at the next poll or SVC, where the `CPU` structure holds the exact guest
  state, and build an A64 signal frame from it. Synchronous faults in guest code need the pc map of I1, and stack maps
  to rebuild the registers.
- **Where.** `src/gnuabi/`, `ir_codegen.c`.
- **ABI.** None for asynchronous signals. Synchronous ones follow I1.
- **Proof.** Programs that install handlers run under `scarletnx-gnuabi` as they do under `qemu-aarch64`.
- **Size and priority.** M, P2. T1's suite can then run more programs.

#### R2 Native execution on AArch64 hosts (research)

The yuzu family runs guest code directly on AArch64 Android devices (NCE) and patches the instructions that need the
emulator, such as SVC and reads of the thread registers. For ScarletNX that would mean a new execution mode beside the
LLVM path: the guest space mapped at its own addresses, host pages of 4 KiB (Apple silicon uses 16 KiB pages, and many
ppc64le kernels use 64 KiB, per `docs/APPLE.md` and `docs/PPC64.md`), preemption by signal in place of polls, and
`IC IVAU` through page protection. The yuzu family's code is GPL-2.0-or-later and can be ported with its copyright lines
and an entry in `docs/CREDITS.md`. XL, P3, and a decision for the maintainers.

Before implementation, specify the Horizon SVC, TLS, scheduling, fault and code-invalidation paths from the OS boundary
section. Validate them independently of instruction throughput. A matching instruction set does not supply a matching
kernel ABI, and host signals alone do not prove that guest preemption is correct.

### 8. Delivery: the daemon, signing and debugging

**Rosetta 2.** The `oahd` daemon translates outside the process and keeps the cache in a protected folder
(Champollion part 1). attesor's README adds that the translations are code-signed and that a debug server handles
translated processes.

**ScarletNX now.** The launcher and the driver recompile before play, and `scarletnx -r` and the recompiler library
(`docs/LIBRARY.md`) are the translation tools. Apple modules are Mach-O dylibs with an ad hoc signature. The GDB stub
(`-S` or `-s`) and `gdb_jit` debug a running game, and the JIT cache file is replaced through a rename
(`jit_service.c`).

#### D1 Recompile in the background when a session found new code

- **Change.** With stable units (A3), the launcher offers a recompile when a session added hints, as `oahd` translates
  apart from the program, and it builds only the units that changed. It says how many functions the last session found.
- **Where.** `src/nx/runtime/driver/recompile.c`, the launcher in `src/gui/`.
- **ABI.** None.
- **Proof.** The next session shows no miss in the scenes the last one reached.
- **Size and priority.** M, P2. P1 on hosts with no JIT, where a hint marks a function that ended a game.

### 9. Debug switches and measurement

**Rosetta 2.** The runtime reads switches such as `ROSETTA_PRINT_IR`, which prints each block with its flag liveness,
and `ROSETTA_PRINT_SEGMENTS` (Champollion part 2).

**ScarletNX now.** `tests/llvm_bench.c` times six assembly kernels (integer, FP, SIMD, crypto, tricks, calls) with no
native or QEMU baseline, and no doc records its numbers. At exit the log gives the AOT variant hits and misses and the
JIT batch statistics (`jit_report` in `guest_jit.c`, `SnxJitStats` in `jit_service.h`). The Windows sampler, Tracy and
`gdb_jit` exist. AOT functions carry `F_<address>` names in the module's symbol table (`ir_codegen.c`, `link_elf.c`),
so Linux `perf` names them, but it cannot name JIT code. `tests/llvm_qemu_differential.c` checks samples of every row of
the A64 decoder against `qemu-aarch64 -cpu max`, one instruction at a time.

Every gain this plan claims needs these items first.

#### T1 Native baselines for the benchmarks

- **Change.** Write the kernels' work as portable C as well, build it natively for the host, and report the ratio of
  native time to translated time. Add a single-thread suite of AArch64 Linux programs run through `scarletnx-gnuabi`,
  with the same programs built natively, so every host that runs the GNU ABI runtime gets a speed ratio. The suite stays
  single-threaded until that runtime runs threads.
- **Where.** `tests/llvm_bench.c`, `tests/CMakeLists.txt`, a new suite beside `tests/llvm_perf/`.
- **ABI.** None.
- **Proof.** CI prints the ratios. Repeated runs give the median and the spread, so a later change can be judged
  against noise.
- **Size and priority.** M, P0.

#### T2 Counters in the runtime

- **Change.** Extend `jit_report` and `SnxJitStats` with the wait time per miss (from `rt_miss` to the thread running
  again) and the calls into NRO code. Add a `cpu_stats` recompile setting that compiles counters into the module for
  lookups, stale and pc check mismatches, and helper calls by kind. Print the counters every few seconds as well as at
  exit.
- **Where.** `rtapi.c`, `guest_jit.c`, `jit_service.c`, `ir_codegen.c`, `settings.txt`.
- **ABI.** None if the counters live in module data and the runtime reads them through an existing path. Otherwise one
  bump for a descriptor field.
- **Proof.** A game run with the setting on shows counts that agree with the recompile statistics of T3.
- **Size and priority.** M, P0.

#### T3 Statistics from the recompile, and a dump switch

- **Change.** After promotion, count for each unit: call sites, the stores and loads of guest state at those sites,
  helper calls by kind, lookups, polls and stale checks. Print the totals in the recompile log. Add a switch that dumps
  one guest function's leaders, blocks, flag origins and promoted registers, as `ROSETTA_PRINT_IR` does for Rosetta.
- **Where.** `ir_promote.c`, `ir_codegen.c`, `module_llvm.c`.
- **ABI.** None.
- **Proof.** The counts for the `tests/llvm_module.c` program match a count by hand.
- **Size and priority.** S, P0.

#### T4 JIT code in Linux profilers

- **Change.** Write a perf map or jitdump file for each JIT unit. With I1, `perf annotate` can show guest addresses.
- **Where.** `jit_service.c`, `code_loader.c`.
- **ABI.** None.
- **Proof.** `perf report` on a game run names guest functions in JIT code as it does in AOT code.
- **Size and priority.** S, P0.

#### T5 A block-level differential fuzzer

- **Change.** Generate random sequences of several blocks, with branches, calls and loops, from the same rows of the
  generated decoder, and compare the end state with QEMU. Run them with the settings that change code generation, and
  on every host that can run the test. Each bug it finds gets a fix that names the mechanism, with the seed as its test.
- **Where.** A new test beside `tests/llvm_qemu_differential.c`, sharing its QEMU harness in `tests/qemu_ref/`.
- **ABI.** None.
- **Proof.** It must find a bug planted in a flag origin across blocks, then pass on the unchanged tree.
- **Size and priority.** M, P0. It must exist before I2, I3, I6 or I13 change how state flows between instructions.

#### T6 Bring the docs up to date

Fix the docs and comments listed under [Docs and comments that are out of date](#docs-and-comments-that-are-out-of-date)
before the plan's changes pile more on top. S, P0.

## Order of work

Execute the [first SIMD milestone](CPU_SIMD_MILESTONE.md) first, using only the measurement and correctness tools its
selected group needs. The table below groups later backlog work. Re-rank it after the milestone's profile and results.
The full T1 through T6 program is not a prerequisite for that milestone. Keep T5's stronger gates for the ABI/register and
atomic-loop redesigns identified in the guide.

Items that help every host come first. Hosts with no JIT need coverage before speed. A1 is accepted, and A2 is normal
AOT coverage work. A3 must preserve optimization opportunities or measure the tradeoff. A4 is temporary pending the
linked SDK layer. A5 and its profile-dependent L2 path are optional and do not gate the main work.
J1 is deferred. L1's standalone PLT binder is retired in favor of the hook expansion planned by 2026-10-31. Prioritize
whole-vector execution, register traffic and coverage. Coordinate SDK call work with the hook system's owner.

| Backlog phase | Items | What it gives |
| --- | --- | --- |
| 0. Measure and guard | T1, T2, T3, T4, T5, T6, I7 | Baselines, counters, the fuzzer, and the one quick fix |
| 1. Execution and coverage | I4, I2, A1, A2, J2, I13, I1, I15 | Cheaper guest calls, fewer misses and serial lane calls, exact fault pcs |
| 2. Registers and instructions | I3, I8, I12 (AArch64), S1, I14 (if hot), I6 (P1 on RISC-V and LoongArch), L3 | Rosetta-like register passing, fewer helper calls, native FP on AArch64 |
| 3. Coverage and correctness | A3, D1, J3, J4, I9, I10, I11, L4, M1 (P1 on PowerPC64), M2, R1 | Stable AOT units, a JIT everywhere, memory order tested on every host |
| Optional profile work | A5, L2 | Workload-specific gains, measured against normal builds |
| Transitional work | A4 | Small compiler improvements pending the linked SDK layer |
| Hook integration | I14 where hot, with L1's integration constraints | SDK replacements share the expanded hook system and preserve its lifecycle |
| Deferred | J1 | Tiered compilation only if later measurements justify it |
| Research | A3's LTO path, M3, I5, R2 | Kept only where measurements show a gain |

## Prior art

Check the license of each file before porting anything. ScarletNX accepts GPL-3.0, GPL-2.0-or-later, LGPL, MIT, BSD,
ISC, zlib and Apache 2.0, never GPL-2.0-only (`CLAUDE.md`).

* **Dynarmic** (0BSD), the A64 to x86-64 JIT of the yuzu family, compares values in its exclusive monitor as ScarletNX
  does. It also offers opt-in unsafe FP optimizations, a model for keeping fast modes as settings that are off by
  default.
* **ARMeilleure**, Ryujinx's JIT (MIT), keeps a profiled persistent translation cache (PPTC), the closest relative of
  `<game>.jitcache` with hints.
* **FEX-Emu** (MIT) and **Box64** (MIT) translate x86-64 to AArch64, Rosetta's direction. Box64 runs whole native host
  libraries in place of translated ones, the model for I14.
* **Microsoft's Prism with ARM64EC** lets native AArch64 code share an x64 process through an ABI that mirrors x64
  register use, another model for I14.
* **The yuzu family's NCE** (GPL-2.0-or-later) runs guest code natively on AArch64 Android (R2).
* **QEMU** is where the LLVM emitters come from (`translate-a64.c`), and its vector code is the model for I9.

## How every change is proven

* Emitter changes pass `ctest -R llvm_qemu_differential` (with `--sweep` on the touched rows), `tests/a64_ported.c`,
  `tests/llvm_module.c` and the block fuzzer of T5, on every host that can run them. A new group that differs fails the
  test.
* An `SNX_ABI` bump updates `tests/module_contract.c` and the table in `docs/ARCHITECTURE.md`.
* Speed claims come from repeated runs of the T1 suite and of `llvm_bench`, reported as a median with its spread, and
  from perf profiles (T4).
* Runtime changes are checked by running games through the scenes they affect (the `snx-run` skill), several times for
  anything that could be intermittent. A build is not a test.

## Risks and non-goals

* There is no interpreter, and none is planned. Keep the current JIT while J1 is deferred. J3 addresses hosts that lack
  a runtime JIT, independently of tiered compilation.
* No delays, retries or special cases for a title ID.
* A mode that changes results is a setting, and the exact behavior stays the default, as with `fpsr_flags` (default 1)
  today.
* The ABI contract mode (I2) can break code that does not follow AAPCS64. Its checker and its default of off are the
  guard.
* `preserve_none` needs a recent LLVM, and a host without it keeps the C convention.
* Every `SNX_ABI` bump that changes the IR costs every game a full recompile, so bumps should be grouped.
* RISC-V, LoongArch and big-endian PowerPC64 have not run on real machines, so their items can only be proven under
  emulation until they do.
* attesor code never enters the repository (see [Sources](#sources)).
* Host OS cooperation is not assumed. Horizon effects must remain observable through the runtime and any linked SDK
  layer. The SDK layer's future availability cannot justify removing today's correct path.

## Decisions for the maintainers

The AOT decisions settled by maintainer feedback are: A1 is accepted, A2 belongs in normal discovery, A3 needs an
explicit optimization/LTO tradeoff, A4 is temporary pending linked SDK translation, and A5 is optional. Revisit these
only if measurements or the SDK interface change the premise.
J1 is deferred by maintainer direction. Do not implement standalone L1 PLT binding ahead of the hook expansion planned
by Saturday, 2026-10-31. Treat that date as a plan, not an availability guarantee. Evaluate remaining import costs
after the hook system's coverage is known.

1. **ABI contract default.** Off until its checker runs clean on the games used for testing, then on. Recommended.
2. **Stale checks without mods.** Keep them, or drop the return checks for modules built without code-patching mods
   and fail loudly on `IC IVAU`. Recommended: decide after T2 and T3 measure their cost.
3. **Native SDK routines.** Only for routines whose code hash matches a known implementation. Recommended.
4. **Recompile from hints.** The launcher asks the player, or recompiles in the background. Recommended: ask, and show
   how many functions the last session found.
5. **Native execution on AArch64.** Worth an XL effort only after I3 and I12, and only on hosts with 4 KiB pages.

## Docs and comments that are out of date

The readers of the code for this plan found these. Each needs a check against the code, then a fix (T6).

* `docs/ARCHITECTURE.md` says guest accesses are volatile, and the header comment of `ir_ldst.c` says the same. The
  `volatile_memory` setting defaults to 0, so they are volatile only when it is on.
* `docs/ARCHITECTURE.md` names `ir_fp_fast`, which does not exist. The fast paths are `fast_binary`, `fast_sqrt` and
  `fast_fma` in `ir_fpu.c`.
* `docs/DIVERGENCES.md` lists FP failures from an older audit. The code now uses guarded fast paths and soft-float
  helpers, and `tests/llvm_qemu_expected.txt` lists no FP group.
* `docs/ROADMAP.md` lists ARM NaN propagation, default NaN and directed rounding as CPU work. The helpers in
  `src/common/cpu/aarch64/helpers/fp.c` implement them, so the item may be done.
* `docs/RISCV-LOONGARCH.md` says the JIT loader handles only x86-64 and AArch64, "as on PowerPC64", while
  `code_loader.c` and `docs/PPC64.md` describe a PowerPC64 JIT.
* The header comment of `rtlib.c` says the library is compiled once with `-march=native`, with the flags of generated
  C. `ir.c` loads its bitcode, removes the CPU attributes and compiles it for the target CPU.
* `docs/AOT_PACKS.md` says the JIT compiles the missed function. It compiles batches of up to 256 pcs
  (`jit_service.h`).
* `docs/NRO.md` says there is no dynamic NRO registration and mentions generated C. `ldr.c` maps NROs at run time
  through the JIT, and LLVM is the only backend.
* `docs/LDR_RO.md` gives the cost of a call into an NRO without the `variant_resolve` step (`guest_jit.c`).
* `jit_cache.h` says the runtime never reads the JIT cache, while `docs/ARCHITECTURE.md` and `guest_jit.c` describe a
  later run loading its units.
* `ir_profile.c` says it discards duplicate identities, but its loop keeps the first entry of each group after an
  unstable sort.

## Sources

* FFRI Security, Project Champollion, "Reverse-engineering Rosetta 2" parts 1 and 2,
  <https://github.com/FFRI/ProjectChampollion> (Apache-2.0). Its data comes from macOS 11 (2021), so later versions may
  differ. It holds analysis documents and a cache parser, not translator code.
* Dougall Johnson, "Why is Rosetta 2 fast?", 2022,
  <https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/>.
* Apple, "Accelerating the performance of Rosetta", Virtualization framework documentation,
  <https://developer.apple.com/documentation/virtualization/accelerating-the-performance-of-rosetta>.
* Apple, "Rosetta 2 on a Mac with Apple silicon", platform security documentation,
  <https://support.apple.com/guide/security/rosetta-2-on-a-mac-with-apple-silicon-secebb113be1/web>.
* LLVM, ThinLTO design and incremental backend caching, <https://llvm.org/docs/ThinLTO.html>.
* Switchbrew, Horizon SVC contracts, <https://switchbrew.org/wiki/SVC>.
* Arm, the Arm Architecture Reference Manual for FEAT_FlagM, FEAT_FlagM2 and FEAT_AFP, <https://developer.arm.com/>.
* attesor, <https://github.com/Inokinoki/attesor>. This plan uses the outline of Rosetta 2 in its README. Its README
  claims the MIT license but the repository has no license file, and the README says AI tools wrote much of it. It
  contains Ghidra output of Apple's Rosetta binaries (`rosetta_decomp.c` and `rosettad_decomp.c`) and code
  reconstructed from that output. None of it can be ported. Its documents conflict with Champollion in places (the cache
  location, an interpreter mode, optimization passes), so this plan uses none of their claims without a second source.
