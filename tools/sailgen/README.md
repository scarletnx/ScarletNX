<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# sailgen

Generates the recompiler's A64 and AArch32 instruction emitters from the Sail of the Arm architecture: sail-arm's
translation of Arm's ASL, kept in `src/common/cpu/aarch64/sail/` and `src/common/cpu/arm/sail/` (their READMEs say
where from and how to update them). Strict C23 with no dependency, built and run during the build as
`tools/decodetree` is (`cmake/sailgen.cmake`).

```sh
sailgen --extract ROOTS -o OUT INPUT...
sailgen --generate ROOTS --backend core.h --backend llvm.h [--profile PROFILE] -o OUT INPUT... [--overlay FILE]...
```

`--extract` copies the definitions of sail-arm's files that the roots reach (`extract.c`). `--generate` writes one C
function per decode function the roots name, which emits that instruction as LLVM IR, and a dispatcher that picks one
by the decode clauses' fixed bits in their order (`gen.c`): `sg_decode` for every clause, or, for each `decoder NAME
FAMILY...` line of the roots, NAME for the clauses of those scattered decode functions alone (AArch32's
`sg_decode_t16`, the `__DecodeT16` clauses of a T16 halfword's 16 bits, apart from A32's and T32's words). A clause
whose decode function throws `Error_See` hands the word on to the next clause that matches it, as Sail's decode does
(the hint space's PAuth hints), and so does one whose guard tests the word beyond its pattern and fails (B T1's
`cond != 0b1111`, read for a roots file with decoder lines; A64's one, PRFM register's, would hand RPRFM's words to a
clause that is no root and stays unread); one an earlier clause covers whole and never hands on is never reached. An
overlay's definitions take the place of the inputs' of the same name: a system's fixups (`nx.sail`, `arm.sail`). With
`--profile`, a feature the profile names in none of its `feature` lines is one no guest has, and a root its `since
VERSION ROOT...` lines name (`a32.profile`) is tried only where the back end's `sg_ArchVersion` is that version or
later. The back end's files are copied at the top of the output in their order: `core.h`, then a machine's
(`llvm.h` for A64, which the back end's `sg_emit` wraps the dispatcher with and
`src/common/cpu/recompiler/llvm/ir_sail.c` includes; `llvm_arm.h` for AArch32, its `sg_emit_t16` and `sg_emit_a32`,
which `src/common/cpu/arm/llvm_thumb.inc` includes).

## How a decode function becomes C

Each decode function is walked with everything it calls inlined, and each value gets the time it is known at:

- **CONST**, when sailgen runs: literals, an overlay's top-level `let`, a feature no guest has, the bits of the word
  that the decode clause walked fixes, and what only they make. An `if` on one keeps one branch, so a fixup such as
  `let SPESampleInFlight : bool = false` leaves no trace, and MUL's decode, whose clause fixes U at 0, never reaches
  PMUL's polynomial product. Bits known here are compared here (`==`, `!=`) and are STATIC C everywhere else. An
  integer past 62 bits (WFE's timeout, `1 << 64`) may be bound and passed on, never computed with.
- **STATIC**, when the recompiler runs: the word's fields, the pc, the guest's features, and all that only they make:
  `datasize`, the immediates, `DecodeBitMasks`, the `if`s and `match`es on fields, the width a `forall 'N` takes from
  an argument of `bits('N)` (FPAbs's `op['N - 2 .. 0]`). They are C in the output; bits wider than 64 (a SIMD&FP
  register's `Zeros(128)`) are a map of constant bits instead.
- **DYN**, when the guest runs: what reads a register, the flags among them. They are calls of the back end that
  build IR.

A mutable local takes the latest time any of its assignments has; the walk repeats until no local's time changes.
sailgen also keeps what it knows of a value it does not: the width of bits a slice or a literal makes, and so the range
of `UInt` and `SInt` of them, so a test of a field against a number it can't hold folds (`t == 15` of a T16 load's
three-bit Rt, which keeps LoadWritePC's run-time alignment check out of it).
A function whose arguments are all STATIC and that reaches no state becomes a C function of its own
(`DecodeBitMasks`, `DecodeShift`); its locals are its own. An `if` on a DYN condition becomes a select of what each
branch assigns, and its branches may not change state; a `BranchTo` in one is taken under that condition (B.cond's `if
ConditionHolds(cond)`, CBZ's `if IsZero(operand1) == iszero`). The one exception is a decode function that is all
`if C then { ... }`, every AArch32 one's `if ConditionPassed()`: where the back end defines `sg_when`, the rest of the
instruction runs under C (`sg_when(g, c)` branches past it where C fails), as the AArch32 block compilers always ran a
conditional instruction. A throw of `Error_Undefined`, an `EndOfInstruction()` (a hint whose feature the guest lacks:
a NOP) and a throw of `Error_See` end the decode function, so they may not be under a DYN condition. A throw of
`Error_Unpredictable` (an AArch32 encoding the architecture leaves UNPREDICTABLE) walks the overlay's
`UnpredictableEncoding()` in its place, what the system's cores do with one (`arm.sail`'s runs it as it is written);
without one sailgen stops at the throw.
A value of a Sail struct (a load's `AccessDescriptor`) is STATIC: a C struct of its fields (`sgt_NAME`, written before
the helpers), which a field's assignment and read reach as C. Anything outside this part of Sail stops sailgen with the
file, the line and why.

A SIMD instruction's element loop, `foreach (e from 0 to elements - 1)` reading and setting element `e` of its operands
(`Elem_read`, `Elem_set`), is one vector operation, as the hand-written emitters build it: the body is walked once
with `e` standing for every lane, so each value it makes per element is a **LANE** value, a vector of one per lane
(`llvm.h`'s `SgL`), and an operation on lanes is that operation on the vector (`sg_l_add`, `sg_l_cmp`, a select for an
`if` per lane). Integers per lane keep their signedness and widen as an exact result needs, and the slice back to
bits narrows them, which LLVM folds to the element's own width. A register the instruction read whole becomes its
lanes as one load, and lanes written to a register one store (`ir_vec_read`, `ir_vec_write`).

An element other than `e` of a `let` that concatenates two values, `Elem_read(concat, 2 * e + 1, esize)` of the pairwise
ops' `operand2 @ operand1`, is a shuffle of the two values' lanes (`sg_l_permute`) whose mask is that index for each
lane, made when the recompiler runs; the concatenation, wider than the back end's 128 bits, is never made whole. A loop
from 1 that folds each element into a local, `sum = sum + x` or `maxmin = Max(maxmin, x)`, the local declared as `x`
for `e` 0 (the same tree with 0 for `e`, every other name bound before it to a value nothing sets), is that fold of
every lane from 0: `llvm.vector.reduce` of `x`'s lanes, an integer's sum exact at the narrowest power of two of bits
that holds it, the result one lane (`sg_l_reduce_add`, `sg_l_reduce_minmax`).

A function that calls itself on bits of the guest's is read in one shape, ADDV's `Reduce__1`: it gives its bits where
they are one element wide (`if 'N == esize then return(input[esize - 1 .. 0])`), and elsewhere calls itself on each
half of them (`input['N - 1 .. half]` and `input[half - 1 .. 0]`, `half` being `DIV('N, 2)`, every other argument
passed on as it came) and gives the two results' `+`. That is the sum of the bits' elements, whatever the order (`+` of
bits is associative and commutative), so it is the same `llvm.vector.reduce.add` of the lanes, found when sailgen
runs: the shape is read on the Sail's tree, and what joins the two results is walked as any Sail, each result a value
that only their `+` may take; a `match` there on a value known when sailgen runs keeps the case it picks, so
`ReduceOp_ADD` picks the `+` and the other ops' cases are not walked. Such a function that joins them any other way (a
floating-point op, whose result depends on the order), gives other than that sum, or changes state where it joins them
is refused, and so is any other function that calls itself there (`sailgen_halves`, `sailgen_refused_halves_*`).

