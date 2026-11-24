/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The USSE's vector float instructions (usse_float.roots; back end usse_float_spirv.h): the multiply-adds VMAD,
   VMAD2, V32NMAD and V16NMAD with their ALU's other operations, the dot product VDP, the complex unit's VCOMP and
   the paired VDUAL (Vita3K's alu.cpp). The back end gives VDUAL's source layouts and sources, which decode its
   operands as decode_src12 does the others' */

/* VDUAL's operations' sources, their layouts and their operands (op 1 or 2: the first operation's or the second's
   source i, as dual_source decodes it from the layout), false where the layouts have none */
val DualLayouts : (int, int, int) -> bool
val DualSource : (int, int, int, int, int, int) -> unit

/* a channel code's four channels, a hex digit each (x the highest) */
function Ch(code : int, i : int) -> int = MOD(DIV(code, if i == 0 then 4096 else if i == 1 then 256 else if i == 2 then 16 else 1), 16)
function SetSwizzleCode(k : int, code : int) -> unit = SetSwizzle(k, Ch(code, 0), Ch(code, 1), Ch(code, 2), Ch(code, 3))

/* VMAD2's swizzles of src0, src1 and src2 (a channel code: XYZW is 0x0123, 291) */
function Vmad2Swizzle(which : int, s : int) -> int =
    if s <= 3 then 4369 * s /* XXXX, YYYY, ZZZZ, WWWW */ else if s == 4 then 291 /* XYZW */
    else if which == 0 then { if s == 5 then 4611 /* YZXW */ else if s == 6 then 307 /* XYWW */ else 8961 /* ZWXY */ }
    else if which == 1 then { if s == 5 then 274 /* XYYZ */ else if s == 6 then 4403 /* YYWW */ else 12579 /* WYZW */ }
    else { if s == 5 then 563 /* XZWW */ else if s == 6 then 18 /* XXYZ */ else 290 /* XYZZ */ }

/* VMAD2: src0 * src1 + src2 rounded once, of the components the mask enables */
function execute_VMAD2() -> unit = {
    let f16 = Bit(58);
    let type = if f16 == 1 then T_F16 else T_F32;
    Dest(DEST, UInt(Field(22, 6)), UInt(Field(32, 2)), 0, 1, 7);
    Src0(SRC0, UInt(Field(12, 6)), Bit(34), 0, 1, 7);
    Src12(SRC1, UInt(Field(6, 6)), UInt(Field(30, 2)), Bit(49), 1, 7);
    Src12(SRC2, UInt(Field(0, 6)), UInt(Field(28, 2)), Bit(48), 1, 7);
    let mask = WriteMask(DEST, UInt(Field(39, 4)), f16);
    SetType(DEST, type);
    SetType(SRC0, type);
    SetType(SRC1, type);
    SetType(SRC2, type);
    SetSwizzleCode(SRC0, Vmad2Swizzle(0, UInt(Field(18, 2)) + 4 * Bit(53)));
    SetSwizzleCode(SRC1, Vmad2Swizzle(1, UInt(Field(20, 2)) + 4 * Bit(44)));
    SetSwizzleCode(SRC2, Vmad2Swizzle(2, UInt(Field(45, 3))));
    if Bit(50) == 1 then AddFlags(SRC0, F_ABS);
    SetFlags(SRC1, Modifier(UInt(Field(37, 2))));
    SetFlags(SRC2, Modifier(UInt(Field(35, 2))));
    Load(A, SRC0, mask, 0);
    Load(B, SRC1, mask, 0);
    Load(C, SRC2, mask, 0);
    if Failed() then throw(Stop());
    Reset(R, Count(A), K_FLOAT);
    foreach (i from 0 to Count(A) - 1) Put(R, i, FFma(At(A, i), At(B, i), At(C, i)));
    Store(DEST, R, mask, 0)
}

/* VMAD: the GPI register gpi0 times src1 plus gpi1, of three components (vec4: four), each source swizzled and its
   modifiers applied; repeat_count repeats of repeat_mode */
function execute_VMAD() -> unit = {
    let type = if Bit(52) == 1 then 2 else 1;
    let mode = UInt(Field(47, 2));
    let mask = UInt(Field(39, 4));
    Src12(SRC1, UInt(Field(0, 6)), UInt(Field(30, 2)), Bit(49), 1, 7);
    Dest(DEST, UInt(Field(22, 6)), UInt(Field(32, 2)), Bit(51), 1, 7);
    Internal(SRC0, UInt(Field(28, 2)), F_GPI, T_F32);
    Internal(SRC2, UInt(Field(12, 2)), F_GPI, T_F32);
    Vec34Swizzle(SRC1, UInt(Field(6, 4)), Bit(10), type);
    Vec34Swizzle(SRC0, UInt(Field(18, 4)), Bit(34), type);
    Vec34Swizzle(SRC2, UInt(Field(14, 4)), Bit(54), type);
    AddFlags(SRC1, F_ABS * Bit(37) + F_NEG * Bit(38));
    AddFlags(SRC0, F_ABS * Bit(46) + F_NEG * Bit(11));
    AddFlags(SRC2, F_ABS * Bit(35) + F_NEG * Bit(36));
    Multiplier(2, 2, 2, 4);
    foreach (r from 0 to UInt(Field(44, 2))) {
        if not_bool(Failed()) then {
            Load(A, SRC0, mask, Rep(SRC0, r, O_SRC0, mode));
            Load(B, SRC1, mask, Rep(SRC1, r, O_SRC1, mode));
            Load(C, SRC2, mask, Rep(SRC2, r, O_SRC2, mode));
            Reset(R, Count(A), K_FLOAT);
            foreach (i from 0 to Count(A) - 1) Put(R, i, FFma(At(A, i), At(B, i), At(C, i)));
            Store(DEST, R, mask, Rep(DEST, r, O_DEST, mode))
        }
    };
    ResetMultiplier()
}

/* V32NMAD and V16NMAD: the ALU operation alu of src1 and src2 (0 multiply, 1 add, 2 FRC: src1 - floor(src2), 3 and 4
   the derivatives of src1, 5 min, 6 max), in each component the mask enables; 7 the dot product of all four, in
   each of them */
function execute_NMAD() -> unit = {
    let type = if Form() == 0 then T_F32 else T_F16;
    let alu = UInt(Field(12, 3));
    let mask = UInt(Field(39, 4));
    Dest(DEST, UInt(Field(22, 6)), UInt(Field(32, 2)), Bit(51), 1, 7);
    Src12(SRC1, UInt(Field(6, 6)), UInt(Field(30, 2)), Bit(49), 1, 7);
    Src12(SRC2, UInt(Field(0, 6)), UInt(Field(28, 2)), Bit(48), 1, 7);
    SetFlags(SRC1, Modifier(UInt(Field(37, 2))));
    if Bit(36) == 1 then SetFlags(SRC2, F_ABS);
    SetType(DEST, type);
    SetType(SRC1, type);
    SetType(SRC2, type);
    Swizzle4(SRC1, UInt(Field(53, 2) @ Field(50, 1) @ Field(34, 2) @ Field(15, 7)));
    Vec34Swizzle(SRC2, UInt(Field(44, 4)), 0, 2);
    let source_mask = if alu == 7 then 15 else mask;
    Load(A, SRC1, source_mask, 0);
    Load(B, SRC2, source_mask, 0);
    if Failed() then throw(Stop());
    Reset(R, Count(A), K_FLOAT);
    if alu != 7 then {
        foreach (i from 0 to Count(A) - 1) {
            let a = At(A, i);
            let b = At(B, i);
            Put(R, i, match alu { 0 => FMul(a, b), 1 => FAdd(a, b), 2 => FSub(a, FRound(1, b)), 3 => FDdx(a),
                                  4 => FDdy(a), 5 => FMin(a, b), _ => FMax(a, b) })
        }
    } else {
        var sum : bits(32) = FMul(At(A, 0), At(B, 0));
        foreach (i from 1 to Count(A) - 1) { sum = FFma(At(A, i), At(B, i), sum) };
        Reset(R, Popcount4(mask), K_FLOAT);
        foreach (i from 0 to Popcount4(mask) - 1) Put(R, i, sum)
    };
    Store(DEST, R, mask, 0)
}

/* VDP: the dot product of src1 (its channels each named, its modifiers) and the GPI register gpi0, of three
   components (vec4: four), into the one component the mask names, which turns a component on each repeat */
function execute_VDP() -> unit = {
    let type = if Bit(52) == 1 then 2 else 1;
    let source_mask = if Bit(52) == 1 then 15 else 7;
    let mode = UInt(Field(47, 2));
    Src12(SRC1, UInt(Field(0, 6)), UInt(Field(30, 2)), Bit(49), 1, 7);
    Dest(DEST, UInt(Field(22, 6)), UInt(Field(32, 2)), Bit(51), 1, 7);
    Internal(SRC2, UInt(Field(28, 2)), F_GPI, T_F32);
    Vec34Swizzle(SRC2, UInt(Field(18, 4)), 0, type);
    SetSwizzle(SRC1, UInt(Field(6, 3)), UInt(Field(9, 3)), UInt(Field(12, 3)), UInt(Field(15, 3)));
    AddFlags(SRC1, F_NEG * Bit(38) + F_ABS * Bit(37));
    if Bit(46) == 1 then AddFlags(SRC2, F_ABS);
    var mask : int = UInt(Field(39, 4));
    foreach (r from 0 to UInt(Field(44, 2))) {
        if not_bool(Failed()) then {
            Load(A, SRC1, source_mask, Rep(SRC1, r, O_SRC1, mode));
            Load(B, SRC2, source_mask, Rep(SRC2, r, O_SRC2, mode));
            if not_bool(Failed()) then {
                var sum : bits(32) = FMul(At(A, 0), At(B, 0));
                foreach (i from 1 to Count(A) - 1) { sum = FFma(At(A, i), At(B, i), sum) };
                Reset(R, 1, K_FLOAT);
                Put(R, 0, sum);
                Store(DEST, R, mask, 0);
                mask = MOD(mask * 2 + DIV(mask, 8), 16)
            }
        }
    }
}

/* VCOMP: the complex unit's reciprocal, reciprocal square root, base 2 logarithm or exponential (of a NaN 1, as a
   game needs) of src1's component src_comp, in each component the mask enables. A repeat at a time */
function VcompType(t : int) -> int = if t == 0 then T_F32 else if t == 1 then T_F16 else if t == 2 then T_C10 else T_UNK
function execute_VCOMP() -> unit = {
    let dest_type = VcompType(UInt(Field(53, 2)));
    let src_type = VcompType(UInt(Field(39, 2)));
    if dest_type == T_UNK | src_type == T_UNK then throw(Refuse("VCOMP with type 3"));
    let op = UInt(Field(41, 2));
    Dest(DEST, UInt(Field(21, 7)), UInt(Field(32, 2)), Bit(51), 1, 8);
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 1, 8);
    SetFlags(SRC1, Modifier(UInt(Field(37, 2))));
    SetType(SRC1, src_type);
    SetType(DEST, dest_type);
    foreach (r from 0 to UInt(Field(44, 4))) {
        if not_bool(Failed()) then {
            Load(A, SRC1, 2 ^ UInt(Field(35, 2)), Rep(SRC1, r, O_SRC1, R_SLMSI));
            if not_bool(Failed()) then {
                let x = At(A, 0);
                Reset(R, 1, K_FLOAT);
                if op == 0 then Put(R, 0, FDiv(One(), x))
                else if op == 1 then Put(R, 0, FInvSqrt(x))
                else if op == 2 then Put(R, 0, FLog2(x))
                else {
                    let nan = FIsNaN(x);
                    let one = One();
                    let e = FExp2(x);
                    Put(R, 0, if nan then one else e)
                };
                Store(DEST, R, UInt(Field(0, 4)), Rep(DEST, r, O_DEST, R_SLMSI))
            }
        }
    }
}

