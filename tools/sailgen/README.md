<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# sailgen

Generates the recompiler's A64 and AArch32 instruction emitters from the Sail of the Arm architecture: sail-arm's
translation of Arm's ASL, kept in `src/common/cpu/aarch64/sail/` and `src/common/cpu/arm/sail/` (their READMEs say
where from and how to update them). Strict C23 with no dependency, built and run during the build as
`tools/decodetree` is (`cmake/sailgen.cmake`).

```sh
sailgen --extract ROOTS -o OUT INPUT...
sailgen --generate ROOTS --backend bits.h --backend FILE... [--profile PROFILE] -o OUT INPUT...
        [--overlay FILE]...
```

`--extract` copies the definitions of sail-arm's files that the roots reach (`extract.c`). `--generate` writes one C
function per decode function the roots name, which emits that instruction as a machine's code, and a dispatcher that picks one
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
later. The back end's files are copied at the top of the output in their order: `bits.h` (the values known while it
recompiles, which every back end shares), then a machine's, as `snx_sailgen_machine_backends` in `cmake/sailgen.cmake`
lists them ([The x86-64 back end](#the-x86-64-back-end), [the RISC back ends](#the-risc-back-ends-rv64h-ppc64h-mips64h-la64h-s390xh-over-risch),
[the arm64 back end](#the-arm64-back-end-arm64h-arm64_asmh-arm64_a64h)).

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

A function that calls itself on bits of the guest's is read in one shape, ADDV's `Reduce__1`: it gives its bits where
they are one element wide (`if 'N == esize then return(input[esize - 1 .. 0])`), and elsewhere calls itself on each
half of them (`input['N - 1 .. half]` and `input[half - 1 .. 0]`, `half` being `DIV('N, 2)`, every other argument
passed on as it came) and gives the two results' `+`. That is the sum of the bits' elements, whatever the order (`+` of
bits is associative and commutative), the back end's `sg_d_reduce_add`, found when sailgen runs: the shape is read on
the Sail's tree, and what joins the two results is walked as any Sail, each result a value
that only their `+` may take; a `match` there on a value known when sailgen runs keeps the case it picks, so
`ReduceOp_ADD` picks the `+` and the other ops' cases are not walked. Such a function that gives other than that sum,
or changes state where it joins them, is refused, and so is any other function that calls itself there
(`sailgen_refused_halves_*`).

Every element loop is the loop it is, and so are the shapes that only
run element by element: an integer of the guest's shifted by an amount it or the recompiler knows, negated, made its
magnitude, or the smaller or larger of two, bits plus such an integer, `BitCount` of bits of the guest's, an element
whose number only the guest knows (TBL's), and the two names of `(y @ x) = v` set at once. A function that calls itself
on each half of its bits and joins them otherwise than by `+` (the floating-point reductions) is the C loop that joins
each two elements next to each other until one is left, as its calls would (`sailgen_halves` holds both). A bit field of a register of bits set
(`FPSR[QC] = 0b1`) is the back end's `sg_REG_field_write(g, path, SG_REG_FIELD, value)`, under the run-time condition
the code is under.

## The back end's files

`bits.h` holds the values known while the code is made, which every machine shares, and the bit map: a DYN bits value
is, until something computes with it, which bit of which source each of its bits is, so slices, concatenations,
extensions, constant shifts and rotations, masks and `Replicate` cost nothing until a value is needed. A machine's
files lower Sail's builtins (`UInt`, `AddWithCarry`, `ConditionHolds`, the divisions, `CountLeadingZeroBits`, the CRC
update, the shifts and the rest), the shared functions sailgen does not inline and the guest's state, through
`mach.h`, which every machine's code shares, and the machine's own assembly (sections below).

A function with no Sail definition that the back end defines as `sg_NAME` is lowered by it: one of the machine's
actions, or a function whose Sail `a64.roots` leaves out with `lower`, keeping its `val` (the floating-point
arithmetic, which works on real numbers). `sg_NAME` takes the context, then the run-time condition the call is under
when its next parameter is named `path` (NULL for none: FCCMP's `FPCompare` runs only where its condition holds; a
call under one of a function that takes none stops sailgen), then the call's arguments unless it takes nothing else
(bits as the back end's maps whatever their time, the rest as they come). Its result is its `val`'s: none, bits or a
bool (nx.sail's guest memory: `GuestMemRead`, the exclusives', the atomics'), an enum known when it recompiles
(`FPRoundingMode`'s is the back end's own "the mode FPCR holds when the guest runs") or a tuple of bits, its parts
through pointers after the arguments (bits, or a bool). The back end gives the hints (`Hint_Yield` the host's spin-wait
hint, `Hint_WFE`
the runtime's `rt_wfe`, `SendEvent` its `rt_sev`), the barriers (`DataMemoryBarrier` and `DataSynchronizationBarrier`
a fence by the accesses they order, `InstructionSynchronizationBarrier` a full one) and the exclusive monitor
(`ClearExclusiveLocal`), each with why, and those that do nothing here (`Hint_WFI`, `SynchronizeErrors`, `Hint_DGH`).

The floating-point functions (`FPAdd`, `FPSub`, `FPMul`, `FPDiv`, `FPMulAdd`, `FPSqrt`, `FPMax`, `FPMin`, `FPMaxNum`,
`FPMinNum`, `FPCompare`, `FPRoundInt`, `FPRoundIntN`, `FPConvert`, `FixedToFP`, `FPToFixed`, `FPToFixedJS`,
`FPConvertBF`) are lowered as the host's IEEE arithmetic behind a guard that
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

A machine's back end keeps them where the CPU does (`cpu.h`: `x[32]`, the flags `n`, `z`, `cf` and `vf`, `v[32]` and
`fpcr`), the ones a function uses most held in the host's registers ([At run time](#at-run-time-x64h-machh-x64_asmh-x64_a64h)).
Guest memory is nx.sail's actions (above), on which its `Mem_read`, `Mem_set`, `MemAtomic` and the exclusive monitor's
functions are written: the host's accesses at CPU.mem plus the address, an acquire load or a release store a
sequentially consistent host atomic, the exclusives `cpu.h`'s value-based monitor, and CASP's 16 bytes `snx_cas128`.

## Another back end

The front (the parse, the inlining, the times) and the Sail don't depend on a machine. Another back end is another
machine's files in `snx_sailgen_machine_backends`, with the same functions over its own values, keeping the registers
its own way behind the same `sg_NAME_read` and `sg_NAME_write`, and the guest's memory behind the same actions.

## Tests

`tests/sailgen/`: `--extract` of a small file copies the closure of its root and nothing else, and of a function it
lowers the `val` and the types it names alone; `--generate` refuses a throw it has no meaning for, an `Error_See` under a
run-time condition, an `Error_Unpredictable` with no overlay's `UnpredictableEncoding()`, a write to a field a `let`
holds fixed, a register or a field the back end does not keep, the back end's function that takes no `path` under a
run-time condition, one whose `val` gives an integer and a function that calls itself other than as the sum of its
bits' elements, naming its line, all with the x86-64 back end's files; `sailgen_halves` says what such a function
becomes on it; `sailgen_a64` generates the whole of `a64.roots`, and `sailgen_roots_subset` holds the back ends' A64
roots (`a64_x64.roots`, `a64_risc.roots`) to it.

## GPU mode (`--gpu`)

```sh
sailgen --gpu ROOTS --backend spirv.h --backend ISA_spirv.h [--backend ISA_FAMILY_spirv.h]... -o OUT INPUT...
```

The shader translators' instruction emitters, from Sail written for a GPU's ISA (`src/nx/gpu/shader/sail/`): a mode of
its own, `gpu.c`, which shares the parse with the CPUs' generator and nothing else. No CPU specification is read in it,
and none of the GPU's in `--generate`: `gen.c`'s decode clauses, PSTATE, element loops and machine back ends are the
CPUs', `gpu.c`'s roots, times and SPIR-V back end the GPUs'. Every instruction of SM86 (`sm86.sail`) and of SM53
(`sm53.sail` and its families' `sm53_<family>.sail`) is emitted so: `sm86_translate.c`'s `emit_insn` and
`spirv_translate.c`'s to_spirv run the generated dispatcher, and refuse an opcode no root takes. So is every
instruction of the 3DS's PICA200 (`src/3ds/gpu/sail/pica.sail`, which `vk/vsgen.c`'s trans_ functions run once they
have given the control flow analysis each instruction's step) and of the Vita's USSE (`src/vita/gpu/sail/usse.sail`
and its families': `usse.c`'s trans_ functions run a pattern's root inside its predicate's if). Each ISA's Sail is
in a folder of its own (`snx_sailgen_gpu`'s `DIR`), `gpu.sail` the one they share.

The decoders stay `tools/decodetree`'s (`sm53.decode`, `sm86.decode`, `pica.decode`, `usse.decode`): the Sail says
what each decoded instruction computes. The roots file names them:

- `prefix NAME`: the generated functions' prefix (`sg86`, `sg53`);
- `include FILE`: another roots file beside this one (a family's);
- `static FN...`: the back end's functions whose values the translator knows (an instruction's fields, whether an
  operand is the zero register): C values, made once where the Sail asks for them, which the Sail's `if`s and
  `match`es on them become;
- `op FN OPCODE...`: `execute_FN`, a Sail function of no parameters, runs for each of the decoder's opcodes; the
  opcode's place in the line is handed to the back end (`sg_set_form`), which an instruction of several forms (SM53's
  `_R`, `_C`, `_I`) reads as `Form()`;
- `fuse FN OPCODE...`: the fusion `FN` of those opcodes in a row, where its guard `FN_ok` holds.

Each value is known when the translator runs (STATIC: fields, immediates, what only they make) or when the program
runs (DYN: a value of the back end, `SgV`). The Sail's functions are inlined; an `if` or a `match` on a STATIC value is
C, the locals the arms set merged after it; an `if` on a DYN value runs both arms and selects each local they set
differently (no arm may change state there); a `foreach` of STATIC bounds is a C loop whose locals' times settle
before it is written. A function with no Sail body is the back end's `sg_NAME`: its val's `int` and enum parameters
take C values, its `bits` and `bool` ones `SgV`s, a type the Sail does not know (`half2`, `f64`, `i64`, `wordptr`) an
opaque `SgV` that the Sail binds, passes and chooses and nothing else; its result is DYN unless the roots name it
static. `throw(E(...))` calls `sg_throw_E` with its arguments and ends the instruction (the translator's refusal).
Sail's operators on bits and the comparisons of `UInt` and `SInt` are the back end's `sg_add`, `sg_ult`... with the
width; `UInt` and `SInt` of DYN bits are only compared, chosen, passed to `Min`, `Max`, `Abs` or made bits again
(`to_bits`); `DIV`, `MOD` and `^` are of ints known when the translator runs. Statements are written in the Sail's
order, left to right as C's arguments were, and a binary operator's constant left operand is made before its
run-time right one, so a moved instruction makes its constants and instructions in the order it made them.

The output is one C function per root (`PREFIX_FN`), one per fusion and its guard, `PREFIX_emit(g, op)` (the root of
an opcode, 0 for none), `PREFIX_takes(op)`, `PREFIX_fuse(g, ops, avail)` (at the first of `avail` instructions whose
opcodes are `ops`, the first fusion whose opcodes and guard they meet, run: how many it took) and
`PREFIX_fuse_starts(op)`.

**What stays C.** The Sail holds each instruction's logic: its fields, forms, refusals, choices and arithmetic. What
carries a translator's own work is called from the Sail as the hand-written emitter called it, through the back end:
the register caches (SM53's `reg_read`, `reg_read_as`, `reg_copy`), the FTZ and rounding helpers, the constant buffer
windows, the global buffers' binding and write-back marks, the interface variables of the attributes, the CAS loops
of the stores and atomics SPIR-V has not, the textures' and surfaces' binding (SM86's `texture()` and `surface()`,
SM53's `shader_tex_*`), the tensor cores' matrices (`hmma`, `ldsm`), and the long typed conversions (SM53's
`shader_f2f`, `shader_f2i`, `shader_i2f`) and EXIT's end of the program (`shader_exit`).

**Fusion.** A fusion replaces instructions in a row by what they compute together, with every architectural write
they make (each register, predicate and flag, in their order), so that it changes nothing a later instruction or the
program's outputs see. Its guard reads the instructions' fields (`FieldAt(k, ...)`), and must hold only where that
is so: the registers the first instructions write are not ones the later ones still read as they were. The translator
offers only instructions that run as the first does (unguarded) and that no branch lands on. Each fusion is held to
its instructions run one after another by the tests (below), which is what keeps a fusion from changing execution.

**Back ends.** `spirv.h` makes SPIR-V (`common/utilities/spirv_emit.h`): bits of up to 32 a u32, of 33 to 64 a u64,
an f32 or an i32 kept so until an operation wants another type, each operator the one instruction the hand-written
emitters make for it; `gpu.sail`'s vals are its floats, halves, subgroup operations and truth tables, PICA's vec4s
and bvec4s, the integer operations that keep an operand's type (an i32 or a u32: the USSE's words) and the image
samples. Three switches follow the translators: `fold`, whose booleans' operations and selects fold the true and
false constants out (SM53's `cc_and`, `shader_select`), `fresh_casts`, which bitcasts a value each time an
operation takes it as another type (SM53's families and the USSE's) instead of once an instruction (SM86's), and
`known`, which makes the constants with `spv_emit_const_*`, the module's table of what it declared, where the
translator makes its own there (PICA's and the USSE's), so that a constant both make is one. The ISA's files after
it (`sm86_spirv.h`, `sm53_spirv.h` and a family's `sm53_<family>_spirv.h`; `pica_spirv.h`; `usse_spirv.h` and a
family's `usse_<family>_spirv.h`) define `struct SgG` and give the state and the helpers above, which the file
sailgen writes is compiled with. What stays the translator's is what it was for SM53: PICA's registers, its uniform
block's window and its structured control flow, the USSE's operand model (its banks, packed types, conversions and
repeats), its buffers and its textures' samplers and gathers. `eval.h` runs the same Sail on values, with an ISA's state file
(`sm86_eval_state.h`) and the roots of the instructions it can run (`sm86_eval.roots`), and says where an instruction
`spirv.h` would make is undefined in SPIR-V (a shift by the width or more), which the tests fail on.

**Tests.** `tests/shader_sail_sm86.c` runs `sm86.sail`'s integer instructions on values against `sm86_eval` (the CPU's
run of the integer ALU) on random encodings and states, checks LOP3's and PLOP3's 256 truth tables whole, and each
fusion against its instructions run in turn, both where its guard takes them and near misses where it must not. The
shader tests (`shader_sm86`, `shader_direct`, `shader_sm86_gpu`...) run the generated emitters' modules, the
3DS's draw tests (`3ds_vk_draw*`) PICA's, and `vita_usse_*` the USSE's.

## Encoders (`--encoders`)

```sh
sailgen --encoders __DecodeA64 [--roots ROOTS] --profile a64.profile -o a64_enc.inc a64.sail --overlay nx.sail
```

A back end that writes an architecture's own code takes its encodings from that architecture's Sail, the same
decode clauses the guest's emitters are generated from (`enc.c`). Each clause's pattern fixes some of the word's bits
and its lets slice the others into the decode function's fields (`let Rd = Slice(__opcode, 0, 5)`); its encoder is
the fixed bits ORed with each field in its place, a field the pattern fixes whole left out of its parameters:

```c
/* decode_add_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg */
static inline SnxU32 sge_a64_add_addsub_shift(SnxU32 Rd, SnxU32 Rn, SnxU32 imm6, SnxU32 Rm, SnxU32 shift, SnxU32 sf);
```

named for the decode function, less `decode_` and its `_aarch64_instrs_...` (clauses whose decode functions are named
alike get the shortest tail of the name that tells them apart: `ldr_imm_gen_post_idx`, `ldr_imm_gen_pre_idx`,
`ldr_imm_gen_immediate_unsigned`), and `sge_a64_table` lists every encoder's fixed bits and fields. `a64.roots`'
`encode __DecodeA64` line copies every A64 decode clause into `a64.sail` alone, nothing it names followed, so the
instructions the guest's emitters make by hand or not at all (B, MRS, SVC, the Advanced SIMD) have encoders too
(`--extract`'s `encode NAME`: the clauses of a decode function, or of every scattered function NAME begins).
`tests/sailgen_encoders.c` (`sailgen_encoders`) holds them to words the Arm ARM gives.

Each clause's `fp` in the table says what its code reaches of the floating-point control and status registers, with
the overlays and the profile as `--generate` takes them: 0 nothing, 1 FPCR read or FPSR written (a saturation's QC),
2 FPCR given to a function the back end lowers, or one that reads it by itself (FPRecipStepFused), what its lowering
reads being the back end's. The code is walked as the generator folds it: a field the pattern fixes whole known (SSHL's
S, which leaves its saturation out), a feature the profile names in no feature line false (FEAT_AFP's FPCR.AH in
FABS), an if or a match down its known side, FPCR_read's value followed to where something reads it; every guess is
the one that reaches more. The arm64 back end runs an Advanced SIMD or floating-point word as the host's own
instruction, under the guest's FPCR only where a clause with the word's bits has an `fp` (`arm64_vec.c`);
`tests/arm64_fpenv_code.c` holds that to the Sail's own code of each clause the arm64 Sail makes.

## The x86-64 back end

A back end that writes x86-64 machine code itself: its instruction selection is derived from the
pseudocode of Intel's manual and kept in the tree (`sdm/x64_isel.inc`), so choosing instructions costs the recompiler
nothing and it does little more than write their bytes at run time.

### The SDM's pages (`--sdm`)

```sh
pdftotext -layout 325383-093US.pdf sdm.txt
sailgen --sdm sdm.txt -o sdm.pages [--overlay OVERRIDE.sdm]...
```

The manual (Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2, order number 325383) is not kept
in the tree. `sdm.c` cuts `pdftotext -layout`'s text of it into its
instruction pages, each the manual's opcode table, operand encoding table, Operation section and Flags Affected
section, verbatim apart from the page furniture (running footers, form feeds) and pdftotext's column artefacts (a row
whose operands a footnote's superscript pushed onto the line above, a footnote's mark after an operand). The format
is vibe86's (its `tools/sdm/extract.py`, which this ports, gives the same 806 pages of 093US byte for byte), with a
sixth column for the tables that have one: the CPUID feature flag (`BMI1`, `BMI2`, `LZCNT`). Where the manual is
wrong, or can't be read as written, an override (a page of only the sections it replaces, `# NAME—Title` first)
takes their place, so the extracted pages stay the manual's own words and every fix is a file that says why.
`tests/sailgen/sdm_fixture.txt` is a page written in the manual's layout for the test, `sailgen_sdm`.

### Instruction selection (`--isel`)

```sh
sailgen --isel sdm.pages --ops tools/sailgen/x64.ops -o x64_isel.inc [--source TEXT] [--report x64_isel.report]
```

The back end's primitives (`x64.ops`: `add`, `subs`, `lslv`, `clz`, `csel`... at 32 and 64 bits) are written in the
SDM's own notation, following the Arm ARM's definitions (AddWithCarry, ShiftReg's `UInt(operand2) MOD datasize`,
CountLeadingZeroBits), and `isel.c` reads both them and every general-purpose form of the manual (an opcode table's row
valid in 64-bit mode, of registers, r/m used as a register and immediates) with one parser (`sdm_code.c`, the port of
vibe86's `tools/ucc/sdm.py`) and evaluates both to terms (`term.c`). A form is selected for a primitive where some binding
of the primitive's operands to the form's makes their terms the same: the result in the register the form writes, and
each of Arm's flags the primitive gives one of Intel's flags or its negation. Arm's conditions (`cond` lines) are then
matched with Intel's condition codes, whose predicates are SETcc's rows ('Set byte if above (CF=0 and ZF=0).'), on the
flags each selected flag-setting form leaves. Nothing about any instruction is written in sailgen; what it knows is
the notation, and these, each a sentence of the manuals:

- in 64-bit mode a 32-bit register write zero extends to the whole register, an 8- or 16-bit one leaves the rest
  (Intel Vol. 1, 3.4.1.1);
- an immediate narrower than the operand size is sign extended to it (Vol. 2A, 2.2.1.5);
- the flags 'set according to the result' (Vol. 1, 3.4.3.1, and Appendix A): CF where the unsigned result doesn't fit
  the destination, OF where the signed one doesn't, SF its top bit, ZF whether it is zero, PF the parity of its low
  byte; a Flags Affected sentence it can't read leaves the flags it names undefined, and the others too unless every
  sentence was read (nothing is taken as unaffected that the text doesn't say is);
- `ZeroExtend(x)` and `SignExtend(x)` extend to the operand size; a division or a product is signed where a comment on
  its line says so (SAR's 'Signed divide, rounding toward negative infinity', IMUL's 'Signed multiplication').

**Evaluation.** Each form is evaluated once, with its operands, the general-purpose registers (with their AL, AH, AX,
EAX views), the six status flags and a cc form's condition as variables; an `IF` on a value known then (the operand
size, the mnemonic in 'instruction is SAL or SHL', 64-bit mode) takes one branch, else both, merged by choices. A
`WHILE (n != 0)` whose body counts n down by one and steps each other name it sets by a shift or a rotation of itself by
one (SHL's `tempDEST := tempDEST * 2`) is that shift by n; another loop is unrolled while its condition is decided, and
as nested choices where it is not (LZCNT's), up to 140 steps. A form whose page can't be read so (prose, memory, a
helper the notation doesn't define) is reported with why and not selected.

**Terms.** `term.c` keeps terms hash-consed in a canonical form, so two ways of writing one function are one index:
additions, subtractions, negations and NOT are linear forms modulo 2^w; AND, OR and XOR flat and sorted; extractions
pushed into what they extract from; integers (UInt, SInt and their sums and products) exact, with the range of their
value, so that a comparison of one is decided by its range or written as one canonical `L >= 0`. The identity that makes
the two manuals' flags meet is `UInt(L modulo 2^w) = L` exactly when L is in [0, 2^w) (and SInt's): Arm's AddWithCarry
gives C as `UInt(result) != unsigned_sum`, Intel's CF is the unsigned result not fitting the destination, and both
become one comparison of the operands, which is how the selection finds that SUBS's C is the negation of SUB's CF,
SBCS's carry in the negation of SBB's (`CF=NOT carry`), and HI the condition code A. A pair of terms the identities miss
compares unequal: that costs a selection, never makes a wrong one.

A primitive may declare what the recompiler makes hold (`ASSUME`), so a form that faults is selected where it faults
exactly where that fails (UDIV's DIV: the divisor 0); its outputs are compared where it holds (a choice whose
condition it decides takes that way). Integer division is exact quotient and remainder terms with ranges, so DIV's
'quotient doesn't fit' folds away with RDX zeroed, and a truncated signed quotient is past the signed range exactly
for the most negative value over -1 (an identity, which IDIV's fault becomes). An input may be a function of
another (`hi H:A`, IDIV's RDX: the high half of A's sign extension). Ranges past 128 bits are unknown and decide no
comparison.

**What it selects** (`--report` lists every candidate, and every form it couldn't read): ADD, SUB, AND, OR, XOR,
NOT, NEG, ANDN (BIC), ADC and SBB with their flags, CMP and TEST, IMUL and MUL (MUL, UMULH and SMULH through RDX), SHL,
SHR, SAR and ROR by an immediate and by CL (x86's count mask is Arm's `MOD datasize`), SHLX, SHRX, SARX and RORX, LZCNT
(CLZ), TZCNT, BSWAP (REV), MOVZX and MOVSX(D), MOV of registers and immediates (a 32-bit MOV for a 64-bit constant below
2^32: its zero extension is derived), CMOVcc (CSEL) and SETcc on a zeroed register (CSET), DIV (UDIV) with RDX zeroed and IDIV (SDIV), each with the bits of an
input the form reads (an input the form reads only the low bits of may hold anything above them), whether its result is
zero extended, the immediate it fits (`imm8` sign extended), the flags it writes and Arm's conditions as condition codes.

**Errata.** A selection is only as right as its page: read as written, CDQ's `EDX := SignExtend(EAX)` was taken for a
zero extension into EDX. `tools/sailgen/sdm/README.md` lists the errata, vibe86's (E-*n*, its `docs/sdm-errata.md`) and
those found here (S-*n*: the rotate page runs RCL's, RCR's, ROL's and ROR's code one after another, its ROR loop reads
the count's bit, a superscript is lost), and which override or reading handles each.

**The output** (`x64_isel.inc`) is, for each selected form, an encoder whose constant bytes sailgen folded (prefixes,
REX's W, the opcode, ModRM's mod and /digit; the registers' bits and the immediate are its arguments), and for each
primitive at each width its candidates, cheapest first (no register zeroed before it, no fixed register, no result tied
to an input, a feature the host may have first): the first whose CPUID feature the host has and whose immediate fits is
the one. The file is in the tree: a build needs neither the manual nor `pdftotext`. `sdm/isel.py` makes it again from the
PDF (its SHA-256 must be 093US's), running pdftotext, `--sdm` with the overrides and `--isel`:

```sh
python3 tools/sailgen/sdm/isel.py --sailgen build/snx-sailgen --pdf 325383-093US.pdf --pdftotext pdftotext
```

The file says in its header which revision and PDF it is from. Whatever changes what `--isel` writes (`x64.ops`,
`sailgen`, an override) is followed by it in the same commit: `sailgen_sdm_isel` makes the file again and compares it with
the tree's, byte for byte, where the PDF is (`SNX_SDM_PDF`, else the CPM cache's) and `pdftotext` is. Elsewhere it is
skipped, saying which is missing. `tests/sailgen/isel_fixture.pages` is a few pages in the manual's notation written for
the test (`sailgen_isel`).

### At run time (`x64.h`, `mach.h`, `x64_asm.h`, `x64_a64.h`)

```sh
sailgen --generate a64_x64.roots --backend bits.h --backend machdef.h --backend x64.h --backend mach.h \
        --backend x64_asm.h --backend x64_a64.h --profile a64.profile -o a64_x64.inc a64.sail --overlay nx.sail
```

The back end is four files: `machdef.h` the code buffer and the forms' types every machine's back end shares,
`x64.h` holds x86-64's encodings and the selection (`x64_isel.inc`), with what `mach.h`
asks of a machine (its registers' counts, the place of a fixed register in a form, the frame's register, a call's
results, the vector forms its front makes values with); `mach.h` what any machine's back end does, below; and
`x64_asm.h` x86-64's assembly of what `mach.h` leaves to the machine (the moves and spills the register allocation
makes, the XMM registers' forms, the constant pool, the cold paths, the frame), through the hooks `mach.h` declares
(`sgx_mov_rr`, `sgx_mov_mem`, `sgx_imm_ok`, `sgx_xdown`...). Another host's back end is its own pair of files around
the same `mach.h`.

The same generator (`gen.c`), with the x86-64 back end at the output's top: the CONST and STATIC times are folded
when sailgen runs and when the emitter runs, and what is DYN is appended to a short SSA
of the block's x86 instructions (`SgXIns`: a selected candidate, virtual registers for its operands, an immediate),
the assembly itself. The emitter does what costs little as it appends: constants fold, an operand that is a constant
picks the candidate whose immediate it fits (`imm8` sign extended before `imm32`), a candidate needing a feature the
host lacks is passed over (SHLX for SHL CL; an SSE form of XMM registers alone that `x64.h`'s `sgx_vf_helped_forms`
lists is a cold call of `x64_helpers.c`'s `snx_x64_sse`, the instruction in C, and CRC32C without SSE4.2 a table's, as
CRC32's; a primitive none can make leaves the word to another back end), an input a
form reads whole is zero extended only where its register isn't (each value knows the bits of its register above it
that are 0: the selection said which forms leave their result so), NOT stays unmade until something reads it (a
subtraction takes the operand it was of), and a chain of operations with constants folds into its first (`x AND k1 AND
k2` is one AND). Flags are the instruction that made them: AddWithCarry's NZCV is that instruction read through its
candidate's map of Arm's flags (SUBS's C is `setae`, ADDS's `setb`), an ANDS on the result of the AND that just set
the flags is that AND (the selection lists its form for `ands` too), and ConditionHolds on them is the condition code
the selection derived for that Arm condition on that form, while no instruction since has written the flags (before
one does, a bool still on them is made 0 or 1 in a register by SETcc, which keeps them). The guards of the scalar
floating-point operations are no bools: a cold path (`sgx_cold_fp`) takes up to four tests, each twice a float's bits
less a constant against a limit made once a region (a normal result above the smallest, a denormal operand FZ flushes,
a NaN), and each test is its own MOVD, LEA, CMP and Jcc to the same cold code.

When the block ends, `sgx_optimize` works on the SSA as a whole, where what reads each value and each instruction's
flags is known: a store a later one to the same place overwrites (no load of it, no call of the runtime between) is
dropped (a flag two instructions of the block set, a register written twice), constant additions in a row whose flags
nothing reads fold into one (`ADD #1; ADD #2; SUB #3` is nothing; an ADDS's carry is the last addition's, so not
there), a sum of 0 is its operand where its register is as zero extended as the sum's was, what nothing reads is
dropped, and an instruction whose result nothing reads but whose flags something does becomes its flags-only
candidate (SUBS to the zero register: CMP). `sgx_assemble` then gives the
virtual registers host ones in one pass (RAX, RCX, RDX, RSI, RDI and R8 to R11; RBX holds the state's address), moving
a value into the fixed register a form needs (CL, RAX and RDX), out of one it writes, and copying an input a two-operand
form overwrites where it is read after, and writes each instruction by its encoder; the r/m operand of the same MOV and
SETcc forms is the state's memory for the loads and stores of the guest's registers and flags. Where no register is
free, the value read latest is spilled: to a free XMM register (MOVQ, as bytes) where nothing calls out before it is
last read, since every XMM register is a caller's, else to a slot of the function's frame (`[RSP + 8k]`), once either
way (an SSA value never changes), and reloaded where it is read; the frame's size is patched into the prologue and the epilogues once the
function is done (the registers and the slots are Windows' own there: below). The general registers are given first and RAX, RDX and RCX last, so MUL's and CL's fixed registers
seldom hold another value. A primitive with no candidate on the host has a composition of the others where one
is written (CLZ without LZCNT: a binary search of shifts, a test and CMOVcc).

`x64_a64.h` keeps A64's state where `cpu.h`'s CPU does (`x[32]`; `n`, `z`, `cf`, `vf` each a 32-bit 0 or 1), reads
a register once a block, stores each write at once, and writes only the ABI's own as bytes: the prologue, the call of
the runtime's `undef` and the return of the next pc. `sg_x64_step` makes one word a function `u64 step(CPU*)`, and
`sg_x64_block` words in a row one (up to a branch or an UNDEF), one SSA: CMP then B.HI is `cmp` then `cmova` of the
two pcs, the condition code the selection derived for HI on CMP's flags, with no flag stored and read back between.
`a64_x64.roots` is a64.roots' integer part whole: the data processing, the branch operands, the hints and barriers, and
the loads and stores of the general-purpose registers; its scalar floating point; and the Advanced SIMD the back end's
own vector code (`x64/x64_vec.c`) leaves, element by element (half precision, the dot products, the rounding and
saturating doubling multiplications, the shifts by a register that round or saturate...), so that no A64 instruction
of a guest's is left to another back end. Guest memory is `CPU.mem` plus the address, a memory operand
`[M + address + disp]` of the same MOV and MOVZX forms (a byte's and a halfword's store MOV r/m8 and r/m16, the 16-bit
forms taking the operand-size prefix the opcode column leaves out); an x86 load is already an acquire one, a release
store is MFENCE after it, a barrier of all accesses MFENCE and one of loads or stores alone nothing (x86's ordering,
Vol. 3A 10.2.2). The exclusive monitor, the LSE atomics, CAS and CASP, CRC32 and CRC32C call
`src/common/cpu/recompiler/x64/x64_helpers.c` (cpu.h's ldx, stx, ldxp, stxp and C11 atomics), which the runtime
library links, through a relocation in an object and at its address in the test; a call spills every value to its
home first and loads each argument from there into its register. ADCS's and SBCS's carry in is x86's CF, made by the
flags of ADD #-1 or CMP #1 on the carry, whichever the selection gives the polarity ADC or SBB reads; UDIV and SDIV are
DIV and IDIV of 64 bits, the divisor 1 where it is 0 or -1 under the most negative value (their selection's ASSUME);
CLS and RBIT are compositions (CLZ of `(x XOR x << 1) OR 1`; BSWAP and three swaps of masks); NZCV read as bits
(CCMP's choice) is four SETcc.

The code's calling convention is the host C's: System V's, or where the features have `SGX_F_WIN64` (not CPUID's: a
Windows host's, or a test's) Microsoft's x64 one, so a guest function is called the same way by the runtime's C,
through the dispatch table and by the code itself. Values are in the registers a call changes there too
(`sgx_pool_win64`: RAX, RCX, RDX and R8 to R11, XMM0 to XMM5), so a function saves RBX and what it pins: RDI and RSI,
its callee's there, hold the pins R11 and R10 hold elsewhere, and the XMM registers it pins are saved in its frame, XMM6
and XMM7 with them for values. The frame's lowest 48 bytes are its callees' (`SGX_WIN64_ARGS`: the four arguments'
shadow, a fifth and a sixth), an atomic's address is in R9 (a pair's in R11) rather than RSI, nothing moves the stack
pointer between the prologue and an epilogue, an epilogue is the instructions Windows' unwinder knows one by (ADD RSP,
the pops, the return or a jump), and a frame of a page or more reads its pages first (`sgx_stack_probe`). No pin is stored
around a C call, which keeps all of them.

**The test** (`tests/sail_x64.c`, `sail_x64` with `SNX_SAIL_X64`): every word of the decoder's rows the roots take
(again with Rd, Rn and Rm 31) is made into x86-64 code twice, on the host's BMI1, BMI2 and LZCNT and without them, and
run beside `qemu-aarch64 -cpu max` (`tests/qemu_ref`) from states of the word's own seed (`tests/sail_qemu.h`): every
register, the flags, FPCR, FPSR, the vector registers, the memory it changed, the next pc and UNDEF must be QEMU's
(`sail_x64_spill` again with three host registers, `--pool 3`, so the values spill). A store-exclusive runs after the
load-exclusive that arms its monitor in half the states. Where QEMU can't run beside it (Windows), the words are held
to a reference of QEMU's ends, a hash of each word's every state, which the qemu-reference job records (`--record`) and
the Windows job reads (`SNX_SAIL_QEMU_REFERENCE`). Then random sequences of two to four of the words are made one
block each and run beside the code of each word alone at its pc, one after another (`--sequences N`), which is what
holds the block's folds, dropped stores and reused flags. `--dump WORD` prints a word's code for a disassembler
(`llvm-mc --disassemble -triple=x86_64`).

The code for Windows is run by an x86-64 Linux host too (`sail_win64`, and over whole functions and AArch32 blocks
`recompiler_win64` and `arm_win64`): `tests/support/host_win64.c` enters it as Windows' C would, with RBX, RBP, RSI, RDI,
R12 to R15, XMM6 to XMM15, MXCSR and the stack past the shadow marked before and read back after, and what it calls goes
through a Windows callee that overwrites the shadow and every register it may. Its COFF objects are loaded by the JIT's
own loader (`code_loader_coff.c`), and their function table is held to what Windows does with it: at each call out, and
with `--step` at every instruction, the stack is unwound by the table back to the entry. `x64_unwind`
(`tests/native_unwind.c`) does that on Windows with the system's `RtlVirtualUnwind`, and a `longjmp` across the code's
frames.

**Integers** known only when the guest runs (`SgI`: `UInt`, `SInt` and what comes of them) are 64-bit values, what their
low bits' readers want; what needs more (a saturation of a sum of two 64-bit elements, a shift right) reads the exact
integer beside (`SgXInt`): the operation it is of others, with how many bits hold it, and its doublewords in two's
complement each made where something reads it, the carry, the borrow and the high half of a product among them. What no
word asks of them is refused: `UInt` and `SInt` of more than 64 bits, the bits past the 64th, a comparison past 64 bits,
a sum past 128, a product of other than two signed doublewords, a shift right by the guest's amount of more than one
doubleword. `SatQ` is one comparison and a choice where the integer fits 64 bits.

### The RISC back ends (`rv64.h`, `ppc64.h`, `mips64.h`, `la64.h`, `s390x.h`, over `risc.h`)

Machines without vector registers here: every A64 instruction is `a64_risc.roots`', the Advanced SIMD as general-purpose
doublewords, element by element, the floating point and AES's tables the exact helpers' (`x64_helpers.c`).
`recompiler_<machine>_refused` fails where a word of the Switch's, or of the Ounce's half precision and dot products, is
left to another back end.

A function whose code is longer than the machine's jump reaches (MIPS64's B 128 KiB, riscv64's JAL 1 MiB, arm64's B.cond
1 MiB) is made again with its longest (`MG_FAR_JUMPS`: BAL and JR, AUIPC and JALR, the opposite B.cond over a B), and
arm64's literals are put in the code before LDR (literal)'s 1 MiB is past (`MG_POOL_REACH`). `recompiler_<machine>_long`
runs one.

### Past the guests' CPUs

`snx_recompiler.h`'s `SNX_RECOMPILER_A64_MAX` decodes all of `a64.decode`, and the nine back ends make it too. What the
Sail has (SHA512, SHA3, SM3, SM4, I8MM, FHM, BF16, FCMA, FRINTTS, JSCVT, FlagM2, CSSC, the tags of MTE in an address)
is roots of `a64_x64.roots` and `a64_risc.roots`, with `nx.sail`'s definitions where the Sail's are not generated. What
the Sail (v9.4-a) has not, or has as more than the guest sees, is written once for the nine in `mach/mach_codegen.inc`:

- `past_guests`: DIT, SSBS and TCO (nothing), SB and DSB nXS (DSB), WFET and WFIT (YIELD), the tags' loads and stores
  (no memory holds tags: LDG reads 0, STZG's and STZ2G's zeros and STGP's pair are stored), LDGM, STGM, STZGM and GCSSTR
  (undefined at EL0);
- `newer`: FPRCVT, FAMINMAX, LUT; `compare_branch`: CMPBR; `pair_atomic`: LSE128, in CASP's loop;
- `memory_op`: MOPS, option A, the whole of it in the first of the three instructions;
- `fp8`: FP8 (FP8FMA, FP8DOT4, FP8DOT2, F8F32MM, F8F16MM) as FPMR zero has it, and FSCALE.

Left undefined, the fault's text naming them: EL1's instructions (HVC, SMC, HLT, ERET and the MSRs of PSTATE's EL1
fields: no guest state), MSR SVCR (no guest of `a64.profile` has SME), and MSR and MRS of FPMR.

FP8's formats, scales and saturations are FPMR's, and `CPU` has no FPMR. Zero (E5M2, no scale, no saturation) is exact
for code that never writes it, which nothing here can. A field for it is for when a guest has FEAT_FP8, with the next
`SNX_ABI`, because of what it costs:

1. the field is a u64 after `poll`, at 904, the end of the 904 bytes: no other field moves, and every machine's
   displacements reach it;
2. `sizeof(CPU)` is `SNX_ABI`'s: modules of another ABI are refused, so every game recompiles once, and the JIT's cache,
   whose key has the ABI, starts anew;
3. a save state of another ABI does not load: every one of them would stop loading, the real cost;
4. svcGetThreadContext3 has the console's layout, which has no FPMR, and the hooks' verify compares what is before
   `mem`: neither would see it;
5. a guest without FP8 pays 8 bytes a thread and not an instruction;
6. MSR and MRS of it are the system registers' of each machine's `sys`;
7. the host's own FP8 is used by no back end, so the host changes nothing: the 17 operations are exact helpers', which
   would read the field.

`snx-test-recompiler-<machine>` proves them on the MAX target: `--qemu` against qemu-aarch64's CPU, `--programs` against
C (CMPBR, MOPS, LSE128), and `--coverage` counts what is made, what calls a helper and what is refused.

### The arm64 back end (`arm64.h`, `arm64_asm.h`, `arm64_a64.h`)

```sh
sailgen --generate a64_x64.roots --backend bits.h --backend machdef.h --backend arm64.h --backend mach.h \
        --backend arm64_asm.h --backend mach_a64.h --backend arm64_a64.h --profile a64.profile \
        -o a64_arm64.inc a64.sail --overlay nx.sail
```

The host's own A64, around the same `mach.h` and `mach_a64.h`: the same roots, the same SSA and folds, the same
register allocation (32 registers of each kind). Every word it writes is an encoder's of the A64 Sail's decode
clauses (`a64_enc.inc`, `--encoders`), so the instruction table is the guest's own Sail. What `arm64.h` writes is
which instruction each of `mach.h`'s primitives is, and that is their names: the primitives are the Arm ARM's own
definitions (`x64.ops` writes them from AddWithCarry, ShiftReg, CountLeadingZeroBits), so SUBS is SUBS (shifted
register), Arm's flags are the host's NZCV and each condition its own code (CMP then B.HI is CMP and CSEL HI), and the
division is Arm's (`SGX_DIV_ARM`: no select keeps a divisor of 0 from it). Its immediates are A64's, which
`sgx_imm_ok` answers for the front: an addition's imm12 (or imm12 shifted by 12) of the value or its negation (ADD of
-k is SUB of k, with the same NZCV for k not 0; an addition of 0 stays one, a subtraction of 0 one, their C differing),
a logical operation's bitmask (`sga_bitmask`, DecodeBitMasks inverted). An input is read whole (mach.h zero extends a
narrower value first) only where the result or the flags depend on its bits above the value's: a compare's, a shift's
to the right, a division's, CLZ's and REV's. A constant is MOVZ or MOVN, ORR of a bitmask, or MOVZ and MOVKs.

`arm64_a64.h` keeps what is the machine's: the floating point through the exact helpers (`x64_helpers.c`'s
`snx_x64_fp_*`, every operation: the host's own arithmetic needs the guest's FPCR, which the runtime's C would then run
under too), the store-exclusive as cpu.h's value-based monitor (its checks, then LDAXR and STLXR of the value the
load-exclusive read, compared at the access's size), the LSE atomics, CAS and CASP as load-exclusive loops, CRC32 the
host's own (FEAT_CRC32) or the exact helper, the barriers the guest's (DMB ISH, ISHLD, ISHST), an acquire load DMB ISHLD
after it and a release store DMB ISH around it, and the ABI (AAPCS64: the state in X19, the frame below the saved X29,
X30, X19 and X20; X16 and X17 the assembler's). It builds on any host: the code it writes is arm64's.

Windows on Arm's convention is AAPCS64 too, with X18 the system's (no value's here), so the code for it
(`SNX_ARM64_F_WIN`) is the same words but for a guest function's frame (`arm64_codegen.c`): there its spill slots are
below the saved registers, a SUB of SP after the prologue's last save, the form the unwinder reads; elsewhere above
them, in STP X29, X30's pre-index and each LDP's post-index (past 408 bytes of them, a SUB of SP before the STP and an
ADD after each LDP). What differs is around it: a COFF object (`x64_elf.c`), whose function table
unwinds each frame by a record of 1 MiB of code at most (a longer function has several), its prologue's codes and its
epilogues'; an epilogue's return or jump right after it, where the unwinder has it end; a frame of a page or more reading
its pages first (`sga_stack_probe`, X16 and X17); an AArch32 block's code of no frame, the exit of a failed entry check,
past the range its frame covers; and `snx_stale` past the 1 MiB COFF's ADRP holds read with an ADD more.

**The test** (`tests/sail_mach.c`, `sail_arm64` where Unicorn is installed): the words of `sail_x64`'s rows, made into
arm64 code and run under Unicorn (QEMU's AArch64, its "max" CPU) beside `qemu-aarch64 -cpu max` from the same states as
`sail_x64`'s; registers, flags, the SIMD&FP state, guest memory, the next pc and UNDEF must be QEMU's. Blocks of two to
four of them run beside the code of each word alone, one after another. The test's memory is mapped into Unicorn at its own addresses, so the code and the test's C share
every pointer; a call out goes to a page of traps whose hook calls the native helper. `sail_arm64_spill` gives it
three registers (`--pool 3`); `--dump WORD...` prints a block for a disassembler.

The code for Windows runs under Unicorn on any host (`recompiler_arm64win`, `arm_arm64win`, `module_desc_arm64win`):
`HOSTUC_ARM64_WIN` loads its COFF objects with the JIT's own loader (`code_loader_coff.c`), marks X18, X19 to X29, the
low halves of V8 to V15 and SP before a call and reads them back after, and has each native function change every
register a callee may. The function table is held to what `RtlVirtualUnwind` does with it (`tests/support/arm64_unwinder.h`):
at each call out, and with `--step` at every instruction, the stack is unwound by it back to the entry. `arm64_unwind`
(`tests/native_unwind.c`) does that on Windows on Arm with the system's own, from each instruction's state, and a
`longjmp` across the code's frames.

### AArch32 (`x64_a32.h`)

```sh
sailgen --generate a32.roots --backend bits.h --backend machdef.h --backend x64.h --backend mach.h \
        --backend x64_asm.h --backend x64_a32.h --profile a32.profile -o a32_x64.inc a32.sail arm.sail
```

The roots of `a32.roots` (`snx_sailgen_a32_x64` in `cmake/sdm.cmake`), for the block compiler
`src/common/cpu/recompiler/x64/x64_arm.c`, which makes an A32 or Thumb block for any profile and memory mode of its
options (`cpu/arm/codegen.h`: "The profiles and the memory modes", below). (On Windows its blocks save RBX alone too: the state
comes in RCX.) `x64_a32.h` keeps the state where `arm.h`'s SnxArmCPU
does (`r[16]`, `cpsr`), reads a register once a region and stores each write at once (the dead-store pass drops the
overwritten ones), and keeps NZCV as the flags of the instruction that made them: the four condition codes are packed
into one store of CPSR at the instruction's end, and a condition read before another instruction writes the flags is
the condition code itself (`sg_condition_origin`). An instruction's condition is a jump past it to a label, made at its
first side effect (`sgx_a32_commit`; `sgx_flags_begin` in `sg_add_with_carry` makes it before the flags it keeps), so a
clause that only reads and hands the word on (`sg_see`) emits nothing. Memory is `CPU.mem` plus the zero-extended
address, each access checked first: misaligned (where it must be) an ALIGNMENT exit, its last byte
past `min(mem_size, 2^32)` a DATA_ABORT one, both with the address as the syndrome, on stubs after the block's code; a
store's range against the exclusive reservation in one check (a cold path clears it where they overlap); STREX a LOCK
CMPXCHG of the value LDREX read. The instructions the Sail has no clause for (loads and stores, LDM and STM, the
multiplies and divides, CLZ, REV, the extends and bitfields, MRS and MSR, the exclusives, the barriers, SVC, BKPT, the
coprocessor's, the ARMv6 media and DSP ones, VFP and Advanced SIMD) are written in `x64_arm.c` over the same SSA. A
Thumb block's 16-bit instructions are the Sail's (`sg_decode_t16`), whose conditions and flags follow ITSTATE, which is
known as the block is made; its 32-bit ones are the shapes `thumb.c` decodes, written in `mach_thumb.inc`, or the A32
instruction of the same meaning whose emitter reads Thumb's PC. `sg_ITSTATE` and `sg_ITSTATE_set` carry it (IT sets the
ones after it; an SVC, a BKPT and a block that ends inside an IT block leave it in CPSR). The VFP's scalar instructions keep their values in XMM
registers, and the arithmetic and the compares take the host's SSE instruction where IEEE 754 gives the console's
bits: FPSCR's rounding to nearest, no trap enabled and IXC already set (for the arithmetic), operands
finite and normal or zero, a result normal or an exact zero the operation allows; anything else calls
`snx_arm_fp_x64` on a cold path, which is `snx_arm_fp`. FZ on or off does not change the choice.

**The profiles and the memory modes** (`snx_x64_arm_options`, every machine's `_arm_options`: the options of the next
blocks) are the same emitters, what differs asked where it matters. `sg_ArchVersion` is the profile's, for the Sail;
the hand-written emitters decode with `snx_a32_decode_profile` and `snx_thumb_decode_profile`, and before Thumb-2 (the
ARM11, the ARM946E-S and the ARM7TDMI) every Thumb halfword is the Sail's. Guest memory goes through four functions of
`mach_a32.h`: `sgx_a32_transfer` begins an instruction's transfer (its alignment and range, a trace's slot, a checked
load's call), `sgx_a32_read` and `sgx_a32_write` move its bytes (`sgx_a32_read_list` a multiple one's words), and
`sgx_a32_written` ends a store (the monitor, a checked store's call).
- *Hosted* blocks check no range and nothing at their entry, and stop rather than set CPSR.E.
- *Checked* blocks call the runtime library for each fetch and each transfer (`snx_arm_checked_fetch`,
  `snx_arm_checked_access`, which call `checked.h`'s callback), a transfer's bytes as numbers in the state's `words`.
- The *ARM7TDMI's* and the *ARM946E's* bus: the access itself is in the hot code, and one whose address the hook may
  take (`sgx_a32_route`) jumps to a cold call of `snx_arm_io_read` or `snx_arm_io_write` with the live caller-saved
  registers kept in the frame around it (`SGX_A32_BUS`, whose code is `mach_arm.inc`'s on every machine); a write
  whose hook returns nonzero ends the block after its instruction (`SGX_A32_OUT`), chained or not. The ARM7TDMI's MULS
  calls `snx_arm_multiply_carry`, and its trace is stores past the state and a bit on a skipped instruction's way.
- The *ARM11's* VFP stops for the backend where FPSCR enables an exception or a short vector, and at a VMSR of bits it
  does not model (`SGX_A32_OUT` too).
- *Chained* hosted blocks (`codegen.h`'s `SnxArmChainedOptions`, the Vita's) go on without their dispatch loop: a step
  back counts the slice's poll down (`count_back`), a target that is no successor is run from the runtime's table
  (`look_up`: `SGX_A32_FIELD` loads the host's own words of it, `SGX_A32_TAIL` is the tail call, `mg_tail` on each
  machine), and an import's stub calls the runtime through `snx_arm_direct` in its SVC's place (`import_call`).

A block's entry checks what `block_begin` does (exit and syndrome cleared, R15 its address, CPSR.T and E clear) on a
shared stub of the object; its end goes on to the block after it with a jump to that block's function where
`nx/loader/module_arm.c` names it a successor (after the poll a backward one takes), past the entry's checks where
nothing in the block can have changed them, and through them after an MSR that may set E. Both ways of a CBZ are such
jumps. Each block is
`void A_xxxxxxxx(SnxArmCPU*)` of one ELF object (a COFF one on Windows), global and hidden. `module_arm.c` makes every
A32 block this way as it discovers it, on all the build's threads, and `nx/loader/module_x64.c` writes the module's
descriptor. The recompile prints `x86-64 back end: N of M AArch32 blocks`, and why one was refused. The 3DS's, the DS's and the GBA's
compilers make their blocks the same way through `cpu/arm/aot.h`, and the Vita's as it discovers them; each writes its
module's index as data (`snx_native_data`).

**The test** (`tests/arm_x64.c`, `arm_x64`): random blocks of one to six words of 42 A32 and VFP classes made by
the back end and run from random states (registers in and out of memory, the flags, the monitor, FPSCR with FZ, DN,
a rounding mode and a trap, denormals, zeros and NaNs): every register, CPSR, the exit and its syndrome, the monitor,
FPSCR, the D registers and memory must match the recorded results (test-references' `arm/`, LLVM -O0's, recorded
while the build had it), and so must the words each block covers. Witnesses count the calls of
`snx_arm_fp_x64` each VFP case makes (the hot path is taken exactly where IEEE 754 allows it), the entry's checks are run
with each of their failures, and an MSR of E is run into the next block. Thumb blocks (`--thumb N`) are made the same
way from random halfwords: 16-bit ones of every form, 32-bit ones of every family and one IT in four blocks, with the
instructions of its block after it; their witnesses are the entry's checks (R15, T, ITSTATE, E, in their order) and
TBB's and TBH's reads past memory. `arm_x64_spill` does it with three host
registers, `arm_x64_plain` without BMI1, BMI2 and LZCNT; `arm_win64` and `arm_arm64win` run the same with the code a
Windows host's. `--profile` and `--memory` run the other profiles and modes
(`arm_x64_a9`, `arm_x64_arm7tdmi_trace`, `arm_x64_arm11_checked`, ... and the same on each other machine), against
the recorded results of the same options: their words are what the profile decodes, hosted memory and the hook's two run on a
window of 4 GiB, and every call of the hook or the checked callback is logged with the state it saw. The hosted modes run
the chained blocks' cases as well, each end against the state, the poll and the calls it must leave.
