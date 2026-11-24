/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The USSE's moves (usse_data.roots; back end usse_spirv.h's): VMOV and its conditional forms, VPCK's packs and
   conversions, LIMM's immediate, and the repeats' increments SMLSI and SMBO set (Vita3K's data.cpp and
   special.cpp) */

/* the zero of a kind: an f32, a u32 or an i32 */
function ZeroOf(kind : int) -> bits(32) =
    if kind == K_FLOAT then Zero() else if kind == K_UINT then UConst(0) else SConst(0)
/* the f32 of a swizzle's constant channel: 0.0, 1.0, 2.0, else 0.5 */
function ChannelConstant(ch : int) -> bits(32) =
    FConst(if ch == SW_0 then 0x00000000 else if ch == SW_1 then 0x3f800000 else if ch == SW_2 then 0x40000000
           else 0x3f000000)

/* VMOV's test of x against zero (compare: 0 ==, 1 !=, 2 <, 3 <=), of the test's kind */
function MoveTest(kind : int, compare : int, x : bits(32), zero : bits(32)) -> bool =
    if kind == K_FLOAT then FCompare(if compare == 0 then 2 else if compare == 1 then 5 else if compare == 2 then 1
                                     else 3, x, zero)
    else if compare == 0 then IEqT(x, zero) else if compare == 1 then INeT(x, zero)
    else if kind == K_UINT then { if compare == 2 then ULtT(x, zero) else ULeT(x, zero) }
    else { if compare == 2 then SLtT(x, zero) else SLeT(x, zero) }

let TEST : int = 4

/* VMOV: src1's components the mask enables into the destination; with a move type, each where the test of src0's
   same component against zero holds (test1 and test2), else src2's. A repeat at a time */
function execute_VMOV() -> unit = {
    let move_type = UInt(Field(46, 2));
    let type = UInt(Field(40, 3));
    if move_type == 3 then throw(Refuse("VMOV move type 3"));
    let doubled = type == T_C10 | type == T_F16 | type == T_F32;
    let conditional = move_type != 0;
    let bits = if doubled then 7 else 6;
    Dest(DEST, UInt(Field(18, 6)), UInt(Field(32, 2)), Bit(51), doubled, bits);
    Src12(SRC1, UInt(Field(6, 6)), UInt(Field(30, 2)), Bit(49), doubled, bits);
    let mask = if type == T_F16 | type == T_F32 then WriteMask(DEST, UInt(Field(24, 4)), type == T_F16)
               else if not_bool(doubled) then 1 else UInt(Field(24, 4));
    if doubled then Vec34Swizzle(SRC1, UInt(Field(35, 4)), 0, 2) else SetSwizzle(SRC1, SW_X, SW_Y, SW_Z, SW_W);
    SetType(SRC1, type);
    SetType(DEST, type);
    let test_type = if move_type == 2 then T_U8 else type;
    let compare = Bit(54) * 2 + Bit(39);
    if conditional then {
        Src0(SRC0, UInt(Field(12, 6)), Bit(34), Bit(50), doubled, bits);
        SetType(SRC0, test_type);
        Src12(SRC2, UInt(Field(0, 6)), UInt(Field(28, 2)), Bit(48), doubled, bits);
        SetType(SRC2, type);
        if Bit(53) == 1 then CopySwizzle(SRC0, SRC1);
        CopySwizzle(SRC2, SRC1)
    };
    if doubled & (type == T_F16 | type == T_F32) then Multiplier(2, 2, 2, 2) else Multiplier(1, 1, 1, 1);
    foreach (r from 0 to UInt(Field(44, 2))) {
        if not_bool(Failed()) then {
            Load(A, SRC1, mask, Rep(SRC1, r, O_SRC1, R_SLMSI));
            CopyValue(R, A);
            if conditional then {
                Load(TEST, SRC0, mask, Rep(SRC0, r, O_SRC0, R_SLMSI));
                Load(B, SRC2, mask, Rep(SRC2, r, O_SRC2, R_SLMSI));
                let kind = KindOf(TEST);
                foreach (i from 0 to Min(Count(A), Count(TEST)) - 1) {
                    let zero = ZeroOf(kind);
                    let cond = MoveTest(kind, compare, At(TEST, i), zero);
                    let a = At(A, i);
                    let b = At(B, i);
                    Put(R, i, if cond then a else b)
                }
            };
            Store(DEST, R, mask, Rep(DEST, r, O_DEST, R_SLMSI))
        }
    };
    ResetMultiplier()
}

