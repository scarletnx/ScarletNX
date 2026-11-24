/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The USSE's tests (usse_test.roots; back end usse_test_spirv.h): VTST into a predicate register and VTSTMSK into a
   register's mask (Vita3K's branch_cond.cpp): an ALU operation of the two sources, its result compared with zero,
   or (a subtraction) the sources compared */

/* the tests' results, a bool each component, and predicate register n written */
val SetTest : (int, bool) -> unit
val TestAt : int -> bool
val PredWrite : (int, bool) -> unit

/* the operations (decode_test_inst) */
let TO_INVALID : int = 0
let TO_ADD : int = 1
let TO_SUB : int = 2
let TO_FRC : int = 3
let TO_RCP : int = 4
let TO_RSQ : int = 5
let TO_LOG : int = 6
let TO_EXP : int = 7
let TO_DP : int = 8
let TO_MIN : int = 9
let TO_MAX : int = 10
let TO_DSX : int = 11
let TO_DSY : int = 12
let TO_MUL : int = 13
let TO_IADD : int = 14
let TO_ISUB : int = 15
let TO_IMUL : int = 16
let TO_AND : int = 17
let TO_OR : int = 18
let TO_XOR : int = 19
let TO_SHL : int = 20
let TO_SHR : int = 21
let TO_ROL : int = 22
let TO_ASR : int = 23

/* the operation of an ALU (alu_sel: 0 the vector floats, 1 the integers, 2 the bytes, 3 the bitwise ones) and its
   op */
function TestOp(sel : int, op : int) -> int =
    if sel == 0 then match op { 2 => TO_ADD, 3 => TO_FRC, 4 => TO_RCP, 5 => TO_RSQ, 6 => TO_LOG, 7 => TO_EXP,
                                8 => TO_DP, 9 => TO_MIN, 10 => TO_MAX, 11 => TO_DSX, 12 => TO_DSY, 13 => TO_MUL,
                                14 => TO_SUB, _ => TO_INVALID }
    else if sel == 1 then {
        if op < 6 then TO_INVALID else if op == 6 | op == 9 | op == 12 | op == 13 then TO_IADD
        else if op == 7 | op == 10 | op == 14 | op == 15 then TO_ISUB else TO_IMUL
    } else if sel == 2 then match op { 0 => TO_IADD, 1 => TO_ISUB, 2 => TO_IADD, 3 => TO_ISUB, 4 => TO_IMUL,
                                       6 => TO_IMUL, 8 => TO_ISUB, _ => TO_INVALID }
    else match op { 0 => TO_AND, 1 => TO_OR, 2 => TO_XOR, 3 => TO_SHL, 4 => TO_SHR, 5 => TO_ROL, 7 => TO_ASR,
                    _ => TO_INVALID }
/* the type its sources are read in (prec: the vector floats' F32) */
function TestType(sel : int, op : int, prec : int) -> int =
    if sel == 0 then (if prec == 1 then T_F32 else T_F16)
    else if sel == 1 then {
        if op < 6 then T_UNK else if op <= 8 then T_I16 else if op <= 11 then T_U16
        else if op == 12 | op == 14 then T_I32 else T_U32
    } else if sel == 2 then T_U8 else T_U32

/* the sources of a test: 64-bit registers for the vector ALU, src2 one component for all with vscomp; src1 negated
   where it says */
function TestSources(sel : int, type : int) -> unit = {
    let doubled = sel == 0;
    let bits = if doubled then 8 else 7;
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), doubled, bits);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), doubled, bits);
    SetType(SRC1, type);
    SetType(SRC2, type);
    if Bit(46) == 1 & doubled then SetSwizzle(SRC2, SW_X, SW_X, SW_X, SW_X);
    if Bit(50) == 1 then AddFlags(SRC1, F_NEG)
}

/* the test of op of src1 and src2's components the mask enables (zero_test 1 includes equality; sign_test 0 is
   (in)equality, 1 less than, 2 greater than), each a bool (SetTest): how many */
function TestCompare(kind : int, eq : bool, sign : int, x : bits(32), y : bits(32)) -> bool =
    if kind == K_FLOAT then FCompare(if eq then (if sign == 0 then 2 else if sign == 1 then 3 else 6)
                                     else (if sign == 0 then 5 else if sign == 1 then 1 else 4), x, y)
    else if sign == 0 then { if eq then IEqT(x, y) else INeT(x, y) }
    else if kind == K_INT then {
        if sign == 1 then { if eq then SLeT(x, y) else SLtT(x, y) } else { if eq then SGeT(x, y) else SGtT(x, y) }
    } else {
        if sign == 1 then { if eq then ULeT(x, y) else ULtT(x, y) } else { if eq then UGeT(x, y) else UGtT(x, y) }
    }
