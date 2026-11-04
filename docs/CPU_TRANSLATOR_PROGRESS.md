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

I8 emits scalar and vector float32/float64 FRECPE and FRSQRTE on x86 hosts with AVX2, including exceptional lanes.
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
the measured regression gate. Vector O0/O2 and scalar O2 residuals passed that gate. Half conversions retain helpers
for AHP, NaNs, underflow boundaries, unsupported rounding modes and direct float64-to-half conversion.

Guarded FABD reuses exact subtraction followed by sign clearing. Directed tests cover all shift counts, every
half encoding, estimate bins/exponents, mixed exceptional lanes, aliases, rounding modes and FPSR. They run through
O0/O2, AOT/JIT loaders and native/portable paths. The executed host is x86-64; these checks do not establish native
execution on other hosts. Native AArch64 FPCR/FPSR integration (I12) remains a separate project. Exact scalar
arithmetic remains inside batched fallbacks where no vector replacement is proven.

T3 now checks all 19 CLI statistics fields against hand counts for two complete integer modules. Each fixture runs
both in one unit and split at function boundaries. The CLI checks also require function leaders, flag origins,
promoted registers and IR before and after promotion.

T5 draws safe data operations from 81 families of the generated decoder. It varies conditional branches, bounded
loop counts, direct and indirect calls, tail calls, memory order and aligned exclusive retry loops. It runs through both loaders and optimization levels, plus portable, hook and volatile configurations.
A 20-configuration seed matrix checks 10,240 complete program states. Two self-tests must detect planted flag-origin
and exclusive-update defects. This remains a bounded instruction
grammar, not exhaustive sequence coverage. Native execution has been checked on x86-64.

T6 corrects the stale documentation and source comments listed in the plan, preserving historical audit examples
as historical evidence rather than current failure claims. Other items require their own implementation and proof.

## SIMD continuation

The 2026-11-04 continuation adds exact whole-vector comparisons. Ordinary single/double comparisons retain the
short floating-point path. Exceptional lanes use integer classification and ordering, including signaling NaNs,
flushed denormals and signed zero. Half comparisons use integer vectors for all lanes. The directed test covers
80 instruction forms, destination aliases and every half encoding across modes, loaders and optimization levels.

A tested extension that batched conversion and rounding fallbacks was rejected. It reduced outer calls but made
portable conversion kernels about 15% slower, and hid repeated values from LLVM's common-call elimination.
These paths retain their pure scalar helpers. The sequence test checks ordinary, DUP and constant-zero producers
before conversion, including full register state. Narrowing broadcasts and constant-zero forms must retain one
shared scalar call. Existing arithmetic batches keep their descriptor and exact fallback behavior.

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