/* VPCK's formats: U8, I8, O8, U16, I16, F16, F32, C10 */
function PackType(f : int) -> int = match f { 0 => T_U8, 1 => T_I8, 2 => T_O8, 3 => T_U16, 4 => T_I16, 5 => T_F16,
                                              6 => T_F32, _ => T_C10 }
let LO : int = 4
let HI : int = 5
let OLD : int = 6

/* VPCK: src1's components (its swizzle the four selectors) converted from the source format to the destination's,
   scaled to the formats' ranges with scale; an F32 source reads src1's x and y, then src2's, as one range of four
   where the selectors or the registers ask it. A repeat at a time */
function execute_VPCK() -> unit = {
    let dest_type = PackType(UInt(Field(38, 3)));
    let src_type = PackType(UInt(Field(41, 3)));
    let mask = UInt(Field(34, 4));
    let scale = Bit(18) == 1;
    let src2_n = UInt(Field(1, 6));
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), false, 7);
    SetType(DEST, dest_type);
    let float_src = IsFloatType(src_type);
    let float_dest = IsFloatType(dest_type);
    let src1_n = if float_src then UInt(Field(8, 6)) else Bit(7) + 2 * UInt(Field(8, 6));
    Src12(SRC1, src1_n, UInt(Field(30, 2)), Bit(49), float_src, 7);
    Src12(SRC2, src2_n, UInt(Field(28, 2)), Bit(48), true, 7);
    SetType(SRC1, src_type);
    let sel0 = Bit(0) + 2 * (if src_type == T_F32 then Bit(7) else MOD(src2_n, 2));
    let sel1 = UInt(Field(16, 2));
    let sel2 = UInt(Field(14, 2));
    let sel3 = UInt(Field(19, 2));
    SetSwizzle(SRC1, sel0, sel1, sel2, sel3);
    let no_cycle = (float_src & float_dest) | (scale & (src_type == T_U8 | dest_type == T_U8));
    let use_src2 = if src_type == T_F32 & BankOf(SRC2) != B_IMM then {
        let same = NumOf(SRC1) == NumOf(SRC2) & BankOf(SRC1) == BankOf(SRC2);
        let contiguous = mask == 1 | mask == 3 | mask == 7 | mask == 15;
        let n = Popcount4(mask);
        let defaulted = (n < 1 | sel0 == 0) & (n < 2 | sel1 == 1) & (n < 3 | sel2 == 2) & (n < 4 | sel3 == 3);
        not_bool(defaulted) | not_bool(same) | not_bool(contiguous)
    } else false;
    /* the selectors cycled over the components the mask enables (two of them, for bytes, when it enables all) */
    if not_bool(no_cycle) then {
        let two = (src_type == T_U8 | dest_type == T_U8) & mask == 15;
        let m0 = MOD(mask, 2);
        let m1 = MOD(DIV(mask, 2), 2);
        let m2 = MOD(DIV(mask, 4), 2);
        let n1 = m0;
        let n2 = m0 + m1;
        let n3 = m0 + m1 + m2;
        SetSwizzle(SRC1, if two then sel0 else if m0 == 1 then sel0 else sel0,
                   if two then sel1 else if m1 == 1 then (if n1 == 0 then sel0 else sel1) else sel1,
                   if two then sel0 else if m2 == 1 then (if n2 == 0 then sel0 else if n2 == 1 then sel1 else sel2)
                   else sel2,
                   if two then sel1 else if DIV(mask, 8) == 1 then
                       (if n3 == 0 then sel0 else if n3 == 1 then sel1 else if n3 == 2 then sel2 else sel3)
                   else sel3)
    };
    Multiplier(1, if float_src then 2 else 1, if float_src then 2 else 1, 1);
    foreach (r from 0 to UInt(Field(44, 4))) {
        if not_bool(Failed()) then {
            let o1 = Rep(SRC1, r, O_SRC1, R_SLMSI);
            let o2 = Rep(SRC2, r, O_SRC2, R_SLMSI);
            if use_src2 then {
                /* the two sources as one range of four: src1's x and y, then src2's */
                CopyOperand(LO, SRC1);
                CopyOperand(HI, SRC2);
                SetSwizzle(LO, SW_X, SW_Y, SW_Z, SW_W);
                SetSwizzle(HI, SW_X, SW_Y, SW_Z, SW_W);
                SetType(HI, src_type);
                Load(LO, LO, 3, o1);
                Load(HI, HI, 3, o2);
                Reset(R, 0, K_FLOAT);
                foreach (i from 0 to 3) {
                    if MOD(DIV(mask, 2 ^ i), 2) == 1 then {
                        let ch = ChannelOf(SRC1, i);
                        Push(R, if ch == SW_X then At(LO, 0) else if ch == SW_Y then At(LO, 1)
                                else if ch == SW_Z then At(HI, 0) else if ch == SW_W then At(HI, 1)
                                else ChannelConstant(ch))
                    }
                }
            } else Load(R, SRC1, mask, o1);
            if float_dest & not_bool(float_src) then ToFloat(R, src_type, scale);
            if not_bool(float_dest) & float_src then ToInt(R, dest_type, scale);
            if not_bool(float_dest) & not_bool(float_src) & IsSignedType(dest_type) != IsSignedType(src_type) then {
                CopyValue(OLD, R);
                SetKind(R, if IsSignedType(dest_type) then K_INT else K_UINT);
                foreach (i from 0 to Count(OLD) - 1) Put(R, i, At(OLD, i))
            };
            Store(DEST, R, mask, Rep(DEST, r, O_DEST, R_SLMSI))
        }
    };
    ResetMultiplier()
}

