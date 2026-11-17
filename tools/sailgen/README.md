<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# sailgen

Generates the recompiler's A64 instruction emitters from the Sail of the Arm architecture: sail-arm's translation of
Arm's ASL, kept in `src/common/cpu/aarch64/sail/` (its README says where from and how to update it). Strict C99 with
no dependency, built and run during the build as `tools/decodetree` is (`cmake/sailgen.cmake`).

```sh
sailgen --extract ROOTS -o OUT INPUT...
sailgen --generate ROOTS --backend llvm.h -o OUT INPUT... [--overlay FILE]...
```

`--extract` copies the definitions of sail-arm's files that the roots reach (`extract.c`). `--generate` writes one C
function per decode function the roots name, which emits that instruction as LLVM IR, and `sg_emit`, which picks one
by the decode clauses' fixed bits in their order (`gen.c`). An overlay's definitions take the place of the inputs' of
the same name: a system's fixups (`nx.sail`).

## How a decode function becomes C

Each decode function is walked with everything it calls inlined, and each value gets the time it is known at:

- **CONST**, when sailgen runs: literals, an overlay's top-level `let`, and what only they make. An `if` on one keeps
  one branch, so a fixup such as `let SPESampleInFlight : bool = false` leaves no trace.
- **STATIC**, when the recompiler runs: the word's fields, the pc, the guest's features, and all that only they make:
  `datasize`, the immediates, `DecodeBitMasks`, the `if`s and `match`es on fields. They are C in the output.
- **DYN**, when the guest runs: what reads a register or a flag. They are calls of the back end that build IR.

A mutable local takes the latest time any of its assignments has; the walk repeats until no local's time changes.
A function whose arguments are all STATIC and that reaches no state becomes a C function of its own
(`DecodeBitMasks`, `DecodeShift`). An `if` on a DYN condition becomes a select of what each branch assigns, and its
branches may not change state. Anything outside this part of Sail stops sailgen with the file, the line and why.

## The back end (`llvm.h`)

`llvm.h` is copied at the top of the output. It holds the lowering to LLVM of Sail's builtins and of the shared
functions sailgen does not inline, and the bit map: a DYN bits value is, until something computes with it, which
bit of which source each of its bits is. Slices, concatenations, extensions, constant shifts and rotations, masks
and `Replicate` cost no IR, and `sg_d_make` builds the cheapest IR for the map: a field (`lshr`, `and`, `shl`), a
sign extended field (`ashr` of `shl`), a rotation or a funnel shift (`fshr`), a byte or bit reversal (`bswap`,
`bitreverse`), or the OR of each source's bits that move by one distance. That is what turns the Sail's bitfield,
extract, reverse and move-wide (masks, `ROR`, loops over bits) into the IR the hand-written emitters made.

Builtins, lowered by the back end rather than from Sail:

| Sail | lowered as |
|---|---|
| `UInt`, `SInt`, `asl_Int`, `+ - *` on integers | an integer of the width it needs (an add one bit more than its operands), which slicing truncates; LLVM narrows it |
| `AddWithCarry(x, NOT(y), '1')`, `(x, y, '0')`, with a carry flag | `x - y`, `x + y`, `x + y + c`, the NZCV formulas of `ir_state.c`, and the flags' origin (below) |
| `ConditionHolds(cond)` | the compare the flags came from, when one made them in this block; else its own Sail |
| `RoundTowardsZero(Real(a) / Real(b))` | `udiv`/`sdiv` with the divisor 1 where it is 0 or -1 under the most negative value (the caller's select gives 0 for the first) |
| `CountLeadingZeroBits`, `CountLeadingSignBits` | `llvm.ctlz` |
| `BitReverse(Poly32Mod2(BitReverse(acc) @ Zeros(n) EOR BitReverse(val) @ Zeros(32), poly))` | the reflected CRC update: SSE4.2's CRC32C, AArch64's CRC32, or the sliced tables |
| `LSL`, `LSR`, `ASR`, `ROR`, `Extend`, `ZeroExtend`, `SignExtend`, `Replicate`, `Zeros`, `Ones`, `IsZero`, `IsZeroBit`, `Bit`, `EOR`, `MOD`, `DIV`, `Min`, `Max`, `HighestSetBit`, `Slice` | the map, or C when the value is STATIC |
| `IsFeatureImplemented(FEAT_X)` | the guest's feature bit (`SNX_A64_FEAT_X` of `a64.profile`) |

## The state model

The generated code reaches the CPU only through the functions every emitter uses (`ir.h`): `ir_x` and `ir_set_x`
for `_R(n)` and `SP_EL0` (X31), `ir_flag_read` for PSTATE's N, Z, C and V, `ir_flags_commit` and `ir_flags_origin`
to write them, `ir_undef` for a throw. So the per-block caches of registers and flags, the alias tags and the
promotion of registers (`ir_promote.c`) treat its accesses as the hand-written emitters' ones. When NZCV is written
from `AddWithCarry(x, NOT(y), '1')` the origin is the compare `x - y`, from `AddWithCarry(x, y, '0')` the sum, and
from `[r[N-1]] @ IsZeroBit(r) @ 0b00` the logical result `r`; `ConditionHolds` then reads a condition on them as one
`icmp` of the operands, as `ir_state.c`'s `ir_flags_cond` does for the emitters still written by hand.

## Another back end

The front (the parse, the inlining, the times) and the Sail don't depend on LLVM. Another back end is another
`llvm.h`, with the same functions over its own values, and its own state model.

## Tests

`tests/sailgen/`: `--extract` of a small file copies the closure of its root and nothing else, and `--generate`
refuses a throw it has no meaning for, naming its line; `sailgen_a64` generates the in-tree A64 subsets whole.
