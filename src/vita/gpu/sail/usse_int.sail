/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The USSE's integer and 8-bit color instructions (usse_int.roots; back end usse_spirv.h's): the bitwise VBW, the
   scalar unit's SOP2 and SOP2M, and the integer multiply-adds I16MAD, I32MAD and I32MAD2 (Vita3K's alu.cpp and
   ialu.cpp) */

/* VBW: src1 and, or, xor src2, shifted left, right or arithmetically right by it (its low five bits), or rotated left
   by it; src2 rotated left by rot and inverted first, or the immediate (src2's number, src2_sel and exth) so,
   of 32 bits or (partial) of 16. A repeat at a time */
function execute_VBW() -> unit = {
    let op1 = UInt(Field(59, 3));
    let op2 = Bit(35);
    if op1 < 2 | op1 > 5 then throw(Refuse("VBW operation %u", op1));
    /* 0 and, 1 or, 2 xor, 3 shl, 4 rol, 5 shr, 6 asr */
    let op = if op1 == 2 then op2 else if op1 == 3 then 2 else if op1 == 4 then 3 + op2 else 5 + op2;
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 0, 7);
    let partial = Bit(34) == 1;
    let bits = if partial then 16 else 32;
    let type = if partial then T_U16 else T_U32;
    SetType(SRC1, type);
    SetType(SRC2, type);
    SetType(DEST, type);
    let rot = UInt(Field(38, 5));
    let invert = Bit(43) == 1;
    Multiplier(1, 1, 1, 1);
    foreach (r from 0 to UInt(Field(44, 4))) {
        if not_bool(Failed()) then {
            let b = if Bit(48) == 1 & BankOf(SRC2) == B_IMM then {
                /* the immediate, rotated and inverted as encoded */
                let v0 : bits(32) = ZeroExtend(Field(36, 2) @ Field(14, 7) @ Field(0, 7), 32);
                let n = MOD(rot, bits);
                let v1 = if n == 0 then v0 else if bits == 16 then
                             (LSL(v0, n) | LSR(v0 & 0x0000ffff, 16 - n)) & 0x0000ffff
                         else LSL(v0, n) | LSR(v0, 32 - n);
                let v2 = if invert then not_vec(v1) else v1;
                UConst(UInt(if bits == 16 then v2 & 0x0000ffff else v2))
            } else {
                Load(B, SRC2, 1, Rep(SRC2, r, O_SRC2, R_SLMSI));
                let b0 = At(B, 0);
                let b1 = if rot != 0 then RotateLeft(b0, UConst(rot), bits) else b0;
                if invert then {
                    let b2 = not_vec(b1);
                    if bits == 16 then AndT(b2, UConst(65535)) else b2
                } else b1
            };
            Load(A, SRC1, 1, Rep(SRC1, r, O_SRC1, R_SLMSI));
            if not_bool(Failed()) then {
                let a = At(A, 0);
                let shift = AndT(b, UConst(31));
                Reset(R, 1, K_UINT);
                Put(R, 0, match op { 0 => AndT(a, b), 1 => OrT(a, b), 2 => XorT(a, b), 3 => ShlT(a, shift),
                                     5 => ShrT(a, shift), 6 => AsrT(a, shift), _ => RotateLeft(a, b, bits) });
                Store(DEST, R, 1, Rep(DEST, r, O_DEST, R_SLMSI))
            }
        }
    };
    ResetMultiplier()
}

/* 1 - x where enable is set */
function Complement(x : bits(32), enable : int) -> bits(32) = if enable == 1 then FSub(One(), x) else x
/* SOP2's and SOP2M's operations: 0 add, 1 subtract, 2 min, 3 max */
function SopApply(op : int, a : bits(32), b : bits(32)) -> bits(32) =
    match op { 0 => FAdd(a, b), 1 => FSub(a, b), 2 => FMin(a, b), _ => FMax(a, b) }

let C1 : int = 0
let C2 : int = 1
let A1 : int = 2
let COLOR : int = 3
let A2 : int = 4
let ALPHA : int = 5
/* SOP2: each source's color (xyz) and alpha (w) as unsigned bytes, each scaled by the factor its selector names (0,
   src1's color, src2's color, src1's alpha, src2's alpha; complemented by its modifier), then added, subtracted or
   the min or max (cop for the color, aop for the alpha). A repeat at a time */
