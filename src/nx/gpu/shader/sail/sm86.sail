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
