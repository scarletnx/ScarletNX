/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's integer, logic, video and predicate instructions (sm53_int.roots; back end sm53_int_spirv.h). The translator's
   helpers that carry its work stay its and the Sail calls them (emit_int.c's: shader_integer_add with the condition
   code, shader_integer_compare, shader_integer_mul_high, shader_xmad_half, the video operands and their end,
   shader_prmt_byte, shader_vm_select, shader_cc_test, the flags of a logic result). Each instruction is its
   emit_int branch, its constants made where those made them (Bits32 of a value known when the translator runs) and
   its booleans' raw operations (Raw*) where those did not fold the true and false constants out */

/* x + y as Maxwell's IADD does it (shader_integer_add): avg 2 negates x, 1 y, 3 adds one; .X the carry in; .CC the flags */
val IntegerAdd : (bits(32), bits(32), int, int, int) -> bits(32)
/* the comparison at 49-51 of x and y, signed with 48, .X with the flags (shader_integer_compare); ICMP's, whose bit 43
   is Rc's (no .X) */
val IntegerCompare : (bits(32), bits(32)) -> bool
val IntegerCompareNoX : (bits(32), bits(32)) -> bool
/* the high word of x * y, each signed or not */
val MulHigh : (bits(32), bits(32), int, int) -> bits(32)
/* XMAD's half of a word: the high one or the low one, sign extended or not */
val XmadHalf : (bits(32), int, int) -> bits(32)
/* the condition code's flags of a logic result, and of one with .X (the zero flag ANDed) */
val FlagsLogic : bits(32) -> unit
val FlagsLogicExtended : (bits(32), int) -> unit
/* PRMT's byte idx (0-7) of the eight of low (0-3) and high (4-7) */
val PrmtByte : (bits(32), bits(32), bits(32)) -> bits(32)
/* a video operand's byte, half or word (sel & 7), sign extended with bit 3: as a u32, and as an i64 (an s33) */
val VmSelect : (bits(32), int) -> bits(32)
val VideoOperand : (bits(32), int) -> i64
/* the end of a scalar video instruction (shader_video_finish): its 64-bit result clamped, merged with c by the mode,
   its low 32 bits */
val VideoFinish : (i64, bits(32), int, int, int, int) -> bits(32)
/* the i64s the video instructions compute in, each one SPIR-V instruction of the i64 type */
val I64Const : int -> i64
val I64Add : (i64, i64) -> i64
val I64Sub : (i64, i64) -> i64
val I64And : (i64, i64) -> i64
val I64Shl : (i64, i64) -> i64
val I64Sar : (i64, i64) -> i64
val I64Shr : (i64, i64) -> i64
val I64Greater : (i64, i64) -> bool
val I64Equal : (i64, i64) -> bool
val I64Select : (bool, i64, i64) -> i64
/* a word as an i64: sign extended (signed) or zero extended */
val I64Of : (bits(32), int) -> i64
/* the condition code's test at 8-12 (shader_cc_test) */
val CcTest : int -> bool
/* x > y of the i32s (bitcast each); x < 0 of the i32; x >> n arithmetic of the i32 */
val SGreater : (bits(32), bits(32)) -> bool
val Negative : bits(32) -> bool
val AshrS : (bits(32), bits(32)) -> bits(32)
/* the booleans' operations and a select that keep the true and false constants' instructions */
val RawNot : bool -> bool
val RawAnd : (bool, bool) -> bool
val RawOr : (bool, bool) -> bool
val RawSelect : (bool, bits(32), bits(32)) -> bits(32)
/* IMNMX's choice: the minimum of x and y (by less) where choose holds, else the maximum; a choice the translator knows
   (PT, !PT) makes only the one it takes */
val MinMax : (bool, bool, bits(32), bits(32)) -> bits(32)
/* R2P's write of predicate p (or flag p) where enabled, its old value elsewhere */
val R2PSet : (int, int, bool, bool) -> unit

/* a comparison's 2-bit combination: 0 and, 1 or, 2 xor, 3 the first alone (bool_combine), the true and false constants
   folded out */
function Combine53(op : int, left : bool, right : bool) -> bool =
    match op { 0 => left & right, 1 => left | right, 2 => left != right, _ => left }
/* an integer form's right operand by its place in the roots line, the register (0), the constant (1), the immediate */
function RightOf(form : int) -> bits(32) = Right(if form == 0 then 0 else if form == 1 then 1 else 2)

function execute_MOV32I() -> unit = R_write(Dst(), Field(20, 32))

/* IMUL, IMUL32I, IMAD and IMAD32I: a * b, its high word with .HI (each operand signed or not), + c for IMAD; IMAD's
   mode negates b (2) or c (1), or adds one (3); .X adds the carry */
