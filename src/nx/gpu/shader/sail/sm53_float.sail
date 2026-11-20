/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's floating point instructions (sm53_float.roots; back end sm53_float_spirv.h). emit_float.c keeps what carries
   the translator's work on them, which the Sail calls: a register's float as the program's FTZ reads it
   (shader_ftz_read), the operands' modifiers (shader_float_mod, shader_double_mod), the constant buffer's and the
   immediates' floats, the flush of a subnormal where the device does not (shader_float_ftz), .SAT, the halves' pairs,
   their FMZ and merge, the doubles' register pairs, and F2F's, F2I's and I2F's conversions (shader_f2f, shader_f2i,
   shader_i2f: the native half, the directed roundings, the F64 bounds) */

/* register r as the instruction's float (the block's float, its FTZ as the program reads it) */
val RegF : int -> bits(32)
val FloatMod : (bits(32), int, int) -> bits(32)
/* the constant buffer word as a float; the immediate of 19 bits (20-38, its sign at 56) and of 32 as floats; the
   former's bits */
val CbufF : unit -> bits(32)
val Imm20F : unit -> bits(32)
val Imm32F : unit -> bits(32)
val ImmBits20 : unit -> bits(32)
/* the program flushes the FTZ instructions' subnormals itself (SH_FTZ_EMULATE), and a flush */
val SoftFtz : unit -> bool
val FloatFtz : bits(32) -> bits(32)
/* the sum, product and fused multiply-add the device must not fuse or split (NoContraction) */
val FAddNC : (bits(32), bits(32)) -> bits(32)
val FMulNC : (bits(32), bits(32)) -> bits(32)
val FmaNC : (bits(32), bits(32), bits(32)) -> bits(32)
/* FMNMX's choice (FMin where choose holds, else FMax), only the one a choice the translator knows takes made */
val MinMaxF : (bool, bits(32), bits(32)) -> bits(32)
/* a pair of halves of a word (its swizzle: 0 both, 1 a float for both, 2 the low twice, 3 the high twice), its abs
   and neg; the immediate pair; the pair's FMA, product and sum the device must not fuse; FMZ's zero products; the
   result as the word's halves (merged with old by 49-50, saturated) */
val HalfPairOf : (bits(32), int) -> half2
val HalfPairMod : (half2, int, int) -> half2
val HalfImmediate : unit -> bits(32)
val Half2FmaNC : (half2, half2, half2) -> half2
val Half2MulNC : (half2, half2) -> half2
val Half2AddNC : (half2, half2) -> half2
val HalfFmz : (half2, half2, half2) -> half2
val HalfFmzFma : (half2, half2, half2, half2) -> half2
val HalfPairResult : (half2, bits(32), int, int) -> bits(32)
/* the doubles: a register pair's, the right operand of a form (0 a pair, 1 the constant's, 2 the immediate), their
   modifiers, a pair written; the sum, product and FMA the device must not fuse; DMNMX's choice with its NaNs; a
   double whose high word is x (the low 0), its reciprocal and reciprocal square root, its high word */
val DoubleRead : int -> f64
val DoubleSource : int -> f64
val DoubleMod : (f64, int, int) -> f64
val DoubleWrite : (int, f64) -> unit
val DAddNC : (f64, f64) -> f64
val DMulNC : (f64, f64) -> f64
val DFmaNC : (f64, f64, f64) -> f64
val DMinMax : (bool, f64, f64) -> f64
val DFromHigh : bits(32) -> f64
val DRcp : f64 -> f64
val DInvSqrt : f64 -> f64
val DHigh : f64 -> bits(32)
val DCompare : (int, f64, f64) -> bool
/* FSWZADD: the lanes (a bit each) whose mode is k (shader_fswzadd_lanes), and the lane in its quad */
val FswzMask : int -> int
val QuadLane : unit -> bits(32)
/* F2F's, F2I's and I2F's conversions */
val F2F : unit -> unit
val F2I : unit -> unit
val I2F : unit -> unit

