/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM86's instructions in Sail (sailgen --gpu): what each one computes, from the fields the decoder (sm86.decode)
   leaves in its 128 bits and the operands it found. sm86.roots names the decoder's opcodes each function runs, and
   sm86_spirv.h is the state: the registers, uniform registers and predicates of sm86_translate.c, the operands as its
   src_bits reads them. The field numbers and what they mean are the hand-written translator's notes, NAK's (Mesa's
   nak) and CuAssembler's sm_86 records */

/* the operands an ALU instruction names: a, b, c, and Second, the operand of a two-source instruction (b, or c in the
   forms that keep a register in b's place) */
enum Operand = {A, B, C, Second}

/* bits lo to lo + n - 1 of the instruction, known when the translator runs */
val Field : forall 'n. (int, int('n)) -> bits('n)
/* the instruction is a uniform one (SM86_K_UALU): its destination a uniform register, its predicates uniform */
val Uniform : unit -> bool
/* the operand's 32 bits: a register, a uniform register, an immediate or a constant buffer word */
val SrcBits : Operand -> bits(32)
/* the operand is negated (a register's or a constant's: an immediate has no modifier) */
val SrcNeg : Operand -> bool
/* the operand reads zero whatever the state: the zero register or the immediate 0 */
val IsZero : Operand -> bool
/* the operand is an immediate, and its value */
val SrcIsImm : Operand -> bool
val SrcImm : Operand -> int
/* the destination, a register or a uniform register, written */
val D_write : bits(32) -> unit
/* the destination register the decoder found */
val Dst : unit -> int
/* register r (RZ reads 0) */
val R_read : int -> bits(32)
val R_write : (int, bits(32)) -> unit
/* predicate p, negated when neg is 1, of the uniform predicates when u is 1 (PT reads true) */
val PredRead : (int, int, int) -> bool
/* written unless p is PT */
val PredWrite : (int, bool, int) -> unit
/* A fusion's instructions (sm86.roots' fuse lines): the same of its instruction k, 0 the first */
val FieldAt : forall 'n. (int, int, int('n)) -> bits('n)
val SrcBitsAt : (int, Operand) -> bits(32)
val SrcNegAt : (int, Operand) -> bool
val IsZeroAt : (int, Operand) -> bool
/* the operand of instruction k is the register instruction j writes (none for RZ) */
val ReadsDstAt : (int, Operand, int) -> bool
val D_writeAt : (int, bits(32)) -> unit
/* an instruction the translation refuses: why */
val Unsupported : string -> unit

let PT : int = 7
let RZ : int = 255

/* the predicate operand at lo (its number) and lo + 3 (its negation) */
function PredAt(lo : int) -> bool = PredRead(UInt(Field(lo, 3)), UInt(Field(lo + 3, 1)), Uniform())

/* an integer operand with its negation, 0 - x */
function SrcInt(s : Operand) -> bits(32) = {
    let v = SrcBits(s);
    if SrcNeg(s) then 0x00000000 - v else v
}

/* a term of a 64-bit sum: its two's complement as NAK's fold reads it, ~x + 1 in 33 bits. With the carries in (.X)
   the modifier is the complement alone, ~x: the low word's sum gave the + 1 as a carry. One game's 64-bit
   subtractions are IADD3 a, b, -c then IADD3.X a', b', ~c', and with ~c' + 1 their high word was one too many */
function Term64(s : Operand, x : bool) -> bits(64) = {
    let v = SrcBits(s);
    if not_bool(SrcNeg(s)) then ZeroExtend(v, 64) else {
        let n = ZeroExtend(not_vec(v), 64);
        if x then n else n + 0x0000000000000001
    }
}

/* a carry in, the predicate at lo, as a 64-bit term */
function CarryIn(lo : int) -> bits(64) = if PredAt(lo) then 0x0000000000000001 else 0x0000000000000000

/* IADD3 and UIADD3: a + b + c, with the carries out in the predicates p (81) and q (84) and, for .X (74), two carries
   in (87 and 77) */
function execute_IADD3() -> unit = {
    let p = UInt(Field(81, 3));
    let q = UInt(Field(84, 3));
    let x = Field(74, 1) == 0b1;
    if p >= PT & q >= PT & not_bool(x) then {
        /* the sum of the terms that aren't zero, a register's or an immediate's whatever the state */
        var sum : bits(32) = 0x00000000;
        var any : bool = false;
        if not_bool(IsZero(A)) then {
            sum = SrcInt(A);
            any = true
        };
        if not_bool(IsZero(B)) then {
            if any then { sum = sum + SrcInt(B) } else { sum = SrcInt(B) };
            any = true
        };
        if not_bool(IsZero(C)) then { if any then { sum = sum + SrcInt(C) } else { sum = SrcInt(C) } };
        D_write(sum)
    } else {
        /* the sum in 64 bits: its carries, and .X's two carries in */
        var sum : bits(64) = Term64(A, x) + Term64(B, x);
        sum = sum + Term64(C, x);
        if x then {
            sum = sum + CarryIn(87);
            sum = sum + CarryIn(77)
        };
        let high = sum[63 .. 32];
        D_write(sum[31 .. 0]);
        PredWrite(p, 0 < UInt(high), Uniform());
        PredWrite(q, 1 < UInt(high), Uniform())
    }
}

/* IMAD and UIMAD: a * b + c. .X (bit 74) adds the carry predicate at 87-90, as IADD3.X does, and as there a negated
   addend is then its complement, ~c: one game's 64-bit division negates a high word with IMAD.X RZ, RZ, ~c, P */
function execute_IMAD() -> unit = {
    let x = Field(74, 1) == 0b1;
    /* a factor of zero (IMAD.MOV's RZ, RZ) leaves the addend; an addend of zero (IMAD.SHL's RZ) the product */
    let zero = IsZero(A) | IsZero(B);
    let product = if zero then 0x00000000 else SrcInt(A) * SrcInt(B);
    var v : bits(32) = product;
    if x | not_bool(IsZero(C)) then {
        let addend = if x & SrcNeg(C) then not_vec(SrcBits(C)) else SrcInt(C);
        if zero then { v = addend } else { v = v + addend }
    };
    if x then { v = v + (if PredAt(87) then 0x00000001 else 0x00000000) };
    D_write(v)
}

/* IMNMX and UIMNMX: the minimum of a and b where the predicate at 87 holds, else the maximum; signed with bit 73 */
function execute_IMNMX() -> unit = {
    let min = PredAt(87);
    let x = SrcBits(A);
    let y = SrcBits(B);
    if Field(73, 1) == 0b1 then
        D_write(to_bits(32, if min then Min(SInt(x), SInt(y)) else Max(SInt(x), SInt(y))))
    else
        D_write(to_bits(32, if min then Min(UInt(x), UInt(y)) else Max(UInt(x), UInt(y))))
}

/* FLO and UFLO: NAK's OpFlo, bit 73 signed, 74 the shift amount (31 - the bit) in place of the bit, 63 the source
   inverted. No 1 gives 0xffffffff, as FindUMsb's -1 */
function execute_FLO() -> unit = {
    var x : bits(32) = SrcBits(B);
    if Field(63, 1) == 0b1 then { x = not_vec(x) };
    let bit = if Field(73, 1) == 0b1 then HighestSignBit32(x) else HighestSetBit32(x);
    if Field(74, 1) == 0b1 then D_write(if bit == 0xffffffff then bit else 0x0000001f - bit) else D_write(bit)
}

/* POPC and UPOPC: the bits set of b, inverted first with bit 63 */
function execute_POPC() -> unit = {
    var x : bits(32) = SrcBits(B);
    if Field(63, 1) == 0b1 then { x = not_vec(x) };
    D_write(PopCount(x))
}

/* BREV and UBREV */
function execute_BREV() -> unit = D_write(BitReverse32(SrcBits(B)))

/* IABS */
function execute_IABS() -> unit = D_write(to_bits(32, Abs(SInt(SrcBits(B)))))

/* MOV: b */
function execute_MOV() -> unit = R_write(Dst(), SrcBits(B))

/* SEL and USEL: a where the predicate at 87 holds, else b */
function execute_SEL() -> unit = {
    let p = PredAt(87);
    let x = SrcBits(A);
    let y = SrcBits(B);
    D_write(if p then x else y)
}

/* LEA and ULEA: (a << shift) + b, with .HI (80) the high word of (c:a) << shift, bit 72 the shifted value negated, the
   carry out in the predicate at 81 and .X (74) the carry in at 87 */
function execute_LEA() -> unit = {
    let shift = UInt(Field(75, 5));
    let p = UInt(Field(81, 3));
    let x = Field(74, 1) == 0b1;
    let a = SrcBits(A);
    var shifted : bits(32) = if Field(80, 1) == 0b1 then
                                 (if shift != 0 then (SrcBits(C) << shift) | (a >> (32 - shift)) else SrcBits(C))
                             else a << shift;
    if Field(72, 1) == 0b1 then { shifted = 0x00000000 - shifted };
    if p >= PT & not_bool(x) then D_write(shifted + SrcInt(B)) else {
        var sum : bits(64) = ZeroExtend(shifted, 64) + ZeroExtend(SrcInt(B), 64);
        if x then { sum = sum + CarryIn(87) };
        D_write(sum[31 .. 0]);
        PredWrite(p, 0 < UInt(sum[63 .. 32]), Uniform())
    }
}

/* LOP3 and ULOP3: the truth table at 72 of a, b and c, and whether the result is not zero in the predicate at 81. A
   predicate operand (80, 87-90) is refused */
function execute_LOP3() -> unit = {
    if Field(80, 1) == 0b1 | Field(87, 4) != 0xf then throw(Unsupported("LOP3 with a predicate operand"));
    let v = Lut3(Field(72, 8), SrcBits(A), SrcBits(B), SrcBits(C));
    D_write(v);
    PredWrite(UInt(Field(81, 3)), v != 0x00000000, Uniform())
}

/* SHF and USHF: the funnel shift of (c:a) by b, right with bit 76 (arithmetic for the signed types at 73-74), its
   high word with .HI (80); the amount wraps at the width with bit 75, else saturates. A shift by 64 is out of
   SPIR-V's range: split in two */
/* SHF of a 32-bit type by an immediate (one game's are nearly all so: SHF.R.U32.HI d, RZ, s, x): (c:a) shifted by
   s, known when the translator runs, of 32-bit words. Its high word or low word is a word shifted, or two words'
   bits put together; s 0 and 32 take a word whole (a shift by 32 is out of SPIR-V's range) */
function Shf32(right : bool, is_signed : bool, high : bool, s : int) -> unit = {
    if s == 0 then D_write(if high then SrcBits(C) else SrcBits(A))
    else if right & high then {
        let c = SrcBits(C);
        if s == 32 then D_write(if is_signed then ASR(c, 31) else 0x00000000)
        else D_write(if is_signed then ASR(c, s) else c >> s)
    } else if not_bool(right) & not_bool(high) then {
        if s == 32 then D_write(0x00000000) else D_write(SrcBits(A) << s)
    } else if s == 32 then D_write(if high then SrcBits(A) else SrcBits(C))
    else {
        /* the low word of a right shift (a's bits down, c's in from above), or the high word of a left shift (c's
           bits up, a's in from below); a word that reads zero adds nothing */
        let from = if right then 32 - s else s;
        let up = if IsZero(C) then 0x00000000 else SrcBits(C) << from;
        let down = if IsZero(A) then 0x00000000 else SrcBits(A) >> (32 - from);
        if IsZero(A) then D_write(up) else if IsZero(C) then D_write(down) else D_write(up | down)
    }
}

function execute_SHF() -> unit = {
    let dtype = UInt(Field(73, 2));
    if dtype >= 2 & SrcIsImm(B) then {
        let b = SrcImm(B);
        Shf32(Field(76, 1) == 0b1, dtype == 2, Field(80, 1) == 0b1, if Field(75, 1) == 0b1 then MOD(b, 32) else Min(b, 32))
    } else Shf(dtype)
}

function Shf(dtype : int) -> unit = {
    let width = if dtype < 2 then 64 else 32;
    let right = Field(76, 1) == 0b1;
    let is_signed = dtype == 0 | dtype == 2;
    var s : bits(32) = SrcBits(B);
    if Field(75, 1) == 0b1 then { s = s & to_bits(32, width - 1) } else { s = to_bits(32, Min(UInt(s), width)) };
    let lo = SrcBits(A);
    let hi = SrcBits(C);
    let value = hi @ lo;
    let s64 = ZeroExtend(s, 64);
    let big = 63 < UInt(s);
    let s_part = if big then 0x000000000000003f else s64;
    let extra = if big then 0x0000000000000001 else 0x0000000000000000;
    let shifted : bits(64) = if not_bool(right) then LSL(LSL(value, s_part), extra)
                             else if is_signed then ASR(ASR(value, s_part), extra)
                             else LSR(LSR(value, s_part), extra);
    if Field(80, 1) == 0b1 then D_write(shifted[63 .. 32]) else D_write(shifted[31 .. 0])
}

/* NAK's IntCmpOp at 76-78: false, <, ==, <=, >, !=, >=, true */
function IntCompare(op : bits(3), is_signed : bool, x : bits(32), y : bits(32)) -> bool =
    match op {
        0b000 => false,
        0b111 => true,
        0b001 => if is_signed then SInt(x) < SInt(y) else UInt(x) < UInt(y),
        0b010 => x == y,
        0b011 => if is_signed then SInt(x) <= SInt(y) else UInt(x) <= UInt(y),
        0b100 => if is_signed then SInt(x) > SInt(y) else UInt(x) > UInt(y),
        0b101 => x != y,
        _ => if is_signed then SInt(x) >= SInt(y) else UInt(x) >= UInt(y)
    }

/* the predicates' combination at 74-75 (FSETP's, ISETP's): and, or, xor */
function Combine(set_op : bits(2), x : bool, acc : bool) -> bool =
    match set_op {
        0b00 => x & acc,
        0b01 => x | acc,
        _ => x != acc
    }

/* ISETP and UISETP: a and b compared (76-78, signed with 73), combined (74) with the predicate at 87 into the one at
   81, and its negation into the one at 84. No negation: bit 72, a's negation in the decoder's fields, is .EX (NAK's
   OpISetP, whose sources take no modifiers). .EX: equal high words take the low words' comparison, the predicate at
   68 */
function execute_ISETP() -> unit = {
    let op = Field(76, 3);
    var cmp : bool = IntCompare(op, Field(73, 1) == 0b1, SrcBits(A), SrcBits(B));
    if Field(72, 1) == 0b1 then {
        let low = PredAt(68);
        let equal = SrcBits(A) == SrcBits(B);
        if op != 0b000 & op != 0b111 then { cmp = (if equal then low else cmp) }
    };
    let acc = PredAt(87);
    let set_op = Field(74, 2);
    PredWrite(UInt(Field(81, 3)), Combine(set_op, cmp, acc), Uniform());
    PredWrite(UInt(Field(84, 3)), Combine(set_op, not_bool(cmp), acc), Uniform())
}

/* PLOP3 and UPLOP3: two truth tables of the predicates at 87, 77 and 68 (the last a uniform one with bit 67), into the
   predicates at 81 (the table at 64-66 and 72-76) and 84 (at 16-23). All of UPLOP3's predicates are uniform (NAK) */
function Plop3(u : int) -> unit = {
    let x = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), u);
    let y = PredRead(UInt(Field(77, 3)), UInt(Field(80, 1)), u);
    let z = PredRead(UInt(Field(68, 3)), UInt(Field(71, 1)), if u == 1 | Field(67, 1) == 0b1 then 1 else 0);
    let lut0 = Field(72, 5) @ Field(64, 3);
    let lut1 = Field(16, 8);
    let p = UInt(Field(81, 3));
    let q = UInt(Field(84, 3));
    if p < PT then PredWrite(p, Lut3Bool(lut0, x, y, z), u);
    if q < PT then PredWrite(q, Lut3Bool(lut1, x, y, z), u)
}
function execute_PLOP3() -> unit = Plop3(0)
function execute_UPLOP3() -> unit = Plop3(1)

