/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c, helper-a64.c, vec_helper.c, vfp_helper.c,
   crypto_helper.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   Copyright (c) 2003 Fabrice Bellard */
/* The integer, branch, load/store and system instructions: each emitter builds the C of its a64_templates.inc case.
   Field values are the template's f_ declarations evaluated in C, as Clang folds them for a constant w. */
#include "common/cpu/recompiler/llvm/ir.h"
#include <string.h>

typedef LLVMValueRef V;
#define B            ir->b
#define F(lo, n)     ((w >> (lo)) & ((1UL << (n)) - 1))
/* (i64)((i32)(w<<shl)>>shr), the template's signed fields */
#define SF(shl, shr) ((SnxU64)(SnxI64)((SnxI32)(SnxU32)(w << (shl)) >> (shr)))
typedef int32_t SnxI32;
typedef int64_t SnxI64;

static V i32c(SnxIr* ir, SnxU32 v) { return ir_i32(ir, v); }
static V i64c(SnxIr* ir, SnxU64 v) { return ir_i64(ir, v); }
static V call1(SnxIr* ir, int id, V a) { return ir_call_helper(ir, id, &a, 1); }
static V call2(SnxIr* ir, int id, V a, V b) {
    V args[2];
    args[0] = a;
    args[1] = b;
    return ir_call_helper(ir, id, args, 2);
}
static V call3(SnxIr* ir, int id, V a, V b, V c) {
    V args[3];
    args[0] = a;
    args[1] = b;
    args[2] = c;
    return ir_call_helper(ir, id, args, 3);
}
static V call4(SnxIr* ir, int id, V a, V b, V c, V d) {
    V args[4];
    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;
    return ir_call_helper(ir, id, args, 4);
}
static V call5(SnxIr* ir, int id, V a, V b, V c, V d, V e) {
    V args[5];
    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;
    args[4] = e;
    return ir_call_helper(ir, id, args, 5);
}
/* (u32)x of a 64-bit value, then back to u64: the template's (u64)((u32)(...)) */
static V zext32(SnxIr* ir, V v) { return ir_u64(ir, ir_u32(ir, v)); }
static V rx32(SnxIr* ir, unsigned n) { return ir_u32(ir, ir_rx(ir, n)); }
static V x32(SnxIr* ir, unsigned n) { return ir_u32(ir, ir_x(ir, n)); }

/* adds32/adds64(c,a,b,cin), subs32/subs64(c,a,b) */
static V adds(SnxIr* ir, int wide, V a, V b, V cin) {
    return call4(ir, wide ? SNX_IR_ADDS64 : SNX_IR_ADDS32, ir->c, a, b, cin);
}
static V subs(SnxIr* ir, int wide, V a, V b) { return call3(ir, wide ? SNX_IR_SUBS64 : SNX_IR_SUBS32, ir->c, a, b); }
static V cond_ok(SnxIr* ir, unsigned cond) { return call2(ir, SNX_IR_COND_OK, ir->c, i32c(ir, cond)); }
static V truth(SnxIr* ir, V v) { return LLVMBuildICmp(B, LLVMIntNE, v, LLVMConstInt(LLVMTypeOf(v), 0, 0), ""); }

/* snx_decode_bitmask (helpers/integer.c), evaluated here: its arguments are fields of the constant word, so the
   immediate is a constant of the generated code instead of a call at run time */
static V decode_bitmask(SnxIr* ir, unsigned n, unsigned imms, unsigned immr, unsigned ds) {
    unsigned v = (n << 6) | (~imms & 0x3F), esize, levels, s, r;
    int len = -1, i;
    SnxU64 welem, result = 0, mask;
    for (i = 6; i >= 0; i--)
        if (v & (1u << i)) {
            len = i;
            break;
        }
    if (len < 1) return i64c(ir, 0);
    esize = 1u << len;
    levels = esize - 1;
    s = imms & levels;
    r = immr & levels;
    if (s == levels) return i64c(ir, 0);
    welem = s + 1 >= 64 ? ~(SnxU64)0 : ((SnxU64)1 << (s + 1)) - 1;
    mask = esize >= 64 ? ~(SnxU64)0 : ((SnxU64)1 << esize) - 1;
    if (r) welem = ((welem >> r) | (welem << (esize - r))) & mask;
    for (v = 0; v < ds; v += esize) result |= welem << v;
    return i64c(ir, ds == 32 ? (SnxU32)result : result);
}

/* ---- data processing, immediate ---- */

static void adr(SnxIr* ir, SnxU32 w, SnxU64 pc, int page) {
    SnxU64 immhi = SF(8, 13), immlo = F(29, 2), offset = (immhi << 2) | immlo;
    ir_setx(ir, F(0, 5), i64c(ir, page ? (pc & ~(SnxU64)0xFFF) + (offset << 12) : pc + offset));
}