/* FCMP: a where c (or the constant in the RC form) compares with 0.0 (48-51), else the second operand */
function execute_FCMP() -> unit = {
    let f = Form();
    let third = R_readF(UInt(Field(39, 8)));
    let comparison = if f == 1 then CbufF() else third;
    let alternative = if f == 1 then R_read(UInt(Field(39, 8))) else if f == 2 then CbufWord()
                      else if f == 3 then ImmBits20() else R_read(RegB());
    let test = FCompare(UInt(Field(48, 4)), comparison, FConst(0x00000000));
    R_write(Dst(), RawSelect(test, R_read(RegA()), alternative))
}

function execute_F2F() -> unit = F2F()
function execute_F2I() -> unit = F2I()
function execute_I2F() -> unit = I2F()

/* FADD, FMUL and their 32I forms: a + b or a * b, each operand's modifiers where its form keeps them, FTZ (44, FMUL's
   44-45, FADD32I's 55, FMUL32I's 53-54 bit 0) flushed where the device does not, FMZ (FMUL's 44-45, FMUL32I's 53-54 = 2:
   a zero factor makes the product 0, even with an infinity), FMUL's scale (41-43), .SAT */
function execute_FADD() -> unit = {
    let f = Form(); /* 0-2 FADD, 3 FADD32I, 4-6 FMUL, 7 FMUL32I */
    let add = f <= 3;
    let left0 = RegF(RegA());
    let right0 = if f == 1 | f == 5 then CbufF() else if f == 2 | f == 6 then Imm20F() else if f == 3 | f == 7 then Imm32F()
                 else RegF(RegB());
    let left = if f <= 2 then FloatMod(left0, UInt(Field(46, 1)), UInt(Field(48, 1)))
               else if f == 3 then FloatMod(left0, UInt(Field(54, 1)), UInt(Field(56, 1))) else left0;
    let right = if f <= 2 then FloatMod(right0, UInt(Field(49, 1)), UInt(Field(45, 1)))
                else if f == 3 then FloatMod(right0, UInt(Field(57, 1)), UInt(Field(53, 1)))
                else if f <= 6 then FloatMod(right0, 0, UInt(Field(48, 1))) else right0;
    let mode = if f == 3 then UInt(Field(55, 1)) else if f == 7 then UInt(Field(53, 2)) else if add then UInt(Field(44, 1))
               else UInt(Field(44, 2));
    let soft_ftz = MOD(mode, 2) == 1 & SoftFtz();
    let l = if soft_ftz then FloatFtz(left) else left;
    let r = if soft_ftz then FloatFtz(right) else right;
    let sum = if add then FAddNC(l, r) else FMulNC(l, r);
    let flushed = if soft_ftz then FloatFtz(sum) else sum;
    let fmz = (if f == 7 then UInt(Field(53, 2)) else if f >= 4 then UInt(Field(44, 2)) else 0) == 2;
    let zeroed = if fmz then {
        let zero = FConst(0x00000000);
        let left_zero = FCompare(2, l, zero);
        let right_zero = FCompare(2, r, zero);
        let either = RawOr(left_zero, right_zero);
        if either then zero else flushed
    } else flushed;
    /* FMUL's scale: 1, 0.5, 0.25, 0.125, 8, 4, 2 */
    let sc = if f >= 4 & f <= 6 then UInt(Field(41, 3)) else 0;
    let scaled = if sc != 0 then FMulNC(zeroed, FConst(match sc { 1 => 0x3f000000, 2 => 0x3e800000, 3 => 0x3e000000,
                                                                     4 => 0x41000000, 5 => 0x40800000, _ => 0x40000000 }))
                 else zeroed;
    let sat = if f == 3 then 0 else if f == 7 then UInt(Field(55, 1)) else UInt(Field(50, 1));
    R_writeF(Dst(), FloatSaturate(scaled, sat))
}

/* FFMA and FFMA32I: a * b + c rounded once (an Fma without NoContraction may be split into a multiply and an add), b's
   and c's negations (FFMA32I: a's and c's), FTZ and FMZ at 53-54 (FMZ: a zero factor gives c), .SAT */