/* VDUAL's operations (op1 0-13, op2 1-13): their sources, whether they read vectors and write them */
function DualSources(code : int) -> int =
    if code == 0 | code == 8 then 3 else if code == 1 | code == 3 | code == 4 | (code >= 9 & code <= 11) then 2 else 1
function DualVectorLoad(code : int) -> bool = code <= 5
function DualVectorStore(code : int) -> bool = code == 0 | code == 3 | code == 4 | code == 5

/* a VDUAL operation of its sources (operands base, base + 1, base + 2), into value out: 0 and 8 a * b + c, 1 and 2
   the dot products a.b and a.a, 3 and 10 a * b, 4 and 9 a + b, 11 FRC (a - floor(b)), 5 a, 6 the reciprocal square
   root, 7 the reciprocal, 12 the base 2 exponential (of a NaN 1), 13 the base 2 logarithm */
function DualAlu(code : int, base : int, count : int, write_mask : int, cct : int, out : int) -> unit = {
    let dot = code == 1 | code == 2;
    let source_mask = if DualVectorLoad(code) then (if dot then (if cct == 1 then 15 else 7) else write_mask) else 1;
    Load(A, base, source_mask, 0);
    if count > 1 then Load(B, base + 1, source_mask, 0) else Reset(B, 0, K_FLOAT);
    if count > 2 then Load(C, base + 2, source_mask, 0) else Reset(C, 0, K_FLOAT);
    Reset(out, Count(A), K_FLOAT);
    if not_bool(Failed()) then {
        if code == 1 | code == 2 then {
            var sum : bits(32) = FMul(At(A, 0), if code == 1 then At(B, 0) else At(A, 0));
            foreach (i from 1 to Count(A) - 1) { sum = FFma(At(A, i), if code == 1 then At(B, i) else At(A, i), sum) };
            Reset(out, 1, K_FLOAT);
            Put(out, 0, sum)
        } else if code == 5 then CopyValue(out, A)
        else {
            foreach (i from 0 to Count(A) - 1) {
                let a = At(A, i);
                if code == 0 | code == 8 then {
                    let product = FMul(a, At(B, i));
                    Put(out, i, FAdd(product, At(C, i)))
                } else if code == 3 | code == 10 then Put(out, i, FMul(a, At(B, i)))
                else if code == 4 | code == 9 then Put(out, i, FAdd(a, At(B, i)))
                else if code == 11 then Put(out, i, FSub(a, FRound(1, At(B, i))))
                else if code == 6 then Put(out, i, FInvSqrt(a))
                else if code == 7 then Put(out, i, FDiv(One(), a))
                else if code == 12 then {
                    let e = FExp2(a);
                    let nan = FIsNaN(e);
                    let one = One();
                    Put(out, i, if nan then one else e)
                } else Put(out, i, FLog2(a))
            }
        }
    }
}