static void addsub_imm(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned sh = index & 1, s = (index >> 1) & 1, op = (index >> 2) & 1, sf = (unsigned)(index >> 3);
    unsigned rd = F(0, 5), rn = F(5, 5);
    SnxU32 imm = sh ? F(10, 12) << 12 : F(10, 12);
    V a, r;
    if (!sf) {
        a = x32(ir, rn);
        if (!s)
            ir_set_x(ir, rd,
                     ir_u64(ir, op ? LLVMBuildSub(B, a, i32c(ir, imm), "") : LLVMBuildAdd(B, a, i32c(ir, imm), "")));
        else
            ir_setx(ir, rd,
                    ir_u64(ir, op ? subs(ir, 0, a, i32c(ir, imm)) : adds(ir, 0, a, i32c(ir, imm), i32c(ir, 0))));
    } else {
        a = ir_x(ir, rn);
        if (!s) {
            r = op ? LLVMBuildSub(B, a, i64c(ir, imm), "") : LLVMBuildAdd(B, a, i64c(ir, imm), "");
            ir_set_x(ir, rd, r);
        } else
            ir_setx(ir, rd, op ? subs(ir, 1, a, i64c(ir, imm)) : adds(ir, 1, a, i64c(ir, imm), i32c(ir, 0)));
    }
}

static V logic_op(SnxIr* ir, unsigned opc, V a, V b) {
    switch (opc) {
        case 1: return LLVMBuildOr(B, a, b, "");
        case 2: return LLVMBuildXor(B, a, b, "");
        default: return LLVMBuildAnd(B, a, b, "");
    }
}

static void logic_imm(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned opc = index & 3, sf = (unsigned)(index >> 2), rd = F(0, 5), rn = F(5, 5);
    V mask = decode_bitmask(ir, F(22, 1), F(10, 6), F(16, 6), sf ? 64 : 32), r;
    if (!sf) {
        /* (u32)RX(rn) & mask: the u32 widens to the u64 mask */
        r = ir_u32(ir, logic_op(ir, opc, ir_u64(ir, rx32(ir, rn)), mask));
        if (opc == 3) {
            call2(ir, SNX_IR_LOGIC32, ir->c, r);
            ir_setx(ir, rd, ir_u64(ir, r));
        } else
            ir_set_x(ir, rd, ir_u64(ir, r));
    } else {
        r = logic_op(ir, opc, ir_rx(ir, rn), mask);
        if (opc == 3) {
            call2(ir, SNX_IR_LOGIC64, ir->c, r);
            ir_setx(ir, rd, r);
        } else
            ir_set_x(ir, rd, r);
    }
}

static void movw(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned opc = index & 3, sf = (unsigned)(index >> 2), rd = F(0, 5), shift = F(21, 2) * 16;
    SnxU64 imm = (SnxU64)F(5, 16) << shift, keep = ~((SnxU64)0xFFFF << shift);
    V r;
    if (opc == 0)
        r = i64c(ir, sf ? ~imm : (SnxU32)~imm);
    else if (opc == 2)
        r = i64c(ir, sf ? imm : (SnxU32)imm);
    else {
        r = LLVMBuildOr(B, LLVMBuildAnd(B, ir_rx(ir, rd), i64c(ir, keep), ""), i64c(ir, imm), "");
        if (!sf) r = zext32(ir, r);
    }
    ir_setx(ir, rd, r);
}

static void bitfield(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned opc = index & 3, sf = (unsigned)(index >> 2), rd = F(0, 5), rn = F(5, 5);
    V immr = i32c(ir, F(16, 6)), imms = i32c(ir, F(10, 6)), ds = i32c(ir, sf ? 64 : 32), r;
    V src = sf ? ir_rx(ir, rn) : ir_u64(ir, rx32(ir, rn));
    if (opc == 0)
        r = call4(ir, SNX_IR_SBFM, src, immr, imms, ds);
    else if (opc == 1)
        r = call5(ir, SNX_IR_BFM, sf ? ir_rx(ir, rd) : ir_u64(ir, rx32(ir, rd)), src, immr, imms, ds);
    else
        r = call4(ir, SNX_IR_UBFM, src, immr, imms, ds);
    ir_setx(ir, rd, sf ? r : zext32(ir, r));
}

static void extr(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned imms = F(10, 6), rd = F(0, 5), rm = F(16, 5), rn = F(5, 5);
    V r;
    if (!index) {
        if (imms)
            r = LLVMBuildOr(B, LLVMBuildLShr(B, rx32(ir, rm), i32c(ir, imms), ""),
                            LLVMBuildShl(B, rx32(ir, rn), i32c(ir, 32 - imms), ""), "");
        else
            r = rx32(ir, rm);
        ir_setx(ir, rd, ir_u64(ir, r));
    } else {
        if (imms)
            r = LLVMBuildOr(B, LLVMBuildLShr(B, ir_rx(ir, rm), i64c(ir, imms), ""),
                            LLVMBuildShl(B, ir_rx(ir, rn), i64c(ir, 64 - imms), ""), "");
        else
            r = ir_rx(ir, rm);
        ir_setx(ir, rd, r);
    }
}

/* ---- system ---- */

static void hint(SnxIr* ir, SnxU32 w) {
    unsigned k = (F(8, 4) << 3) | F(5, 3);
    LLVMTypeRef p = ir->t_ptr;
    V c = ir->c;
    if (k == 1)
        ir_yield(ir);
    else if (k == 2)
        ir_rt_call(ir, ir_rt_wfe(), ir->t_void, &p, &c, 1);
    else if (k == 4 || k == 5)
        ir_rt_call(ir, ir_rt_sev(), ir->t_void, NULL, NULL, 0);
}

static void sys(SnxIr* ir, SnxU32 w) {
    if (F(16, 3) == 3 && F(12, 4) == 7 && F(8, 4) == 4 && F(5, 3) == 1)
        call2(ir, SNX_IR_DC_ZVA, ir->M, ir_rx(ir, F(0, 5)));
}