function execute_IMUL() -> unit = {
    let f = Form();
    let imm32 = f == 3 | f == 8;
    let mad = f >= 4;
    let lhs = R_read(RegA());
    let rhs0 = if imm32 then Bits32(Field(20, 32)) else if f == 5 then R_read(UInt(Field(39, 8)))
               else if f == 6 then CbufWord() else Right(if f == 0 | f == 4 then 0 else if f == 1 then 1 else 2);
    var rhs : bits(32) = rhs0;
    var addend : bits(32) = 0x00000000;
    if mad then {
        addend = if f == 8 then R_read(Dst()) else if f == 5 then CbufWord() else R_read(UInt(Field(39, 8)));
        let mode = if imm32 then UInt(Field(55, 2)) else UInt(Field(51, 2));
        if mode == 2 then { rhs = 0x00000000 - rhs };
        if mode == 1 then { addend = 0x00000000 - addend };
        if mode == 3 then { addend = addend + 0x00000001 };
        if not_bool(imm32) & Field(49, 1) == 0b1 then { addend = addend + (if FlagRead(2) then 0x00000001 else 0x00000000) }
    };
    let high = Field(if imm32 then 53 else if mad then 54 else 39, 1) == 0b1;
    let signed_lhs = UInt(Field(if imm32 then 54 else if mad then 48 else 40, 1));
    let signed_rhs = UInt(Field(if imm32 then (if mad then 57 else 55) else if mad then 53 else 41, 1));
    let product = if high then MulHigh(lhs, rhs, signed_lhs, signed_rhs) else lhs * rhs;
    R_write(Dst(), if mad then product + addend else product)
}

/* FLO: the highest bit set (of the inverse with bit 40; of a negative value's inverse when signed, 48), its distance from
   31 with bit 41 (none: 0xffffffff), from the bits spread below it and counted */
function execute_FLO() -> unit = {
    var value : bits(32) = RightOf(Form());
    if Field(40, 1) == 0b1 then { value = not_vec(value) };
    if Field(48, 1) == 0b1 then {
        let negative = Negative(value);
        value = if negative then not_vec(value) else value
    };
    var spread : bits(32) = value;
    foreach (k from 0 to 4) { spread = spread | (spread >> to_bits(32, 2 ^ k)) };
    let index = PopCount(spread) - 0x00000001;
    if Field(41, 1) == 0b1 then {
        let found = value != 0x00000000;
        R_write(Dst(), if found then 0x0000001f - index else index)
    } else R_write(Dst(), index)
}

function execute_POPC() -> unit = {
    let x = RightOf(Form());
    R_write(Dst(), PopCount(if Field(40, 1) == 0b1 then not_vec(x) else x))
}

/* MOV: with the write mask at 39-42 1 or 15; a register's as the block holds it (reg_copy) */
function execute_MOV() -> unit = if Field(39, 4) == 0x1 | Field(39, 4) == 0xf then {
    if Form() == 0 then R_copy(Dst(), UInt(Field(20, 8))) else R_write(Dst(), RightOf(Form()))
}

function execute_IADD32I() -> unit = {
    let immediate = Bits32(Field(20, 32));
    R_write(Dst(), IntegerAdd(R_read(RegA()), immediate, UInt(Field(55, 2)), UInt(Field(53, 1)), UInt(Field(52, 1))))
}

/* IADD3: a + b + c, each negated with 51, 50 or 49; the register form's halves (35-36 a's, 33-34 b's, 31-32 c's: 1
   the low, 2 the high) and the shift of the first sum (37-38: 1 right by 16 with its carry as bit 16, 2 left by 16);
   .X (48) the carry in, .CC (47) the flags */
function HalfOf3(v : bits(32), half : int) -> bits(32) =
    if half == 1 then v & 0x0000ffff else if half == 2 then v >> 16 else v
function execute_IADD3() -> unit = {
    let reg_form = Form() == 0;
    let first0 = R_read(RegA());
    let second0 = RightOf(Form());
    let third0 = R_read(UInt(Field(39, 8)));
    let first1 = if reg_form then HalfOf3(first0, UInt(Field(35, 2))) else first0;
    let second1 = if reg_form then HalfOf3(second0, UInt(Field(33, 2))) else second0;
    let third1 = if reg_form then HalfOf3(third0, UInt(Field(31, 2))) else third0;
    let first = if Field(51, 1) == 0b1 then 0x00000000 - first1 else first1;
    let second = if Field(50, 1) == 0b1 then 0x00000000 - second1 else second1;
    let third = if Field(49, 1) == 0b1 then 0x00000000 - third1 else third1;
    let sum0 = first + second;
    let carry0 = UInt(sum0) < UInt(first);
    let shift = if reg_form then UInt(Field(37, 2)) else 0;
    /* the 33-bit sum shifted: its carry becomes bit 16 */
    let sum = if shift == 1 then (sum0 >> 16) | (if carry0 then 0x00010000 else 0x00000000)
              else if shift == 2 then sum0 << 16 else sum0;
    var sum2 : bits(32) = sum + third;
    let carry1 = UInt(sum2) < UInt(sum);
    var carry2 : bool = false;
    if Field(48, 1) == 0b1 then {
        let cin = if FlagRead(2) then 0x00000001 else 0x00000000;
        let total = sum2 + cin;
        carry2 = UInt(total) < UInt(sum2);
        sum2 = total
    };
    if Field(47, 1) == 0b1 then {
        let carry12 = RawOr(carry1, carry2);
        let carry = if shift != 1 & shift != 2 then RawOr(carry12, carry0) else carry12;
        let zero0 = sum2 == 0x00000000;
        let zero = if Field(48, 1) == 0b1 then RawAnd(zero0, FlagRead(0)) else zero0;
        let sign = (sum2 & 0x80000000) != 0x00000000;
        FlagWrite(2, carry);
        FlagWrite(0, zero);
        FlagWrite(1, sign)
    };
    R_write(Dst(), sum2)
}

