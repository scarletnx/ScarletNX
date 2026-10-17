<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Cortex-A57 / AArch64 divergences

## Index

* [Verdict and scope](#verdict-and-scope)
* [Public PDF references and profile selection](#public-pdf-references-and-profile-selection)
  * [Not available on the base Cortex-A57](#not-available-on-the-base-cortex-a57)
* [Corrections made in this follow-up](#corrections-made-in-this-follow-up)
* [Remaining base-instruction failures](#remaining-base-instruction-failures)
  * [FP arithmetic, fused operations and square root](#fp-arithmetic-fused-operations-and-square-root)
  * [FP/integer and FP-format conversions](#fpinteger-and-fp-format-conversions)
  * [Estimates and refinement](#estimates-and-refinement)
* [System and memory-model limitations](#system-and-memory-model-limitations)
* [Evidence and reproduction](#evidence-and-reproduction)

## Verdict and scope

**ScarletNX is not a fully conforming Cortex-A57 CPU implementation.** The
application recompiler implements most A64 instruction families, but remaining
FP arithmetic/conversion/estimate failures and the intentionally virtualized
machine/system behavior prevent that claim. Passing samples are not a proof
for every operand or encoding.

This audit targets the **base Cortex-A57 A64 ISA**, not later ARMv8 revisions,
AArch32 execution, or a complete system emulator. It follows the earlier
[rounding/min/max/comparison repair](../tests/a64_fp.md). The shallow-clone
starting revision was `e7c4210`; both sets of changes are cumulative.

The instruction ledger, `docs/A57_INSTRUCTION_AUDIT.tsv` as
[regenerated](../tests/a64_audit.md#instruction-ledger) (not checked in), indexes all **575 instruction
descriptions** in the selected ARM ARM, including aliases and separately
specified forms. Evidence in that ledger is explicitly **mnemonic-family level**:
it does not imply that every addressing mode, lane arrangement, condition,
immediate or register combination in a description was executed. System
operations and unsupported machine operations have separate statuses.

## Public PDF references and profile selection

Primary architecture reference:

- **ARM DDI0487A.k**, ARM Architecture Reference Manual, ARMv8, for ARMv8-A
  architecture profile, 2016, non-confidential, 5,732 PDF pages.
- [Official PDF](https://documentation-service.arm.com/static/5f8ee92bf86e16515cdbe531?token=)
- SHA-256: `4c01cf306ad32433742c0450691dd93f12bf0d0dd4dd97eabfa848c510aa1e8b`
- C4: allocation; C5: system instructions; C6.2: base instructions;
  C7.2: SIMD/FP instructions; shared pseudocode supplies FP and memory rules.
- C2.2.1 is important: parenthesized encoding bits are SBZ/SBO constraints,
  **not** unconditional fixed-bit allocation rules. Violating them can permit
  UNDEFINED, NOP, execution with the prescribed bit, or UNKNOWN destinations.

Implementation reference:

- **ARM DDI0488H**, Cortex-A57 MPCore Processor Technical Reference Manual,
  577 PDF pages.
- [Official PDF](https://documentation-service.arm.com/static/5e906b9fc8052b1608760b6b?token=)
- SHA-256: `30c47dac07e432b706e1d938dbab9668a1cab461bce2340007a612df1042e8e5`
- Section 3.1: features and optional Cryptography Extension.
- Table 4-34, `ID_AA64ISAR0_EL1`: **CRC32 is implemented in AArch64 on A57**.
- Tables 14-3/14-4: FPCR/FPSR fields and RES0 bits.

An older DDI0487A.a beta was used for the initial investigation; the ledger and
final architectural references use the released A.k edition above. PDFs are
not redistributed with this patch.

### Not available on the base Cortex-A57

- **Optional Cryptography Extension:** AESE, AESD, AESMC, AESIMC; SHA1C,
  SHA1M, SHA1P, SHA1H, SHA1SU0, SHA1SU1; SHA256H, SHA256H2, SHA256SU0,
  SHA256SU1; and the **64-bit-element** PMULL/PMULL2 forms. ScarletNX implements
  these and does not currently feature-gate them. They are outside the requested
  base profile, not missing base instructions. A strict base-only CPU mode would
  have to reject them. The **8-bit polynomial** PMULL/PMULL2 forms are base SIMD
  and must not be disabled with the optional crypto forms.
- **FP16 arithmetic/general-register FP16 moves:** not A57. The four general
  `FMOV W/X,H` / `FMOV H,W/X` encodings previously accepted here are now rejected.
  This does **not** remove the base half/single/double **format conversions**, nor
  integer operations and bitwise moves on 16-bit vector elements.
- **ARMv8.1 Limited Ordering Regions:** LDLAR/STLLR and their byte/halfword
  forms are not A57. The decoder previously treated their words as ordinary
  acquire/release loads/stores; it now rejects them.
- LSE atomics (CAS/SWP/LDADD families), RDM, RCpc loads, dot-product extensions,
  FRINT32Z/FRINT32X/FRINT64Z/FRINT64X, SHA512/SHA3, pointer authentication,
  SVE and SME are not base A57 requirements. Selected later-ISA probes are in
  the harness; this is not an exhaustive catalogue of later encodings.
- **FP exception-enable traps and FZ16 are not implemented by A57.** Their FPCR
  bits are RES0, so absent FP trapping is not a missing base-A57 feature.

A later instruction assigned to the backward-compatible HINT space can legally
behave as a hint/NOP on A57. Do not infer implementation of its later feature
merely from execution without an undefined-instruction exception.

## Corrections made in this follow-up

1. **Load/store allocation:** reject SIMD unprivileged imm9 forms, invalid
   pre/post-indexed or unprivileged prefetch forms, and non-temporal LDPSW.
   See ARM ARM C6.2.82, C6.2.97-102 and C6.2.143, together with C4 allocation.
2. **SIMD structure allocation:** reject nonzero Rm in no-offset structure
   loads/stores, and bit 21 in multiple-structure forms. See C7.2.152/153 and
   C7.2.275/276. `0c427020` and `0c607020` each failed both emitters before the
   fix. Register writeback and V31-to-V0 register-list wraparound are also tested.
3. **FP/SIMD allocation:** reject the later general FP16 FMOV forms
   (C7.2.114 explicitly makes type `11` unallocated), invalid scalar FCVTL,
   URECPE/URSQRTE, AdvSIMD FSQRT, and three-register scalar ADDP encodings.
4. **Exclusive allocation:** reject LDLAR/STLLR on this profile. SBO violations
   in unused exclusive-instruction fields now choose UNDEFINED, an outcome
   permitted by C2.2.1. Previously ignoring those SBO fields was **not by itself
   an architectural violation**; it is distinct from accepting later LOR words.
5. **Defined integer C arithmetic:** negative ADR/ADRP offsets no longer
   left-shift a negative signed C value. SIMD wrapping ABS/NEG and SADDW/SSUBW,
   widening shifts, 64-bit saturation bounds, and doubled signed products avoid
   signed overflow/invalid shifts. In particular, `2 * INT32_MIN * INT32_MIN`
   needs a wider intermediate before saturation. See C7.2.206, .247-.250 and
   .274. These are portability/UB corrections, not a claim that all old runtime
   builds produced incorrect integer results; production also uses `-fwrapv`.
6. **A57 FP state:** MSR FPCR retains only `0x07c00000`; MSR FPSR retains only
   `0xf800009f`. This follows the TRM, rather than the more permissive Unicorn
   register implementation. Direct specification witnesses test readback.

`SNX_ABI` is **9** (8 moved the dispatch miss to `RtAPI.miss`, and 9 joins it with main's emitter changes). Rebuild the runtime and regenerate game modules;
stale AOT code must not retain the old decoder/operation semantics.

## Remaining base-instruction failures

These are open defects, not expected-success waivers. The audit exits nonzero
when it finds them. Both generated AOT steps and the generated interpreter are
checked; neither is used as the reference for the other.

### FP arithmetic, fused operations and square root

Affected families include FADD/FSUB/FMUL/FDIV, FMADD/FMSUB/FNMADD/FNMSUB,
vector/by-element FMLA/FMLS, FADDP and FSQRT. Not every form fails every test.

- Native host operations do not reliably implement ARM's NaN sign, payload,
  signalling priority, default-NaN handling or fused-operation operand priority.
- Host exception status is not consistently accumulated into guest FPSR.
  Missing IOC/DZC/OFC/UFC/IXC/IDC cannot be dismissed when result bits happen
  to match.
- Some paths widen single-precision operands to host double before inspecting
  NaNs, potentially quieting an SNaN too early.
- FMA behavior depends on the host FMA path versus the software fallback.
  Directed rounding and extreme intermediate values are not certified.

Concrete captured examples with FPCR=0 and initial FPSR=0:

- `1e221820`, `FDIV S0,S1,S2`, S1=`00000000`, S2=`80000000`:
  native result `ffc00000`, FPSR=0; reference `7fc00000`, FPSR.IOC=1.
- `1f020c20`, `FMADD S0,S1,S2,S3`, S1=`ffffffff`, S2=S3=`7fffffff`:
  the tested native-FMA build selected `ffffffff`; ARM's addend-first NaN
  processing yields `7fffffff`.
- `1e222820`, `FADD S0,S1,S2`, S1=`55555555`, S2=`00000008`:
  result bits match, but FPSR.IXC is missing.

### FP/integer and FP-format conversions

FCVT[NAMPZ][SU], SCVTF/UCVTF, their fixed-point forms, vector conversion/narrowing
forms and half/single/double FCVT still have counterexamples. Legacy helpers
lack CPU state, so they cannot correctly implement all required input flushing,
rounding, invalid/inexact reporting, DN and AHP behavior. The earlier FRINT
repair does not automatically repair conversions using other helpers.

Examples:

- `1e200020`, `FCVTNS W0,S1`, S1=`ffffffff`: integer result zero agrees,
  but invalid conversion must set FPSR.IOC; ScarletNX leaves it clear.
- `1e220020`, `SCVTF S0,W1`, W1=`7fffffff`: rounded result agrees, but
  FPSR.IXC is missing.
- `1e22c020`, `FCVT D0,S1`, S1=`7f800456`: SNaN conversion loses IOC.
- Half-format helpers assume IEEE half rather than honoring all FPCR.AHP and
  directed-rounding behavior. Half **conversion** is a base-A57 obligation.

### Estimates and refinement

FRECPE, FRSQRTE, FRECPX, FRECPS and FRSQRTS require further correction of NaNs,
exceptions, flushing and extreme-value behavior. For example, reciprocal
estimate of zero must report divide-by-zero, not merely return infinity.
The dedicated `5ea1d820` FRECPE control sweep reports 2,176 differing executions
out of 3,072 AOT/interpreter comparisons.

A suitable next implementation step is a common raw-bit/stateful FP backend
shared by both emitters, not per-game fixes or masking differing status bits.

## System and memory-model limitations

These are source-reviewed boundaries, not certified by single-instruction
Unicorn comparisons:

- **Exclusive monitors:** the runtime stores address/size/loaded values and
  uses CAS. Ordinary stores do not participate in a full coherent reservation
  model. Intervening writes, ABA and reservation-granule invalidation across
  PEs are not faithfully modeled. Passing uncontended success/failure tests
  does not prove architectural exclusivity.
- **Ordering/barriers:** the current implementation relies on x86-64 TSO,
  compiler barriers for some DMB/DSB forms, and stronger host atomics/fences for
  others. No multi-thread litmus or device-memory ordering proof was performed;
  this must not be generalized to other hosts.
- **Memory/access faults:** flat translated memory is not an A57 MMU. Access
  permissions, memory types, translation, SP/exclusive alignment checks and
  architectural fault delivery are not comprehensively modeled or tested.
  LDTR/STTR cannot be certified for their privilege-sensitive access behavior.
- **System instructions:** SVC/BRK and scheduling hints use runtime/HLE policy;
  cache/TLB maintenance and SYS operations are partial proxies or no-ops.
  DC ZVA has a 64-byte zeroing helper, but full permissions/fault behavior is not
  established. Cache maintenance/ISB does not prove self-modifying AOT code is
  correctly invalidated and regenerated.
- **System registers:** only a small subset is modeled. Unknown MRS reads
  return zero and unknown MSR writes are ignored rather than applying the
  architectural access/exception rules. IDs, counters and topology are partly
  synthesized; their values are not evidence of a complete A57 configuration.
- **Machine execution:** exception levels, security state, debug, interrupt
  delivery, ERET/DRPS/HVC/SMC/DCPS/HLT/SYSL and AArch32 are not a complete
  processor implementation. Some of these instructions legitimately trap at
  EL0; an undefined callback is not a model of the whole exception mechanism.

## Evidence and reproduction

See [the harness guide](../tests/a64_audit.md) for exact commands and limitations.
`--report` writes a run's output: each word's family, check and mismatch counts
and first counterexample, and the totals. No summary of the runs is checked in,
and there is no known-failure suppression list.

The extended pre-fix portable corpus found **39,153 differences / 704,096
comparisons**, across 708 words. Decoder corrections removed the 67 failing
allocation words in that corpus. Two further structure-encoding defects were
then reproduced separately and corrected. The expanded final corpus includes
additional structure modes, aliases, bitfields and exclusive success/failure
states, so its totals are not a like-for-like subtraction from that baseline.

A scalar FP/control sweep (`--match '^(FP|FCVT)' --states 1536 --aliases`) found
**374,112 differences / 795,612 comparisons** over 1,776 words. It repeats the
edge patterns under all RMode/FZ/DN/AHP combinations. It does not claim equivalent
control coverage for every vector arithmetic instruction.

The final broad native run covered **23,104 words / 821,426 comparisons**, with
**39,441 differences across 641 words**. The strict-C sanitizer run completed
without UB traps and reported 39,375 FP differences across the same 641 words.
Alignment instrumentation was disabled.

The separate exclusive subset passed **5,048 comparisons / 80 words**, including
primed and unprimed reservations. Specification witnesses and `native_codegen`
also passed, including a fresh packaging-time rerun. The completed follow-up
FP-specific regression (`--random 8 --aliases --sanitize`) recorded **23,219,328
comparisons / 331 cases, zero failures**. Its log was recovered after the backend
outage. This passing subset does not cancel the broader audit's open defects.
All observed executed-word differences in the broad corpus were FP-related;
this is a measured result for that corpus, not an integer/memory certification.

Unicorn 2.1.4 is explicitly configured as Cortex-A57, but it is a QEMU-derived
software oracle, **not physical A57 hardware**. It enables optional crypto and
allows FPCR bits that this TRM makes RES0. Capstone supplies diagnostic names,
not allocation truth. PDF rules and independent fixed witnesses therefore take
precedence, and constrained-unpredictable choices must be reviewed rather than
blindly equated with one oracle outcome.

No game assets, gameplay/scene validation, physical-A57 measurements or full
runtime build were part of this work. Only the selected build targets, emitted
instruction execution, specification witnesses and documented audits were run.