/* IADD3 lo, P, a, b, RZ then IADD3.X hi, a', b', RZ, P, !PT: the 64-bit sum (a':a) + (b':b), or with b and b' negated
   the difference, as NVIDIA's compiler adds a 64-bit offset to an address. Each instruction's sum is made in 64 bits
   for its carries; the pair is one 32-bit add, its carry, and the high words' add, with the same results: lo, the
   carry in P, hi. The low word's write must leave the high word's sources as they were */
function IADD3_WIDE_ok() -> bool = {
    let p = UInt(FieldAt(0, 81, 3));
    FieldAt(0, 74, 1) == 0b0 & p < PT & UInt(FieldAt(0, 84, 3)) == PT & IsZeroAt(0, C) & not_bool(SrcNegAt(0, C))
    & FieldAt(1, 74, 1) == 0b1 & UInt(FieldAt(1, 87, 3)) == p & FieldAt(1, 90, 1) == 0b0
    & FieldAt(1, 77, 4) == 0xf & UInt(FieldAt(1, 81, 3)) == PT & UInt(FieldAt(1, 84, 3)) == PT
    & IsZeroAt(1, C) & not_bool(SrcNegAt(1, C))
    & not_bool(SrcNegAt(0, A)) & not_bool(SrcNegAt(1, A)) & SrcNegAt(0, B) == SrcNegAt(1, B)
    & not_bool(ReadsDstAt(1, A, 0)) & not_bool(ReadsDstAt(1, B, 0))
}
function IADD3_WIDE() -> unit = {
    let a0 = SrcBitsAt(0, A);
    let b0 = SrcBitsAt(0, B);
    let a1 = SrcBitsAt(1, A);
    let b1 = SrcBitsAt(1, B);
    let sub = SrcNegAt(0, B);
    let lo = if sub then a0 - b0 else a0 + b0;
    /* the low word's carry: no borrow of a subtraction, a sum that wrapped */
    let carry = if sub then UInt(a0) >= UInt(b0) else UInt(lo) < UInt(a0);
    let hi = a1 + (if sub then not_vec(b1) else b1) + (if carry then 0x00000001 else 0x00000000);
    D_writeAt(0, lo);
    PredWrite(UInt(FieldAt(0, 81, 3)), carry, 0);
    D_writeAt(1, hi)
}

/* ---- floating point ---- */

/* the operand as a float, with its absolute value and negation (an immediate has neither) */
val SrcF : Operand -> bits(32)
/* the operand takes its absolute value (an immediate does not) */
val SrcAbs : Operand -> bool
/* the decoder's form (bits 9-11), which places the sources */
val Form : unit -> int
/* the modifiers as the decoder left them, an immediate's too (MUFU.F16's and F2I's half) */
val SrcAbsRaw : Operand -> bool
val SrcNegRaw : Operand -> bool
/* the operand is a register, and which */
val SrcIsReg : Operand -> bool
val SrcReg : Operand -> int
/* the instruction's name, for a refusal */
val OpName : unit -> string
/* x flushed to the zero of its sign where it is subnormal, when on and the module does not flush by itself
   (sm86_to_spirv_ftz's soft_ftz); noted in the program's info as the FTZ it asks for */
val Ftz : (bits(32), int) -> bits(32)
val NoteFtz : int -> unit
/* the rounding modes' sums and products (78-79: 0 nearest, 1 down, 2 up, 3 toward zero), FMUL's scaled by 2^(scale - 4) */
val FloatAdd : (int, bits(32), bits(32)) -> bits(32)
val FloatMul : (int, int, bits(32), bits(32)) -> bits(32)
val FloatFma : (int, bits(32), bits(32), bits(32)) -> bits(32)
/* .SAT: clamped to 0 to 1, a NaN 0 */
val Saturate : (bits(32), int) -> bits(32)
/* x decorated NoContraction (a sum the device must not fuse) */
val NoContract : bits(32) -> bits(32)
/* the FMUL being written as a perspective's attribute times w, taken whole (fmul_over_w): its value, 0 for none */
val FmulOverW : unit -> int
val ValueF : int -> bits(32)
/* a system value (S2R's numbering): the lane, the thread ids... */
val SystemValue : int -> bits(32)
/* a pair of halves to the word PackHalf2x16 makes, as the console converts (pack_half) */
val PackHalf : half2 -> bits(32)
/* a float's bfloat16, rounded to nearest */
val Bf16 : bits(32) -> bits(32)
/* a 32-bit integer's float to nearest, as the device's conversion might not (spv_emit_int_to_f32_nearest) */
val IntToF32Nearest : (bits(32), int) -> bits(32)
/* where the device has doubles; where a conversion to f32 is replaced by IntToF32Nearest (1 signed, 2 unsigned) */
val Float64 : unit -> bool
val I2fNearest : unit -> int
/* an integer's double, and that double rounded to an f32 by the mode */
val ToF64 : (bits(32), int) -> f64
val RoundF64 : (int, f64) -> bits(32)

enum FOp = {OpFADD, OpFMUL, OpFFMA}

/* FADD, FMUL and FFMA: bit 77 saturate, 78-79 the rounding mode, 80 FTZ, and FMUL's 84-86 the scale 2^(field - 4),
   which nvdisasm names .D4 (2), .D2 (3) and none (4). An FMUL fmul_over_w takes is its value */