function execute_FFMA() -> unit = {
    let f = Form(); /* 0 R, 1 RC, 2 CR, 3 I, 4 32I */
    let left0 = RegF(RegA());
    let right0 = if f == 4 then Imm32F() else if f == 2 then CbufF() else if f == 3 then Imm20F()
                 else RegF(if f == 1 then UInt(Field(39, 8)) else RegB());
    let addend0 = if f == 1 then CbufF() else RegF(if f == 4 then Dst() else UInt(Field(39, 8)));
    let left = if f == 4 then FloatMod(left0, 0, UInt(Field(56, 1))) else left0;
    let right = if f == 4 then right0 else FloatMod(right0, 0, UInt(Field(48, 1)));
    let addend = if f == 4 then FloatMod(addend0, 0, UInt(Field(57, 1))) else FloatMod(addend0, 0, UInt(Field(49, 1)));
    let soft_ftz = Field(53, 1) == 0b1 & SoftFtz();
    let l = if soft_ftz then FloatFtz(left) else left;
    let r = if soft_ftz then FloatFtz(right) else right;
    let s = if soft_ftz then FloatFtz(addend) else addend;
    let fma = FmaNC(l, r, s);
    let flushed = if soft_ftz then FloatFtz(fma) else fma;
    let result = if Field(53, 2) == 0b10 then {
        let zero = FConst(0x00000000);
        let left_zero = FCompare(2, l, zero);
        let right_zero = FCompare(2, r, zero);
        if left_zero | right_zero then s else flushed
    } else flushed;
    R_writeF(Dst(), FloatSaturate(result, if f == 4 then UInt(Field(55, 1)) else UInt(Field(50, 1))))
}

/* FMNMX: the minimum of a and b where the predicate at 39 holds, else the maximum; a NaN gives the other operand */
function execute_FMNMX() -> unit = {
    let f = Form();
    let left0 = R_readF(RegA());
    let right0 = if f == 1 then CbufF() else if f == 2 then Imm20F() else R_readF(RegB());
    let left = FloatMod(left0, UInt(Field(46, 1)), UInt(Field(48, 1)));
    let right = FloatMod(right0, UInt(Field(49, 1)), UInt(Field(45, 1)));
    let is_left_nan = FIsNaN(left);
    let is_right_nan = FIsNaN(right);
    let choose_min = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let value = MinMaxF(choose_min, left, right);
    let v1 = if is_right_nan then left else value;
    R_writeF(Dst(), if is_left_nan then right else v1)
}

/* FSETP: a and b compared (48-51), combined with the predicate at 39 into the one at 3 and its negation into the one at
   0; the immediate is the float of its 19 bits as the high ones */
function execute_FSETP() -> unit = {
    let f = Form();
    let left = FloatMod(R_readF(RegA()), UInt(Field(7, 1)), UInt(Field(43, 1)));
    let right0 = if f == 1 then CbufF()
                 else if f == 2 then FConst(UInt(Field(20, 19)) * 4096 + UInt(Field(56, 1)) * 2147483648)
                 else R_readF(RegB());
    let right = FloatMod(right0, UInt(Field(44, 1)), UInt(Field(6, 1)));
    let test = FCompare(UInt(Field(48, 4)), left, right);
    let pc = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let op_bool = UInt(Field(45, 2));
    let yes = Combine53(op_bool, test, pc);
    let inverse = RawNot(test);
    let no = Combine53(op_bool, inverse, pc);
    PredWrite(UInt(Field(3, 3)), yes);
    PredWrite(UInt(Field(0, 3)), no)
}