function execute_IADD() -> unit = {
    let left = R_read(RegA());
    let right = RightOf(Form());
    R_write(Dst(), IntegerAdd(left, right, UInt(Field(48, 2)), UInt(Field(43, 1)), UInt(Field(47, 1))))
}

/* the logic operation of two values: 0 and, 1 or, 2 xor, 3 the second */
function Logic(op : int, left : bits(32), right : bits(32)) -> bits(32) =
    match op { 0 => left & right, 1 => left | right, 2 => EOR(left, right), _ => right }

/* LOP32I: a's and the immediate's operation (53-54), each inverted with 55 and 56; .CC (52) with .X (57) */
function execute_LOP32I() -> unit = {
    let left0 = R_read(RegA());
    let right0 = Bits32(Field(20, 32));
    let left = if Field(55, 1) == 0b1 then not_vec(left0) else left0;
    let right = if Field(56, 1) == 0b1 then not_vec(right0) else right0;
    let value = Logic(UInt(Field(53, 2)), left, right);
    R_write(Dst(), value);
    if Field(52, 1) == 0b1 then FlagsLogicExtended(value, UInt(Field(57, 1)))
}

/* LOP: a's and b's operation (41-42), each inverted with 39 and 40; .CC (47) with .X (43); the predicate at 48 by the
   mode at 44-45: false, true, the result zero, not zero */
function LopPredicate(value : bits(32), mode : int) -> bool =
    match mode { 0 => false, 1 => true, 2 => value == 0x00000000, _ => value != 0x00000000 }
function execute_LOP() -> unit = {
    let left0 = R_read(RegA());
    let right0 = RightOf(Form());
    let left = if Field(39, 1) == 0b1 then not_vec(left0) else left0;
    let right = if Field(40, 1) == 0b1 then not_vec(right0) else right0;
    let value = Logic(UInt(Field(41, 2)), left, right);
    R_write(Dst(), value);
    if Field(47, 1) == 0b1 then FlagsLogicExtended(value, UInt(Field(43, 1)));
    if UInt(Field(48, 3)) < 7 then PredWrite(UInt(Field(48, 3)), LopPredicate(value, UInt(Field(44, 2))))
}

/* LOP3: the truth table (28-35 for the register form, else 48-55) of a, b and c, as the fewest operations that make it
   (lut3.sail); .CC (47) with .X (38 for the register form, 56 for the constant's, 57 for the immediate's); the register
   form's predicate at 48 by the mode at 36-37 as LOP's */
function execute_LOP3() -> unit = {
    let f = Form();
    let first = R_read(RegA());
    let second = RightOf(f);
    let third = R_read(UInt(Field(39, 8)));
    let value = Lut3(if f == 0 then Field(28, 8) else Field(48, 8), first, second, third);
    R_write(Dst(), value);
    if Field(47, 1) == 0b1 then FlagsLogicExtended(value, UInt(Field(if f == 0 then 38 else if f == 1 then 56 else 57, 1)));
    if f == 0 & UInt(Field(48, 3)) < 7 then PredWrite(UInt(Field(48, 3)), LopPredicate(value, UInt(Field(36, 2))))
}

/* SEL: a where the predicate at 39 holds, else b */
function execute_SEL() -> unit = {
    let condition = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let rhs = RightOf(Form());
    R_write(Dst(), if condition then R_read(RegA()) else rhs)
}

/* XMAD: Each form's operands and fields, as Eden reads them: B is Rb (R), Rc at bit 39 (RC), a constant buffer word
   (CR) or a 16-bit immediate (I), and C is Rc or the constant buffer word. The product of A's and B's halves (signed
   by bits 48 and 49) is shifted up 16 by PSL; the select picks C, its low or high half, or adds B << 16 to it (CBCC);
   MRG puts B's low half in the result's high half. X adds the carry and CC writes the flags, as IADD's do (Eden
   refuses both) */
function execute_XMAD() -> unit = {
    let f = Form();
    let narrow = f == 1 | f == 2;
    let source_b = if f == 0 then R_read(RegB()) else if f == 1 then R_read(UInt(Field(39, 8)))
                   else if f == 2 then CbufWord() else Bits32(ZeroExtend(Field(20, 16), 32));
    let source_c = if f == 1 then CbufWord() else R_read(UInt(Field(39, 8)));
    let sel = if narrow then UInt(Field(50, 2)) else UInt(Field(50, 3));
    let half_b = if f == 0 then UInt(Field(35, 1)) else if narrow then UInt(Field(52, 1)) else 0;
    let psl = if f == 2 then Field(55, 1) == 0b1 else if f == 1 then false else Field(36, 1) == 0b1;
    let mrg = if f == 2 then Field(56, 1) == 0b1 else if f == 1 then false else Field(37, 1) == 0b1;
    let extended = if narrow then UInt(Field(54, 1)) else UInt(Field(38, 1));
    let sixteen = Bits32(0x00000010);
    let left = XmadHalf(R_read(RegA()), UInt(Field(53, 1)), UInt(Field(48, 1)));
    let right = XmadHalf(source_b, half_b, UInt(Field(49, 1)));
    let product0 = left * right;
    let product = if psl then product0 << sixteen else product0;
    let third = if sel == 1 then XmadHalf(source_c, 0, 0) else if sel == 2 then XmadHalf(source_c, 1, 0)
                else if sel == 4 then (source_b << sixteen) + source_c else source_c;
    let sum = IntegerAdd(product, third, 0, extended, UInt(Field(47, 1)));
    R_write(Dst(), if mrg then (sum & 0x0000ffff) | ((source_b & 0x0000ffff) << sixteen) else sum)
}