The body may only set locals per lane: one that sets state (a register, an action), carries a local from one element to
the next (REV's container loop), holds another loop (PMUL's polynomial product), reads an element other than `e` but
of a concatenation, uses a value per lane where one is wanted, folds into a local other than as above, sets an element
in a loop from 1, or leaves the loop with an element's value rather than the vector `Elem_set` built, is walked again
as the loop it is, every element its own IR, which `tests/llvm_sail_ir.c` shows as longer than the hand-written
emitter's (`sailgen_lanes` holds each outcome).

## The back end (`core.h`, then `llvm.h` or `llvm_arm.h`)

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
| `LSL_C`, `LSR_C`, `ASR_C`, `ROR_C` (AArch32's shifter and its carry) | the map for an amount known when it recompiles; for a register's, 0 to 255, the AArch32 block compiler's `shift` (every LLVM shift kept below the width) |
| `BitCount`, `LowestSetBit`, `Align` | C of a register list or a pc; `Align` of bits known only at run time the map |
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

## AArch32 (`llvm_arm.h`)

The AArch32 block compiler (`src/common/cpu/arm/llvm.c`) builds its IR with its own builder (`ArmIR`), which the file
sailgen writes is compiled with (`llvm_thumb.inc` includes it), so `llvm_arm.h` builds on its helpers as `llvm.h` does
on `ir.h`'s: `GPR[n]` is a load or a store of `SnxArmCPU.r[n]`, PSTATE's N, Z, C, V, T and E are CPSR's bits, written
together in one store of CPSR as the instruction ends (`sg_flags_flush`, as `nz` wrote them), guest memory is `readmem`
and `writemem` after `transfer_begin` (flat, checked or hosted, with the ARM7TDMI's and the ARM946E-S's bus rules), a
multiple transfer's whole range checked first (`GuestTransfer`), and UNDEFINED, SVC and BKPT the block's exits
(`trap`). What the block compiler decides when it compiles a block the back end answers as values the recompiler
knows: a back end function whose result is `SgS`, `int` or `long long` gives a STATIC value (bits, a bool or an enum,
an integer), not IR: `CurrentInstrSet`, `ITSTATE`, `AArch32_CurrentCond` (the instruction's condition: an A32 word's, a
B<c>'s own, else the IT block's) and `ArchVersion`. `LSL_C`, `LSR_C`, `ASR_C` and `ROR_C`, AArch32's shifter with its
carry, are the map for an amount known when it recompiles, and `llvm.c`'s `shift` for a register's (0 to 255);
`BitCount`, `LowestSetBit` and `Align` are C of what the recompiler knows, or the map. `sg_emit_t16` runs a T16
halfword and `sg_emit_a32` an A32 word, each under the condition its decode's `ConditionPassed()` gives (`sg_when`);
`sg_emit_a32` hands back a word none of its clauses takes, which llvm.c's emitters run. `CurrentInstrSet` is the
block's until an instruction selects the other (`SetInstrSet`: BLX's target is aligned as the set it goes to).