function Test(op : int, mask : int) -> int = {
    let sign = UInt(Field(42, 2));
    if sign == 3 then throw(Refuse("a test of sign 3"));
    Load(A, SRC1, mask, 0);
    Load(B, SRC2, mask, 0);
    CopyValue(R, A);
    if Failed() then throw(Stop());
    let kind = KindOf(A);
    if op != TO_SUB & op != TO_ISUB then {
        foreach (i from 0 to Count(A) - 1) {
            let x = At(A, i);
            let y = At(B, i);
            if op == TO_ADD then Put(R, i, FAdd(x, y))
            else if op == TO_MUL then Put(R, i, FMul(x, y))
            else if op == TO_MIN then Put(R, i, FMin(x, y))
            else if op == TO_MAX then Put(R, i, FMax(x, y))
            else if op == TO_FRC then Put(R, i, FSub(x, FRound(1, y)))
            else if op == TO_DSX then Put(R, i, FDdx(x))
            else if op == TO_DSY then Put(R, i, FDdy(x))
            else if op == TO_RCP then Put(R, i, FDiv(One(), x))
            else if op == TO_RSQ then Put(R, i, FInvSqrt(x))
            else if op == TO_LOG then Put(R, i, FLog2(x))
            else if op == TO_EXP then Put(R, i, FExp2(x))
            else if op == TO_IADD then Put(R, i, IAddT(x, y))
            else if op == TO_IMUL then Put(R, i, IMulT(x, y))
            else if op == TO_AND then Put(R, i, AndT(x, y))
            else if op == TO_OR then Put(R, i, OrT(x, y))
            else if op == TO_XOR then Put(R, i, XorT(x, y))
            else if op == TO_SHL then Put(R, i, ShlT(x, y))
            else if op == TO_SHR then Put(R, i, ShrT(x, y))
            else if op == TO_ASR then Put(R, i, AsrT(x, y))
            else if op == TO_ROL then Put(R, i, RotateLeft(x, y, 32))
        };
        if op == TO_DP then { /* the sum of the products, compared once */
            var sum : bits(32) = FMul(At(A, 0), At(B, 0));
            foreach (i from 1 to Count(A) - 1) { sum = FFma(At(A, i), At(B, i), sum) };
            Put(R, 0, sum);
            SetCount(R, 1)
        };
        foreach (i from 0 to Count(R) - 1) Put(B, i, ZeroOf(kind))
    };
    let eq = UInt(Field(40, 2)) == 1;
    foreach (i from 0 to Count(R) - 1) SetTest(i, TestCompare(kind, eq, sign, At(R, i), At(B, i)));
    Count(R)
}

/* VTST: the test of src1's and src2's component chan into predicate register pdst_n */
function execute_VTST() -> unit = {
    let sel = UInt(Field(18, 2));
    let alu_op = UInt(Field(14, 4));
    let op = TestOp(sel, alu_op);
    let type = TestType(sel, alu_op, Bit(47));
    if op == TO_INVALID | type == T_UNK then throw(Refuse("VTST of ALU %u operation %u", sel, alu_op));
    let chan = UInt(Field(36, 3));
    if chan >= 4 then throw(Refuse("VTST of channel %u", chan));
    TestSources(sel, type);
    let n = Test(op, 2 ^ chan);
    if n == 0 then throw(Stop());
    PredWrite(UInt(Field(34, 2)), TestAt(0))
}

/* VTSTMSK: the test's result as a mask into the destination: all ones (a float's 1.0, a signed type's largest)
   where it holds, else zero, in each component (of a vector test's four with mask type 2) */
function execute_VTSTMSK() -> unit = {
    let sel = UInt(Field(18, 2));
    let alu_op = UInt(Field(14, 4));
    let op = TestOp(sel, alu_op);
    let type = TestType(sel, alu_op, Bit(47));
    if op == TO_INVALID | type == T_UNK then throw(Refuse("VTSTMSK of ALU %u operation %u", sel, alu_op));
    let mask_type = UInt(Field(36, 2));
    if mask_type == 3 then throw(Refuse("VTSTMSK of mask type 3"));
    if Bit(20) == 0 then throw(Refuse("VTSTMSK without its write"));
    TestSources(sel, type);
    let doubled = sel == 0;
    let four = doubled & mask_type == 2;
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), doubled, if doubled then 8 else 7);
    SetType(DEST, type);
    let n = Test(op, if op == TO_DP | four then 15 else 1);
    if n == 0 then throw(Stop());
    let kind = if IsFloatType(type) then K_FLOAT else if IsSignedType(type) then K_INT else K_UINT;
    let zero = ZeroOf(kind);
    let ones = if type == T_F16 | type == T_F32 then One() else if type == T_I8 then SConst(127)
               else if type == T_I16 then SConst(32767) else if type == T_I32 then SConst(2147483647)
               else if type == T_U8 then UConst(255) else if type == T_U16 then UConst(65535) else UConst(4294967295);
    Reset(R, n, kind);
    foreach (i from 0 to n - 1) Put(R, i, if TestAt(i) then ones else zero);
    Store(DEST, R, 15, 0)
}
