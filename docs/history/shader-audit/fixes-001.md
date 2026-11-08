<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors -->
# Shader corrections, batch 001

## Index

* [Corrections](#corrections)
* [Annotation errata discovered during implementation](#annotation-errata-discovered-during-implementation)
* [Executed evidence](#executed-evidence)
* [Deliberately unresolved](#deliberately-unresolved)
* [Reference source and licensing](#reference-source-and-licensing)

This is a bounded corrective patch, not an implementation of every finding in
`instructions.md`. Eden is authoritative for this batch.

- ScarletNX baseline: `9a3ff6c762ad72551ae8fbde4d2e0a9aa2b2f1c8`
- Eden reference: `6352612425ae0765802eaa8c5904ff652b97222a`

The diff is independent of the earlier NRO/HLE patches and the annotation-only
patch. No existing NRO/HLE changes are included or reverted.

## Corrections

- Decode ATOMS.CAS, CCTLL, P2R immediate, SUATOM, SUATOM.CAS and CCTLT with the
  reference masks. Recognize unsupported STP separately so broadening ATOMS.CAS
  cannot turn STP into an atomic result write. This does not implement atomics.
- Snapshot combining predicates for FSETP, ISETP and CSETP. Compute both PSETP
  results before writing either destination, with the second result negating
  input A before the first boolean operation.
- HSETP2 snapshots both lane results. H_AND combines the lane results after each
  has consumed the combining predicate, then complements the combined result
  for the second output.
- FSET, ISET, PSET, CSET, BFI, LOP and I2I now reset C/O on their supported CC
  paths. LOP derives flags from the computed value even when Rd is RZ.
  Arithmetic and other instruction families retain their separate flag rules.
- I2I uses the three-bit selector, 32-bit ABS/NEG, reference signed/unsigned
  saturation bounds, and zero extension after non-saturating narrowing even for
  signed destinations. Invalid or unsafe sizes/selectors receive an unsupported
  diagnostic rather than an invented conversion.
- MOV accepts only masks 1 and 15. Other masks retain the old destination,
  matching Eden's intentional stub. MOV32I uses its distinct mask field at
  bits 12..15, not the ordinary MOV field at 39..42.
- SHF uses the full immediate operand, clamps shifts at 32/63, and wraps with
  masks 31/62. Mask 62 is deliberate reference behavior. Unsupported CC/X/size
  modes receive diagnostics and do not write a fabricated value.
- BFI retains the base when offset is at least 32 and updates its CC outputs.
- P2R immediate no longer truncates the clear mask to eight bits.
- F2I reads ABS at bit 45 and NEG at bit 49.
- Dual-vertex merging preserves EXIT's flow condition as well as its predicate
  when rewriting it into a branch to the second program.

## Annotation errata discovered during implementation

The historical ledger's MOV32I claim that no local data difference was found is
incorrect for masks other than 1/15. Its PSETP claim that the two-stage boolean
equation already matched is also incorrect. Eden's second result is
`((!A) BOP1 B) BOP2 C`, not `(!(A BOP1 B)) BOP2 C`. Both corrections are tested
here. The earlier annotation package remains a historical source review, not a
certificate that every semantic difference was found.

## Executed evidence

This section records a historical audit of the removed text translator. Its differential harness was retired with
that translator; the figures below describe the old implementation.

- Built ScarletNX's production shader C sources and selected unmodified Eden
  visitors/IR sources with Clang 21.1.5 and UBSan.
- Executed 20,856 synthetic state vectors over 2,329 distinct encodings across
  41 instruction variants. The original ScarletNX sources disagree with the
  reference-IR CPU executor on 15,471 vectors. The patched sources disagree on
  zero vectors. A difference can concern any register, predicate or flag.
- Enumerated 65,536 decoder prefixes. Eden's native decoder confirmed the
  table-selected result for all 48,084 known prefixes. The corrected entries
  and STP guard pass their positive and negative ScarletNX checks. This does
  not certify the other decoder families.
- Executed 496 dual-vertex encoding-preservation checks: 480 fail before the
  patch and zero fail after it.
- Passed all 2,329 distinct tested encodings through the complete
  `shader_to_glsl()` entry point in one synthetic compute program. glslang
  15.2.0 compiled it for Vulkan 1.1 and SPIRV-Tools 2025.1 validated the result.
- The existing tools-only native build also passed 13/13 CTests. That build
  includes the previously delivered NRO/HLE work. It is ancillary regression
  evidence, not a shader differential test or a full game/runtime build.

The semantic comparison executes actual frontend outputs through two bounded
CPU adapters. It does not execute Eden's backend or either shader on a GPU.
The GLSL/SPIR-V check is compilation and validation only. No game was run.

## Deliberately unresolved

Most of the original ledger remains open. In particular, this patch does not
fix IADD/IADD3/ISCADD, extended comparisons, all condition flags, general FP
rounding/FTZ, textures, resource tags, memory, atomics, barriers, subgroup
semantics, stage coverage or general CFG/call/return behavior. VOTE's predicate
aliasing is not fixed by the predicate helper change.

BFI still clamps count to the remaining width. Eden can emit an out-of-range
bitfield operation when count is at most 32 but exceeds the remaining width.
Those cases have no successful backend oracle here and are excluded from the
semantic claim. Invalid I2I selectors and rejected SHF modes use ScarletNX's
existing diagnostic mechanism, which is not equivalent to Eden throwing an
exception. Full rejection/error policy remains unresolved.

F2I testing covers finite F32-to-S32 values and all ABS/NEG/rounding combinations
in reg/cbuf forms. NaN conversion results, 64-bit formats, FTZ and CC remain
open. The shared field correction also reaches ScarletNX's immediate extension,
which Eden rejects and which is not reference-validated. P2R reg/cbuf extensions
remain unvalidated. MOV/MOV32I resource-tag invalidation is still incomplete.

## Reference source and licensing

All paths below are under Eden's
`src/shader_recompiler/frontend/maxwell/` at the pinned revision:

- `maxwell.inc` for decoder masks and precedence.
- `translate/impl/{predicate_set_predicate,condition_code_set,floating_point_set_predicate,integer_set_predicate,half_floating_point_set_predicate}.cpp`
  for predicate equations and ordering.
- `translate/impl/{floating_point_compare_and_set,integer_compare_and_set,predicate_set_register,logic_operation,integer_to_integer_conversion,bitfield_insert}.cpp`
  for flags and integer results.
- `translate/impl/{move_register,move_predicate_to_register,integer_funnel_shift,floating_point_conversion_integer}.cpp`
  for masks, shifts and conversion modifiers.
- `control_flow.cpp` for EXIT flow tests.

The referenced visitor files and opcode table credit the 2021 yuzu Emulator
Project under GPL-2.0-or-later. Eden support files also retain their Eden
copyright and GPL-3.0-or-later notices. They are compiled from the user's
unchanged checkout, not copied into this patch. ScarletNX's GPL-3.0-or-later
notices are preserved.