/* VADD, VABSDIFF, VSET, VSHL and VSHR: Ryujinx's decoding and PTX's semantics: A's and B's selects at 36 and 28 with their
   signs at 48 and 49 (the shifts' B is unsigned, and bit 49 wraps their count), B a 16-bit immediate when bit 50 is
   clear, a signed destination at 54 and saturate at 55 (VSET's comparison is there instead), the secondary operation
   at 51-53, and VADD's negation/plus-one mode at 56-57 */
function execute_VIDEO() -> unit = {
    let op = Form(); /* 0 VADD, 1 VABSDIFF, 2 VSET, 3 VSHL, 4 VSHR */
    let shift = op >= 3;
    let asel = UInt(Field(36, 3)) + 8 * UInt(Field(48, 1));
    let bsel = UInt(Field(28, 3)) + (if shift then 0 else 8 * UInt(Field(49, 1)));
    let left = VideoOperand(R_read(RegA()), asel);
    let right = if Field(50, 1) == 0b1 then VideoOperand(R_read(RegB()), bsel)
                else VideoOperand(Bits32(ZeroExtend(Field(20, 16), 32)), 4 + 8 * DIV(bsel, 8));
    let zero64 = I64Const(0);
    let one64 = I64Const(1);
    let result : i64 = if op == 0 then {
        let mode = UInt(Field(56, 2));
        let r = if mode == 1 then I64Sub(left, right) else if mode == 2 then I64Sub(right, left) else I64Add(left, right);
        if mode == 3 then I64Add(r, one64) else r
    } else if op == 1 then {
        let difference = I64Sub(left, right);
        I64Select(I64Greater(zero64, difference), I64Sub(zero64, difference), difference)
    } else if op == 2 then {
        let compare = UInt(Field(54, 3));
        let less = I64Greater(right, left);
        let greater = I64Greater(left, right);
        let equal = I64Equal(left, right);
        let test = match compare {
            0 => false, 1 => less, 2 => equal, 3 => not_bool(greater), 4 => greater, 5 => not_bool(equal),
            6 => not_bool(less), _ => true
        };
        I64Select(test, one64, zero64)
    } else {
        /* the count wraps to 5 bits (bit 49) or clamps at 32; VSHR fills with A's sign when A is signed */
        let count = if Field(49, 1) == 0b1 then I64And(right, I64Const(31))
                    else I64Select(I64Greater(right, I64Const(32)), I64Const(32), right);
        if op == 3 then I64Shl(left, count) else if Field(48, 1) == 0b1 then I64Sar(left, count) else I64Shr(left, count)
    };
    R_write(Dst(), VideoFinish(result, R_read(UInt(Field(39, 8))), UInt(Field(51, 3)),
                               if op != 2 then UInt(Field(55, 1)) else 0, if op != 2 then UInt(Field(54, 1)) else 0,
                               if op == 3 & Field(48, 1) == 0b0 then 1 else 0))
}

/* PRMT: Bytes 0-3 are Ra's and 4-7 C's (Rc at bit 39, or the constant buffer word in the RC form). The selector (Rb, Rc
   in the RC form, the constant buffer word, or the immediate) picks each result byte: in the index mode (0) its nibble
   i, whose bit 3 replicates the byte's sign; in the others by c, its low two bits, as PTX's prmt defines F4E (1), B4E
   (2), RC8 (3), ECL (4), ECR (5) and RC16 (6) */
function execute_PRMT() -> unit = {
    let f = Form();
    let low = R_read(RegA());
    let selector = if f == 0 then R_read(RegB()) else if f == 1 then R_read(UInt(Field(39, 8)))
                   else if f == 2 then CbufWord()
                   else Bits32(ZeroExtend(Field(20, 19), 32) | (if Field(56, 1) == 0b1 then 0xfff80000 else 0x00000000));
    let high = if f == 1 then CbufWord() else R_read(UInt(Field(39, 8)));
    let mode = UInt(Field(48, 3));
    var result : bits(32) = 0x00000000;
    let three = Bits32(0x00000003);
    let sel = selector & three;
    foreach (k from 0 to 3) {
        let position = Bits32(to_bits(32, k));
        let nibble = if mode == 0 then (selector >> to_bits(32, 4 * k)) & 0x0000000f else 0x00000000;
        let idx = match mode {
            0 => nibble,
            1 => sel + position,
            2 => sel - position,
            3 => sel,
            4 => if UInt(sel) < UInt(position) then position else sel,
            5 => if UInt(sel) < UInt(position) then sel else position,
            _ => ((sel & 0x00000001) << 1) + to_bits(32, MOD(k, 2))
        };
        let byte0 = PrmtByte(low, high, idx);
        let byte = if mode == 0 then {
            /* a nibble's bit 3 replicates the byte's sign across it */
            let replicate = (nibble & 0x00000008) != 0x00000000;
            let negative = (byte0 & 0x00000080) != 0x00000000;
            if replicate then (if negative then 0x000000ff else 0x00000000) else byte0
        } else byte0;
        result = result | (byte << to_bits(32, 8 * k))
    };
    R_write(Dst(), result)
}