function execute_SOP2() -> unit = {
    let csel1 = UInt(Field(38, 3));
    let csel2 = UInt(Field(35, 3));
    let asel1 = UInt(Field(52, 2));
    let asel2 = UInt(Field(41, 2));
    if csel1 > 4 | csel2 > 4 | asel1 > 2 | asel2 > 2 then
        throw(Refuse("SOP2 selectors %u %u %u %u", csel1, csel2, asel1, asel2));
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 0, 7);
    SetType(SRC1, T_U8);
    SetType(SRC2, T_U8);
    SetType(DEST, T_U8);
    let mod1 = Bit(20);
    foreach (r from 0 to UInt(Field(44, 3))) {
        if not_bool(Failed()) then {
            let o1 = Rep(SRC1, r, O_SRC1, R_SLMSI);
            let o2 = Rep(SRC2, r, O_SRC2, R_SLMSI);
            Load(C1, SRC1, 7, o1);
            ToFloat(C1, T_U8, 1);
            Load(C2, SRC2, 7, o2);
            ToFloat(C2, T_U8, 1);
            Load(A1, SRC1, 8, o1);
            ToFloat(A1, T_U8, 1);
            Load(A2, SRC2, 8, o2);
            ToFloat(A2, T_U8, 1);
            if not_bool(Failed()) then {
                foreach (i from 0 to 2) Put(C1, i, Complement(At(C1, i), mod1));
                Put(A1, 0, Complement(At(A1, 0), mod1));
                Reset(COLOR, 3, K_FLOAT);
                foreach (i from 0 to 2) {
                    let zero = Zero();
                    let c1 = At(C1, i);
                    let c2 = At(C2, i);
                    let pick1 = match csel1 { 0 => zero, 1 => c1, 2 => c2, 3 => At(A1, 0), _ => At(A2, 0) };
                    let pick2 = match csel2 { 0 => zero, 1 => c1, 2 => c2, 3 => At(A1, 0), _ => At(A2, 0) };
                    let lhs = FMul(Complement(pick1, Bit(56)), c1);
                    let rhs = FMul(Complement(pick2, Bit(47)), c2);
                    Put(COLOR, i, SopApply(UInt(Field(18, 2)), lhs, rhs))
                };
                let zero = Zero();
                let a1 = At(A1, 0);
                let a2 = At(A2, 0);
                let pick1 = match asel1 { 0 => zero, 1 => a1, _ => a2 };
                let pick2 = match asel2 { 0 => zero, 1 => a1, _ => a2 };
                let lhs = FMul(Complement(pick1, Bit(43)), a1);
                let rhs = FMul(Complement(pick2, Bit(34)), a2);
                Reset(ALPHA, 1, K_FLOAT);
                Put(ALPHA, 0, SopApply(UInt(Field(16, 2)), lhs, rhs));
                let od = Rep(DEST, r, O_DEST, R_SLMSI);
                ToInt(COLOR, T_U8, 1);
                Store(DEST, COLOR, 7, od);
                ToInt(ALPHA, T_U8, 1);
                Store(DEST, ALPHA, 8, od)
            }
        }
    }
}

/* SOP2M: SOP2's operations on the components the write mask enables (its first bit the alpha), each source's factor
   one of 0, src1's component, src1's alpha, src2's component and src2's alpha (sel1 and sel2: 0, 2, 3, 6, 7) */
function execute_SOP2M() -> unit = {
    let sel1 = UInt(Field(38, 3));
    let sel2 = UInt(Field(35, 3));
    if sel1 == 1 | sel1 == 4 | sel1 == 5 | sel2 == 1 | sel2 == 4 | sel2 == 5 then
        throw(Refuse("SOP2M selectors %u %u", sel1, sel2));
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 0, 7);
    SetType(SRC1, T_U8);
    SetType(SRC2, T_U8);
    SetType(DEST, T_U8);
    Load(A, SRC1, 15, 0);
    ToFloat(A, T_U8, 1);
    Load(B, SRC2, 15, 0);
    ToFloat(B, T_U8, 1);
    if Failed() then throw(Stop());
    let w = UInt(Field(43, 4));
    let wmask = DIV(w, 2) + 8 * MOD(w, 2);
    Reset(R, 0, K_FLOAT);
    foreach (i from 0 to 3) {
        if MOD(DIV(wmask, 2 ^ i), 2) == 1 then {
            let zero = Zero();
            let a = At(A, i);
            let b = At(B, i);
            let x = FMul(Complement(match sel1 { 0 => zero, 2 => a, 3 => At(A, 3), 6 => b, _ => At(B, 3) }, Bit(56)), a);
            let y = FMul(Complement(match sel2 { 0 => zero, 2 => a, 3 => At(A, 3), 6 => b, _ => At(B, 3) }, Bit(47)), b);
            Push(R, SopApply(if i == 3 then UInt(Field(41, 2)) else UInt(Field(52, 2)), x, y))
        }
    };
    ToInt(R, T_U8, 1);
    Store(DEST, R, wmask, 0)
}

/* the integer multiply-add of values A, B and C's first components, of A's type (B's and C's bitcast to it), into R */
function Imad() -> unit = {
    let kind = KindOf(A);
    let b = if KindOf(B) != kind then { if kind == K_INT then AsI32(At(B, 0)) else AsU32(At(B, 0)) } else At(B, 0);
    let c = if KindOf(C) != kind then { if kind == K_INT then AsI32(At(C, 0)) else AsU32(At(C, 0)) } else At(C, 0);
    Reset(R, 1, kind);
    Put(R, 0, IAddT(IMulT(At(A, 0), b), c))
}