function FArith(op : FOp) -> unit = {
    let on = UInt(Field(80, 1));
    let mode = UInt(Field(78, 2));
    let scale = UInt(Field(84, 3));
    NoteFtz(on);
    let attr = match op { OpFMUL => FmulOverW(), _ => 0 };
    let v : bits(32) = if attr != 0 then ValueF(attr) else {
        let x = Ftz(SrcF(A), on);
        match op {
            OpFADD => FloatAdd(mode, x, Ftz(SrcF(Second), on)),
            OpFMUL => {
                if scale == 0 then throw(Unsupported("FMUL with scale field 0"));
                FloatMul(mode, scale, x, Ftz(SrcF(B), on))
            },
            _ => {
                let y = Ftz(SrcF(B), on);
                FloatFma(mode, x, y, Ftz(SrcF(C), on))
            }
        }
    };
    D_write(Bits32(Saturate(Ftz(v, on), UInt(Field(77, 1)))))
}
function execute_FADD() -> unit = FArith(OpFADD)
function execute_FMUL() -> unit = FArith(OpFMUL)
function execute_FFMA() -> unit = FArith(OpFFMA)

/* FSWZADD: a at 24, b at 64, the swizzle at 32-39. Each lane of a quad does the mode in its two bits of the swizzle,
   lane L's at bits 6 - 2L (lane 0 the upper left), as Maxwell's FSWZADD: a + b (0), b - a (1), a - b (2) or b (3).
   CuAssembler's sm_86 names show the order: 0xcc is ZPPPZPPP, b alone in lanes 0 and 2. Bit 77 is NDV, which
   changes nothing computed. The other bits are refused */
function execute_FSWZADD() -> unit = {
    if (Field(72, 19) & not_vec(0b0000000000000100000)) != 0b0000000000000000000 then
        throw(Unsupported("FSWZADD with modifiers 0x%x", Field(72, 19)));
    let swizzle = Field(32, 8);
    let a = Float32(R_read(UInt(Field(24, 8))));
    let b = Float32(R_read(UInt(Field(64, 8))));
    Capability(61);
    let lane = SystemValue(0) & 0x00000003;
    let shift = 0x00000006 - (lane << 1);
    let mode = (ZeroExtend(swizzle, 32) >> shift) & 0x00000003;
    let x = if mode == 0x00000001 then FNeg(a) else a;
    let y = if mode == 0x00000002 then FNeg(b) else b;
    let sum = NoContract(FAdd(x, y));
    D_write(Bits32(if mode == 0x00000003 then b else sum))
}

/* FMNMX: the minimum of a and b where the predicate at 87 holds, else the maximum, FTZ with bit 80 */
function execute_FMNMX() -> unit = {
    let min = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), 0);
    let on = UInt(Field(80, 1));
    let x = Ftz(SrcF(A), on);
    let y = Ftz(SrcF(B), on);
    let lo = FMinN(x, y);
    let hi = FMaxN(x, y);
    D_write(Bits32(if min then lo else hi))
}

/* FSET: 1.0 where a and b compare (76-79) combined (74) with the predicate at 87, else 0.0 */
function execute_FSET() -> unit = {
    let on = UInt(Field(80, 1));
    let x = Ftz(SrcF(A), on);
    let cmp = FCompare(UInt(Field(76, 4)), x, Ftz(SrcF(B), on));
    let acc = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), 0);
    let r = Combine(Field(74, 2), cmp, acc);
    D_write(Bits32(if r then FConst(0x3f800000) else FConst(0x00000000)))
}

/* FSETP: a and b compared (76-79), combined (74) with the predicate at 87 into the one at 81, and its negation into
   the one at 84 */
function execute_FSETP() -> unit = {
    let on = UInt(Field(80, 1));
    let x = Ftz(SrcF(A), on);
    let cmp = FCompare(UInt(Field(76, 4)), x, Ftz(SrcF(B), on));
    let acc = PredAt(87);
    let set_op = Field(74, 2);
    PredWrite(UInt(Field(81, 3)), Combine(set_op, cmp, acc), Uniform());
    PredWrite(UInt(Field(84, 3)), Combine(set_op, not_bool(cmp), acc), Uniform())
}

/* the operand of a 16-bit instruction as a pair of halves: its swizzle sw (0 both, 1 a float, 2 the low half in both,
   3 the high one), then its absolute value and negation (an immediate has neither) */
function HalfOperand(s : Operand, sw : int, what : string) -> half2 = {
    if sw == 1 then throw(Unsupported("%s with a float operand", what));
    let pair = UnpackHalf2(SrcBits(s));
    let swizzled = if sw >= 2 then {
        let h = Half2At(pair, sw - 2);
        Half2(h, h)
    } else pair;
    let absed = if SrcAbs(s) then Half2Abs(swizzled) else swizzled;
    if SrcNeg(s) then Half2Neg(absed) else absed
}

/* HSETP2: the halves of a and of b, each pair compared by the operation at 76-79 (FSETP's codes), then each with the
   predicate at 87-90 by the operation at 69-70: the low halves into the predicate at 81-83, the high ones into 84-86.
   The forms of the 16-bit ALU, as CuAssembler's sm_86 encodings show them: b a register at 32 (1), two halves at
   32-63 (2), a constant buffer (3). a's swizzle is at 74-75 and b's at 60-61 (HADD2's codes). FTZ (80) and the other
   bits of 64-71, which no program sets, are refused */
function execute_HSETP2() -> unit = {
    if Form() > 3 | Field(80, 1) != 0b0 | Field(64, 5) != 0b00000 | Field(71, 1) != 0b0 then
        throw(Unsupported("HSETP2 of form %u with modifiers 0x%x", Form(), Field(64, 17)));
    let swa = UInt(Field(74, 2));
    let swb = if SrcIsImm(Second) then 0 else UInt(Field(60, 2));
    let va = HalfOperand(A, swa, "HSETP2");
    let vb = HalfOperand(Second, swb, "HSETP2");
    let acc = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), 0);
    let set_op = Field(69, 2);
    foreach (k from 0 to 1) {
        let cmp = FCompare(UInt(Field(76, 4)), Half2At(va, k), Half2At(vb, k));
        PredWrite(UInt(Field(81 + 3 * k, 3)), Combine(set_op, cmp, acc), 0)
    }
}

/* FSEL: a where the predicate at 87 holds, else b, as floats */
function execute_FSEL() -> unit = {
    let p = PredAt(87);
    let x = Bits32(SrcF(A));
    let y = Bits32(SrcF(B));
    D_write(if p then x else y)
}

/* the half a 16-bit source names (bit 60), with b's modifiers: MUFU.F16's and F2I's */
function HalfOf(s : Operand) -> bits(32) = {
    let x = Half2At(UnpackHalf2(SrcBits(s)), UInt(Field(60, 1)));
    let absed = if SrcAbsRaw(s) then FAbs(x) else x;
    if SrcNegRaw(s) then FNeg(absed) else absed
}

/* MUFU: the function at 74-79 of b. .F16 (bit 73): the operand is the half bit 60 names, with b's modifiers, and the
   result the low half */
function execute_MUFU() -> unit = {
    let half = Field(73, 1) == 0b1;
    let fn = UInt(Field(74, 6));
    if half & Field(61, 1) == 0b1 then throw(Unsupported("MUFU.F16 with a swizzle"));
    let x = if half then HalfOf(B) else SrcF(B);
    let v : bits(32) = match fn {
        0 => FCos(FMul(x, FConst(0x40c90fdb))), /* the argument in turns, as the hardware takes it */
        1 => FSin(FMul(x, FConst(0x40c90fdb))),
        2 => FExp2(x),
        3 => FLog2(x),
        4 => FDiv(FConst(0x3f800000), x),
        5 => FInvSqrt(x),
        8 => FSqrt(x),
        9 => FTanh(x),
        _ => throw(Unsupported("MUFU function %u", fn))
    };
    if half then D_write(PackHalf(Half2(v, FConst(0x00000000)))) else D_write(Bits32(v))
}

/* F2F: NAK's log2 of the destination's bytes at 75, of the source's at 84, the rounding at 78 and FTZ at 80. One
   game's programs only narrow a float to the low half, rounding to nearest */
function execute_F2F() -> unit = {
    let dsize = UInt(Field(75, 2));
    let ssize = UInt(Field(84, 2));
    let rnd = UInt(Field(78, 2));
    if dsize != 1 | ssize != 2 | rnd != 0 then
        throw(Unsupported("F2F from %u to %u bytes, rounding %u", 1 << ssize, 1 << dsize, rnd));
    D_write(PackHalf(Half2(SrcF(B), FConst(0x00000000))))
}

/* F2I's fields (NAK), to a 64-bit integer: the register pair dst, dst + 1. One game's compute programs have
   F2I.U64.F32.TRUNC, the only form here: the hardware saturates, and a NaN gives 0 */
function execute_F2I64() -> unit = {
    let dsize = UInt(Field(75, 2));
    let ssize = UInt(Field(84, 2));
    if dsize != 3 | ssize != 2 | Field(72, 1) != 0b0 then
        throw(Unsupported("F2I.64 of sizes %u, %u, signed %u", ssize, dsize, Field(72, 1)));
    let x = FRound(UInt(Field(78, 2)), SrcF(B));
    let nan = FIsNaN(x);
    let high = FCompare(6, x, FConst(0x5f800000));
    /* the largest float under 2^64 keeps the conversion in range where the result is not taken */
    let clamped = FMinN(FMaxN(x, FConst(0x00000000)), FConst(0x5f7fffff));
    let u = FToU64(clamped);
    let v = if nan then 0x0000000000000000 else if high then 0xffffffffffffffff else u;
    R_write(Dst(), v[31 .. 0]);
    if Dst() < RZ then R_write(Dst() + 1, v[63 .. 32])
}

/* I2F's fields, from a 64-bit integer in the register pair b, b + 1: the same programs' I2F.F32.U64, to the nearest
   and upward. The conversion rounds as the device does, so for a direction the result is stepped to the float on the
   asked side of the integer when it is on the other */