/* DADD, DMUL, DFMA and DMNMX of register pairs (DFMA's RC form c a pair at 39, its CR form b the constant's) */
function execute_DARITH() -> unit = {
    let f = Form(); /* 0-2 DADD, 3-5 DMUL, 6-9 DFMA (R, RC, CR, I), 10-12 DMNMX */
    let left0 = DoubleRead(RegA());
    let right0 = if f == 7 then DoubleRead(UInt(Field(39, 8))) else if f == 8 then DoubleSource(1)
                 else if f >= 6 & f <= 9 then DoubleSource(if f == 9 then 2 else 0)
                 else if f <= 2 then DoubleSource(f) else if f <= 5 then DoubleSource(f - 3) else DoubleSource(f - 10);
    let value : f64 = if f <= 2 then {
        let left = DoubleMod(left0, UInt(Field(46, 1)), UInt(Field(48, 1)));
        let right = DoubleMod(right0, UInt(Field(49, 1)), UInt(Field(45, 1)));
        DAddNC(left, right)
    } else if f <= 5 then {
        let left = DoubleMod(left0, 0, UInt(Field(48, 1)));
        DMulNC(left, right0)
    } else if f <= 9 then {
        let right = DoubleMod(right0, 0, UInt(Field(48, 1)));
        let third = if f == 7 then DoubleSource(1) else DoubleRead(UInt(Field(39, 8)));
        let addend = DoubleMod(third, 0, UInt(Field(49, 1)));
        DFmaNC(left0, right, addend)
    } else {
        let left = DoubleMod(left0, UInt(Field(46, 1)), UInt(Field(48, 1)));
        let right = DoubleMod(right0, UInt(Field(49, 1)), UInt(Field(45, 1)));
        /* as FMNMX: PT or !PT picks one as the program is translated */
        DMinMax(PredRead(UInt(Field(39, 3)), UInt(Field(42, 1))), left, right)
    };
    DoubleWrite(Dst(), value)
}

/* DSET and DSETP: doubles compared, as FSET and FSETP */
function execute_DSET() -> unit = {
    let is_set = Form() <= 2;
    let left = DoubleMod(DoubleRead(RegA()), if is_set then UInt(Field(54, 1)) else UInt(Field(7, 1)), UInt(Field(43, 1)));
    let right = DoubleMod(DoubleSource(if is_set then Form() else Form() - 3), UInt(Field(44, 1)),
                          if is_set then UInt(Field(53, 1)) else UInt(Field(6, 1)));
    let comparison = DCompare(UInt(Field(48, 4)), left, right);
    let predicate = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    if is_set then {
        let test = Combine53(UInt(Field(45, 2)), comparison, predicate);
        let truth = Bits32(if Field(52, 1) == 0b1 then 0x3f800000 else 0xffffffff);
        let value = RawSelect(test, truth, 0x00000000);
        if Field(47, 1) == 0b1 then FlagsLogic(value);
        R_write(Dst(), value)
    } else {
        let inverse = RawNot(comparison);
        PredWrite(UInt(Field(3, 3)), Combine53(UInt(Field(45, 2)), comparison, predicate));
        PredWrite(UInt(Field(0, 3)), Combine53(UInt(Field(45, 2)), inverse, predicate))
    }
}

/* FSET: 1.0 (52) or all ones where a and b compare, combined with the predicate at 39, else 0; .CC the flags */
function execute_FSET() -> unit = {
    let f = Form();
    let left0 = R_readF(RegA());
    let right0 = if f == 1 then CbufF() else if f == 2 then Imm20F() else R_readF(RegB());
    let left = FloatMod(left0, UInt(Field(54, 1)), UInt(Field(43, 1)));
    let right = FloatMod(right0, UInt(Field(44, 1)), UInt(Field(53, 1)));
    let test0 = FCompare(UInt(Field(48, 4)), left, right);
    let pc = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let test = Combine53(UInt(Field(45, 2)), test0, pc);
    let truth = Bits32(if Field(52, 1) == 0b1 then 0x3f800000 else 0xffffffff);
    let value = RawSelect(test, truth, 0x00000000);
    if Field(47, 1) == 0b1 then FlagsLogic(value);
    R_write(Dst(), value)
}

