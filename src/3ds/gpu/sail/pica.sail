/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The PICA200's shader instructions in Sail (sailgen --gpu, tools/sailgen/README.md's "GPU mode"): what each computes,
   from the fields of its 32-bit word that gpu/pica.decode names it by, as vk/vsgen.c's vertex shaders run it (Azahar's
   glsl_shader_decompiler.cpp, without sanitize_mul). The back end (pica_spirv.h) gives the registers, the uniforms
   and the program's structured control flow (the subroutines, IF's blocks, LOOP and the JMPs' labels), which are
   vsgen.c's own work; the roots (pica.roots) name the opcodes each instruction runs for */

/* bits lo to lo + n - 1 of the instruction, known when the translator runs */
val Field : forall 'n. (int, int('n)) -> bits('n)
/* the opcode's place in its roots line */
val Form : unit -> int
/* the swizzle pattern of an operand descriptor: the destination mask at 0-3, then for each source n (1-3) its negation
   (4, 13, 22) and its selectors (5, 14, 23: w at the lowest two bits, x at the highest) */
val Pattern : int -> bits(32)
/* input register r (0-15), temporary r (0-15), float uniform r (0-95), and float uniform base offset by an address
   register's value (Azahar's get_offset_register: an offset outside -128..127 taken as 0, the index wrapped to 7 bits,
   1.0 past the 96 uniforms) */
val InputReg : int -> vec4
val TempReg : int -> vec4
val UniformF : int -> vec4
val UniformOffset : (int, bits(32)) -> vec4
/* the address registers (0 a0.x, 1 a0.y, 2 aL), signed */
val AddrRead : int -> bits(32)
val AddrWrite : (int, bits(32)) -> unit
/* the variable of destination register r (0-15 an output, 0 one the output mask leaves out; 16-31 a temporary), made
   at its first use; its value and a value written to it */
val DestVar : int -> int
val VarLoad : int -> vec4
val VarStore : (int, vec4) -> unit
/* the conditional codes x (0) and y (1) */
val CondRead : int -> bool
val CondWrite : (int, bool) -> unit
/* the bool uniforms, a bit each */
val UniformB : unit -> bits(32)
/* if (c) { ... }: the instructions between IfBegin and IfEnd run where c holds */
val IfBegin : bool -> unit
val IfEnd : unit -> unit
/* the flow control: END's return; a JMP to dest where c holds; the subroutine dest to dest + num called (where c
   holds); the body up to dest where c holds, else the else part dest to dest + num; LOOP of the int uniform id up to
   dest */
val End : unit -> unit
val JumpTo : (bool, int) -> unit
val CallSub : (int, int) -> unit
val CallIf : (bool, int, int) -> unit
val IfElse : (bool, int, int) -> unit
val Loop : (int, int) -> unit
/* an instruction the decompiler refuses: why */
val Untranslated : string -> unit

/* ---- operands ---- */

/* source register reg (0x00-0x0f inputs, 0x10-0x1f temporaries, 0x20-0x7f float uniforms, which address register
   addr offsets: 0 none, 1 a0.x, 2 a0.y, 3 aL) as source n of pattern p: swizzled, then negated */
function Source(reg : int, addr : int, p : bits(32), n : int) -> vec4 = {
    let v = if reg < 16 then InputReg(reg) else if reg < 32 then TempReg(reg - 16)
            else if addr != 0 then UniformOffset(reg - 32, AddrRead(addr - 1)) else UniformF(reg - 32);
    let at = if n == 1 then 5 else if n == 2 then 14 else 23;
    let x = UInt(p[at + 7 .. at + 6]);
    let y = UInt(p[at + 5 .. at + 4]);
    let z = UInt(p[at + 3 .. at + 2]);
    let w = UInt(p[at + 1 .. at]);
    let s = if x != 0 | y != 1 | z != 2 | w != 3 then V4Shuffle(v, v, x, y, z, w) else v;
    if p[at - 1 .. at - 1] == 0b1 then V4FNeg(s) else s
}

/* SetDest: the components pattern p enables of the destination's variable d (0 none) get value's same components */
function SetDest(p : bits(32), d : int, value : vec4) -> unit = {
    let mask = UInt(p[3 .. 0]);
    if d != 0 & mask != 0 then {
        if mask != 15 then {
            let old = VarLoad(d);
            VarStore(d, V4Shuffle(old, value, if p[3 .. 3] == 0b1 then 4 else 0, if p[2 .. 2] == 0b1 then 5 else 1,
                                  if p[1 .. 1] == 0b1 then 6 else 2, if p[0 .. 0] == 0b1 then 7 else 3))
        } else VarStore(d, value)
    }
}
/* a scalar spread over the enabled components */
function SetDestScalar(p : bits(32), d : int, value : bits(32)) -> unit = {
    let mask = UInt(p[3 .. 0]);
    if d != 0 & mask != 0 then SetDest(p, d, V4Splat(value))
}

/* format 1: the destination at 21, the address register at 19 (which offsets src1, the 7-bit register at 12), src2
   the 5-bit register at 7, the operand descriptor at 0. 1i (inverted, the I forms): src1 the 5-bit register at 14,
   src2 the 7-bit register at 7, which the address register offsets */
function ArithPattern() -> bits(32) = Pattern(UInt(Field(0, 7)))
function ArithA(p : bits(32), inverted : bool) -> vec4 =
    if inverted then Source(UInt(Field(14, 5)), 0, p, 1) else Source(UInt(Field(12, 7)), UInt(Field(19, 2)), p, 1)
function ArithB(p : bits(32), inverted : bool) -> vec4 =
    if inverted then Source(UInt(Field(7, 7)), UInt(Field(19, 2)), p, 2) else Source(UInt(Field(7, 5)), 0, p, 2)
function ArithDest() -> int = DestVar(UInt(Field(21, 5)))

/* ---- arithmetic ---- */

/* ADD, MUL, FLR, MAX, MIN, MOV: the sources' sum, product, floor, maximum, minimum, the first source */
function execute_ARITH() -> unit = {
    let f = Form();
    let p = ArithPattern();
    let a = ArithA(p, false);
    let b = ArithB(p, false);
    let d = ArithDest();
    SetDest(p, d, match f { 0 => V4FAdd(a, b), 1 => V4FMul(a, b), 2 => V4Floor(a), 3 => V4FMax(a, b),
                            4 => V4FMin(a, b), _ => a })
}

/* EX2 and LG2: 2^x and log2 x of the first source's x, over the enabled components */
function execute_EXP() -> unit = {
    let f = Form();
    let p = ArithPattern();
    let a = ArithA(p, false);
    let b = ArithB(p, false);
    let d = ArithDest();
    let x = V4At(a, 0);
    SetDestScalar(p, d, if f == 0 then FExp2(x) else FLog2(x))
}

/* DP3: the dot product of the sources' x, y and z */
function execute_DP3() -> unit = {
    let p = ArithPattern();
    let a = ArithA(p, false);
    let b = ArithB(p, false);
    let d = ArithDest();
    SetDestScalar(p, d, V3Dot(V3Of4(a), V3Of4(b)))
}

/* DP4, and DPH and DPHI (homogeneous: the first source's w taken as 1.0, the hardware's) */
function execute_DOT4() -> unit = {
    let f = Form(); /* 0 DP4, 1 DPH, 2 DPHI */
    let p = ArithPattern();
    let a0 = ArithA(p, f == 2);
    let b = ArithB(p, f == 2);
    let d = ArithDest();
    let a = if f != 0 then V4Shuffle(a0, V4Const(0x00000000, 0x00000000, 0x00000000, 0x3f800000), 0, 1, 2, 7) else a0;
    SetDestScalar(p, d, V4Dot(a, b))
}

/* RCP and RSQ: 1 / x and 1 / sqrt(x) of the first source's x; a zero operand (RSQ: one not above zero) leaves the
   destination alone, which avoids NaN, and a NaN one passes RCP's unordered != */
function execute_RCP() -> unit = {
    let rsq = Form() == 1;
    let p = ArithPattern();
    let a = ArithA(p, false);
    let b = ArithB(p, false);
    let d = ArithDest();
    let v = V4At(a, 0);
    IfBegin(if rsq then FCompare(4, v, FConst(0x00000000)) else FCompare(13, v, FConst(0x00000000)));
    SetDestScalar(p, d, if rsq then FInvSqrt(v) else FDiv(FConst(0x3f800000), v));
    IfEnd()
}

/* MOVA: a0.x and a0.y, each the mask enables, the first source's x and y made signed integers */
function execute_MOVA() -> unit = {
    let p = ArithPattern();
    let a = ArithA(p, false);
    let b = ArithB(p, false);
    let d = ArithDest();
    if p[3 .. 3] == 0b1 then AddrWrite(0, FToS32(V4At(a, 0)));
    if p[2 .. 2] == 0b1 then AddrWrite(1, FToS32(V4At(a, 1)))
}

/* SGE, SLT and their I forms: 1.0 in each component where a >= b (a < b), else 0.0 */
function execute_SETCMP() -> unit = {
    let f = Form(); /* 0 SGE, 1 SGEI, 2 SLT, 3 SLTI */
    let inverted = f == 1 | f == 3;
    let p = ArithPattern();
    let a = ArithA(p, inverted);
    let b = ArithB(p, inverted);
    let d = ArithDest();
    let mask = V4FCompare(if f <= 1 then 6 else 1, a, b);
    SetDest(p, d, V4Select(mask, V4Const(0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000),
                           V4Const(0x00000000, 0x00000000, 0x00000000, 0x00000000)))
}

/* CMP (format 1c): the conditional codes, x's and y's comparisons of the sources' x and y (24 and 21: ==, !=, <, <=, >,
   >=, as GLSL compiles them: != unordered); an unknown mode leaves the codes, as Azahar does */
function CompareCode(m : int) -> int = match m { 0 => 2, 1 => 13, 2 => 1, 3 => 3, 4 => 4, _ => 6 }
function execute_CMP() -> unit = {
    let p = ArithPattern();
    let a = ArithA(p, false);
    let b = ArithB(p, false);
    /* the register the arithmetic format's destination field names (the comparisons' bits): the other arithmetic
       instructions' code makes its variable, which the module keeps */
    let unused = ArithDest();
    let mx = UInt(Field(24, 3));
    let my = UInt(Field(21, 3));
    if mx <= 5 & my <= 5 then {
        CondWrite(0, FCompare(CompareCode(mx), V4At(a, 0), V4At(b, 0)));
        CondWrite(1, FCompare(CompareCode(my), V4At(a, 1), V4At(b, 1)))
    }
}

/* MAD and MADI (format 5: the destination at 24, the address register at 22, src1 the 5-bit register at 17, src2 at
   10 and src3 at 5, the operand descriptor the 5 bits at 0; MAD's src2 is 7 bits and the address register offsets it,
   MADI's src3): a * b + c */
function execute_MAD() -> unit = {
    let inverted = Form() == 1;
    let p = Pattern(UInt(Field(0, 5)));
    let a = Source(UInt(Field(17, 5)), 0, p, 1);
    let b = if inverted then Source(UInt(Field(12, 5)), 0, p, 2) else Source(UInt(Field(10, 7)), UInt(Field(22, 2)), p, 2);
    let c = if inverted then Source(UInt(Field(5, 7)), UInt(Field(22, 2)), p, 3) else Source(UInt(Field(5, 5)), 0, p, 3);
    let product = V4FMul(a, b);
    let d = DestVar(UInt(Field(24, 5)));
    SetDest(p, d, V4FAdd(product, c))
}

/* DST, LITP and DSTI, which Azahar does not translate either */
function execute_UNTRANSLATED_ARITH() -> unit =
    throw(Untranslated("an arithmetic instruction Azahar does not translate either"))

/* ---- flow control ---- */

/* NOP; EMIT and SETEMIT, a geometry shader's: nothing in a vertex shader */
function execute_NOP() -> unit = ()

function execute_END() -> unit = End()

/* BREAK and BREAKC, which Azahar does not translate either */
function execute_BREAK() -> unit = throw(Untranslated("an instruction Azahar does not translate either"))

/* the condition of format 2: the conditional codes against refx (25) and refy (24): cond (22) 0 either, 1 both, 2 x
   only, 3 y only */
function CodeIs(i : int, set : bits(1)) -> bool = {
    let v = CondRead(i);
    if set == 0b1 then v else not_bool(v)
}
function Condition() -> bool = {
    let c = UInt(Field(22, 2));
    if c == 2 then CodeIs(0, Field(25, 1)) else if c == 3 then CodeIs(1, Field(24, 1)) else {
        let x = CodeIs(0, Field(25, 1));
        let y = CodeIs(1, Field(24, 1));
        if c == 0 then x | y else x & y
    }
}
/* format 3's bool uniform (id at 22), inverted: whether it is clear */
function UniformBool(inverted : bool) -> bool = {
    let masked = UniformB() & LSL(0x00000001, UInt(Field(22, 4)));
    if inverted then masked == 0x00000000 else masked != 0x00000000
}
/* the target (10) and the instruction count (0) */
function Dest() -> int = UInt(Field(10, 12))
function Num() -> int = UInt(Field(0, 8))

/* JMPC, and JMPU: taken when the bool uniform differs from num's bit 0 */
function execute_JMPC() -> unit = JumpTo(Condition(), Dest())
function execute_JMPU() -> unit = JumpTo(UniformBool(Field(0, 1) == 0b1), Dest())

function execute_CALL() -> unit = CallSub(Dest(), Num())
function execute_CALLC() -> unit = CallIf(Condition(), Dest(), Num())
function execute_CALLU() -> unit = CallIf(UniformBool(false), Dest(), Num())

function execute_IFC() -> unit = IfElse(Condition(), Dest(), Num())
function execute_IFU() -> unit = IfElse(UniformBool(false), Dest(), Num())

/* LOOP: aL = i[id].y, then the body (up to dest) i[id].x + 1 times, aL += i[id].z after each */
function execute_LOOP() -> unit = Loop(UInt(Field(22, 2)), Dest())