function execute_I2F64() -> unit = {
    let dsize = UInt(Field(75, 2));
    let ssize = UInt(Field(84, 2));
    let mode = UInt(Field(78, 2));
    if dsize != 2 | ssize != 3 | Field(74, 1) != 0b0 | not_bool(SrcIsReg(B)) then
        throw(Unsupported("I2F.64 of sizes %u, %u, signed %u", ssize, dsize, Field(74, 1)));
    let lo_reg = SrcReg(B);
    let hi_reg = if lo_reg + 1 < RZ then lo_reg + 1 else RZ;
    let lo = R_read(lo_reg);
    let hi = R_read(hi_reg);
    let x = hi @ lo;
    let r = U64ToF(x);
    if mode == 0 then D_write(Bits32(r)) else {
        let big = FCompare(6, r, FConst(0x5f800000));
        let back = FToU64(FMinN(r, FConst(0x5f7fffff)));
        let bits = Bits32(r);
        if mode == 2 then {
            /* up: under the integer, and not 2^64 itself */
            let under = UInt(back) < UInt(x);
            let step = if under then 0x00000001 else 0x00000000;
            let step2 = if big then 0x00000000 else step;
            D_write(Bits32(Float32(bits + step2)))
        } else {
            /* down, toward zero: over the integer, as 2^64 is */
            let over = UInt(x) < UInt(back);
            let step = if over then 0x00000001 else 0x00000000;
            let step2 = if big then 0x00000001 else step;
            D_write(Bits32(Float32(bits - step2)))
        }
    }
}

/* F2I: NAK's log2 of the destination's bytes at 75 and of the source's at 84, signed with 72, the rounding at 78. A
   half's source is the one bit 60 names, with b's modifiers, as MUFU.F16's. The hardware saturates, and a NaN gives
   0; an 8- or 16-bit result saturates to its range, then fills the register sign or zero extended */
function execute_F2I() -> unit = {
    let dsize = UInt(Field(75, 2));
    let ssize = UInt(Field(84, 2));
    if dsize == 3 | (ssize != 1 & ssize != 2) then throw(Unsupported("F2I of sizes %u, %u", ssize, dsize));
    let from = if ssize == 1 then HalfOf(B) else SrcF(B);
    let x = FRound(UInt(Field(78, 2)), from);
    let is_signed = Field(72, 1) == 0b1;
    let nan = FIsNaN(x);
    let v : bits(32) = if dsize < 2 then {
        let lo = if dsize == 0 then (if is_signed then 0xc3000000 else 0x00000000)
                 else (if is_signed then 0xc7000000 else 0x00000000);
        let hi = if dsize == 0 then (if is_signed then 0x42fe0000 else 0x437f0000)
                 else (if is_signed then 0x46fffe00 else 0x477fff00);
        let clamped = FMinN(FMaxN(x, FConst(lo)), FConst(hi));
        if is_signed then Bits32(FToS32(clamped)) else FToU32(clamped)
    } else if is_signed then {
        let high = FCompare(6, x, FConst(0x4f000000));
        let clamped = FMaxN(x, FConst(0xcf000000));
        let s = Bits32(FToS32(clamped));
        if high then 0x7fffffff else s
    } else {
        let high = FCompare(6, x, FConst(0x4f800000));
        let clamped = FMaxN(x, FConst(0x00000000));
        let u = FToU32(clamped);
        if high then 0xffffffff else u
    };
    D_write(if nan then 0x00000000 else v)
}

/* F2IP.U8.F32.NTZ d, a, b, c: the floats a and b each rounded to an integer and saturated to 8 bits, a NaN 0 (.NTZ),
   packed as PTX's cvt.pack packs I2IP's: d = c << 16 | a' << 8 | b'. Bits 72-79 (CuAssembler's sm_86 records): 0x04
   .U8.F32.NTZ, bit 76 .S8 (-128 to 127), bit 75 .RELU (0 to 127). Bits 78-79 are the rounding, in F2I's place and
   order: CuAssembler records none but the nearest, and one game's mesh programs give 3 to F2IP.U8 of a float times
   255, as a shader's uint() truncates. Its compute programs pack colours with F2IP.U8 then PRMT */
function F2ipPart(r : int, s8 : bool, lo : bits(32), hi : bits(32), rounding : int) -> bits(32) = {
    let x = Float32(R_read(r));
    let nan = FIsNaN(x);
    let rounded = FRound(rounding, x);
    let clamped = FMinN(FMaxN(rounded, FConst(lo)), FConst(hi));
    let i = if s8 then Bits32(FToS32(clamped)) else FToU32(clamped);
    let byte = i & 0x000000ff;
    if nan then 0x00000000 else byte
}
function execute_F2IP() -> unit = {
    let flags = Field(72, 8);
    if (flags & not_vec(0xd8)) != 0x04 then throw(Unsupported("F2IP with flags 0x%x", flags));
    let s8 = flags[4] == 0b1;
    let relu = flags[3] == 0b1;
    let lo = if s8 & not_bool(relu) then 0xc3000000 else 0x00000000;
    let hi = if s8 then 0x42fe0000 else 0x437f0000;
    let p0 = F2ipPart(UInt(Field(24, 8)), s8, lo, hi, UInt(flags[7 .. 6]));
    let p1 = F2ipPart(UInt(Field(32, 8)), s8, lo, hi, UInt(flags[7 .. 6]));
    let v = (p0 << 8) | p1;
    R_write(Dst(), (R_read(UInt(Field(64, 8))) << 16) | v)
}

/* I2F and I2FP: the rounding mode in 78-79, a 32-bit integer being exact in a double. An 8- or 16-bit source (84-85)
   is the byte (.B1 to .B3) or halfword (.H1) that 60-61 select (CuAssembler sm_86). A device without doubles
   converts to a float, which a 32-bit integer keeps no more exactly: it rounds to nearest, as the mode 0 conversion
   does */
function execute_I2F() -> unit = {
    let src_size = UInt(Field(84, 2));
    if Field(75, 2) != 0b10 | src_size == 3 then
        throw(Unsupported("%s of sizes %u, %u", OpName(), src_size, Field(75, 2)));
    let b = SrcBits(B);
    let is_signed = Field(74, 1) == 0b1;
    let x = if src_size < 2 then {
        let width = if src_size == 1 then 16 else 8;
        if is_signed then Bits32(BitsS(b, to_bits(32, width * UInt(Field(60, 2))), to_bits(32, width)))
        else BitsU(b, to_bits(32, width * UInt(Field(60, 2))), to_bits(32, width))
    } else b;
    let mode = if Float64() then UInt(Field(78, 2)) else 0;
    let nearest = if is_signed then MOD(I2fNearest(), 2) == 1 else MOD(DIV(I2fNearest(), 2), 2) == 1;
    if mode == 0 then {
        if src_size == 2 & nearest then D_write(Bits32(IntToF32Nearest(x, if is_signed then 1 else 0)))
        else D_write(Bits32(if is_signed then S32ToF(x) else U32ToF(x)))
    } else D_write(Bits32(RoundF64(mode, ToF64(x, if is_signed then 1 else 0))))
}

/* FRND: b rounded to an integral float by the mode at 78-79 */
function execute_FRND() -> unit = {
    if Field(75, 2) != 0b10 | Field(84, 2) != 0b10 then throw(Unsupported("FRND of 64-bit or 16-bit floats"));
    D_write(Bits32(FRound(UInt(Field(78, 2)), SrcF(B))))
}

/* F2FP.PACK_AB Rd, Ra, Rb: a in the high half, b in the low one, as PTX's cvt.rn.f16x2.f32; bit 76 BF16 (CuAssembler
   sm_86). The halves round to nearest. .MERGE_C (78, NAK's note): b converted in the low half, c's low half (its
   high one with .H1, bit 72) in the high half, a unused */
function execute_F2FP() -> unit = {
    if Field(78, 1) == 0b1 then {
        let c = SrcBits(C);
        let keep = if Field(72, 1) == 0b1 then c >> 16 else c;
        let low = PackHalf(Half2(SrcF(B), FConst(0x00000000)));
        D_write((keep << 16) | low)
    } else {
        let hi = SrcF(A);
        let lo = SrcF(Second);
        if Field(76, 1) == 0b1 then D_write((Bf16(hi) << 16) | Bf16(lo))
        else D_write(PackHalf(Half2(lo, hi)))
    }
}

/* ---- the rest of the integer ALU, the uniform registers and the system registers ---- */

/* the register after the operand's (its pair's high word), of its kind; the zero register's is itself */
val SrcNext : Operand -> bits(32)
/* the destination's next register (a pair's high word), of its kind; whether it has one (it is no zero register) */
val D_writeNext : bits(32) -> unit
val DstHasNext : unit -> bool
/* the operand is a constant buffer's word */
val SrcIsCbuf : Operand -> bool
/* uniform register r (URZ reads 0), and written */
val U_read : int -> bits(32)
val U_write : (int, bits(32)) -> unit
/* the operand is the zero register (not URZ, nor an immediate 0) */
val IsRegZero : Operand -> bool
/* the Int64 capability and the u64 type, made where the instruction makes them first */
val Int64 : unit -> unit
/* a system register (S2R's, S2UR's numbering) as the program reads it */
val ProgramSr : int -> bits(32)
/* the ballot of the predicate over the invocation's 32 lanes */
val Ballot32 : bool -> bits(32)
/* the instruction's index in the program */
val InsnIndex : unit -> int
/* the program's stage (shader.h's ST_*), and a geometry program asks for passthrough */
val Stage : unit -> int
val Passthrough : unit -> bool
/* a geometry program's vertex emitted (with its outputs), and its primitive ended */
val GsEmit : unit -> unit
val EndPrimitive : unit -> unit
/* the device scope's barrier of buffer, workgroup and image memory, acquire and release */
val MemoryBarrierDevice : unit -> unit

let URZ : int = 63
let ST_GS : int = 3

/* IMAD.WIDE and UIMAD.WIDE: NAK's OpIMad64, a * b (bit 73 signed) + the pair at c, into the pair at the destination.
   The carry out of the 64-bit sum goes to the predicate at 81-83 as IADD3's first (nvdisasm: IMAD.WIDE.U32 R14, P0,
   R10, R19, R14): one game's 64-bit division chains IMAD.WIDE ..., P0 into IMAD.X. With .X (bit 74; nvdisasm:
   IMAD.WIDE.U32.X R6, RZ, R5, R6, P1) the carry predicate at 87-90 joins the sum */
function execute_IMAD_WIDE() -> unit = {
    let is_signed = Field(73, 1) == 0b1;
    Int64();
    let a32 = SrcInt(A);
    let b32 = SrcInt(B);
    let a64 = if is_signed then SignExtend(a32, 64) else ZeroExtend(a32, 64);
    let b64 = if is_signed then SignExtend(b32, 64) else ZeroExtend(b32, 64);
    if SrcIsImm(C) | SrcIsCbuf(C) then throw(Unsupported("%s with c an immediate or a constant", OpName()));
    let chi = SrcNext(C);
    let product = a64 * b64;
    let clo = SrcBits(C);
    var sum : bits(64) = product + (chi @ clo);
    var carry : bool = UInt(sum) < UInt(product);
    if Field(74, 1) == 0b1 then {
        let more = sum + CarryIn(87);
        carry = (if carry then true else UInt(more) < UInt(sum));
        sum = more
    };
    PredWrite(UInt(Field(81, 3)), carry, Uniform());
    D_write(sum[31 .. 0]);
    if DstHasNext() then D_writeNext(sum[63 .. 32])
}