static void mrs(SnxIr* ir, SnxU32 w) {
    LLVMTypeRef p[2];
    V args[2];
    p[0] = ir->t_ptr;
    p[1] = ir->t_i32;
    args[0] = ir->c;
    args[1] = i32c(ir, F(5, 15));
    ir_setx(ir, F(0, 5), ir_call(ir, ir_extern(ir, "snx_mrs", ir->t_i64, p, 2), args, 2));
}

static void msr(SnxIr* ir, SnxU32 w) {
    LLVMTypeRef p[3];
    V args[3];
    p[0] = ir->t_ptr;
    p[1] = ir->t_i32;
    p[2] = ir->t_i64;
    args[0] = ir->c;
    args[1] = i32c(ir, F(5, 15));
    args[2] = ir_rx(ir, F(0, 5));
    ir_call(ir, ir_extern(ir, "snx_msr", ir->t_void, p, 3), args, 3);
}

/* ---- loads and stores ---- */

/* The single-register access of (size, V, lopc), as LDST_UIMM/IMM9/REG specialize it: rt from/to address */
static void access(SnxIr* ir, unsigned size, unsigned v, unsigned lopc, unsigned rt, V address) {
    unsigned bits = 8u << size;
    if (!v && size == 3 && lopc == 2) return; /* PRFM: no access (pre/post-index forms still write back) */
    if (!v) {
        if (lopc == 0)
            ir_wr(ir, address, bits, ir_rx(ir, rt));
        else if (lopc == 1)
            ir_setx(ir, rt, ir_u64(ir, ir_rd(ir, address, bits)));
        else if (lopc == 2)
            ir_setx(ir, rt, ir_s64(ir, ir_rd(ir, address, bits)));
        else
            ir_setx(ir, rt, ir_u64(ir, ir_cast(ir, ir_rd(ir, address, bits), ir->t_i32, 1)));
    } else if (lopc >= 2) { /* size 0: the 128-bit register */
        if (lopc == 2)
            ir_wrq(ir, address, ir_cpu(ir, ir_off_v(rt)));
        else
            ir_rdq(ir, ir_cpu(ir, ir_off_v(rt)), address);
    } else if (lopc == 0)
        ir_wr(ir, address, bits, ir_vd(ir, rt, 0));
    else {
        V t = ir_u64(ir, ir_rd(ir, address, bits));
        ir_set_vd(ir, rt, 1, i64c(ir, 0));
        ir_set_vd(ir, rt, 0, t);
    }
}

static void ldst_uimm(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned lopc = index & 3, v = (index >> 2) & 1, size = (unsigned)(index >> 3);
    unsigned shift = v && !size && lopc >= 2 ? 4 : size;
    V address = LLVMBuildAdd(B, ir_x(ir, F(5, 5)), i64c(ir, (SnxU64)F(10, 12) << shift), "");
    access(ir, size, v, lopc, F(0, 5), address);
}

static void ldst_imm9(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned mode = index & 3, lopc = (index >> 2) & 3, v = (index >> 4) & 1, size = (unsigned)(index >> 5);
    unsigned rn = F(5, 5), rt = F(0, 5);
    SnxU64 imm = SF(11, 23);
    V a;
    if (mode == 1) { /* post-index */
        a = ir_x(ir, rn);
        access(ir, size, v, lopc, rt, a);
        ir_set_x(ir, rn, LLVMBuildAdd(B, a, i64c(ir, imm), ""));
    } else if (mode == 3) { /* pre-index */
        a = LLVMBuildAdd(B, ir_x(ir, rn), i64c(ir, imm), "");
        access(ir, size, v, lopc, rt, a);
        ir_set_x(ir, rn, a);
    } else
        access(ir, size, v, lopc, rt, LLVMBuildAdd(B, ir_x(ir, rn), i64c(ir, imm), ""));
}

static void ldst_reg(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned lopc = index & 3, v = (index >> 2) & 1, size = (unsigned)(index >> 3);
    unsigned shift = v && !size && lopc >= 2 ? 4 : size;
    V offset = call3(ir, SNX_IR_EXT64, ir_rx(ir, F(16, 5)), i32c(ir, F(13, 3)), i32c(ir, F(12, 1) ? shift : 0));
    access(ir, size, v, lopc, F(0, 5), LLVMBuildAdd(B, ir_x(ir, F(5, 5)), offset, ""));
}

static void ldr_lit(SnxIr* ir, SnxU32 w, SnxU64 pc, size_t index) {
    unsigned rt = F(0, 5);
    V a = i64c(ir, pc + SF(8, 13) * 4), t;
    switch (index) {
        case 0: ir_setx(ir, rt, ir_u64(ir, ir_rd(ir, a, 32))); break;
        case 2: ir_setx(ir, rt, ir_rd(ir, a, 64)); break;
        case 4: ir_setx(ir, rt, ir_s64(ir, ir_rd(ir, a, 32))); break;
        case 5: ir_rdq(ir, ir_cpu(ir, ir_off_v(rt)), a); break;
        default: /* 1, 3: { u64 t_=RDn(..); c->v[rt].d[1]=0; c->v[rt].d[0]=t_; } */
            t = ir_u64(ir, ir_rd(ir, a, index == 1 ? 32 : 64));
            ir_set_vd(ir, rt, 1, i64c(ir, 0));
            ir_set_vd(ir, rt, 0, t);
            break;
    }
}