let UNIFIED : int = 0
let GPI_DEST : int = 1
let R1 : int = 3
let R2 : int = 4
/* the destination's mask: the internal register's three or four components, a vector's two words (f16: four
   halves), a scalar's one */
function DualMask(bank : int, vector_store : bool, cct : int, f16 : int) -> int =
    if bank == B_INTERNAL then (if cct == 1 then 15 else 7)
    else if vector_store then (if f16 == 1 then 15 else 3) else (if f16 == 1 then 3 else 1)

/* VDUAL: two operations that share their register reads, op1 into the unified store or the internal register
   (prim_ustore), op2 into the other, both written after both are evaluated */
function execute_VDUAL() -> unit = {
    let cct = Bit(59);
    let ext = Bit(54);
    let f16 = Bit(53);
    let ustore = Bit(42) == 1;
    let op1 = (if cct == 0 & ext == 1 then 8 else 0) + UInt(Field(44, 3));
    let op2 = 8 * Bit(43) + UInt(Field(18, 3));
    if op1 >= 14 | op2 == 0 | op2 >= 14 then throw(Refuse("VDUAL opcode"));
    let n1 = DualSources(op1);
    let n2 = DualSources(op2);
    let unified_vector = if ustore then DualVectorStore(op1) else DualVectorStore(op2);
    let type = if f16 == 1 then T_F16 else T_F32;
    Internal(GPI_DEST, UInt(Field(28, 2)), 0, type);
    Dest(UNIFIED, UInt(Field(21, 7)), UInt(Field(32, 2)), 0, unified_vector, if unified_vector then 8 else 7);
    SetType(UNIFIED, type);
    let op1_dest = if ustore then UNIFIED else GPI_DEST;
    let op2_dest = if ustore then GPI_DEST else UNIFIED;
    if not_bool(DualLayouts(n1, n2, UInt(Field(16, 2)))) then throw(Refuse("VDUAL source layout"));
    let fixed_mask = UInt(Field(7, 3)) + (if cct == 1 & ext == 1 then 8 else 0);
    let op1_mask = if ustore then DualMask(BankOf(op1_dest), DualVectorStore(op1), cct, f16) else fixed_mask;
    let op2_mask = if ustore then fixed_mask else DualMask(BankOf(op2_dest), DualVectorStore(op2), cct, f16);
    foreach (i from 0 to n1 - 1) DualSource(2 + i, 1, i, n1, type, DualVectorLoad(op1));
    foreach (i from 0 to n2 - 1) DualSource(5 + i, 2, i, n1, type, DualVectorLoad(op2));
    if Failed() then throw(Stop());
    DualAlu(op1, 2, n1, op1_mask, cct, R1);
    DualAlu(op2, 5, n2, op2_mask, cct, R2);
    if Failed() then throw(Stop());
    Store(op1_dest, R1, op1_mask, 0);
    Store(op2_dest, R2, op2_mask, 0)
}
