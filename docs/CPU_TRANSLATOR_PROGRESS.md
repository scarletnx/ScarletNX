<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# CPU plan implementation and candidate measurements

## Index

* [SIMD continuation](#simd-continuation)
* [Prepare the next implementation](#prepare-the-next-implementation)
* [Search a bounded set of exact candidates](#search-a-bounded-set-of-exact-candidates)
* [Compiler diagnostics](#compiler-diagnostics)

The [plan](CPU_TRANSLATOR_PLAN.md) contains 41 items. Its full active backlog is the implementation scope,
starting with the [SIMD milestone](CPU_SIMD_MILESTONE.md). This is not a claim that all items are implemented.
`cpu_plan_status.json` records verified progress separately from unverified existing code, research, deferred and
retired items. Inspect an item's proof and prerequisites before changing architecture or guest state contracts:

```sh
python3 tools/cpu_plan.py list
python3 tools/cpu_plan.py show I15
```

The earlier prerequisite started at upstream `34c0be36`. The O0 vector-arithmetic prerequisite moves residual calculations
behind the sticky IXC check and removes a redundant O0 shortcut guard. Directed tests compare the complete CPU
state across optimization levels and loaders. This does not complete I15's mixed-lane fallback replacement.
I7 classifies the audited vector FP helpers at their first declaration. A fixture keeps a dirty X4 live across
one batched helper call, verifies zero reloads of it and checks the emitted synchronization counts. This affects register
promotion at O1 and above. It does not add promotion to O0.

I8 emits scalar and vector float32/float64 FRECPE and FRSQRTE on little-endian x86 and AArch64 emitter paths,
including exceptional lanes. Without AVX2, ordinary reciprocal inputs retain the proven division sequence.
The shared estimate tables contain 256 reciprocal and 384 reciprocal-square-root entries per IR unit that needs
them. Unsigned URECPE/URSQRTE also use bounded table lookups. All table indices and shift counts are bounded even
in discarded lanes. NaN payloads, signed zeros, denormals, rounding modes and sticky exceptions are checked against
the exact helper. FPSR contributions are reduced across the vector once. Scalar 32/64-bit FRECPX uses integer
exponent operations with exact NaN and denormal handling on all targets. Half estimates now use the integer paths described below on little-endian targets. Other unproven host lowerings
retain the exact helper paths.

I9 emits rounding and saturating shifts for 8/16/32/64-bit lanes on x86 and AArch64. Shift counts use their signed
low byte, are bounded before LLVM shifts, and contribute to QC once per vector. x86 without AVX2 uses bounded
constant-stage shifts. Other hosts retain exact helpers.

I11 adds guarded scalar rounding, FPCR-selected scalar/vector rounding, F16C half conversions and exact FMA
inexactness residuals. The FMA residual runs only when IXC is clear. Float64 residuals use the error expansion of
[Boldo and Muller](https://perso.ens-lyon.fr/jean-michel.muller/correct_fma_err.pdf), with conservative intermediate
range guards and exact fallback. Scalar O0 FMA retains the original emitted sequence: its candidate did not pass
the measured regression gate. Vector O0/O2 and scalar O2 residuals passed that gate. Vector half-to-single and single-to-half conversions now
use whole-vector exact fixups. Direct float64-to-half conversion retains the helper.

Native Linux AArch64 CI at `9a7cfd0d` passed the full QEMU sweep, rounding, half-conversion and scalar/vector
residual tests. The x86-64 local sweep also passed. This completes I11's guarded-FP host checks for these native
hosts. The newer I15 conversion changes still require their separate integration checks.

Guarded FABD reuses exact subtraction followed by sign clearing. Directed tests cover all shift counts, every
half encoding, estimate bins/exponents, mixed exceptional lanes, aliases, rounding modes and FPSR. They run through
O0/O2, AOT/JIT loaders and native/portable paths. The executed host is x86-64; these checks do not establish native
execution on other hosts. Native AArch64 FPCR/FPSR integration (I12) remains a separate project. Exact scalar
arithmetic remains inside batched fallbacks where no vector replacement is proven.

T3 now checks all 19 CLI statistics fields against hand counts for two complete integer modules. Each fixture runs
both in one unit and split at function boundaries. The CLI checks also require function leaders, flag origins,
promoted registers and IR before and after promotion.

T5 draws safe data operations from 81 families of the generated decoder. It varies conditional branches, bounded
loop counts, direct and indirect calls, tail calls, memory order and aligned exclusive retry loops. It runs
through both loaders and optimization levels, plus portable, hook and volatile configurations.
A 20-configuration seed matrix checks 10,240 complete program states. Two self-tests must detect planted flag-origin
and exclusive-update defects. This remains a bounded instruction grammar, not exhaustive sequence coverage.
Native Linux x86-64 and AArch64 CI passed the seed matrix and both planted-defect tests at `72deb357`.

T6 corrects the stale documentation and source comments listed in the plan, preserving historical audit examples
as historical evidence rather than current failure claims. Other items require their own implementation and proof.

## SIMD continuation

The 2026-11-04 continuation adds exact whole-vector comparisons. Ordinary single/double comparisons retain the
short floating-point path. Exceptional lanes use integer classification and ordering, including signaling NaNs,
flushed denormals and signed zero. Half comparisons use integer vectors for all lanes. The directed test covers
80 instruction forms, destination aliases and every half encoding across modes, loaders and optimization levels.

A tested extension that batched conversion and rounding fallbacks was rejected. It reduced outer calls but made
portable conversion kernels about 15% slower, and hid repeated values from LLVM's common-call elimination.
A later integer-vector conversion replaces the vector half-to-single and single-to-half paths, including exceptional
lanes and all FPCR modes. The sequence test checks ordinary, DUP and constant-zero producers before conversion,
including full register state, and requires zero conversion helper calls. Other conversion and rounding paths
retain their pure scalar helpers. Existing arithmetic batches keep their descriptor and exact fallback behavior.

The half-conversion tests compare full CPU state across every half encoding, rounding and alternate-half modes,
source/destination aliases, both loaders and O0/O2. Random single encodings cover narrowing boundaries and special
values. A reversed-byte model checks lane packing separately from native big-endian ABI execution. An independent
Python rational-arithmetic check proves exact widening for all 129,024 finite IEEE and alternate-half encodings.
The first 24-kernel narrowing timing search was inconclusive for two ordinary-input regression bounds. A fresh
25-round paired check of all four native ordinary-input forms cleared the unchanged regression gate, with speedups
of 1.058 to 1.207. The exceptional and portable-emitter forms in the original search improved by about 5.6 to 7.3
times. Widening passed its original gate, with a 9.48-times geometric kernel ratio and a 95-percent bootstrap
interval of 9.17 to 9.63. These figures describe the supplied synthetic kernels on x86-64, not game FPS.

The retained candidate also passed the full half-conversion and estimate tests under `qemu-x86_64 -cpu qemu64`:
71,303,168 conversion cases and 40,225,792 estimate cases. This verifies correctness under the emulator's baseline
x86 feature set. It does not measure performance on a physical older CPU. Selected optimized half-conversion
objects for baseline x86-64, Cortex-A57 and generic RISC-V contain no conversion-helper calls. The entry's
FP-environment normalization remains where needed.

The estimate continuation rejected tables for every reciprocal input without AVX2: ordinary reciprocal kernels
regressed to about 0.29 times baseline speed. A hybrid keeps the original ordinary reciprocal calculation, uses
integer fixups for exceptional lanes, and uses the table path for reciprocal-square-root estimates. The revised
16-kernel search passed the unchanged gate with a 2.65-times geometric ratio and a 95-percent interval of 2.62 to
2.73. The emitter lacked AVX2, but the target machine still used the test host's native ISA. Physical old-x86
performance and native AArch64 performance remain separate checks.

A mixed-lane arithmetic experiment kept ordinary lanes in vectors when IXC was sticky. Its first mask encoded
lane weights incorrectly, which full-state tests caught through missing exception flags. A sign-extended mask
corrected the defect, and the directed tests passed. Both timing searches still failed the fixed regression gate:
the revised candidate improved mixed sticky-IXC kernels by about 29 to 35 percent, but several IXC-clear paths
regressed by about 2 to 3 percent. The experiment was removed, including its helper descriptor extension. The
original batched arithmetic fallback remains. This is an open part of I15, not a completed optimization.

Half FRECPE/FRSQRTE use bounded integer-vector tables, and half FRECPX uses exponent operations on little-endian
targets. FZ16 flushes input denormals without IDC. Reciprocal estimate output denormals remain unflushed, as the exact
helper requires. Register shifts also use a constant-stage barrel sequence when the x86 emitter lacks AVX2. AArch64
uses generic vector shifts, and other targets retain the previous exact helper. Native tests ran on x86-64.

Two independent Python searches used two warmups, nine measured paired rounds and 20,000 bootstrap resamples.
The selected comparison sequence had a 5.18-times geometric speedup across its supplied synthetic workloads.
Half-estimate kernels improved by about 2.5 to 8.8 times. Portable-emitter shift kernels improved by about 2 to
9.8 times on the native test machine. That machine's LLVM target still used its native ISA, so portable-emitter
results are not timings from an SSE2-only processor. These figures do not establish whole-game FPS gains.

The small-helper experiment imports only the audited FP-environment leaf from the embedded runtime bitcode.
Only functions with host FP operations use its inline copy. The checked integer-shift object remains byte-identical.
An initial broad import did not pass the regression gate. The selective version passed 19 paired rounds across six
FP-focused workloads, with about a 13-percent geometric gain. `SNX_IR_OBJECT_VERSION` increases to five to invalidate
objects made by the old pipeline. `SNX_ABI` remains 17, and old modules remain compatible. Recompile to use the new
emitter paths. Large arithmetic helpers remain out of line.

The min/max continuation removes helper calls from vector and pairwise min/max paths on little-endian targets.
Integer ordering handles NaNs, numeric variants, input flushes and signed zero, with one FPSR reduction. A new
30,112,768-case test covers all half encodings, mixed exceptional lanes and aliases. An independent rational model
proves the ordering of all 63,488 finite half encodings, which establishes the order of 4,030,726,144 finite pairs
with signed-zero ties handled explicitly. The original 60-kernel search measured a 4.53-times geometric ratio.
A fresh 35-round ordinary-input check passed the fixed regression bounds. O0 single/double paths retain the
original fallback after their separate regression gate failed. All twelve checked O0 min/max objects remain
byte-identical. O0 half min/max retains its measured vector improvement.

Rounding uses integer masks for half vectors and sanitized host rounding plus exact NaN/flush fixups for supported
single/double hosts. The expanded test checks 19,970,048 full states, including aliases. An independent rational
model checks all 634,880 finite-half/mode/flush combinations. The first 144-kernel search measured a 3.94-times
geometric ratio but rejected two O0 dynamic-rounding regressions. Those paths retain their original fallback.
Eight checked O0 single/double rounding objects remain byte-identical. The revised candidate passed the fixed
bounds in a fresh 35-round ordinary-input check. These ratios are synthetic kernels, not game-FPS multipliers.

The combined 681-test local suite passed all CPU tests, including the full QEMU sweep. The known `platform_posix`
page-tracking assertion still fails. Both ASTC Vulkan tests fail on the Intel device and pass with lavapipe.
Their 61 tracked build inputs match upstream, and their link includes no CPU backend. This GPU test discrepancy
is separate from the CPU emitter changes. The retained O0 decisions also passed the focused eleven-check run.
Native Linux x86-64, Linux AArch64 and Windows CI passed at `efb12311`, including the native CPU tests.
The final game rebuild exposed a compiler bottleneck before its scene comparison could run.

LLVM's tail-call analysis followed each temporary pointer through the helper's returned FPSR and later control-flow
joins. On one private unit, the isolated pass finished in 3.63 seconds with accurate non-capture attributes. The
original declaration exceeded the 35-second limit. This is an isolated pass comparison, not a complete rebuild ratio.
The helper consumes its buffers synchronously and never retains their addresses. Its pointer parameters now declare
that fact without adding a no-alias claim.

At O1 and above, batched FP calls also reuse one function-local scratch allocation. The helper reads an input lane
before it writes that lane, so output can reuse the first input's space. The allocation grows only when a larger
input set is emitted, to at most three 16-byte inputs, or 48 bytes. O0 keeps its existing allocation sequence.
The alias test compares all helper kinds and modes against their exact results and FPSR. A guarded-loop fixture
checks buffer growth, function boundaries and full CPU state with and without the pointer attributes.
The initial 48-workload runtime search had inconclusive per-workload bounds. A fresh 35-round comparison checks
the 16 distinct O1 kernels, each with 20,000 bootstrap resamples. Every workload clears the unchanged 2-percent
regression bound. The aggregate ratio is 1.004 with a 95-percent interval of 0.999 to 1.011, so runtime improvement
is inconclusive. This is a compiler-cost and stack-space change, not a demonstrated runtime speedup. All four O0
arithmetic objects remain byte-identical. Each O1/O2 arithmetic object is 16 bytes smaller, and the corresponding
O1 and O2 objects are byte-identical for both candidates. A fresh game rebuild remains required.

The earlier half-conversion and estimate continuation was recompiled and checked against its preceding module with the
same current runtime and restored private save. Both 100-second scene runs completed without crashes, and both
screenshots rendered correctly. The final four five-second gameplay windows averaged 32.95 FPS for the preceding
module and 33.00 FPS for the candidate. This single pair does not establish a game-FPS gain. The captures recorded
56,707 and 59,154 CPU samples with zero ring drops. Timer coalescing remains separate from ring drops. Game-derived
files and saves remain local.

The Linux CPU-clock sampler starts its buffer search at one slot and reports overflows, timer coalescing and exit
recovery separately. A native calibration selected two slots per thread. A later real game capture lost one record
at two slots, so the next run used four slots and lost none. The capture's weighted CPU samples placed FP helpers
at a small share of the scene's CPU work. This limits the expected effect of SIMD improvements on that scene.
The [development guide](DEVELOPMENT.md#cpu-sampling-without-perf) gives the commands and the file format.

## Prepare the next implementation

The workflow commands reduce repeated navigation and test setup. They do not generate proven implementations or
change the status tracker. An item without recorded prerequisites still needs its own audit and proof.

```sh
python3 tools/cpu_plan.py next
python3 tools/cpu_plan.py packet --out /absolute/path/to/new-work-packets
python3 tools/cpu_plan.py packet I8 --out /absolute/path/to/new-reciprocal-packet
python3 tools/cpu_plan.py verify T3 --out /absolute/path/to/new-check-results
```

`next` orders active work by recorded prerequisites and priority. Partial prerequisites remain blockers. Deferred,
retired and implemented items are excluded. Research stays identified as research. `packet` saves the current
requirements, recorded progress, dependency blockers, source filename matches, source hashes and known check
commands. It searches the repository's `src`, `tests`, `tools` and `docs`, without reading game data or external
reference directories. A filename match is a navigation hint, not proof of implementation.

`verify` builds the mapped targets using the process's CPU affinity as its default job count. It discovers the
configured tests before running them and saves logs, CTest XML and `checks.json`. Missing groups, skipped tests,
timeouts and failures prevent a passing verdict. I7, I8, I9, I11, I15, T3 and T5 have audited check mappings.
Other items require adding their own proofs first. Passing known checks does not mark an item complete or establish
PR readiness, cross-platform correctness or better game performance. Output folders must be new.

Expand a supplied candidate grid with literal `${axis}` placeholders in argv and working directories:

```json
{
  "item": "I15",
  "axes": {"variant": ["baseline", "candidate"]},
  "candidate": {
    "name": "${variant}",
    "cwd": "/absolute/checkouts/${variant}",
    "build": ["cmake", "--build", "build", "--target", "the-item-benchmark"],
    "check": ["build/the-item-benchmark", "--check"],
    "bench": {"ordinary": ["build/the-item-benchmark", "--bench", "ordinary"]}
  }
}
```

Replace the illustrative commands with real independent checks and equivalent workloads before running:

```sh
python3 tools/cpu_plan.py matrix I15 /absolute/path/to/grid.json --out /absolute/path/to/new-manifest.json
python3 tools/cpu_plan.py run I15 /absolute/path/to/new-manifest.json --out /absolute/path/to/new-results
```

Expansion validates the commands and unique names, rejects unknown axes and limits the grid to 256 candidates.
It executes nothing. The existing runner then builds and checks supplied candidates before serial timing. These
tools do not change optimization levels, ABI settings or runtime code, and never infer completion from filenames
or benchmark results. Use isolated worktrees when build commands can write shared outputs.

## Search a bounded set of exact candidates

`tools/tune_cpu_sequences.py` applies the same procedure to any plan item's supplied candidates. Each candidate
must have an independent correctness command. Build and check phases finish before timing starts. Builds run
in parallel only when candidates have distinct working directories; these must be isolated builds/worktrees.
All timings run serially, with two warmups and at least five shuffled measured rounds. Stop games, builds and
other profiling before timing. Diagnostic builds and timed builds must keep equivalent instrumentation costs.

Example manifest structure (replace these illustrative commands with the item's real checks and benchmark):

```json
{
  "item": "I15",
  "candidates": [{
    "name": "baseline",
    "cwd": "/absolute/path/to/isolated-baseline",
    "build": ["cmake", "--build", "build", "--target", "the-item-benchmark"],
    "check": ["ctest", "--test-dir", "build", "-R", "the-item-correctness-tests", "--output-on-failure"],
    "bench": {"ordinary": ["build/the-item-benchmark", "--bench", "ordinary"]},
    "code_bytes": 684
  }]
}
```

Add candidates with the same workload names and distinct output names. Each timed command must print a final JSON
line containing positive finite `elapsed_ns` and a result `checksum`. A different checksum aborts the comparison.
If code size is supplied, supply it for every candidate and measure the same code region. Record compiler, source,
feature and recompile settings in the manifest as additional metadata. Keep game-derived manifests and artifacts local.

```sh
python3 tools/cpu_plan.py run I15 /absolute/path/to/manifest.json --out /absolute/path/to/new-results
python3 tests/cpu_plan_tools.py
```

The report includes every raw observation, median, spread, MAD, failed candidate check, host affinity and manifest
hash. Its Pareto set retains candidates that trade speed in one workload or code size for another. It does not
claim a universal optimum, infer game FPS from instruction benchmarks, apply code, or mark a plan item complete.
Small median differences within run-to-run variation remain inconclusive. Use the item's independent directed,
differential, concurrency, module-contract, host and gameplay proofs before accepting a candidate.

The runner also computes paired bootstrap intervals for each workload and a weighted geometric aggregate. An
optional `workload_weights` object must give every workload a positive finite weight; the default is equal weights.
The recommendation requires a minimum aggregate improvement and a lower confidence bound within the regression
budget for every workload. A code-size budget requires measured `code_bytes` for every candidate:

```sh
python3 tools/tune_cpu_sequences.py /absolute/path/to/manifest.json --out /absolute/path/to/new-results \
  --rounds 11 --baseline baseline --bootstrap-samples 10000 --min-speedup 0.01 --max-regression 0.02
```

`--max-code-bytes` adds the optional size constraint. Statistical calculations may use multiple processes after
timing finishes; timings remain serial. These are per-comparison intervals over supplied candidates and workloads,
not a proof of a global optimum. Keep the reference when no candidate clears the thresholds. Select instruction
families separately when one family regresses; do not relax the threshold to accept it.

ABI/register and atomic-loop redesigns still require T5's block-level fuzzer. A benchmark script is not that fuzzer.
J1 stays deferred, L1 stays retired, and research items produce measured feasibility reports before implementation.

The reciprocal comparison is ready to run from the repository root after building:

```sh
python3 tools/cpu_plan.py run I8 tests/llvm_perf/recpe_candidates.json --out /absolute/path/to/reciprocal-results
python3 tools/cpu_plan.py run I15 tests/llvm_perf/shift_round_candidates.json --out /absolute/path/to/shift-round-results
python3 tools/cpu_plan.py run I11 tests/llvm_perf/half_candidates.json --out /absolute/path/to/half-results
```

It compares the actual emitted lane-helper path with integer-vector code at O0/O2. Both use the same target machine
and helper library. The kernels use fixed raw inputs and preserve a checksum of the result and FPSR. They measure
instruction throughput, not game frame rate, helper bodies compiled at a game's O0 setting, or a cold table's cost.
The table and larger exceptional sequence increase code size, so faster execution is not a claim of smaller modules.
LLVM may lower a table lookup into scalar loads on some CPUs at O0. Report the actual host and assembly along with
the timing instead of assuming the IR gather guarantees one hardware gather on every AVX2 CPU.

## Compiler diagnostics

The setting `ir_stats` (`--ir_stats=1` on `-r`, or the game's settings for the JIT) enables per-unit static counts in a non-quiet AOT recompile log. These count IR before LLVM's
optimization pipeline, not dynamic execution. Synchronization stores include returns, with the call-site subset
reported separately. Reloads count copies after calls, excluding entry loads. O0 skips promotion, so zero promoted
copies does not mean zero CPU structure accesses. Diagnostic output does not change generated code or its ABI.

The setting `ir_dump_pc` (a hex address) prints the selected function's leaders, flag origins, promoted slots and IR before and
after promotion to stderr. The address must be the start of a discovered function. Dumps can contain game code and
stay local. The whole-module test checks that the CLI prints counts. The helper-effects test independently checks
four FP calls, three stores totaling 32 bytes, zero stores at calls and zero reloads in its small fixture.

```sh
build/scarletnx -r /local/game/input --ir_stats=1
build/scarletnx -r /local/game/input --ir_dump_pc=100000
```