static void ldstp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned l = index & 1, mode = (index >> 1) & 3, v = (index >> 3) & 1, opc = (unsigned)(index >> 4);
    unsigned rn = F(5, 5), rt = F(0, 5), rt2 = F(10, 5), bytes, bits;
    SnxU64 offset;
    V a, a2, v1, v2;
    bytes = v ? 4u << opc : (opc == 2 ? 8 : 4);
    bits = bytes * 8;
    offset = SF(10, 25) * bytes;
    a = mode == 1 ? ir_x(ir, rn) : LLVMBuildAdd(B, ir_x(ir, rn), i64c(ir, offset), "");
    a2 = LLVMBuildAdd(B, a, i64c(ir, bytes), "");
    if (v && bytes == 16) {
        if (!l) {
            ir_wrq(ir, a, ir_cpu(ir, ir_off_v(rt)));
            ir_wrq(ir, a2, ir_cpu(ir, ir_off_v(rt2)));
        } else { /* V128 q1_,q2_; RDQ(q1_,a_); RDQ(q2_,a_+16); c->v[rt]=q1_; c->v[rt2]=q2_; */
            V q1 = ir_local(ir, ir_v128(ir)), q2 = ir_local(ir, ir_v128(ir));
            ir_rdq(ir, q1, a);
            ir_rdq(ir, q2, a2);
            LLVMBuildMemCpy(B, ir_cpu(ir, ir_off_v(rt)), 1, q1, 1, i64c(ir, 16));
            LLVMBuildMemCpy(B, ir_cpu(ir, ir_off_v(rt2)), 1, q2, 1, i64c(ir, 16));
        }
    } else if (!l) {
        ir_wr(ir, a, bits, v ? ir_vd(ir, rt, 0) : ir_rx(ir, rt));
        ir_wr(ir, a2, bits, v ? ir_vd(ir, rt2, 0) : ir_rx(ir, rt2));
    } else {
        int sign = !v && opc == 1; /* LDPSW */
        v1 = ir_cast(ir, ir_rd(ir, a, bits), ir->t_i64, sign);
        v2 = ir_cast(ir, ir_rd(ir, a2, bits), ir->t_i64, sign);
        if (v) {
            ir_set_vd(ir, rt, 0, v1);
            ir_set_vd(ir, rt, 1, i64c(ir, 0));
            ir_set_vd(ir, rt2, 0, v2);
            ir_set_vd(ir, rt2, 1, i64c(ir, 0));
        } else {
            ir_setx(ir, rt, v1);
            ir_setx(ir, rt2, v2);
        }
    }
    if (mode == 1)
        ir_set_x(ir, rn, LLVMBuildAdd(B, a, i64c(ir, offset), ""));
    else if (mode == 3)
        ir_set_x(ir, rn, a);
}

static void ldstx(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned o1 = index & 1, l = (index >> 1) & 1, o2 = (index >> 2) & 1, size = (unsigned)(index >> 3);
    unsigned rn = F(5, 5), rt = F(0, 5), rs = F(16, 5), rt2 = F(10, 5);
    V a = ir_x(ir, rn), sz = i32c(ir, size);
    if (o2) {
        if (l)
            ir_setx(ir, rt, ir_u64(ir, ir_ldacq(ir, a, 8u << size)));
        else
            ir_strel(ir, a, 8u << size, ir_rx(ir, rt));
    } else if (!o1) {
        if (l)
            ir_setx(ir, rt, call4(ir, SNX_IR_LDX, ir->c, ir->M, a, sz));
        else {
            V args[5];
            args[0] = ir->c;
            args[1] = ir->M;
            args[2] = a;
            args[3] = sz;
            args[4] = ir_rx(ir, rt);
            ir_setx(ir, rs, ir_u64(ir, ir_call_helper(ir, SNX_IR_STX, args, 5)));
        }
    } else if (l) {
        V v1 = ir_local(ir, ir->t_i64), v2 = ir_local(ir, ir->t_i64), args[6];
        args[0] = ir->c;
        args[1] = ir->M;
        args[2] = a;
        args[3] = sz;
        args[4] = v1;
        args[5] = v2;
        ir_call_helper(ir, SNX_IR_LDXP, args, 6);
        ir_setx(ir, rt, ir_load(ir, ir->t_i64, v1));
        ir_setx(ir, rt2, ir_load(ir, ir->t_i64, v2));
    } else {
        V args[6];
        args[0] = ir->c;
        args[1] = ir->M;
        args[2] = a;
        args[3] = sz;
        args[4] = ir_rx(ir, rt);
        args[5] = ir_rx(ir, rt2);
        ir_setx(ir, rs, ir_u64(ir, ir_call_helper(ir, SNX_IR_STXP, args, 6)));
    }
}

/* ---- data processing, register ---- */