/* PSET: 1.0 (44) or all ones where the predicates' combination holds, else 0; .CC the flags */
function execute_PSET() -> unit = {
    let pa = PredRead(UInt(Field(12, 3)), UInt(Field(15, 1)));
    let pb = PredRead(UInt(Field(29, 3)), UInt(Field(32, 1)));
    let pc = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let test = Combine53(UInt(Field(45, 2)), Combine53(UInt(Field(24, 2)), pa, pb), pc);
    let value = RawSelect(test, if Field(44, 1) == 0b1 then 0x3f800000 else 0xffffffff, 0x00000000);
    if Field(47, 1) == 0b1 then FlagsLogic(value);
    R_write(Dst(), value)
}
function execute_PSETP() -> unit = {
    let pa = PredRead(UInt(Field(12, 3)), UInt(Field(15, 1)));
    let pb = PredRead(UInt(Field(29, 3)), UInt(Field(32, 1)));
    let pc = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let first = Combine53(UInt(Field(24, 2)), pa, pb);
    let second = Combine53(UInt(Field(24, 2)), not_bool(pa), pb);
    PredWrite(UInt(Field(3, 3)), Combine53(UInt(Field(45, 2)), first, pc));
    PredWrite(UInt(Field(0, 3)), Combine53(UInt(Field(45, 2)), second, pc))
}
/* CSETP and CSET: the condition code's test (8-12) combined with the predicate at 39 */
function execute_CSETP() -> unit = {
    let test = CcTest(UInt(Field(8, 5)));
    let condition = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let yes = Combine53(UInt(Field(45, 2)), test, condition);
    if Form() == 1 then {
        let truth = Bits32(if Field(44, 1) == 0b1 then 0x3f800000 else 0xffffffff);
        let result = RawSelect(yes, truth, 0x00000000);
        if Field(47, 1) == 0b1 then FlagsLogic(result);
        R_write(Dst(), result)
    } else {
        PredWrite(UInt(Field(3, 3)), yes);
        PredWrite(UInt(Field(0, 3)), Combine53(UInt(Field(45, 2)), not_bool(test), condition))
    }
}

/* P2R: a's bits of the mask (its low byte but of the immediate), at the byte 41-42 selects, from P0-P6 or with bit 40
   the flags z, s, c, o */
function execute_P2R() -> unit = {
    let mask0 = RightOf(Form());
    let mask = if Form() != 2 then mask0 & 0x000000ff else mask0;
    let flags = Field(40, 1) == 0b1;
    var bits : bits(32) = 0x00000000;
    foreach (p from 0 to (if flags then 3 else 6)) {
        let value = if flags then FlagRead(p) else PredRead(p, 0);
        let bit = RawSelect(value, to_bits(32, 2 ^ p), 0x00000000);
        bits = bits | bit
    };
    let shift = Bits32(to_bits(32, UInt(Field(41, 2)) * 8));
    let shifted_mask = mask << shift;
    let base = R_read(RegA()) & not_vec(shifted_mask);
    let picked = bits & mask;
    R_write(Dst(), base | (picked << shift))
}
/* R2P: the predicates P0-P6 (or with bit 40 the flags) of a's bits of the byte at 41-42, where the mask has them */
function execute_R2P() -> unit = {
    let source0 = R_read(RegA());
    let source = if Field(41, 2) != 0b00 then source0 >> to_bits(32, UInt(Field(41, 2)) * 8) else source0;
    let mask = RightOf(Form());
    let flags = Field(40, 1) == 0b1;
    foreach (p from 0 to (if flags then 3 else 6)) {
        let enabled = (mask & to_bits(32, 2 ^ p)) != 0x00000000;
        let bit = source & to_bits(32, 2 ^ p);
        R2PSet(p, if flags then 1 else 0, enabled, bit != 0x00000000)
    }
}

/* ISETP and ISET: a and b compared (shader_integer_compare), combined with the predicate at 39 */
function execute_ISETP() -> unit = {
    let left = R_read(RegA());
    let right = RightOf(Form());
    let test = IntegerCompare(left, right);
    let condition = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let yes = Combine53(UInt(Field(45, 2)), test, condition);
    let no = Combine53(UInt(Field(45, 2)), RawNot(test), condition);
    PredWrite(UInt(Field(3, 3)), yes);
    PredWrite(UInt(Field(0, 3)), no)
}
function execute_ISET() -> unit = {
    let left = R_read(RegA());
    let right = RightOf(Form());
    let test0 = IntegerCompare(left, right);
    let condition = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let test = Combine53(UInt(Field(45, 2)), test0, condition);
    let truth = Bits32(if Field(44, 1) == 0b1 then 0x3f800000 else 0xffffffff);
    let result = RawSelect(test, truth, 0x00000000);
    if Field(47, 1) == 0b1 then FlagsLogic(result);
    R_write(Dst(), result)
}

/* IMNMX: the minimum of a and b where the predicate at 39 holds, else the maximum, signed with 48 */
function execute_IMNMX() -> unit = {
    let left = R_read(RegA());
    let right = RightOf(Form());
    let less = if Field(48, 1) == 0b1 then SGreater(right, left) else UInt(left) < UInt(right);
    let choose_min = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let value = MinMax(choose_min, less, left, right);
    if Field(47, 1) == 0b1 then FlagsLogic(value);
    R_write(Dst(), value)
}