/* LIMM: its 32-bit immediate (26-31 at 44, 21-25 at 36, 0-20 at 0) into the destination */
function execute_LIMM() -> unit = {
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), false, 7);
    SetType(DEST, T_U32);
    Reset(R, 1, K_UINT);
    Put(R, 0, UConst(UInt(Field(44, 6) @ Field(36, 5) @ Field(0, 21))));
    Store(DEST, R, 1, 0)
}

/* SMLSI: each operand's increments (src0, src1, src2, the destination): with its mode, the four two-bit ones of its
   byte, else repeat i's i times the signed byte */
function execute_SMLSI() -> unit = {
    foreach (k from 0 to 3) {
        let at = if k == 3 then 24 else 16 - 8 * k;
        let mode = if k == 3 then Bit(35) else Bit(34 - k);
        if mode == 1 then {
            foreach (i from 0 to 3) SetIncrease(k, i, UInt(Field(at + 2 * i, 2)))
        } else {
            foreach (i from 0 to 16) SetIncrease(k, i, i * SInt(Field(at, 8)))
        }
    }
}

/* SMBO: each operand's base offset (src0 at 24, src1 at 12, src2 at 0, the destination at 36) */
function execute_SMBO() -> unit = {
    foreach (k from 0 to 3) {
        let at = if k == 3 then 36 else 24 - 12 * k;
        foreach (i from 0 to 16) SetIncrease(k, i, i + UInt(Field(at, 12)))
    }
}

/* PHAS, NOP, BR (the program's blocks are translate_code's) and the special instructions SPEC names: nothing */
function execute_NOTHING() -> unit = ()