static void logic_reg(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned n = index & 1, opc = (index >> 1) & 3, sf = (unsigned)(index >> 3);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V shift = i32c(ir, F(22, 2)), amount = i32c(ir, F(10, 6)), operand, r;
    if (!sf) {
        operand = call3(ir, SNX_IR_SHF32, rx32(ir, rm), shift, amount);
        if (n) operand = LLVMBuildNot(B, operand, "");
        r = logic_op(ir, opc, rx32(ir, rn), operand);
        if (opc == 3) call2(ir, SNX_IR_LOGIC32, ir->c, r);
        ir_setx(ir, rd, ir_u64(ir, r));
    } else {
        operand = call3(ir, SNX_IR_SHF64, ir_rx(ir, rm), shift, amount);
        if (n) operand = LLVMBuildNot(B, operand, "");
        r = logic_op(ir, opc, ir_rx(ir, rn), operand);
        if (opc == 3) call2(ir, SNX_IR_LOGIC64, ir->c, r);
        ir_setx(ir, rd, r);
    }
}

static void addsub_sreg(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned s = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V shift = i32c(ir, F(22, 2)), amount = i32c(ir, F(10, 6)), a, b;
    if (!sf) {
        a = rx32(ir, rn);
        b = call3(ir, SNX_IR_SHF32, rx32(ir, rm), shift, amount);
        if (!s)
            ir_setx(ir, rd, ir_u64(ir, op ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, "")));
        else
            ir_setx(ir, rd, ir_u64(ir, op ? subs(ir, 0, a, b) : adds(ir, 0, a, b, i32c(ir, 0))));
    } else {
        a = ir_rx(ir, rn);
        b = call3(ir, SNX_IR_SHF64, ir_rx(ir, rm), shift, amount);
        if (!s)
            ir_setx(ir, rd, op ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, ""));
        else
            ir_setx(ir, rd, op ? subs(ir, 1, a, b) : adds(ir, 1, a, b, i32c(ir, 0)));
    }
}

static void addsub_ext(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned s = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V e = call3(ir, SNX_IR_EXT64, ir_rx(ir, rm), i32c(ir, F(13, 3)), i32c(ir, F(10, 3))), a;
    if (!sf) {
        V b = ir_u32(ir, e);
        a = x32(ir, rn);
        if (!s)
            ir_set_x(ir, rd, ir_u64(ir, op ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, "")));
        else
            ir_setx(ir, rd, ir_u64(ir, op ? subs(ir, 0, a, b) : adds(ir, 0, a, b, i32c(ir, 0))));
    } else {
        a = ir_x(ir, rn);
        if (!s)
            ir_set_x(ir, rd, op ? LLVMBuildSub(B, a, e, "") : LLVMBuildAdd(B, a, e, ""));
        else
            ir_setx(ir, rd, op ? subs(ir, 1, a, e) : adds(ir, 1, a, e, i32c(ir, 0)));
    }
}

static void adc(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned s = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V cf, a, b;
    if (!sf) {
        a = rx32(ir, rn);
        b = rx32(ir, rm);
        cf = ir_cpu_load(ir, ir->t_i32, ir_off_cf());
        if (!s) {
            if (op) b = LLVMBuildNot(B, b, "");
            ir_setx(ir, rd, ir_u64(ir, LLVMBuildAdd(B, LLVMBuildAdd(B, a, b, ""), cf, "")));
        } else /* sbcs32(c,a,b,cin) is adds32(c,a,~b,cin) */
            ir_setx(ir, rd, ir_u64(ir, adds(ir, 0, a, op ? LLVMBuildNot(B, b, "") : b, cf)));
    } else {
        a = ir_rx(ir, rn);
        b = ir_rx(ir, rm);
        cf = ir_cpu_load(ir, ir->t_i32, ir_off_cf());
        if (!s) {
            if (op) b = LLVMBuildNot(B, b, "");
            ir_setx(ir, rd, LLVMBuildAdd(B, LLVMBuildAdd(B, a, b, ""), ir_u64(ir, cf), ""));
        } else
            ir_setx(ir, rd, adds(ir, 1, a, op ? LLVMBuildNot(B, b, "") : b, cf));
    }
}

static void ccmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned imm = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2), rn = F(5, 5);
    LLVMBasicBlockRef compare = ir_block(ir, "ccmp"), flags = ir_block(ir, "nzcv"), done = ir_block(ir, "done");
    V a, b;
    LLVMBuildCondBr(B, truth(ir, cond_ok(ir, F(12, 4))), compare, flags);
    LLVMPositionBuilderAtEnd(B, compare);
    if (!sf) {
        a = rx32(ir, rn);
        b = imm ? i32c(ir, F(16, 5)) : rx32(ir, F(16, 5));
    } else {
        a = ir_rx(ir, rn);
        b = imm ? i64c(ir, F(16, 5)) : ir_rx(ir, F(16, 5));
    }
    if (op)
        subs(ir, sf, a, b);
    else
        adds(ir, sf, a, b, i32c(ir, 0));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, flags);
    call2(ir, SNX_IR_SET_NZCV, ir->c, i32c(ir, F(0, 4)));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
}

static void csel(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned o2 = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V ok = truth(ir, cond_ok(ir, F(12, 4))), a, b;
    if (!sf) {
        a = rx32(ir, rn);
        b = rx32(ir, rm);
    } else {
        a = ir_rx(ir, rn);
        b = ir_rx(ir, rm);
    }
    if (!op && o2)
        b = LLVMBuildAdd(B, b, LLVMConstInt(LLVMTypeOf(b), 1, 0), "");
    else if (op && !o2)
        b = LLVMBuildNot(B, b, "");
    else if (op && o2)
        b = LLVMBuildSub(B, LLVMConstInt(LLVMTypeOf(b), 0, 0), b, "");
    ir_setx(ir, rd, ir_u64(ir, LLVMBuildSelect(B, ok, a, b, "")));
}