/* VSETP: Each operand is independently sign- or zero-extended to 33 bits before comparing */
function execute_VSETP() -> unit = {
    let left = VmSelect(R_read(RegA()), UInt(Field(36, 3)) + 8 * UInt(Field(48, 1)));
    let right = if Field(50, 1) == 0b1 then VmSelect(R_read(RegB()), UInt(Field(28, 3)) + 8 * UInt(Field(49, 1)))
                else VmSelect(Bits32(ZeroExtend(Field(20, 16), 32)), 4 + 8 * UInt(Field(49, 1)));
    let left64 = I64Of(left, UInt(Field(48, 1)));
    let right64 = I64Of(right, UInt(Field(49, 1)));
    let less = I64Greater(right64, left64);
    let greater = I64Greater(left64, right64);
    let equal = I64Equal(left64, right64);
    let cmp = UInt(Field(43, 2)) + 4 * UInt(Field(47, 1));
    let result = match cmp {
        0 => false, 1 => less, 2 => equal, 3 => not_bool(greater), 4 => greater, 5 => not_bool(equal),
        6 => not_bool(less), _ => true
    };
    let condition = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    PredWrite(UInt(Field(3, 3)), Combine53(UInt(Field(45, 2)), result, condition));
    PredWrite(UInt(Field(0, 3)), Combine53(UInt(Field(45, 2)), not_bool(result), condition))
}
function execute_VMAD() -> unit = {
    let left = VmSelect(R_read(RegA()), UInt(Field(36, 3)) + 8 * UInt(Field(48, 1)));
    let right = if Field(50, 1) == 0b1 then VmSelect(R_read(RegB()), UInt(Field(28, 3)) + 8 * UInt(Field(49, 1)))
                else VmSelect(Bits32(ZeroExtend(Field(20, 16), 32)), 4 + 8 * UInt(Field(49, 1)));
    let product = left * right;
    R_write(Dst(), product + R_read(UInt(Field(39, 8))))
}
/* VMNMX: the minimum (or with 56 the maximum) of a's and b's selects, each signed or not, saturated to the destination's
   format (54 signed, 55 saturate), then the secondary operation with c (51-53) */
function execute_VMNMX() -> unit = {
    let asel = UInt(Field(36, 3)) + 8 * UInt(Field(48, 1));
    let bsel = UInt(Field(28, 3)) + 8 * UInt(Field(49, 1));
    let sa = DIV(asel, 8) == 1;
    let sb = DIV(bsel, 8) == 1;
    let maximum = Field(56, 1) == 0b1;
    let va = VmSelect(R_read(RegA()), asel);
    let vb = if Field(50, 1) == 0b1 then VmSelect(R_read(RegB()), bsel)
             else Bits32(if sb then SignExtend(Field(20, 16), 32) else ZeroExtend(Field(20, 16), 32));
    let less = if sa == sb then (if sa then SGreater(vb, va) else UInt(va) < UInt(vb))
               else if sa then RawOr(Negative(va), UInt(va) < UInt(vb))
               else RawAnd(RawNot(Negative(vb)), UInt(va) < UInt(vb));
    let picked = RawSelect(less, if maximum then vb else va, if maximum then va else vb);
    let result_signed = if sa == sb then sa else not_bool(maximum);
    let signed_format = Field(54, 1) == 0b1;
    let saturated = if Field(55, 1) == 0b1 & signed_format & not_bool(result_signed) then {
        let cap = Bits32(0x7fffffff);
        let above = UInt(cap) < UInt(picked);
        RawSelect(above, cap, picked)
    } else if Field(55, 1) == 0b1 & not_bool(signed_format) & result_signed then {
        let negative = Negative(picked);
        RawSelect(negative, 0x00000000, picked)
    } else picked;
    let third = R_read(UInt(Field(39, 8)));
    let mode = UInt(Field(51, 3));
    let result = if mode < 4 then BitsInsert(third, saturated, to_bits(32, if mode == 0 | mode == 3 then 16 else 0),
                                             to_bits(32, if mode == 2 | mode == 3 then 8 else 16))
                 else if mode == 4 then saturated + third
                 else if mode == 5 | mode == 6 then {
                     let cmp = if signed_format then SGreater(saturated, third) else UInt(third) < UInt(saturated);
                     RawSelect(cmp, if mode == 5 then third else saturated, if mode == 5 then saturated else third)
                 } else saturated;
    R_write(Dst(), result)
}

/* ICMP: a where c (or the second operand in the RC form) compares with zero, else the other; ICMP has no .X: bit 43 is
   part of Rc */
function execute_ICMP() -> unit = {
    let f = Form();
    let third = R_read(UInt(Field(39, 8)));
    let right = if f == 1 | f == 2 then CbufWord() else if f == 3 then Right(2) else R_read(RegB());
    let comparison = if f == 1 then right else third;
    let alternative = if f == 1 then third else right;
    let test = IntegerCompareNoX(comparison, 0x00000000);
    R_write(Dst(), RawSelect(test, R_read(RegA()), alternative))
}

/* BFI: a's low bits into the base at the packed operand's offset (its low byte) and width (its second), an offset past
   31 leaving the base, a width cut at the word's end; .CC the flags */