/* MUFU (the function at 20-23 of a, its abs at 46 and neg at 48, .SAT at 50) and RRO (b's float, its abs at 49 and neg
   at 45, as it is: the range reduction the functions take is none here). RCP64H and RSQ64H (6, 7): a is a double's
   high word (the low one 0), the result the high word of its reciprocal or reciprocal square root, as Mesa's nouveau
   uses them */
function execute_MUFU() -> unit = {
    let f = Form(); /* 0 MUFU, 1-3 RRO R, C, I */
    let fn = UInt(Field(20, 4));
    if f == 0 & (fn == 6 | fn == 7) then {
        let value = DoubleMod(DFromHigh(R_read(RegA())), UInt(Field(46, 1)), UInt(Field(48, 1)));
        R_write(Dst(), DHigh(if fn == 6 then DRcp(value) else DInvSqrt(value)))
    } else {
        let value0 = if f == 2 then CbufF() else if f == 3 then Imm20F() else R_readF(if f == 0 then RegA() else RegB());
        let absolute = if f == 0 then Field(46, 1) == 0b1 else Field(49, 1) == 0b1;
        let negative = if f == 0 then Field(48, 1) == 0b1 else Field(45, 1) == 0b1;
        let value1 = if absolute then FAbs(value0) else value0;
        let value = if negative then FNeg(value1) else value1;
        if f == 0 then {
            let r = match fn { 0 => FCos(value), 1 => FSin(value), 2 => FExp2(value), 3 => FLog2(value),
                               5 => FInvSqrt(value), 8 => FSqrt(value), 4 => FDiv(FConst(0x3f800000), value), _ => value };
            R_writeF(Dst(), FloatSaturate(r, UInt(Field(50, 1))))
        } else R_writeF(Dst(), value)
    }
}

/* FSWZADD: Each lane of a quad (0 the upper left, 1 the upper right, 2 the lower left, 3 the lower right) does the mode
   in its bits of the swizzle (shader_fswzadd_lanes): a + b (0), b - a (1), a - b (2) or b (3). Nothing crosses lanes:
   compilers read the neighbour with SHFL.BFLY first, and dFdx is FSWZADD of it and x with 0x99 (nouveau's handleDFDX,
   NAK's ddx). Nouveau's manual TXD fixes the order of the lanes and the modes: 0xcc adds the gradient in a to b on the
   right lanes and keeps b on the left ones. Eden and Ryujinx count the lanes from the low bits, and Eden negates every
   mode. The rounding mode (39-40) is round to nearest, as FADD's is, and NDV (38) changes nothing computed */
function execute_FSWZADD() -> unit = {
    let left0 = RegF(RegA());
    let right0 = RegF(RegB());
    let soft_ftz = Field(44, 1) == 0b1 & SoftFtz();
    let left = if soft_ftz then FloatFtz(left0) else left0;
    let right = if soft_ftz then FloatFtz(right0) else right0;
    /* without subgroups, the place FragCoord gives */
    let lane = if FswzMask(UInt(Field(28, 2))) != 15 then QuadLane() else 0x00000000;
    /* pick k: whether this lane's mode is k + 1, where some lanes' are and others' aren't */
    let m1 = FswzMask(1);
    let m2 = FswzMask(2);
    let m3 = FswzMask(3);
    let some1 = m1 != 0 & m1 != 15;
    let some2 = m2 != 0 & m2 != 15;
    let some3 = m3 != 0 & m3 != 15;
    let pick1 = if some1 then ((to_bits(32, m1) >> lane) & 0x00000001) != 0x00000000 else false;
    let pick2 = if some2 then ((to_bits(32, m2) >> lane) & 0x00000001) != 0x00000000 else false;
    let pick3 = if some3 then ((to_bits(32, m3) >> lane) & 0x00000001) != 0x00000000 else false;
    if m3 == 15 then R_writeF(Dst(), right) else {
        let lhs0 = if m1 == 15 then FNeg(left) else left;
        let lhs = if some1 then (if pick1 then FNeg(left) else left) else lhs0;
        let rhs0 = if m2 == 15 then FNeg(right) else right;
        let rhs = if some2 then (if pick2 then FNeg(right) else right) else rhs0;
        let sum = FAddNC(lhs, rhs);
        let flushed = if soft_ftz then FloatFtz(sum) else sum;
        R_writeF(Dst(), if some3 then (if pick3 then right else flushed) else flushed)
    }
}