/* IMAD.HI: the high word of a * b + c (bit 73 signed): the addend goes into the 64-bit product, below the word that is
   kept, and with .X the carry predicate at 87-90 with it; the carry out of the 64 bits goes to the predicate at 81-83.
   One game's 64-bit division is written that way (IMAD.HI R8, R7, R11, R8 adds the middle column's low word and hands
   its carry to P1): with the addend added to the high word instead, 4239360 / 20480 came out 728513, and with this 207
   (tools/div_model.py) */
function execute_IMAD_HI() -> unit = {
    let is_signed = Field(73, 1) == 0b1;
    Int64();
    let a32 = SrcInt(A);
    let b32 = SrcInt(B);
    let product = if is_signed then SignExtend(a32, 64) * SignExtend(b32, 64) else ZeroExtend(a32, 64) * ZeroExtend(b32, 64);
    let c32 = SrcInt(C);
    let addend = if is_signed then SignExtend(c32, 64) else ZeroExtend(c32, 64);
    var sum : bits(64) = product + addend;
    var carry : bool = UInt(sum) < UInt(product);
    if Field(74, 1) == 0b1 then {
        let more = sum + CarryIn(87);
        carry = (if carry then true else UInt(more) < UInt(sum));
        sum = more
    };
    PredWrite(UInt(Field(81, 3)), carry, Uniform()); /* signed too: its 64 bits' carry, as IMAD.WIDE's */
    D_write(sum[63 .. 32])
}

/* SGXT and USGXT: the low b bits (at most 32) of a, sign (bit 73) or zero extended; 0 bits give 0 */
function execute_SGXT() -> unit = {
    let n = to_bits(32, Min(UInt(SrcBits(B)), 32));
    let a = SrcBits(A);
    let v = if Field(73, 1) == 0b1 then Bits32(BitsS(a, 0x00000000, n)) else BitsU(a, 0x00000000, n);
    D_write(if n == 0x00000000 then 0x00000000 else v)
}

/* PRMT and UPRMT, the index mode (72-74 = 0): output byte k is byte (sel >> 4k) & 7 of c:a, its sign spread with
   bit 3 */
function execute_PRMT() -> unit = {
    if Field(72, 3) != 0b000 then throw(Unsupported("PRMT mode %u", Field(72, 3)));
    let a = SrcBits(A);
    let sel = SrcBits(B);
    let hi = SrcBits(C);
    var v : bits(32) = 0x00000000;
    foreach (k from 0 to 3) {
        let nib = BitsU(sel, to_bits(32, 4 * k), 0x00000004);
        let from_c = (nib & 0x00000004) != 0x00000000;
        let word = if from_c then hi else a;
        let shift = (nib & 0x00000003) << 3;
        let byte = BitsU(word, shift, 0x00000008);
        let spread = (nib & 0x00000008) != 0x00000000;
        let sign = if (byte & 0x00000080) != 0x00000000 then 0x000000ff else 0x00000000;
        let b = if spread then sign else byte;
        v = v | (b << (8 * k))
    };
    D_write(v)
}

/* P2R and UP2UR: P2R Rd, PR, Ra, mask: Ra with the masked bits (at byte 76-77) from P0-P6, PT the bit 7 */
function execute_P2R() -> unit = {
    let byte = UInt(Field(76, 2));
    if not_bool(SrcIsImm(B)) then throw(Unsupported("%s with a register mask", OpName()));
    let mask = SrcImm(B);
    var bits : bits(32) = 0x00000080;
    foreach (i from 0 to 6) {
        if MOD(DIV(mask, 2 ^ i), 2) == 1 then {
            bits = bits | (if PredRead(i, 0, if Uniform() then 1 else 0) then to_bits(32, 2 ^ i) else 0x00000000)
        }
    };
    let pr = bits & to_bits(32, MOD(mask, 256));
    let keep = SrcBits(A) & not_vec(to_bits(32, MOD(mask, 256) * 2 ^ (8 * byte)));
    D_write(keep | (pr << (8 * byte)))
}

/* R2P: the masked bits of a (at byte 76-77: nvdisasm's .B1 sets bit 76, .B2 bit 77) into P0-P6 */
function execute_R2P() -> unit = {
    let byte = UInt(Field(76, 2));
    if not_bool(SrcIsImm(B)) then throw(Unsupported("R2P with a register mask"));
    let mask = SrcImm(B);
    let x = SrcBits(A);
    foreach (i from 0 to 6) {
        if MOD(DIV(mask, 2 ^ i), 2) == 1 then PredWrite(i, (x & to_bits(32, 2 ^ (i + 8 * byte))) != 0x00000000, 0)
    }
}

/* PSETP as a vertex's cull test (sm86.decode): how it combines its predicates isn't known, and in the programs seen
   the predicate it writes only ends the vertex program early, its outputs unwritten. Never culled, the vertex is drawn
   as without the test, and the rasterizer clips what is outside. Refused, every draw of such a program was skipped */
function execute_PSETP() -> unit = {
    let p = UInt(Field(81, 3));
    let q = UInt(Field(84, 3));
    if p < PT then PredWrite(p, false, 0);
    if q < PT then PredWrite(q, false, 0)
}

/* CS2R of SRZ: zero, into a pair with bit 80 */
function execute_CS2R() -> unit = {
    if Field(72, 8) != 0xff then throw(Unsupported("CS2R of system register 0x%x", Field(72, 8)));
    R_write(Dst(), 0x00000000);
    if Field(80, 1) == 0b1 & Dst() < RZ then R_write(Dst() + 1, 0x00000000)
}
function execute_S2R() -> unit = R_write(Dst(), ProgramSr(UInt(Field(72, 8))))
/* the register is the same in each invocation: its value as it is */
function execute_R2UR() -> unit = U_write(UInt(Field(16, 6)), R_read(UInt(Field(24, 8))))
function execute_S2UR() -> unit = U_write(UInt(Field(16, 6)), ProgramSr(UInt(Field(72, 8))))
function execute_UMOV() -> unit = U_write(Dst(), SrcBits(B))

/* SHFL: PTX's shfl.sync. c's bits 8-12 split the 32 lanes into segments, its bits 0-4 clamp the lane. Mode (58-59) 0
   IDX, 1 UP, 2 DOWN, 3 BFLY. The lane is at 53-57 (an immediate) or 32 (a register), c at 40-52 (an immediate) or 64
   (a register: SHFL (imm, reg) 0x989 and SHFL (reg, reg) 0x389, as CuAssembler's sm_86 records "SHFL.UP PT, R7, R8,
   0x1, RZ" and "SHFL.DOWN P1, R32, R33, R32, R31" show). One game's prefix sums are SHFL.UP P0, R12, R19, 0x1, RZ.
   UP is in range from the clamp's lane on, as PTX's j >= maxLane */
function Shfl(b_imm : bool, c_reg : bool) -> unit = {
    let mode = UInt(Field(58, 2));
    let cc = UInt(Field(40, 13));
    let creg = if c_reg then R_read(UInt(Field(64, 8))) else 0x00000000;
    let b = if b_imm then to_bits(32, UInt(Field(53, 5))) else R_read(UInt(Field(32, 8)));
    let id = SystemValue(0);
    let lane = id & 0x0000001f;
    /* c's fields as the immediate's, at run time */
    let seg = if c_reg then (creg >> 8) & 0x0000001f else to_bits(32, MOD(DIV(cc, 256), 32));
    let not_seg_reg = if c_reg then not_vec(seg) & 0x0000001f else 0x00000000;
    let min_lane = lane & seg;
    let clamp_bits = if c_reg then creg & not_seg_reg else to_bits(32, cc) & not_vec(to_bits(32, DIV(cc, 256))) & 0x0000001f;
    let max_lane = min_lane | clamp_bits;
    let j : bits(32) = match mode {
        0 => min_lane | (b & (if c_reg then not_seg_reg else not_vec(to_bits(32, DIV(cc, 256))) & 0x0000001f)),
        1 => lane - b,
        2 => lane + b,
        _ => EOR(lane, b)
    };
    let ok = if mode == 1 then SInt(j) >= SInt(max_lane) else UInt(j) <= UInt(max_lane);
    Capability(65);
    /* the lane in the invocation's own 32, on a subgroup of 32 or 64 */
    let source = (id & 0xffffffe0) | (j & 0x0000001f);
    let value = R_read(UInt(Field(24, 8)));
    let got = Shuffle(value, source);
    R_write(Dst(), if ok then got else value);
    PredWrite(UInt(Field(81, 3)), ok, 0)
}
function execute_SHFL_II() -> unit = Shfl(true, false)
function execute_SHFL_RI() -> unit = Shfl(false, false)
function execute_SHFL_IR() -> unit = Shfl(true, true)
function execute_SHFL_RR() -> unit = Shfl(false, true)

/* VOTE and UVOTE: NAK's OpVote, the ballot of the predicate at 87-90 into the destination, and ALL (0), ANY (1) or EQ
   (2, bits 72-73) of it into the predicate at 81-83. The ballot, and the vote, are of the invocation's 32 lanes, from
   the ballots of the predicate and of the active lanes: a vote of the subgroup takes both warps of a subgroup of 64 */
function Vote(u : bool) -> unit = {
    let pred = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), 0);
    let how = UInt(Field(72, 2));
    Capability(64);
    let ballot = Ballot32(pred);
    if u then U_write(UInt(Field(16, 6)), ballot) else R_write(Dst(), ballot);
    if how > 2 then throw(Unsupported("VOTE mode %u", how));
    let active = Ballot32(true);
    let all = ballot == active;
    let none = ballot == 0x00000000;
    let vote = match how { 0 => all, 1 => not_bool(none), _ => all | none };
    PredWrite(UInt(Field(81, 3)), vote, if u then 1 else 0)
}
function execute_VOTE() -> unit = Vote(false)
function execute_UVOTE() -> unit = Vote(true)

/* MATCH: NAK's OpMatch, the lanes of the invocation's 32 whose value (a pair with bit 73) is the same. ANY (bit 79)
   gives them; ALL gives all the active lanes when they all are, else 0, and the predicate */