function execute_BFI() -> unit = {
    let f = Form();
    let base = if f == 1 then CbufWord() else R_read(UInt(Field(39, 8)));
    let packed = if f == 2 then CbufWord() else if f == 1 then R_read(UInt(Field(39, 8)))
                 else if f == 3 then Right(2) else R_read(RegB());
    let mask = Bits32(0x000000ff);
    let offset = packed & mask;
    let width0 = (packed >> 8) & mask;
    let thirty_two = Bits32(0x00000020);
    let valid = UInt(offset) < UInt(thirty_two);
    let safe_offset = if valid then offset else 0x00000000;
    let remaining = thirty_two - safe_offset;
    let fits = UInt(width0) < UInt(remaining);
    let width = if fits then width0 else remaining;
    let inserted = BitsInsert(base, R_read(RegA()), safe_offset, width);
    let value = if valid then inserted else base;
    if Field(47, 1) == 0b1 then FlagsLogic(value);
    R_write(Dst(), value)
}

/* I2I: an integer's byte (41-43) or half of src_size (10-11), signed with 13, its absolute value (49) or negation
   (45), saturated (50) to the destination's size (8-9) and signedness (12), and cut to it */
function execute_I2I() -> unit = {
    var value : bits(32) = RightOf(Form());
    let src_size = UInt(Field(10, 2));
    let dst_size = UInt(Field(8, 2));
    let s13 = Field(13, 1) == 0b1;
    let s12 = Field(12, 1) == 0b1;
    if src_size < 2 then {
        let offset = Bits32(to_bits(32, UInt(Field(41, 3)) * 8));
        let width = Bits32(to_bits(32, 8 * 2 ^ src_size));
        value = if s13 then Bits32(BitsS(value, offset, width)) else BitsU(value, offset, width)
    };
    if Field(49, 1) == 0b1 then {
        let negative = Negative(value);
        let absolute = 0x00000000 - value;
        value = RawSelect(negative, absolute, value)
    };
    if Field(45, 1) == 0b1 then { value = 0x00000000 - value };
    if Field(50, 1) == 0b1 & dst_size < 2 then {
        let high = DIV(if s12 then 2147483647 else 4294967295, 2 ^ (32 - 8 * 2 ^ dst_size));
        let hi = Bits32(to_bits(32, high));
        if s13 then {
            let negative = Negative(value);
            if s12 then {
                let lo = Bits32(not_vec(to_bits(32, high)));
                let below = SGreater(lo, value);
                value = RawSelect(below, lo, value)
            } else { value = RawSelect(negative, 0x00000000, value) }
        };
        let above = if s13 & s12 then SGreater(value, hi) else UInt(hi) < UInt(value);
        value = RawSelect(above, hi, value)
    } else if Field(50, 1) == 0b1 & s12 != s13 then {
        /* 32 bits saturate only across a change of signedness */
        if s13 then { value = RawSelect(Negative(value), 0x00000000, value) } else {
            let top = Bits32(0x7fffffff);
            value = RawSelect(UInt(top) < UInt(value), top, value)
        }
    };
    if dst_size < 2 then {
        let width = Bits32(to_bits(32, 8 * 2 ^ dst_size));
        value = BitsU(value, 0x00000000, width)
    };
    if Field(47, 1) == 0b1 then FlagsLogic(value);
    R_write(Dst(), value)
}

/* ISCADD and ISCADD32I: (a << shift) + b, the shift at 39 (53 for ISCADD32I), b negated or a by 48-49, .CC at 47 (52);
   ISCADD32I: a 32-bit immediate, and no negation */
function execute_ISCADD() -> unit = {
    let imm32 = Form() == 3;
    let left = R_read(RegA());
    let avg = if imm32 then 0 else UInt(Field(48, 2));
    let shifted = left << to_bits(32, UInt(Field(if imm32 then 53 else 39, 5)));
    let rhs = if imm32 then Bits32(Field(20, 32)) else RightOf(Form());
    R_write(Dst(), IntegerAdd(shifted, rhs, avg, 0, UInt(Field(if imm32 then 52 else 47, 1))))
}

/* BFE: the field of a (bit-reversed with 40) at the packed operand's offset (its low byte, clamped to 31) and width
   (its second byte, cut at the word's end), signed with 48; an empty field, or one at 32 or past unsigned, is 0 */
function execute_BFE() -> unit = {
    let packed = RightOf(Form());
    let source0 = R_read(RegA());
    let source = if Field(40, 1) == 0b1 then BitReverse32(source0) else source0;
    let raw_offset = packed & 0x000000ff;
    let thirty_one = Bits32(0x0000001f);
    let offset = if UInt(thirty_one) < UInt(raw_offset) then thirty_one else raw_offset;
    let raw_width = (packed >> 8) & 0x000000ff;
    let remaining = 0x00000020 - offset;
    let width = if UInt(remaining) < UInt(raw_width) then remaining else raw_width;
    let nonzero0 = width != 0x00000000;
    let nonzero = if Field(48, 1) == 0b0 then nonzero0 & (UInt(raw_offset) < 32) else nonzero0;
    let safe_width = if nonzero then width else 0x00000001;
    let extracted = if Field(48, 1) == 0b1 then Bits32(BitsS(source, offset, safe_width)) else BitsU(source, offset, safe_width);
    let value = if nonzero then extracted else 0x00000000;
    if Field(47, 1) == 0b1 then FlagsLogic(value); /* BFE.CC, as Eden writes it */
    R_write(Dst(), value)
}