/* the half instructions: HADD2, HMUL2, HFMA2 and their forms. Each form's operands and fields, as Eden's
   half_floating_point_*.cpp decode them. A constant buffer operand is one F32 for both halves (swizzle 1). The 32I forms
   write both halves (no merge), take A's swizzle at 53, and HFMA2_32I adds Rd. FTZ isn't modelled: the halves are
   computed as F32 */
function execute_HARITH() -> unit = {
    let f = Form(); /* 0-3 HADD2 R, C, I, 32I; 4-7 HMUL2 R, C, I, 32I; 8-12 HFMA2 R, RC, CR, I, 32I */
    let fma = f >= 8;
    let multiply = f >= 4 & f <= 7;
    let imm32 = f == 3 | f == 7 | f == 12;
    let g = if f <= 7 then MOD(f, 4) else f; /* the HADD2 and HMUL2 forms alike */
    let raw_b = if g == 0 | f == 8 then R_read(RegB()) else if g == 1 then CbufWord() else if g == 2 | f == 11 then HalfImmediate()
                else if g == 3 | f == 12 then Bits32(Field(20, 32)) else if f == 9 then R_read(UInt(Field(39, 8))) else CbufWord();
    let swizzle_b = if g == 0 | f == 8 then UInt(Field(28, 2)) else if g == 1 then 1 else if f == 9 then UInt(Field(53, 2))
                    else if f == 10 then 1 else 0;
    let abs_b = if g == 0 & not_bool(fma) then UInt(Field(30, 1)) else if g == 1 & not_bool(fma) then UInt(Field(54, 1)) else 0;
    let neg_b = if g == 0 & not_bool(fma) then UInt(Field(31, 1)) else if f == 1 then UInt(Field(56, 1))
                else if f == 8 then UInt(Field(31, 1)) else if f == 9 | f == 10 then UInt(Field(56, 1)) else 0;
    let raw_c = if f == 8 then R_read(UInt(Field(39, 8))) else if f == 9 then CbufWord()
                else if f == 10 | f == 11 then R_read(UInt(Field(39, 8))) else if f == 12 then R_read(Dst()) else 0x00000000;
    let swizzle_c = if f == 8 then UInt(Field(35, 2)) else if f == 9 then 1 else if f == 10 | f == 11 then UInt(Field(53, 2)) else 0;
    let neg_c = if f == 8 then UInt(Field(30, 1)) else if f >= 9 & f <= 11 then UInt(Field(51, 1)) else if f == 12 then UInt(Field(52, 1)) else 0;
    let saturate = if g == 0 & not_bool(fma) then UInt(Field(32, 1)) else if f == 8 then UInt(Field(32, 1))
                   else if f == 12 then 0 else UInt(Field(52, 1));
    let precision = if multiply then UInt(Field(if imm32 then 55 else 39, 2)) else if f == 8 then UInt(Field(37, 2))
                    else if f >= 9 & f <= 11 then UInt(Field(57, 2)) else if f == 12 then UInt(Field(55, 2)) else 0;
    let neg_a = if f == 3 then UInt(Field(56, 1)) else if not_bool(fma) & not_bool(imm32) then (if f == 4 then 0 else UInt(Field(43, 1))) else 0;
    let abs_a = if not_bool(fma) & not_bool(imm32) & f != 3 then UInt(Field(44, 1)) else 0;
    let lhs = HalfPairMod(HalfPairOf(R_read(RegA()), UInt(Field(if imm32 then 53 else 47, 2))), abs_a, neg_a);
    let rhs = HalfPairMod(HalfPairOf(raw_b, swizzle_b), abs_b, neg_b);
    let pair : half2 = if fma then {
        let addend = HalfPairMod(HalfPairOf(raw_c, swizzle_c), 0, neg_c);
        let p = Half2FmaNC(lhs, rhs, addend);
        if precision == 2 then HalfFmzFma(p, lhs, rhs, addend) else p
    } else if multiply then {
        let p = Half2MulNC(lhs, rhs);
        if precision == 2 then HalfFmz(p, lhs, rhs) else p
    } else Half2AddNC(lhs, rhs);
    R_write(Dst(), HalfPairResult(pair, R_read(Dst()), if imm32 then 0 else UInt(Field(49, 2)), saturate))
}