function execute_MATCH() -> unit = {
    let any = Field(79, 1) == 0b1;
    let pair = Field(73, 1) == 0b1;
    let r = UInt(Field(24, 8));
    let lo = R_read(r);
    let hi = if pair then R_read(if r >= RZ then r else r + 1) else 0x00000000;
    let base = SystemValue(0) & 0xffffffe0;
    Capability(65);
    Capability(64);
    var mask : bits(32) = 0x00000000;
    foreach (j from 0 to 31) {
        let lane = base | to_bits(32, j);
        let same_lo = Shuffle(lo, lane) == lo;
        let same = if pair then same_lo & (Shuffle(hi, lane) == hi) else same_lo;
        mask = mask | (if same then to_bits(32, 2 ^ j) else 0x00000000)
    };
    let active = Ballot32(true);
    let m = mask & active;
    if any then R_write(Dst(), m) else {
        let all = m == active;
        R_write(Dst(), if all then m else 0x00000000);
        PredWrite(UInt(Field(81, 3)), all, 0)
    }
}

/* REDUX: NAK's OpRedux, of the source over the invocation's 32 lanes, into a uniform register: 78-80 0 and, 1 or, 2
   xor, 3 sum, 4 min, 5 max, bit 73 signed. A clustered reduction of 32 keeps it to those lanes */
function execute_REDUX() -> unit = {
    let how = UInt(Field(78, 3));
    if how > 5 then throw(Unsupported("REDUX operation %u", how));
    let is_signed = Field(73, 1) == 0b1;
    /* OpGroupNonUniform BitwiseAnd 359, BitwiseOr 360, BitwiseXor 361, IAdd 349, UMin 354, UMax 357, SMin 353, SMax 356 */
    let opcode = match how {
        0 => 359,
        1 => 360,
        2 => 361,
        3 => 349,
        4 => if is_signed then 353 else 354,
        _ => if is_signed then 356 else 357
    };
    Capability(63);
    Capability(67);
    U_write(UInt(Field(16, 6)), ClusterReduce32(opcode, R_read(UInt(Field(24, 8)))))
}

/* the instructions that only manage the warp's convergence or wait for scoreboards: nothing is computed. BREAK takes
   its lanes out of a convergence barrier and goes on: the BRA after it leaves */
function execute_NOP() -> unit = ()

/* MEMBAR: the device scope, for every scope */
function execute_MEMBAR() -> unit = MemoryBarrierDevice()

/* LEPC: the instruction's address, which only the exit stub's setup reads (STUB_SET) */
function execute_LEPC() -> unit = {
    if Dst() < RZ then R_write(Dst(), to_bits(32, 16 * InsnIndex()));
    if Dst() + 1 < RZ then R_write(Dst() + 1, 0x00000000)
}

/* STUB_SET and STUB_END: a geometry program's exit stub: STUB_SET sets where EXIT goes on, and the stub's OUT finishes
   the output. An invocation here has nothing left to finish when it ends (SPIR-V ends each primitive at the
   invocation's end), so EXIT ends it and the stub does not run */
function execute_STUB() -> unit = if Stage() != ST_GS then throw(Unsupported("%s outside a geometry program", OpName()))

/* OUT: bits 78-79 emit (1), cut (2) or both, 0 the final one (nothing here); the new output handle Rd is the old one,
   Ra (nxnative's notes) */
function execute_OUT() -> unit = {
    if Stage() != ST_GS then throw(Unsupported("OUT outside a geometry program"));
    let kind = UInt(Field(78, 2));
    if kind != 0 & Passthrough() then throw(Unsupported("OUT emits in a passthrough geometry program"));
    if MOD(kind, 2) == 1 then GsEmit();
    if kind >= 2 then EndPrimitive();
    if Dst() < RZ then R_write(Dst(), SrcInt(A))
}

/* BMOV.32 R, B reads a convergence barrier, which the structured translation does not keep. Into RZ (a BMOV.32.CLEAR
   RZ, B0 before the BSSY B0 of an NVN vertex program) it changes nothing computed */
function execute_BMOV_R() -> unit = if Dst() != RZ then throw(Unsupported("BMOV (to register) into R%u", Dst()))

/* A write of a convergence barrier, whose value the structured translation does not keep (BSSY's writes neither), and
   which no translated instruction reads. The debug variants of the driver's programs start with BMOV.32 B0,
   0xffffffff, BMOV.32 B1, B0 to BMOV.32 B15, B14 and BMOV.32 B15, 0x0. A state register (28-29 set, for BMOV.32 B, B at
   16-23) is refused */
function Bmov(at : int) -> unit = {
    let to = UInt(Field(at, 8));
    if to >= 16 then throw(Unsupported("%s into state register %u", OpName(), to))
}
function execute_BMOV_BI() -> unit = Bmov(24)
function execute_BMOV_BL() -> unit = Bmov(16)

/* ---- memory, attributes, textures, surfaces, the tensor cores ---- */
/* What the translator does with a binding model of its own stays its: the global buffers an address is in
   (global_address, global_dynamic), the constant buffers' windows (cbuf_load, cbuf_subword), the attributes
   (attr_load, attr_store, gs_attr_load, ipa_input), the textures, surfaces and their handles (texture, texel_fetch,
   texture_query, texture_grad, uniform_handle, surface), the tensor cores' matrices (hmma, ldsm, fused_ldsm_hmma,
   movm) and the ISBE (isbe). The Sail says what each instruction reads and where it goes */

/* NAK's atomic operation of a type and an operation: a SPIR-V opcode, 0 for none here */
val AtomicOpcode : (int, int) -> int
/* the instruction's address is a pointer from memory, or derived from one, that no buffer key names */
val AddressFromMemory : unit -> bool
/* such an access, by its kind (0 store, 1 load, 2 atomic), its atomic's opcode and whether it is a float add */
val GlobalDynamic : (int, int, int) -> unit
/* the global buffer the instruction's address is in (uniform_form: the LDG/STG/ATOMG form with a uniform register),
   for a write when write: 0 when none is (the instruction is refused); its address is Address's */
val GlobalAddress : (int, int) -> int
/* a shared or local memory instruction's address, its uniform register ur (URZ for none) added */
val StackAddress : int -> unit
val Address : unit -> bits(32)
/* the global buffer GlobalAddress named read, and written at the address for bytes */
val MarkRead : unit -> unit
val MarkWritten : int -> unit
/* the word at the address of buffer (0 shared memory), its atomic of opcode with data: the old value */
val AtomicU32 : (int, int, bits(32)) -> bits(32)
val AtomicFAdd : (int, bits(32)) -> bits(32)
val AtomicShared : (int, bits(32)) -> bits(32)
/* the shared memory's variable declared */
val SharedVar : unit -> unit
/* a load of type (NAK's MemType) at the address of buffer into the registers from d, and a store of those from data;
   local memory is the buffer MEM_LOCAL */
val MemLoad : (int, int, int) -> unit
val MemStore : (int, int, int) -> unit
val MEM_LOCAL : unit -> int
/* a constant buffer's word at a byte offset, plus a register's value; a byte or a halfword (type 0 to 3) of it */
val CbufLoad : (int, int) -> bits(32)
val CbufLoadAt : (int, int, bits(32)) -> bits(32)
val CbufSubword : (int, int, int) -> bits(32)
val CbufSubwordAt : (int, int, bits(32), int) -> bits(32)
/* the LDC reads an element of a table of texture handles (prepare_handles): its element's index noted, from the
   byte offset and the register's value */
val HandleIndexed : unit -> bool
val NoteHandleIndex : (bits(32), int) -> unit
/* an attribute: a vertex's input (a geometry program's of a vertex), an output stored */
val AttrLoad : int -> bits(32)
val GsAttrLoad : (int, bits(32)) -> bits(32)
val AttrStore : (int, bits(32)) -> unit
/* a fragment's inputs: FragCoord's component, a generic's (interpolated at loc, of frequency freq), whether the face
   is the front one, PointCoord's component, the primitive's number */
val FragCoord : int -> bits(32)
val IpaInput : (int, int, int, int) -> bits(32)
val FrontFacing : unit -> bool
val PointCoord : int -> bits(32)
val PrimitiveId : unit -> bits(32)
/* the layer a fragment program reads (BuiltIn Layer, with the Geometry capability or ShaderLayer), 0 with neither */
val LayerIn : unit -> bits(32)
/* the map of the SPH's legacy varyings, the device takes a fragment's PrimitiveId (the Geometry capability), and the
   SPH's imap reads generic i's component k */
val LegacyAddress : int -> int
val GeometryOk : unit -> bool
val ImapReads : (int, int) -> bool
/* a texture instruction's handle was found in memory (memory_handles), and the translator's texture instructions */
val HasHandleSlot : unit -> bool
val Texture : unit -> unit
val TextureGrad : unit -> unit
val TexelFetch : unit -> unit
val TextureQuery : unit -> unit
val UniformHandle : unit -> unit
val Surface : unit -> unit
/* the tensor cores: HMMA, LDSM, the fused pair of them (fuse.c), MOVM */
val Hmma : unit -> unit
val Ldsm : unit -> unit
val FusedLdsmHmma : unit -> unit
val Movm : unit -> unit
/* ISBERD and ISBEWR, a task or mesh program's ISBE */
val Isbe : unit -> unit
/* a fragment's discard: noted as one (the renderer tests depth before the program only for one that keeps its pixels),
   and its lane demoted to a helper */
val Demote : unit -> unit
/* the workgroup's barrier */
val ControlBarrier : unit -> unit
/* a sum of pairs that the device must not fuse, and a pair of halves' fused multiply-add rounded once (sum_to_odd) */
val NoContract2 : half2 -> half2
val SumToOdd : (half2, half2) -> half2

/* ATOMG, ATOMG.NOU and RED: NAK's OpAtom on global memory, AtomicOpcode's operation at 87-90, the data at 32; RED
   returns nothing. Of type 3 (F32, .FTZ.RN) the ADD alone, AtomicFAdd: one game's compute programs sum positions into a
   buffer with RED.E.ADD.F32.FTZ.RN */
function Atomg(red : bool, uniform_form : bool) -> unit = {
    let how = if red then UInt(Field(87, 3)) else UInt(Field(87, 4));
    let kind = UInt(Field(73, 3));
    let opcode = AtomicOpcode(kind, how);
    let fadd = kind == 3 & how == 0;
    if opcode == 0 & not_bool(fadd) then throw(Unsupported("%s of type %u, operation %u", OpName(), kind, how));
    if AddressFromMemory() then GlobalDynamic(2, opcode, if fadd then 1 else 0) else {
        let buffer = GlobalAddress(if uniform_form then 1 else 0, 1);
        if buffer != 0 then {
            MarkRead();
            MarkWritten(4);
            let old = if fadd then AtomicFAdd(buffer, R_read(UInt(Field(32, 8))))
                      else AtomicU32(opcode, buffer, R_read(UInt(Field(32, 8))));
            if not_bool(red) then R_write(Dst(), old)
        }
    }
}
function execute_ATOMG() -> unit = Atomg(false, true)
function execute_ATOMG_NOU() -> unit = Atomg(false, false)
function execute_RED() -> unit = Atomg(true, Field(91, 1) == 0b1)

