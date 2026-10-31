<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# First CPU milestone: one measured SIMD improvement

Profile lane-helper cost in a repeatable game scene, replace one hot instruction group with exact vector code, and
measure that scene again. Deliver one bounded change with evidence. The [CPU implementation plan](CPU_TRANSLATOR_PLAN.md)
is the supporting backlog. This is the active scope, refined on 2026-10-30.

Use one available host for the first measured result. Preserve other hosts through the existing exact path and check
lowering for affected targets. Record which hosts actually ran. The full tooling program is not a prerequisite.
Tiered compilation stays deferred, standalone PLT binding stays retired, and SDK replacement follows the hook system.

## Performance hypothesis

An illustrative CPU-limited scenario gives about **5 to 18 percent higher throughput** if affected work occupies
10 to 20 percent of baseline elapsed time and becomes 2 to 4 times faster. These are unmeasured assumptions. Use
`S = 1 / ((1 - p) + p / s)` and the main plan's [scenario table](CPU_TRANSLATOR_PLAN.md#simd-performance-scenarios).
At 40 percent affected work, the same assumptions give 25 to 43 percent. This is not an expected result or a Rosetta
benchmark. A profile must establish the affected fraction, and repeated timings must establish the actual speedup.

Report the prediction and measurements separately. GPU limits, parallel execution and frame caps can hide a CPU gain.
Percentages here describe throughput, not time saved. Removing four calls does not prove a 4-times improvement.

## 1. Find the cost

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

## 2. Check LLVM before changing the emitter

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

## 3. Replace one group exactly

Apply Rosetta's documented SIMD-to-SIMD principle: translate the guest vector operation into a short host-vector
sequence. Prefer whole-vector LLVM IR, with proven lane masks and selects for guest semantic differences. Apple FP
hardware modes cannot be assumed on another host.

Use the main plan's [Rosetta evidence and adaptations](CPU_TRANSLATOR_PLAN.md#i15-whole-vector-operations-instead-of-serial-lane-helpers)
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

## 4. Prove results and measure the same scene

Add focused tests under `tests/` and a kernel in `tests/llvm_bench.c` where needed. Run the QEMU differential test,
including `--sweep` with its required runner arguments, plus directed cases. Compare full result bits and relevant
flags for ordinary lanes, each position of one exceptional lane, multiple exceptional lanes, relevant FPCR modes and
initial sticky FPSR states. Include short consuming instruction sequences where needed to expose dependencies.
Verify affected big-endian lane order, or retain its old path until tested. Run affected module tests and the relevant
suite. Do not hide a new mismatch in `tests/llvm_qemu_expected.txt`.

Recompile guest modules for each emitter version. Time the focused benchmark and the same game scene with the same
hook configuration, settings and cache preparation. Keep diagnostic counters out of timed builds unless their cost
is equivalent. Do not build during timing. Follow the [implementer guide](CPU_TRANSLATOR_GUIDE.md) and applicable run
procedures.

Use at least five measured runs per version, alternate their order, and report individual results with median and
spread. Include frame-time median and tail, plus CPU samples or time where available. Record GPU limits, frame caps
or pacing that could hide a CPU gain. A hidden or inconclusive game result does not establish a frame-rate gain.

## Acceptance and handoff

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