static int dp1(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned rd = F(0, 5), rn = F(5, 5);
    V r;
    switch (index) {
        case 0: r = ir_u64(ir, call1(ir, SNX_IR_RBIT32, rx32(ir, rn))); break;
        case 1: r = zext32(ir, call1(ir, SNX_IR_REV16_64, ir_u64(ir, rx32(ir, rn)))); break;
        case 2: r = ir_u64(ir, call1(ir, SNX_IR_BSWAP32, rx32(ir, rn))); break;
        case 4: r = ir_u64(ir, call1(ir, SNX_IR_CLZ32, rx32(ir, rn))); break;
        case 5: r = ir_u64(ir, call1(ir, SNX_IR_CLS32, rx32(ir, rn))); break;
        case 64: r = call1(ir, SNX_IR_RBIT64, ir_rx(ir, rn)); break;
        case 65: r = call1(ir, SNX_IR_REV16_64, ir_rx(ir, rn)); break;
        case 66: r = call1(ir, SNX_IR_REV32_64, ir_rx(ir, rn)); break;
        case 67: r = call1(ir, SNX_IR_BSWAP64, ir_rx(ir, rn)); break;
        case 68: r = ir_u64(ir, call1(ir, SNX_IR_CLZ64, ir_rx(ir, rn))); break;
        case 69: r = ir_u64(ir, call1(ir, SNX_IR_CLS64, ir_rx(ir, rn))); break;
        default: return 0;
    }
    ir_setx(ir, rd, r);
    return 1;
}

/* snx_crc32's byte-at-a-time recurrence as a 256-entry table per polynomial, built here once per module */
static V crc_table(SnxIr* ir, unsigned castagnoli) {
    const char* name = castagnoli ? "snx_crc32c_table" : "snx_crc32_table";
    V table = LLVMGetNamedGlobal(ir->mod, name), entries[256];
    SnxU32 poly = castagnoli ? 0x82F63B78UL : 0xEDB88320UL, v;
    unsigned i, k;
    if (table) return table;
    for (i = 0; i < 256; ++i) {
        for (v = i, k = 0; k < 8; ++k) v = (v >> 1) ^ ((v & 1) ? poly : 0);
        entries[i] = i32c(ir, v);
    }
    table = LLVMAddGlobal(ir->mod, LLVMArrayType(ir->t_i32, 256), name);
    LLVMSetLinkage(table, LLVMInternalLinkage);
    LLVMSetGlobalConstant(table, 1);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_i32, entries, 256));
    return table;
}

/* the low 64 bits of the carry-less product of two 64-bit values: PCLMULQDQ */
static V clmul_low(SnxIr* ir, V a, V b) {
    LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
    V args[3];
    args[0] = LLVMBuildInsertElement(B, LLVMConstNull(t), a, i32c(ir, 0), "");
    args[1] = LLVMBuildInsertElement(B, LLVMConstNull(t), b, i32c(ir, 0), "");
    args[2] = ir_i8(ir, 0);
    return LLVMBuildExtractElement(B, ir_intrinsic(ir, "llvm.x86.pclmulqdq", NULL, args, 3), i32c(ir, 0), "");
}

/* CRC32[BHWX] and CRC32C*: snx_crc32(acc, value, bytes, castagnoli). CRC32C is the SSE4.2 CRC32 instruction on x86
   (CPUID crc32); CRC32 (and CRC32C without that) a Barrett reduction with PCLMULQDQ; otherwise a table lookup per byte */
static V crc32(SnxIr* ir, V acc, V value, unsigned bytes, unsigned castagnoli) {
    V table, x;
    unsigned i;
    if (castagnoli && ir->features->x86 && ir->features->crc32) {
        static const char* const names[4] = {"llvm.x86.sse42.crc32.32.8", "llvm.x86.sse42.crc32.32.16", NULL,
                                             "llvm.x86.sse42.crc32.32.32"};
        V args[2];
        if (bytes == 8) {
            args[0] = ir_u64(ir, acc);
            args[1] = ir_u64(ir, value);
            return ir_u32(ir, ir_intrinsic(ir, "llvm.x86.sse42.crc32.64.64", NULL, args, 2));
        }
        args[0] = acc;
        args[1] = ir_cast(ir, value, LLVMIntTypeInContext(ir->ctx, 8 * bytes), 0);
        return ir_intrinsic(ir, names[bytes - 1], NULL, args, 2);
    }
    if (ir->features->x86 && ir->features->pclmul) {
        /* Barrett reduction with PCLMULQDQ (wunkolo, "pclmulqdq Tricks"): an n-bit update is the CRC of
           (acc ^ value) placed at the top of 32 bits, x, then crc = (clmul(clmul(x, mu) mod 2^32, P) >> 32) ^ acc >> n,
           with P the reflected 33-bit polynomial and mu = floor(x^64 / P) reflected */
        SnxU64 poly = castagnoli ? 0x105EC76F1ULL : 0x1DB710641ULL, mu = castagnoli ? 0xDEA713F1ULL : 0x1F7011641ULL;
        V chunks[2];
        unsigned k, nchunks = bytes == 8 ? 2 : 1, n = bytes == 8 ? 32 : 8 * bytes;
        x = ir_u64(ir, value);
        chunks[0] = ir_u32(ir, x);
        chunks[1] = ir_u32(ir, LLVMBuildLShr(B, x, i64c(ir, 32), ""));
        for (k = 0; k < nchunks; ++k) {
            V v = LLVMBuildXor(B, acc, chunks[k], ""), t;
            if (n < 32) v = LLVMBuildShl(B, LLVMBuildAnd(B, v, i32c(ir, (1u << n) - 1), ""), i32c(ir, 32 - n), "");
            t = clmul_low(ir, ir_u64(ir, v), i64c(ir, mu));
            t = clmul_low(ir, LLVMBuildAnd(B, t, i64c(ir, 0xFFFFFFFFULL), ""), i64c(ir, poly));
            t = ir_u32(ir, LLVMBuildLShr(B, t, i64c(ir, 32), ""));
            acc = n < 32 ? LLVMBuildXor(B, t, LLVMBuildLShr(B, acc, i32c(ir, n), ""), "") : t;
        }
        return acc;
    }
    table = crc_table(ir, castagnoli);
    x = ir_u64(ir, value);
    for (i = 0; i < bytes; ++i) {
        V byte = ir_u32(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, x, i64c(ir, 8 * i), ""), i64c(ir, 0xFF), ""));
        V index[2], entry;
        index[0] = i64c(ir, 0);
        index[1] = ir_u64(ir, LLVMBuildAnd(B, LLVMBuildXor(B, acc, byte, ""), i32c(ir, 0xFF), ""));
        entry = ir_load(ir, ir->t_i32, LLVMBuildInBoundsGEP2(B, LLVMArrayType(ir->t_i32, 256), table, index, 2, ""));
        acc = LLVMBuildXor(B, entry, LLVMBuildLShr(B, acc, i32c(ir, 8), ""), "");
    }
    return acc;
}