/* I16MAD: src0 * src1 + src2 of 16-bit integers (mode 2 and 3 signed), src1 and src2 a byte of theirs (the high one
   with sel1h, sel2h) where their format says. A repeat at a time */
function execute_I16MAD() -> unit = {
    let signed = UInt(Field(42, 2)) >= 2;
    let type = if signed then T_I16 else T_U16;
    let bytes = if signed then T_I8 else T_U8;
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 0, 7);
    Src0(SRC0, UInt(Field(14, 7)), Bit(34), 0, 0, 7);
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    SetType(DEST, type);
    SetType(SRC0, type);
    SetType(SRC1, type);
    SetType(SRC2, type);
    let f1 = UInt(Field(38, 2));
    let f2 = UInt(Field(40, 2));
    if f1 > 1 | f2 > 1 then throw(Refuse("I16MAD of sign-extended bytes"));
    if f1 == 1 then SetType(SRC1, bytes);
    if f2 == 1 then SetType(SRC2, bytes);
    let m1 = if f1 == 1 & Bit(52) == 1 then 2 else 1;
    let m2 = if f2 == 1 & Bit(37) == 1 then 2 else 1;
    foreach (r from 0 to UInt(Field(44, 3))) {
        if not_bool(Failed()) then {
            Load(A, SRC0, 1, Rep(SRC0, r, O_SRC0, R_SLMSI));
            Load(B, SRC1, m1, Rep(SRC1, r, O_SRC1, R_SLMSI));
            Load(C, SRC2, m2, Rep(SRC2, r, O_SRC2, R_SLMSI));
            if not_bool(Failed()) then {
                Imad();
                Store(DEST, R, 1, Rep(DEST, r, O_DEST, R_SLMSI))
            }
        }
    }
}

/* I32MAD: src0 * src1 + src2 of 16-bit sources (each its high half where it says; src2 type 2 a 32-bit source), a
   32-bit result. A repeat at a time */
function execute_I32MAD() -> unit = {
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 0, 7);
    Src0(SRC0, UInt(Field(14, 7)), Bit(34), 0, 0, 7);
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    let signed = Bit(43) == 1;
    let wide = if signed then T_I32 else T_U32;
    let narrow = if signed then T_I16 else T_U16;
    let src2_type = UInt(Field(38, 2));
    SetType(DEST, wide);
    SetType(SRC0, narrow);
    SetType(SRC1, narrow);
    SetType(SRC2, if src2_type == 2 then wide else narrow);
    let m0 = if Bit(56) == 1 then 2 else 1;
    let m1 = if Bit(53) == 1 then 2 else 1;
    let m2 = if src2_type != 2 & Bit(52) == 1 then 2 else 1;
    Multiplier(1, 1, 1, 1);
    foreach (r from 0 to UInt(Field(44, 3))) {
        if not_bool(Failed()) then {
            Load(A, SRC0, m0, Rep(SRC0, r, O_SRC0, R_SLMSI));
            Load(B, SRC1, m1, Rep(SRC1, r, O_SRC1, R_SLMSI));
            Load(C, SRC2, m2, Rep(SRC2, r, O_SRC2, R_SLMSI));
            if not_bool(Failed()) then {
                Imad();
                Store(DEST, R, 1, Rep(DEST, r, O_DEST, R_SLMSI))
            }
        }
    };
    ResetMultiplier()
}

/* I32MAD2: src0 * src1 + src2 of 32-bit integers, src1 and src2 negated where they say; sn passes src2 through
   (Vita3K's hardware tests). A repeat at a time */
function execute_I32MAD2() -> unit = {
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 0, 7);
    Src0(SRC0, UInt(Field(14, 7)), Bit(34), Bit(47), 0, 7);
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    let type = if Bit(41) == 1 then T_I32 else T_U32;
    SetType(DEST, type);
    SetType(SRC0, type);
    SetType(SRC1, type);
    SetType(SRC2, type);
    if Bit(40) == 1 then AddFlags(SRC1, F_NEG);
    if Bit(39) == 1 then AddFlags(SRC2, F_NEG);
    Multiplier(1, 1, 1, 1);
    foreach (r from 0 to UInt(Field(44, 3))) {
        if not_bool(Failed()) then {
            Load(A, SRC0, 1, Rep(SRC0, r, O_SRC0, R_SLMSI));
            Load(B, SRC1, 1, Rep(SRC1, r, O_SRC1, R_SLMSI));
            Load(C, SRC2, 1, Rep(SRC2, r, O_SRC2, R_SLMSI));
            if not_bool(Failed()) then {
                if UInt(Field(52, 2)) != 0 then Store(DEST, C, 1, Rep(DEST, r, O_DEST, R_SLMSI))
                else {
                    Imad();
                    Store(DEST, R, 1, Rep(DEST, r, O_DEST, R_SLMSI))
                }
            }
        }
    };
    ResetMultiplier()
}