/* HSET2: each half pair compared (35-38, else 49-52), combined with the predicate at 39, as 1.0 (49, else 53) or all
   ones in its half */
function execute_HSET2() -> unit = {
    let f = Form();
    let raw_b = if f == 0 then R_read(RegB()) else if f == 1 then CbufWord() else HalfImmediate();
    let lhs = HalfPairMod(HalfPairOf(R_read(RegA()), UInt(Field(47, 2))), UInt(Field(44, 1)), UInt(Field(43, 1)));
    let rhs = HalfPairMod(HalfPairOf(raw_b, if f == 0 then UInt(Field(28, 2)) else if f == 1 then 1 else 0),
                          if f == 0 then UInt(Field(30, 1)) else 0,
                          if f == 0 then UInt(Field(31, 1)) else if f == 1 then UInt(Field(56, 1)) else 0);
    let predicate = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let compare = UInt(Field(if f == 0 then 35 else 49, 4));
    let truth = if Field(if f == 0 then 49 else 53, 1) == 0b1 then 15360 else 65535; /* 0x3c00, 0xffff */
    var result : bits(32) = 0x00000000;
    foreach (lane from 0 to 1) {
        let left = Half2At(lhs, lane);
        let right = Half2At(rhs, lane);
        let test0 = FCompare(compare, left, right);
        let test = Combine53(UInt(Field(45, 2)), test0, predicate);
        let bits = RawSelect(test, to_bits(32, truth * 65536 ^ lane), 0x00000000);
        result = result | bits
    };
    R_write(Dst(), result)
}

/* HSETP2: each half pair compared, combined with the predicate at 39: into the predicates at 3 and 0, or with bit 49
   (53) their AND and its negation */
function execute_HSETP2() -> unit = {
    let f = Form();
    let raw_b = if f == 0 then R_read(RegB()) else if f == 1 then CbufWord() else HalfImmediate();
    let lhs = HalfPairMod(HalfPairOf(R_read(RegA()), UInt(Field(47, 2))), UInt(Field(44, 1)), UInt(Field(43, 1)));
    let rhs = HalfPairMod(HalfPairOf(raw_b, if f == 0 then UInt(Field(28, 2)) else if f == 1 then 1 else 0),
                          if (f == 1 & Field(54, 1) == 0b1) | (f == 0 & Field(30, 1) == 0b1) then 1 else 0,
                          if f == 0 then UInt(Field(31, 1)) else if f == 1 then UInt(Field(56, 1)) else 0);
    let predicate = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let compare = UInt(Field(if f == 0 then 35 else 49, 4));
    let o0 = Combine53(UInt(Field(45, 2)), FCompare(compare, Half2At(lhs, 0), Half2At(rhs, 0)), predicate);
    let o1 = Combine53(UInt(Field(45, 2)), FCompare(compare, Half2At(lhs, 1), Half2At(rhs, 1)), predicate);
    if Field(if f == 0 then 49 else 53, 1) == 0b1 then {
        let both = RawAnd(o0, o1);
        PredWrite(UInt(Field(3, 3)), both);
        PredWrite(UInt(Field(0, 3)), RawNot(both))
    } else {
        PredWrite(UInt(Field(3, 3)), o0);
        PredWrite(UInt(Field(0, 3)), o1)
    }
}

/* FCHK: no float the console checks is out of the hardware's range here: the predicate at 3 true */
function execute_FCHK() -> unit = PredWrite(UInt(Field(3, 3)), true)
