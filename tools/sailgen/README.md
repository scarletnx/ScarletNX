<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# sailgen

Generates the recompiler's A64 instruction emitters from the Sail of the Arm architecture: sail-arm's translation of
Arm's ASL, kept in `src/common/cpu/aarch64/sail/` (its README says where from and how to update it). Strict C23 with
no dependency, built and run during the build as `tools/decodetree` is (`cmake/sailgen.cmake`).

```sh
sailgen --extract ROOTS -o OUT INPUT...
sailgen --generate ROOTS --backend llvm.h [--profile a64.profile] -o OUT INPUT... [--overlay FILE]...
```

`--extract` copies the definitions of sail-arm's files that the roots reach (`extract.c`). `--generate` writes one C
function per decode function the roots name, which emits that instruction as LLVM IR, and `sg_emit`, which picks one
by the decode clauses' fixed bits in their order (`gen.c`) and gives a branch's operand: whether it is taken (an i1)
and where to (an i64). A clause whose decode function throws `Error_See` hands the word on to the next clause that
matches it, as Sail's decode does (the hint space's PAuth hints); one an earlier clause covers whole and never hands
on is never reached. An overlay's definitions take the place of the inputs' of the same name: a system's fixups
(`nx.sail`). With `--profile`, a feature the decoder's profile names in none of its `feature` lines is one no guest
has. `src/common/cpu/recompiler/llvm/ir_sail.c` includes the output.

## How a decode function becomes C

Each decode function is walked with everything it calls inlined, and each value gets the time it is known at:

- **CONST**, when sailgen runs: literals, an overlay's top-level `let`, a feature no guest has, and what only they
  make. An `if` on one keeps one branch, so a fixup such as `let SPESampleInFlight : bool = false` leaves no trace. An
  integer past 62 bits (WFE's timeout, `1 << 64`) may be bound and passed on, never computed with.
- **STATIC**, when the recompiler runs: the word's fields, the pc, the guest's features, and all that only they make:
  `datasize`, the immediates, `DecodeBitMasks`, the `if`s and `match`es on fields. They are C in the output.
- **DYN**, when the guest runs: what reads a register, the flags among them. They are calls of the back end that
  build IR.

A mutable local takes the latest time any of its assignments has; the walk repeats until no local's time changes.
A function whose arguments are all STATIC and that reaches no state becomes a C function of its own
(`DecodeBitMasks`, `DecodeShift`). An `if` on a DYN condition becomes a select of what each branch assigns, and its
branches may not change state; a `BranchTo` in one is taken under that condition (B.cond's `if ConditionHolds(cond)`,
CBZ's `if IsZero(operand1) == iszero`). A throw of `Error_Undefined`, an `EndOfInstruction()` (a hint whose feature
the guest lacks: a NOP) and a throw of `Error_See` end the decode function, so they may not be under a DYN condition.
Anything outside this part of Sail stops sailgen with the file, the line and why.

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
| `IsFeatureImplemented(FEAT_X)` | the guest's feature bit (`SNX_A64_FEAT_X` of `a64.profile`); false, a CONST, for a feature `--profile` names nowhere |
| `EndOfInstruction()` | the end of the instruction: nothing more is emitted |
| `BranchTo(target, type, conditional)`, `BranchNotTaken(type, conditional)` | the branch's operand for the recompiler's flow (`ir_codegen.c`): the target, and the run-time condition it is under; their profiling, branch records and address tagging do nothing at the NX's EL0 |

A function with no Sail definition that the back end defines as `sg_NAME` is one of the machine's actions: a
statement, given the call's arguments (DYN ones as the back end's values, the others as C integers), or the context
alone when `sg_NAME` takes nothing else. `llvm.h` gives the hints (`Hint_Yield` the host's spin-wait hint, `Hint_WFE`
the runtime's `rt_wfe`, `SendEvent` its `rt_sev`), the barriers (`DataMemoryBarrier` and `DataSynchronizationBarrier`
a fence by the accesses they order, `InstructionSynchronizationBarrier` a full one) and the exclusive monitor
(`ClearExclusiveLocal`), each with why, and those that do nothing here (`Hint_WFI`, `SynchronizeErrors`, `Hint_DGH`).

## The state

The registers are the Sail's: sail-arm's `PSTATE` (its `struct ProcState`), and `nx.sail`'s `GPR`, the array ScarletNX's
CPU keeps X0 to X30 and SP_EL0 in, which `get_R`, `set_R`, `SP_read` and `SP_set` index where sail-arm picks one of 31
registers by a match. A read or a write of a register `NAME` is a call of the back end's `sg_NAME_read` or
`sg_NAME_write`: of the whole value for a register of bits or a bool, of an element for a vector of them (`GPR[n]`), of
a field for a struct (`PSTATE.N`, which the back end numbers `SG_PSTATE_N`); consecutive fields assigned together
(`PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V = nzcv`) are one write of the first and how many. A call of an overload
(`_R`) is the member that takes that many arguments. sailgen checks that the back end defines each name it calls and
stops at the line that reaches one it does not. The pc is the one register sailgen knows itself: the instruction's. A
top-level `let` named for a register's field holds that field fixed (`PSTATE_EL`, `PSTATE.EL`): a read of the field is
the `let`'s value, and a write to it stops sailgen.

`llvm.h` keeps them where the CPU does (`cpu.h`: `x[32]`, and the flags `n`, `z`, `cf` and `vf`, each the i32 0 or 1).
Each write is stored at once, and the values are cached for the basic block in `ir->state`, with what the four flags
came from. That cache is every emitter's: its functions over the IR builder are also `ir.h`'s accessors (`ir_x`,
`ir_set_x`, `ir_flag_read`, `ir_flags_commit`, `ir_flags_origin` and the invalidations, which `ir_sail.c` gives) for the
emitters still written by hand, so a block that mixes the two loads a register once and reads a write back as the value
written, and the alias tags and the promotion of registers (`ir_promote.c`) see one kind of load and store. When the
four flags are written from `AddWithCarry(x, NOT(y), '1')` they keep the compare `x - y` as their origin, from
`AddWithCarry(x, y, '0')` the sum, and from `[r[N-1]] @ IsZeroBit(r) @ 0b00` the logical result `r`; `ConditionHolds`
then reads a condition on them as one `icmp` of the operands, by the laws of `sg_origin_cond`, which `ir_state.c`'s
`ir_flags_cond` uses too for B.cond and the other emitters written by hand. Flags written alone (CFINV, RMIF, SETF8) are
stored with the others at the end of the instruction.

## Another back end

The front (the parse, the inlining, the times) and the Sail don't depend on LLVM. Another back end is another
`llvm.h`, with the same functions over its own values, keeping the registers its own way behind the same
`sg_NAME_read` and `sg_NAME_write`.

## Tests

`tests/sailgen/`: `--extract` of a small file copies the closure of its root and nothing else, and `--generate`
refuses a throw it has no meaning for, an `Error_See` under a run-time condition, a write to a field a `let` holds
fixed and a register or a field the back end does not keep, naming its line;
`sailgen_a64` generates the in-tree A64 subsets whole.

`tests/llvm_sail_ir.c` holds the generated emitters to the hand-written ones they replaced
(`tests/support/ir_int_reference.c`), word by word on three paths: the same optimized IR, or the same instructions in
another order, or another IR no longer than theirs; and the same results when both run.