/* ATOMS: the same on shared memory, addressed as an LDS or STS is */
function execute_ATOMS() -> unit = {
    let how = UInt(Field(87, 4));
    let kind = UInt(Field(73, 3));
    let opcode = AtomicOpcode(kind, how);
    if opcode == 0 then throw(Unsupported("%s of type %u, operation %u", OpName(), kind, how));
    StackAddress(if Field(91, 1) == 0b1 then UInt(Field(64, 6)) else URZ);
    SharedVar();
    R_write(Dst(), AtomicShared(opcode, R_read(UInt(Field(32, 8)))))
}
/* ATOMS.POPC.INC: adds the lanes that run it, and returns nothing: one each here, where every lane does its own */
function execute_ATOMS_POPC_INC() -> unit = {
    StackAddress(URZ);
    SharedVar();
    let _ = AtomicShared(234, 0x00000001) /* OpAtomicIAdd */
}

/* HFMA2.MMA: only a product of RZ. It is a signed zero (the sign of a's and b's negations), and the sum with the two
   halves of c, in floats, is exact. ptxas loads two halves with HFMA2.MMA R, -RZ, RZ, imm, imm. Other modifiers (73-91
   but b's negation at 75) are refused */
function execute_HFMA2_MMA() -> unit = {
    let zero = IsRegZero(A) | IsRegZero(B);
    if not_bool(zero) | (Field(73, 19) & not_vec(0b0000000000000000100)) != 0b0000000000000000000 then
        throw(Unsupported("HFMA2.MMA of a product other than RZ, or with modifiers 0x%x", Field(73, 19)));
    let sign = if SrcNegRaw(A) != SrcNegRaw(B) then 0x80000000 else 0x00000000;
    let p = Half2(Float32(sign), Float32(sign));
    let c0 = UnpackHalf2(SrcBits(C));
    let c1 = if SrcIsReg(C) & SrcAbsRaw(C) then Half2Abs(c0) else c0;
    let c2 = if SrcIsReg(C) & SrcNegRaw(C) then Half2Neg(c1) else c1;
    D_write(PackHalf(NoContract2(Half2Add(p, c2))))
}

/* a 16-bit ALU operand as a pair: its swizzle (1, a float, refused), then its abs and neg */
function HalfPair(s : Operand, sw : int, absed : bool, neg : bool, what : string) -> half2 = {
    if sw == 1 then throw(Unsupported("%s with a float operand", what));
    let pair = UnpackHalf2(SrcBits(s));
    let swizzled = if sw >= 2 then {
        let h = Half2At(pair, sw - 2);
        Half2(h, h)
    } else pair;
    let a = if absed then Half2Abs(swizzled) else swizzled;
    if neg then Half2Neg(a) else a
}

/* HFMA2: a * b + c by halves. a as HADD2's. The operand in the register at 64 keeps its swizzle at 81-82 and its
   modifiers at 83 (abs) and 84 (neg), not where a three-source float's are. Bit 79 is .RELU: a result at or below zero
   becomes zero where the predicate at 87 holds, while infinity and NaN stay. Bit 86 is .H0_NH1: b's halves swap and the
   new low one is negated, the second term of a complex product. Bits 78 and 85 are refused. A product of two halves is
   exact in a float of 32 bits, and its sum with c is rounded once (sum_to_odd), as the console's fused multiply-add of
   halves is */
function Hfma2Operand(k : int, s : Operand, b_at_64 : bool, h0_nh1 : bool) -> half2 = {
    let at_64 = k != 0 & ((k == 1) == b_at_64);
    let imm = SrcIsImm(s);
    let sw = if imm then 0 else if k == 0 then UInt(Field(74, 2)) else if at_64 then UInt(Field(81, 2)) else UInt(Field(60, 2));
    let absed = if imm then false else if at_64 then Field(83, 1) == 0b1 else SrcAbsRaw(s);
    let neg = if imm then false else if at_64 then Field(84, 1) == 0b1 else SrcNegRaw(s);
    if sw == 1 then throw(Unsupported("HFMA2 with a float operand"));
    if k == 1 & h0_nh1 & (sw != 0 | absed) then throw(Unsupported("HFMA2.H0_NH1 of an operand with a swizzle or abs"));
    let pair = UnpackHalf2(SrcBits(s));
    let swizzled = if sw >= 2 then {
        let h = Half2At(pair, sw - 2);
        Half2(h, h)
    } else pair;
    let swapped = if k == 1 & h0_nh1 then {
        let low = Half2At(swizzled, 0);
        let high = Half2At(swizzled, 1);
        Half2(FNeg(high), low)
    } else swizzled;
    let a = if absed then Half2Abs(swapped) else swapped;
    if neg then Half2Neg(a) else a
}
function execute_HFMA2() -> unit = {
    let relu = Field(79, 1) == 0b1;
    let h0_nh1 = Field(86, 1) == 0b1;
    if Field(78, 1) == 0b1 | Field(85, 1) == 0b1 | (relu & Field(77, 1) == 0b1) then
        throw(Unsupported("HFMA2 with modes 0x%x, 0x%x, saturate %u", Field(78, 2), Field(85, 2), Field(77, 1)));
    let b_at_64 = Form() == 2 | Form() == 3 | Form() == 7;
    let va = Hfma2Operand(0, A, b_at_64, h0_nh1);
    let vb = Hfma2Operand(1, B, b_at_64, h0_nh1);
    let vc = Hfma2Operand(2, C, b_at_64, h0_nh1);
    let p = NoContract2(Half2Mul(va, vb));
    let r0 = SumToOdd(p, vc);
    let r1 = if Field(77, 1) == 0b1 then Half2Clamp01(r0) else r0;
    let r = if relu then {
        let holds = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), 0);
        /* a comparison NaN fails, and -0 passes on its way to +0 */
        let x0 = Half2At(r1, 0);
        let l0 = if holds & FCompare(3, x0, FConst(0x00000000)) then FConst(0x00000000) else x0;
        let x1 = Half2At(r1, 1);
        let l1 = if holds & FCompare(3, x1, FConst(0x00000000)) then FConst(0x00000000) else x1;
        Half2(l0, l1)
    } else r1;
    D_write(PackHalf(r))
}

/* HMUL2 and HMNMX2: two halves multiplied, or the smaller or larger of each pair by the predicate at 87 (FMNMX's). The
   operands as HADD2's. HMUL2: bit 77 saturate, 76 and 80 the handling of zeros and denormals, which floats of 32 bits
   don't lose here */
function Hmul2(min_max : bool) -> unit = {
    let swa = UInt(Field(74, 2));
    let swb = if SrcIsImm(Second) then 0 else UInt(Field(60, 2));
    if not_bool(min_max) & Field(78, 2) != 0b00 then throw(Unsupported("HMUL2 with mode 0x%x", Field(78, 2)));
    let va = HalfPair(A, swa, SrcAbs(A), SrcNeg(A), OpName());
    let vb = HalfPair(Second, swb, SrcAbs(Second), SrcNeg(Second), OpName());
    let r : half2 = if not_bool(min_max) then {
        let m = NoContract2(Half2Mul(va, vb));
        if Field(77, 1) == 0b1 then Half2Clamp01(m) else m
    } else {
        let min = PredRead(UInt(Field(87, 3)), UInt(Field(90, 1)), 0);
        let lo = Half2MinN(va, vb);
        let hi = Half2MaxN(va, vb);
        let l0 = if min then Half2At(lo, 0) else Half2At(hi, 0);
        let l1 = if min then Half2At(lo, 1) else Half2At(hi, 1);
        Half2(l0, l1)
    };
    D_write(PackHalf(r))
}
function execute_HMUL2() -> unit = Hmul2(false)
function execute_HMNMX2() -> unit = Hmul2(true)

/* HADD2: two halves added, bit 77 saturate, 78 a float result, 80 FTZ. .F32 (bit 78): the low half's sum as a float,
   as ptxas's HADD2.F32 R, -RZ, R.H0_H0 widens a half */
function execute_HADD2() -> unit = {
    let swa = UInt(Field(74, 2));
    let swb = if SrcIsImm(Second) then 0 else UInt(Field(60, 2));
    let va = HalfPair(A, swa, SrcAbs(A), SrcNeg(A), "HADD2");
    let vb = HalfPair(Second, swb, SrcAbs(Second), SrcNeg(Second), "HADD2");
    let s0 = NoContract2(Half2Add(va, vb));
    let sum = if Field(77, 1) == 0b1 then Half2Clamp01(s0) else s0;
    if Field(78, 1) == 0b1 then D_write(Bits32(Half2At(sum, 0))) else D_write(PackHalf(sum))
}

/* ULDC.IDX: ULDC UR, c[bank][UR + offset], the uniform register at 24, the offset 38-53 signed. Of NAK's MemType
   (73-75) as LDC's: a byte or a halfword (0 to 3) as CbufSubword reads it, a word, or a pair. One game's compute
   program walks a byte code in a constant buffer with ULDC.U8 UR, c[0x4][UR15 + k] */
function execute_ULDC_IDX() -> unit = {
    let kind = UInt(Field(73, 3));
    let bank = UInt(Field(54, 5));
    let off = UInt(Field(38, 16));
    if kind > 5 then throw(Unsupported("ULDC (indexed) of type %u", kind));
    let bias = if off >= 32768 then off - 65536 else off;
    let d = UInt(Field(16, 6));
    let index = U_read(UInt(Field(24, 6)));
    if kind < 4 then U_write(d, CbufSubwordAt(bank, bias, index, kind)) else {
        U_write(d, CbufLoadAt(bank, bias, index));
        if kind == 5 & d < URZ then U_write(d + 1, CbufLoadAt(bank, bias + 4, index))
    }
}
/* ULDC: the types as ULDC.IDX's (CuAssembler's sm_86 records give ULDC.U8, .S8 and .U16 too) */
function execute_ULDC() -> unit = {
    let kind = UInt(Field(73, 3));
    let bank = UInt(Field(54, 5));
    let off = UInt(Field(38, 16));
    if Field(91, 1) == 0b1 | kind > 5 then throw(Unsupported("ULDC of type %u", kind));
    let d = UInt(Field(16, 6));
    if kind < 4 then U_write(d, CbufSubword(bank, off, kind)) else {
        U_write(d, CbufLoad(bank, off));
        if kind == 5 & d < URZ then U_write(d + 1, CbufLoad(bank, off + 4))
    }
}