static int dp2(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V r;
    switch (index) {
        case 2: r = ir_u64(ir, call2(ir, SNX_IR_UDIV32, rx32(ir, rn), rx32(ir, rm))); break;
        case 3: r = ir_u64(ir, call2(ir, SNX_IR_SDIV32, rx32(ir, rn), rx32(ir, rm))); break;
        case 8:
            r = ir_u64(ir, LLVMBuildShl(B, rx32(ir, rn), LLVMBuildAnd(B, rx32(ir, rm), i32c(ir, 31), ""), ""));
            break;
        case 9:
            r = ir_u64(ir, LLVMBuildLShr(B, rx32(ir, rn), LLVMBuildAnd(B, rx32(ir, rm), i32c(ir, 31), ""), ""));
            break;
        case 10:
            r = ir_u64(ir, LLVMBuildAShr(B, rx32(ir, rn), LLVMBuildAnd(B, rx32(ir, rm), i32c(ir, 31), ""), ""));
            break;
        case 11: r = ir_u64(ir, call2(ir, SNX_IR_ROR32, rx32(ir, rn), rx32(ir, rm))); break;
        case 16:
        case 17:
        case 18:
        case 20:
        case 21:
        case 22: r = ir_u64(ir, crc32(ir, rx32(ir, rn), rx32(ir, rm), 1u << (index & 3), index >= 20)); break;
        case 66: r = call2(ir, SNX_IR_UDIV64, ir_rx(ir, rn), ir_rx(ir, rm)); break;
        case 67: r = call2(ir, SNX_IR_SDIV64, ir_rx(ir, rn), ir_rx(ir, rm)); break;
        case 72: r = LLVMBuildShl(B, ir_rx(ir, rn), LLVMBuildAnd(B, ir_rx(ir, rm), i64c(ir, 63), ""), ""); break;
        case 73: r = LLVMBuildLShr(B, ir_rx(ir, rn), LLVMBuildAnd(B, ir_rx(ir, rm), i64c(ir, 63), ""), ""); break;
        case 74: r = LLVMBuildAShr(B, ir_rx(ir, rn), LLVMBuildAnd(B, ir_rx(ir, rm), i64c(ir, 63), ""), ""); break;
        case 75: r = call2(ir, SNX_IR_ROR64, ir_rx(ir, rn), rx32(ir, rm)); break;
        case 83:
        case 87: r = ir_u64(ir, crc32(ir, rx32(ir, rn), ir_rx(ir, rm), 8, index == 87)); break;
        default: return 0;
    }
    ir_setx(ir, rd, r);
    return 1;
}

static int dp3(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5), ra = F(10, 5);
    V r, product;
    switch (index) {
        case 0:
        case 1:
            product = LLVMBuildMul(B, rx32(ir, rn), rx32(ir, rm), "");
            r = index ? LLVMBuildSub(B, rx32(ir, ra), product, "") : LLVMBuildAdd(B, rx32(ir, ra), product, "");
            r = ir_u64(ir, r);
            break;
        case 16:
        case 17:
            product = LLVMBuildMul(B, ir_rx(ir, rn), ir_rx(ir, rm), "");
            r = index == 17 ? LLVMBuildSub(B, ir_rx(ir, ra), product, "") : LLVMBuildAdd(B, ir_rx(ir, ra), product, "");
            break;
        case 18:
        case 19:
        case 26:
        case 27: {
            int sign = index < 20;
            product = LLVMBuildMul(B, ir_cast(ir, rx32(ir, rn), ir->t_i64, sign),
                                   ir_cast(ir, rx32(ir, rm), ir->t_i64, sign), "");
            r = index & 1 ? LLVMBuildSub(B, ir_rx(ir, ra), product, "") : LLVMBuildAdd(B, ir_rx(ir, ra), product, "");
            break;
        }
        case 20: r = call2(ir, SNX_IR_SMULH64, ir_rx(ir, rn), ir_rx(ir, rm)); break;
        case 28: r = call2(ir, SNX_IR_UMULH64, ir_rx(ir, rn), ir_rx(ir, rm)); break;
        default: return 0;
    }
    ir_setx(ir, rd, r);
    return 1;
}