/* SHL: a << b, the count wrapped to 5 bits with 39, else 0 from 32 on */
function execute_SHL() -> unit = {
    let shift = RightOf(Form());
    let wrap = Field(39, 1) == 0b1;
    let safe = if wrap then shift & 0x0000001f else shift;
    let value = R_read(RegA()) << safe;
    if wrap then R_write(Dst(), value) else {
        let bound = Bits32(0x00000020);
        let valid = UInt(shift) < UInt(bound);
        R_write(Dst(), if valid then value else 0x00000000)
    }
}

/* LEA: (a << shift) + b, its high form the high word of (c:a) << shift; negated with 45 (37, 56 in the high forms: the
   high word's two's complement with the low word's carry). .X adds the carry in (LO at 46, HI at 38 or 57), .CC writes
   the flags, and LO's predicate takes the add's carry out: LEA.LO ... P0 then LEA.HI.X is how a 64-bit address is
   formed */
function execute_LEA() -> unit = {
    let f = Form(); /* 0 LR, 1 LC, 2 LI, 3 HR, 4 HC */
    let high_form = f >= 3;
    let shift = UInt(Field(if high_form then (if f == 3 then 28 else 51) else 39, 5));
    let negative = Field(if high_form then (if f == 3 then 37 else 56) else 45, 1) == 0b1;
    let source0 = R_read(RegA());
    let rhs = if f == 0 | f == 3 then R_read(RegB()) else if f == 2 then Right(2) else Right(1);
    let value : bits(32) = if high_form then {
        let hi = R_read(UInt(Field(39, 8)));
        let v = if shift != 0 then (source0 >> to_bits(32, 32 - shift)) | (hi << to_bits(32, shift)) else hi;
        if negative then {
            let carry = (source0 << to_bits(32, shift)) == 0x00000000;
            not_vec(v) + (if carry then 0x00000001 else 0x00000000)
        } else v
    } else {
        let source = if negative then 0x00000000 - source0 else source0;
        source << to_bits(32, shift)
    };
    let extended = Field(if f == 3 then 38 else if f == 4 then 57 else 46, 1) == 0b1;
    let old_carry = if extended then FlagRead(2) else false;
    let sum = IntegerAdd(value, rhs, 0, if extended then 1 else 0, UInt(Field(47, 1)));
    R_write(Dst(), sum);
    if UInt(Field(48, 3)) < 7 then {
        let carry0 = UInt(sum) < UInt(value);
        let carry = if extended then RawOr(carry0, RawAnd(old_carry, sum == value)) else carry0;
        PredWrite(UInt(Field(48, 3)), carry)
    }
}

/* SHF: the funnel shift of (c:a) by b, left or right, the count wrapped (50) or clamped to the type's (37-38: 2 and 3 the
   64-bit ones, 3 signed) */
function execute_SHF() -> unit = {
    let f = Form(); /* 0 LR, 1 LI, 2 RR, 3 RI */
    let left_shift = f <= 1;
    let shift = if f == 0 | f == 2 then R_read(RegB()) else Bits32(ZeroExtend(Field(20, 19), 32));
    let longest = UInt(Field(37, 2)) >= 2;
    let limit = Bits32(if longest then 0x0000003f else 0x00000020);
    let wrapped = shift & (if longest then 0x0000003f else 0x0000001f);
    let safe = if Field(50, 1) == 0b1 then wrapped else (if UInt(limit) < UInt(shift) then limit else shift);
    let low = R_read(RegA());
    let high = R_read(UInt(Field(39, 8)));
    let small = safe & 0x0000001f;
    let left = high << small;
    let reverse = (0x00000020 - safe) & 0x0000001f;
    let right = low >> reverse;
    let combined0 = left | right;
    let combined = if left_shift then (if safe == 0x00000000 then high else combined0) else {
        let low_part = low >> small;
        let high_part = high << reverse;
        let c = low_part | high_part;
        if safe == 0x00000000 then low else c
    };
    let long_shift = (safe - 0x00000020) & 0x0000001f;
    let long_value = if left_shift then low << long_shift else if Field(37, 2) == 0b11 then AshrS(high, long_shift)
                     else high >> long_shift;
    R_write(Dst(), if UInt(safe) < 32 then combined else long_value)
}

/* SHR: a >> b, arithmetic with 48 (the count clamped to 31 unless wrapped, 39), logical else (0 from 32 on unless
   wrapped); a bit-reversed first with 40 */
function execute_SHR() -> unit = {
    let shift = RightOf(Form());
    let source0 = R_read(RegA());
    let source = if Field(40, 1) == 0b1 then BitReverse32(source0) else source0;
    let safe0 = shift & 0x0000001f;
    let wrap = Field(39, 1) == 0b1;
    let value = if Field(48, 1) == 0b1 then {
        let safe = if not_bool(wrap) then (if UInt(shift) < 31 then shift else 0x0000001f) else safe0;
        AshrS(source, safe)
    } else source >> safe0;
    if Field(48, 1) == 0b0 & not_bool(wrap) then {
        let valid = UInt(shift) < 32;
        R_write(Dst(), if valid then value else 0x00000000)
    } else R_write(Dst(), value)
}