/* LDC: a constant buffer's word (or byte, halfword, pair: NAK's MemType at 73), at the offset 38-53 plus the register
   at 24 */
function execute_LDC() -> unit = {
    let kind = UInt(Field(73, 3));
    let bank = UInt(Field(54, 5));
    let off = UInt(Field(38, 16));
    let r = UInt(Field(24, 8));
    let mode = UInt(Field(78, 2));
    if (mode != 0 & mode != 2) | kind > 5 then throw(Unsupported("LDC of type %u, mode %u", kind, mode));
    if r < RZ then {
        let index = R_read(r);
        if mode == 2 then {
            /* NAK's LdcMode::IndexedSegmented (.IS): the register holds the bank past the instruction's in its upper
               half and the byte offset in its lower half, as NAK packs them with PRMT for a bank known only at run
               time. A select over the banks from the instruction's on (an unbound one reads zeros). One game's vertex
               program that draws its frame into the window reads a record of bank 1 for each vertex,
               c[(VertexId << 5) + 0x10000 + 8] */
            let seg = index >> 16;
            let low = index & 0x0000ffff;
            foreach (w from 0 to (if kind == 5 then 1 else 0)) {
                var v : bits(32) = 0x00000000;
                foreach (b from bank to 17) {
                    let here = if kind < 4 then CbufSubwordAt(b, off, low, kind) else CbufLoadAt(b, off + 4 * w, low);
                    let is = seg == to_bits(32, b - bank);
                    v = if is then here else v
                };
                if Dst() + w < RZ then R_write(Dst() + w, v)
            }
        } else {
            if HandleIndexed() then NoteHandleIndex(index, off);
            if kind < 4 then R_write(Dst(), CbufSubwordAt(bank, off, index, kind)) else {
                R_write(Dst(), CbufLoadAt(bank, off, index));
                if kind == 5 & Dst() < RZ then R_write(Dst() + 1, CbufLoadAt(bank, off + 4, index))
            }
        }
    } else if kind < 4 then R_write(Dst(), CbufSubword(bank, off, kind)) else {
        R_write(Dst(), CbufLoad(bank, off));
        if kind == 5 & Dst() < RZ then R_write(Dst() + 1, CbufLoad(bank, off + 4))
    }
}

/* LDG, STG and their forms without the uniform register: a pointer from memory, or derived from one, that no buffer key
   names is GlobalDynamic's; else the global buffer the address is in */
function Ldg(load : bool, uniform_form : bool) -> unit = {
    if AddressFromMemory() then GlobalDynamic(if load then 1 else 0, 0, 0) else {
        let buffer = GlobalAddress(if uniform_form then 1 else 0, if load then 0 else 1);
        if buffer != 0 then {
            let kind = UInt(Field(73, 3));
            if load then MemLoad(buffer, kind, Dst()) else {
                MarkWritten(if kind < 2 then 1 else if kind < 4 then 2 else if kind == 4 then 4 else if kind == 5 then 8 else 16);
                MemStore(buffer, kind, UInt(Field(32, 8)))
            }
        }
    }
}
function execute_LDG() -> unit = Ldg(true, true)
function execute_STG() -> unit = Ldg(false, true)
function execute_LDG_NOU() -> unit = Ldg(true, false)
function execute_STG_NOU() -> unit = Ldg(false, false)

/* LDS, STS, LDL and STL: the uniform register is there with bit 91 alone, as CuAssembler's sm_86 encodings show: at
   32 for a load, at 64 for a store, whose data is at 32 */
function Lds(load : bool, local : bool) -> unit = {
    let ur = if Field(91, 1) == 0b1 then (if load then UInt(Field(32, 6)) else UInt(Field(64, 6))) else URZ;
    StackAddress(ur);
    let buffer = if local then MEM_LOCAL() else 0;
    if not_bool(local) then SharedVar();
    if load then MemLoad(buffer, UInt(Field(73, 3)), Dst()) else MemStore(buffer, UInt(Field(73, 3)), UInt(Field(32, 8)))
}
function execute_LDS() -> unit = Lds(true, false)
function execute_STS() -> unit = Lds(false, false)
function execute_LDL() -> unit = Lds(true, true)
function execute_STL() -> unit = Lds(false, true)

/* ALD and AST: a geometry program's ALD names its input vertex in the register at 32 (from ISBERD), and its AST the
   output vertex's handle at 64 (from OUT), which SPIR-V's emits stand in for. The vertex index read once: the
   destination may be its register, as Maxwell's ALD reads it first */
function Ald(load : bool) -> unit = {
    let addr = UInt(Field(40, 10));
    let comps = UInt(Field(74, 2)) + 1;
    let vtx = if load then UInt(Field(32, 8)) else UInt(Field(64, 8));
    let geometry = Stage() == ST_GS;
    if UInt(Field(24, 8)) != RZ | (vtx != RZ & not_bool(geometry)) | Field(76, 2) != 0b00 | (load & Field(79, 1) == 0b1) then
        throw(Unsupported("%s indexed, of a patch or of an output", OpName()));
    let vertex = if geometry & load & vtx < RZ then R_read(vtx) else 0x00000000;
    foreach (k from 0 to comps - 1) {
        if load then {
            if Dst() < RZ then R_write(Dst() + k, if geometry then GsAttrLoad(addr + 4 * k, vertex) else AttrLoad(addr + 4 * k))
        } else {
            let data = UInt(Field(32, 8));
            AttrStore(addr + 4 * k, R_read(if data >= RZ then data else data + k))
        }
    }
}
function execute_ALD() -> unit = Ald(true)
function execute_AST() -> unit = Ald(false)

/* IPA: a fragment's input at the attribute address at 64 (a word's), bits 76-77 NAK's InterpLoc (0 the default, 1 the
   centroid, 2 at an offset), 78-79 its frequency. FragCoord (0x70), the generics (0x80-0x27f), the front face (NAK's
   NAK_ATTR_FRONT_FACE, all ones for a front face, as spirv_translate.c reads Maxwell's), PointCoord (NAK's
   NAK_ATTR_POINT_SPRITE_S and _T; the Maxwell path flips T with the sprite's origin, a push constant of the fragment
   stage that this translation does not have), the primitive's number (NAK's NAK_ATTR_PRIMITIVE_ID, its bits as they
   are) */
function execute_IPA() -> unit = {
    let raw = UInt(Field(64, 8)) * 4;
    let addr = LegacyAddress(raw);
    let loc = UInt(Field(76, 2));
    let freq = UInt(Field(78, 2));
    if UInt(Field(32, 8)) != RZ | UInt(Field(24, 8)) != 0 | freq > 1 | loc > 1 then
        throw(Unsupported("IPA indexed, at an offset or of frequency %u", freq));
    /* the addresses: FragCoord 0x70-0x7f, the generics 0x80-0x27f, the front face 0x3fc, PointCoord 0x2e0 and 0x2e4,
       the primitive's number 0x60, the layer 0x64 */
    let v : bits(32) = if addr >= 112 & addr < 128 then FragCoord(MOD(DIV(addr, 4), 4))
        else if addr >= 128 & addr < 640 then IpaInput(DIV(addr - 128, 16), MOD(DIV(addr, 4), 4), loc, freq)
        else if addr == 1020 then Float32(if FrontFacing() then 0xffffffff else 0x00000000)
        else if addr == 736 | addr == 740 then PointCoord(MOD(DIV(addr, 4), 2))
        else if addr == 96 & Stage() == 4 & GeometryOk() then Float32(PrimitiveId())
        else if addr == 100 & Stage() == 4 then Float32(LayerIn())
        else throw(Unsupported("IPA of attribute 0x%x", addr));
    R_write(Dst(), Bits32(v))
}

/* IPA R, a[UR + addr]: the form with bit 91 and the uniform register at 24, which ptxas gives an input array's index.
   The index is the same in every lane: the generic input whose address the sum is, of those the SPH's imap reads,
   picked by its address (0 for none). The rest is IPA's */
function execute_IPA_U() -> unit = {
    let loc = UInt(Field(76, 2));
    let freq = UInt(Field(78, 2));
    if Field(91, 1) != 0b1 | UInt(Field(32, 8)) != RZ | freq > 1 | loc > 1 then
        throw(Unsupported("IPA with a uniform index, at an offset or of frequency %u", freq));
    let at = U_read(UInt(Field(24, 6))) + to_bits(32, UInt(Field(64, 8)) * 4);
    var v : bits(32) = 0x00000000;
    foreach (i from 0 to 31) {
        foreach (k from 0 to 3) {
            if ImapReads(i, k) then {
                let is = at == to_bits(32, 128 + 16 * i + 4 * k);
                let x = Bits32(IpaInput(i, k, loc, freq));
                v = if is then x else v
            }
        }
    };
    R_write(Dst(), v)
}

/* the texture instructions whose handle a uniform register holds: memory_handles found the slot, or none (UniformHandle) */
function TexU(kind : int) -> unit =
    if not_bool(HasHandleSlot()) then UniformHandle()
    else match kind { 0 => Texture(), 1 => TexelFetch(), 2 => TextureQuery(), _ => TextureGrad() }
function execute_TEX_U() -> unit = TexU(0)
function execute_TLD_U() -> unit = TexU(1)
function execute_TXQ_U() -> unit = TexU(2)
function execute_TXD_U() -> unit = TexU(3)
function execute_TEX() -> unit = Texture()
function execute_TXD() -> unit = TextureGrad()
function execute_TLD() -> unit = TexelFetch()
function execute_TXQ() -> unit = TextureQuery()
/* TXQ.B: the handle from memory, as a TEX.B's (memory_handles) */
function execute_TXQ_B() -> unit =
    if not_bool(HasHandleSlot()) then throw(Unsupported("TXQ.B whose handle is not memory's")) else TextureQuery()
function execute_SURFACE() -> unit = Surface()
function execute_HMMA() -> unit = Hmma()
function execute_LDSM() -> unit = Ldsm()
function execute_LDSM_HMMA() -> unit = FusedLdsmHmma()
function execute_MOVM() -> unit = Movm()
function execute_ISBE() -> unit = Isbe()

/* KILL: the lane demoted to a helper (DemoteToHelperInvocation), which keeps its derivatives' neighbours */
function execute_KILL() -> unit = {
    Capability(5379);
    Demote()
}

/* BAR.SYNC 0: the workgroup's barrier; other barriers are refused */
function execute_BAR() -> unit = {
    if Field(77, 2) != 0b00 | Field(64, 28) != 0x0000000 then throw(Unsupported("BAR other than BAR.SYNC 0"));
    ControlBarrier()
}