/* ---- dispatch ---- */

int snx_ir_plain(SnxIr* ir, SnxU32 w, SnxU64 pc, const SnxA64Case* instruction) {
    size_t index;
    const SnxA64Pattern* pattern = snx_a64_case_pattern(instruction, &index);
    const char* name;
    if (!pattern) return 0;
    /* snx_a64_static_step and codegen.c try the SIMD shortcuts first, for every PLAIN word */
    if (snx_ir_fast_simd(ir, w)) return 1;
    if (!instruction->first[0]) return 1; /* an empty body: PRFM, MSR (immediate), ... */
    name = pattern->name;
    if (!strcmp(name, "ADR"))
        adr(ir, w, pc, 0);
    else if (!strcmp(name, "ADRP"))
        adr(ir, w, pc, 1);
    else if (!strcmp(name, "ADDSUB_IMM"))
        addsub_imm(ir, w, index);
    else if (!strcmp(name, "LOGIC_IMM"))
        logic_imm(ir, w, index);
    else if (!strcmp(name, "MOVW"))
        movw(ir, w, index);
    else if (!strcmp(name, "BITFIELD"))
        bitfield(ir, w, index);
    else if (!strcmp(name, "EXTR"))
        extr(ir, w, index);
    else if (!strcmp(name, "HINT"))
        hint(ir, w);
    else if (!strcmp(name, "CLREX"))
        ir_cpu_store(ir, i32c(ir, 0), ir_off_excl_on());
    else if (!strcmp(name, "BARRIER"))
        ir_fence(ir);
    else if (!strcmp(name, "SYS"))
        sys(ir, w);
    else if (!strcmp(name, "MRS"))
        mrs(ir, w);
    else if (!strcmp(name, "MSR"))
        msr(ir, w);
    else if (!strcmp(name, "LDST_UIMM"))
        ldst_uimm(ir, w, index);
    else if (!strcmp(name, "LDST_IMM9"))
        ldst_imm9(ir, w, index);
    else if (!strcmp(name, "LDST_REG"))
        ldst_reg(ir, w, index);
    else if (!strcmp(name, "LDR_LIT"))
        ldr_lit(ir, w, pc, index);
    else if (!strcmp(name, "LDSTP"))
        ldstp(ir, w, index);
    else if (!strcmp(name, "LDSTX"))
        ldstx(ir, w, index);
    else if (!strcmp(name, "LOGIC_REG"))
        logic_reg(ir, w, index);
    else if (!strcmp(name, "ADDSUB_SREG"))
        addsub_sreg(ir, w, index);
    else if (!strcmp(name, "ADDSUB_EXT"))
        addsub_ext(ir, w, index);
    else if (!strcmp(name, "ADC"))
        adc(ir, w, index);
    else if (!strcmp(name, "CCMP"))
        ccmp(ir, w, index);
    else if (!strcmp(name, "CSEL"))
        csel(ir, w, index);
    else if (!strcmp(name, "DP1"))
        return dp1(ir, w, index);
    else if (!strcmp(name, "DP2"))
        return dp2(ir, w, index);
    else if (!strcmp(name, "DP3"))
        return dp3(ir, w, index);
    else if (!strncmp(name, "SIMD", 4))
        return snx_ir_more_simd(ir, w) || snx_ir_simd_plain(ir, w, pc, pattern, index);
    else
        return snx_ir_fp_plain(ir, w, pc, pattern, index);
    return 1;
}

LLVMValueRef snx_ir_flow_operand(SnxIr* ir, SnxU32 w, SnxU64 pc, const SnxA64Case* instruction) {
    size_t index;
    const SnxA64Pattern* pattern = snx_a64_case_pattern(instruction, &index);
    const char* name = pattern ? pattern->name : "";
    unsigned rt = F(0, 5);
    (void)pc;
    if (!strcmp(name, "BCOND")) return truth(ir, cond_ok(ir, F(0, 4)));
    if (!strcmp(name, "CBZ")) {
        V value = index & 2 ? ir_rx(ir, rt) : rx32(ir, rt);
        return LLVMBuildICmp(B, index & 1 ? LLVMIntNE : LLVMIntEQ, value, LLVMConstInt(LLVMTypeOf(value), 0, 0), "");
    }
    if (!strcmp(name, "TBZ")) {
        V bit =
            LLVMBuildAnd(B, LLVMBuildLShr(B, ir_rx(ir, rt), i64c(ir, (F(31, 1) << 5) | F(19, 5)), ""), i64c(ir, 1), "");
        return LLVMBuildICmp(B, index ? LLVMIntNE : LLVMIntEQ, bit, i64c(ir, 0), "");
    }
    if (!strcmp(name, "BR") || !strcmp(name, "BLR") || !strcmp(name, "RET")) return ir_rx(ir, F(5, 5));
    return i32c(ir, F(5, 16)); /* SVC, BRK */
}
