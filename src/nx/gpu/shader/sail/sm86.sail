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
