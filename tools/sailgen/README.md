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
  `datasize`, the immediates, `DecodeBitMasks`, the `if`s and `match`es on fields, the width a `forall 'N` takes from
  an argument of `bits('N)` (FPAbs's `op['N - 2 .. 0]`). They are C in the output; bits wider than 64 (a SIMD&FP
  register's `Zeros(128)`) are a map of constant bits instead.
- **DYN**, when the guest runs: what reads a register, the flags among them. They are calls of the back end that
  build IR.

A mutable local takes the latest time any of its assignments has; the walk repeats until no local's time changes.
A function whose arguments are all STATIC and that reaches no state becomes a C function of its own
(`DecodeBitMasks`, `DecodeShift`). An `if` on a DYN condition becomes a select of what each branch assigns, and its
branches may not change state; a `BranchTo` in one is taken under that condition (B.cond's `if ConditionHolds(cond)`,
CBZ's `if IsZero(operand1) == iszero`). A throw of `Error_Undefined`, an `EndOfInstruction()` (a hint whose feature
the guest lacks: a NOP) and a throw of `Error_See` end the decode function, so they may not be under a DYN condition.
A value of a Sail struct (a load's `AccessDescriptor`) is STATIC: a C struct of its fields (`sgt_NAME`, written before
the helpers), which a field's assignment and read reach as C. Anything outside this part of Sail stops sailgen with the
file, the line and why.

A SIMD instruction's element loop, `foreach (e from 0 to elements - 1)` reading and setting element `e` of its operands
(`Elem_read`, `Elem_set`), is one vector operation, as the hand-written emitters build it: the body is walked once
with `e` standing for every lane, so each value it makes per element is a **LANE** value, a vector of one per lane
(`llvm.h`'s `SgL`), and an operation on lanes is that operation on the vector (`sg_l_add`, `sg_l_cmp`, a select for an
`if` per lane). Integers per lane keep their signedness and widen as an exact result needs, and the slice back to
bits narrows them, which LLVM folds to the element's own width. A register the instruction read whole becomes its
lanes as one load, and lanes written to a register one store (`ir_vec_read`, `ir_vec_write`). The body may only set
locals per lane: one that sets state (a register, an action), carries a local from one element to the next (REV's
container loop), holds another loop (MUL's, which PMUL's polynomial product shares), reads an element other than `e`,
uses a value per lane where one is wanted, or leaves the loop with an element's value rather than the vector `Elem_set`
built, is walked again as the loop it is, every element its own IR, which `tests/llvm_sail_ir.c` shows as longer than
the hand-written emitter's (`sailgen_lanes` holds both outcomes).

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
| `LSL`, `LSR`, `ASR`, `ROR`, `Extend`, `ZeroExtend`, `SignExtend`, `Replicate`, `Zeros`, `Ones`, `IsZero`, `IsZeroBit`, `Bit`, `EOR`, `MOD`, `DIV`, `Min`, `Max`, `HighestSetBit`, `Slice`, `Elem_set` | the map, or C when the value is STATIC |
| `IsFeatureImplemented(FEAT_X)` | the guest's feature bit (`SNX_A64_FEAT_X` of `a64.profile`); false, a CONST, for a feature `--profile` names nowhere |
| `EndOfInstruction()` | the end of the instruction: nothing more is emitted |
| `BranchTo(target, type, conditional)`, `BranchNotTaken(type, conditional)` | the branch's operand for the recompiler's flow (`ir_codegen.c`): the target, and the run-time condition it is under; their profiling, branch records and address tagging do nothing at the NX's EL0 |

A function with no Sail definition that the back end defines as `sg_NAME` is lowered by it: one of the machine's
actions, or a function whose Sail `a64.roots` leaves out with `lower`, keeping its `val` (the floating-point
arithmetic, which works on real numbers). `sg_NAME` takes the context, then the run-time condition the call is under
when its next parameter is named `path` (NULL for none: FCCMP's `FPCompare` runs only where its condition holds; a
call under one of a function that takes none stops sailgen), then the call's arguments unless it takes nothing else
(bits as the back end's maps whatever their time, the rest as they come). Its result is its `val`'s: none, bits or a
bool (nx.sail's guest memory: `GuestMemRead`, the exclusives', the atomics'), an enum known when it recompiles
(`FPRoundingMode`'s is the back end's own "the mode FPCR holds when the guest runs") or a tuple of bits, its parts
through pointers after the arguments. `llvm.h` gives the hints (`Hint_Yield` the host's spin-wait hint, `Hint_WFE`
the runtime's `rt_wfe`, `SendEvent` its `rt_sev`), the barriers (`DataMemoryBarrier` and `DataSynchronizationBarrier`
a fence by the accesses they order, `InstructionSynchronizationBarrier` a full one) and the exclusive monitor
(`ClearExclusiveLocal`), each with why, and those that do nothing here (`Hint_WFI`, `SynchronizeErrors`, `Hint_DGH`).

The floating-point functions (`FPAdd`, `FPSub`, `FPMul`, `FPDiv`, `FPMulAdd`, `FPSqrt`, `FPMax`, `FPMin`, `FPMaxNum`,
`FPMinNum`, `FPCompare`, `FPRoundInt`, `FPRoundIntN`, `FPConvert`, `FixedToFP`, `FPToFixed`, `FPToFixedJS`,
`FPConvertBF`) are lowered as the recompiler's scalar emitters made them: the host's IEEE arithmetic behind a guard that
proves it gives the console's result and FPSR flags for the FPCR the Sail hands them (round to nearest, the operands
FZ would flush, a result that can't underflow; IXC exact where cheap, else only once it is set), and cpu.h's
soft-float helpers otherwise, which raise their FPSR flags as `FPProcessException` does. `FPAbs`, `FPNeg`,
`VFPExpandImm`, `FPZero` and the registers' moves stay the Sail's.

## The state

The registers are the Sail's: sail-arm's `PSTATE` (its `struct ProcState`), and `nx.sail`'s `GPR`, the array ScarletNX's
CPU keeps X0 to X30 and SP_EL0 in, which `get_R`, `set_R`, `SP_read` and `SP_set` index where sail-arm picks one of 31
registers by a match; its `_V`, the 32 SIMD&FP registers of 128 bits that `V_read` and `V_set` index where sail-arm
keeps SVE's `_Z`, and `FPCR`. A read or a write of a register `NAME` is a call of the back end's `sg_NAME_read` or
`sg_NAME_write`: of the whole value for a register of bits or a bool, of an element for a vector of them (`GPR[n]`; `sg_NAME_read_bits` for `NAME[n][hi .. lo]` where the back end
defines it, so a scalar's lane is all it loads), of
a field for a struct (`PSTATE.N`, which the back end numbers `SG_PSTATE_N`); consecutive fields assigned together
(`PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V = nzcv`) are one write of the first and how many. A call of an overload
(`_R`) is the member that takes that many arguments. sailgen checks that the back end defines each name it calls and
stops at the line that reaches one it does not. The pc is the one register sailgen knows itself: the instruction's. A
top-level `let` named for a register's field holds that field fixed (`PSTATE_EL`, `PSTATE.EL`): a read of the field is
the `let`'s value, and a write to it stops sailgen.

`llvm.h` keeps them where the CPU does (`cpu.h`: `x[32]`, the flags `n`, `z`, `cf` and `vf`, each the i32 0 or 1, `v[32]`
and `fpcr`). `_V` goes through `ir.h`'s vector accessors and their per-block cache of each register's low lanes, every
emitter's; a write stores both doublewords but one that holds the value the register already does (Vpart_set's).
Each write is stored at once, and the values are cached for the basic block in `ir->state`, with what the four flags
came from. That cache is every emitter's: its functions over the IR builder are also `ir.h`'s accessors (`ir_x`,
`ir_set_x`, `ir_flag_read`, `ir_flags_commit`, `ir_flags_origin` and the invalidations, which `ir_sail.c` gives) for the
emitters still written by hand, so a block that mixes the two loads a register once and reads a write back as the value
written, and the alias tags and the promotion of registers (`ir_promote.c`) see one kind of load and store. When the
four flags are written from `AddWithCarry(x, NOT(y), '1')` they keep the compare `x - y` as their origin, from
`AddWithCarry(x, y, '0')` the sum, and from `[r[N-1]] @ IsZeroBit(r) @ 0b00` the logical result `r`; `ConditionHolds`
then reads a condition on them as one `icmp` of the operands, by the laws of `sg_origin_cond`, which `ir_state.c`'s
`ir_flags_cond` uses too for B.cond and the other emitters written by hand. Flags written alone (CFINV, RMIF, SETF8) are
stored with the others at the end of the instruction. FCCMP's four come from both ways of its condition, the compare
where it holds and the immediate where it does not: each is a phi of the two, stored at the end of each way, as the
emitter written by hand stored them, and cached as the phi (`sg_flags_on_each_way`).

Guest memory is nx.sail's actions (above), on which its `Mem_read`, `Mem_set`, `MemAtomic` and the exclusive monitor's
functions are written: `llvm.h` makes them with `ir.h`'s accessors, as the emitters written by hand did: a plain access
`ir_rd` and `ir_wr` (CPU.mem plus the address, unaligned, volatile with `volatile_memory`; a pair of doublewords two),
an acquire load or a release store a sequentially consistent host atomic (`ir_ldacq`, `ir_strel`), the exclusives
`cpu.h`'s value-based monitor through its helpers (the load-exclusive's values kept for the exclusive reads after it,
the store-exclusive's status for `ExclusiveMonitorsStatus`), and the LSE atomics an `atomicrmw` or a `cmpxchg` of the
access size (a compare-and-swap loop where the host's byte order or PowerPC64's partword compares need one,
`snx_cas128` for CASP's 16 bytes). An instruction's two word accesses of adjacent addresses (LDPSW's loads, STNP's
stores, two in their Sail) become one 8-byte access as it ends (`sg_accesses_combine`), as a pair of words always was.

## Another back end

The front (the parse, the inlining, the times) and the Sail don't depend on LLVM. Another back end is another
`llvm.h`, with the same functions over its own values, keeping the registers its own way behind the same
`sg_NAME_read` and `sg_NAME_write`, and the guest's memory behind the same actions.

## Tests

`tests/sailgen/`: `--extract` of a small file copies the closure of its root and nothing else, and of a function it
lowers the `val` and the types it names alone; `--generate` refuses a throw it has no meaning for, an `Error_See` under a
run-time condition, a write to a field a `let` holds fixed, a register or a field the back end does not keep, the back
end's function that takes no `path` under a run-time condition and one whose `val` gives an integer, naming its line;
`sailgen_a64` generates the in-tree A64 subsets whole.

`tests/llvm_sail_ir.c` holds the generated emitters to the hand-written ones they replaced
(`tests/support/ir_int_reference.c`, `tests/support/ir_fpu_reference.c`), word by word on three paths: the same
optimized IR, or the same instructions in another order, or another IR no longer than theirs; and the same results
when both run, the floating point from FPCRs and operands chosen for its paths.