## Another back end

The front (the parse, the inlining, the times) and the Sail don't depend on LLVM. Another back end is another
`llvm.h`, with the same functions over its own values, keeping the registers its own way behind the same
`sg_NAME_read` and `sg_NAME_write`, and the guest's memory behind the same actions. `core.h` holds what every machine's
shares (the bits, the map, the integers, AddWithCarry's flags) and asks the machine's file for its builder, its LLVM
context and its intrinsics (`sg_builder`, `sg_llvm_context`, `sg_intrinsic`, `sg_select_phi`).

## Tests

`tests/sailgen/`: `--extract` of a small file copies the closure of its root and nothing else, and of a function it
lowers the `val` and the types it names alone; `--generate` refuses a throw it has no meaning for, an `Error_See` under a
run-time condition, an `Error_Unpredictable` with no overlay's `UnpredictableEncoding()`, a write to a field a `let`
holds fixed, a register or a field the back end does not keep, the back end's function that takes no `path` under a
run-time condition, one whose `val` gives an integer and a function that calls itself other than as the sum of its
bits' elements, naming its line; `sailgen_lanes` says which element loops become lanes and `sailgen_halves` that such
a function is one reduction; `sailgen_a64` generates the in-tree A64 subsets whole, and `sailgen_a32` the AArch32
ones.

`tests/llvm_sail_ir.c` holds the generated emitters to the hand-written ones they replaced
(`tests/support/ir_int_reference.c`, `tests/support/ir_fpu_reference.c`), word by word on three paths: the same
optimized IR, or the same instructions in another order, or another IR no longer than theirs; and the same results
when both run, the floating point from FPCRs and operands chosen for its paths. `tests/arm_sail_ir.c` does the same for
AArch32's T16 halfwords against the emitters written by hand (`snx_thumb_llvm_block_t16_reference`), on every profile
and memory mode, alone and in an IT block.
