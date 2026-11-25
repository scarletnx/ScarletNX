/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, A64's part of its own (after mach_a64.h, which keeps the guest's registers, its memory
   and its branches): the floating point (the SSE instruction behind the LLVM back end's guards, the exact helper on
   a cold path), the exclusives' stores, the LSE atomics and CRC32 inline with their inputs in fixed registers, the
   barriers as x86's ordering asks, and the ABI's bytes. The state's address is in RBX.

   What isn't an instruction the selection chose is the ABI's, written as bytes here: the prologue (the state's
   address from RDI into RBX, kept as RBX is the callee's), the runtime's UNDEF (cpu.h's RtAPI.undef(state, pc, word))
   and the return (the next pc in RAX) */

/* ---- the hot paths ----
   Each operation is the SSE instruction's, and the exact helper's (snx_x64_fp_*) on a cold path where the result
   may not be the console's: the guards are the LLVM back end's (llvm.h's sg_fp_fast_*). FPCR rounds to nearest
   (DN matters to NaNs only, which go cold), under FZ no operand is a denormal (sgx_fp_flushed: many games run with FZ
   set, and a guard of FZ clear sent all of their arithmetic to the helpers), FPSR's IXC is set already where the
   operation may be inexact (the inline code doesn't raise it), and the result is a normal number above the smallest
   (no underflow, overflow or NaN: no flag but IXC, and nothing FZ flushes) or a zero that is exact. The host's MXCSR
   is its default, round to nearest without FTZ or DAZ, as the runtime keeps it */

#define SGX_FPCR_RMODE (3u << 22)
#define SGX_FPCR_FZ (1u << 24)
#define SGX_FPCR_AHP (1u << 26)
#define SGX_FPSR_IXC (1u << 4)
/* FPCR's mask bits set, or (ixc) FPSR's IXC clear: the hot path may not be the console's. Made once a region for
   FPCR's mode and FPSR's values, and for FZ alone (an MSR or a call forgets them: sgx_call) */
static inline SgB sgx_fp_bad(SgCtx* g, SgD fpcr, SnxU32 mask, int ixc) {
    SgX* x = sgx(g);
    SgD f, bad;
    if (mask == SGX_FPCR_RMODE && g->fp_state[ixc]) return g->fp_state[ixc];
    if (mask == SGX_FPCR_FZ && !ixc && g->fp_fz) return g->fp_fz;
    f = sg_d_and(g, sg_d_zext(g, sg_d_slice(g, fpcr, 31, 0), 32), sgx_const(32, mask));
    if (ixc) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        SgD fpsr;
        i->w = 32;
        i->disp = (int)offsetof(CPU, fpsr);
        i->def = sgx_vreg(x, x->n - 1, 32);
        fpsr = sgx_val(32, i->def);
        f = sg_d_or(g, f, sg_d_xor(g, sg_d_and(g, fpsr, sgx_const(32, SGX_FPSR_IXC)), sgx_const(32, SGX_FPSR_IXC)));
    }
    bad = f;
    {
        SgB b = sg_b_not(g, sg_d_is_zero(g, bad));
        if (mask == SGX_FPCR_RMODE) g->fp_state[ixc] = b;
        if (mask == SGX_FPCR_FZ && !ixc) g->fp_fz = b;
        return b;
    }
}
/* a float's bits (w 32 or 64) */
static inline SgD sgx_fp_bits(SgCtx* g, SgD v) {
    SgD d = v;
    sgx_reg(sgx(g), &d);
    return d;
}
/* |bits| in (the smallest normal, infinity): neither zero, tiny, infinite nor a NaN */
static inline SgB sgx_fp_roomy(SgCtx* g, SgD bits, unsigned w) {
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull, inf = w == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
    SgD m = sg_d_lsl(g, bits, 1); /* the magnitude, doubled: the sign shifted out */
    SgD t = sg_d_sub(g, m, sgx_const(w, (normal + 1) << 1));
    return sgx_d_ult(g, t, sgx_const(w, (inf - normal - 1) << 1));
}
/* the limit of a result r's test (SGX_T_NORMAL, w 32 or 64): its doubled magnitude less the smallest normal's (and 1
   more) below it, unsigned, is a normal number above the smallest, below infinity. The limit is 0 where FPCR rounds
   otherwise or FPSR's IXC is clear: a register made once a region, so the test costs a MOVD, LEA, CMP and Jcc and no
   load of FPCR or FPSR an operation */
static inline int sgx_fp_bound(SgCtx* g, SgD fpcr, unsigned w) {
    SgX* x = sgx(g);
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull, inf = w == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
    SgD b;
    if (g->fp_bound[w == 64]) return g->fp_bound[w == 64];
    b = sg_d_select(g, sgx_fp_bad(g, fpcr, SGX_FPCR_RMODE, 1), sgx_const(w, 0), sgx_const(w, (inf - normal - 1) << 1));
    sgx_reg(x, &b);
    return g->fp_bound[w == 64] = b.v;
}
/* and of the test for a denormal operand (w 32 or 64) that FPCR.FZ flushes, raising IDC (SGX_T_DENORMAL): the helpers'
   case (llvm.h's sg_fp_unflushed). The doubled smallest normal less 2 where FZ is set, 0 where it is clear (never), a
   register made once a region, so the test costs a MOVD, LEA, CMP and Jcc */
static inline int sgx_fp_dbound(SgCtx* g, SgD fpcr, unsigned w) {
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull;
    SgD b;
    if (g->fp_dbound[w == 64]) return g->fp_dbound[w == 64];
    b = sg_d_select(g, sgx_fp_bad(g, fpcr, SGX_FPCR_FZ, 0), sgx_const(w, (normal << 1) - 2), sgx_const(w, 0));
    sgx_reg(sgx(g), &b);
    return g->fp_dbound[w == 64] = b.v;
}
/* t's n tests and one more: of kind on the float of (0 the result, 1 + k operand k) of w bits, with its limit */
static inline int sgx_fp_test(SgCtx* g, SgXTest* t, int n, int kind, int of, SgD fpcr, unsigned w) {
    t[n].kind = kind;
    t[n].of = of;
    t[n].limit = kind == SGX_T_NORMAL     ? sgx_fp_bound(g, fpcr, w)
                 : kind == SGX_T_DENORMAL ? sgx_fp_dbound(g, fpcr, w)
                                          : 0;
    t[n].w = w;
    return n + 1;
}
/* the tests of an operation of nops operands whose result is hot (SGX_T_NORMAL), and no operand FZ flushes. Made before
   the operation, so that its limits' registers are, and the result's is read by its cold path alone (sgx_pin_target) */
static inline int sgx_fp_tests(SgCtx* g, SgXTest* t, SgD fpcr, unsigned w, int nops) {
    int k, n = sgx_fp_test(g, t, 0, SGX_T_NORMAL, 0, fpcr, w);
    for (k = 0; k < nops; ++k) n = sgx_fp_test(g, t, n, SGX_T_DENORMAL, 1 + k, fpcr, w);
    return n;
}
/* the cold path of an operation sgx_fp_tests tests: the helper's call where a test holds, but a zero result the hot
   path's where FPCR rounds to nearest, under FZ no operand is a denormal (llvm.h's sg_fp_unflushed: many games run with
   FZ set and compute zeros), and (zargs, a mask of ops) one of those operands is a zero (none: any zero is exact, as a
   sum's is) */
static inline SgD sgx_fp_cold_z(SgCtx* g, const SgXTest* t, int nt, SgD fast, const char* sym, SgD* ops, int nops,
                                SnxU32 desc, unsigned zargs) {
    return sgx_cold_fp(sgx(g), 0, fast, sym, ops, nops, nops, nops + 1, desc, (int)offsetof(CPU, fpcr),
                       (int)offsetof(CPU, fpsr), 0xC0 | zargs, SGX_FPCR_RMODE, t, nt);
}
static inline SgB sgx_fp_zero(SgCtx* g, SgD bits) { return sg_d_is_zero(g, sg_d_lsl(g, bits, 1)); }
/* the arguments of an snx_x64_fp_* helper (x64_helpers.c): the operands' bits, FPCR, the constants packed */
static inline SgD sgx_fp_cold(SgCtx* g, SgB bad, SgD fast, const char* sym, SgD* ops, int nops, SgD fpcr, int has_desc,
                              SnxU32 desc, const SgXTest* t, int nt) {
    (void)fpcr;
    if (nops > 3) sg_fail("an FP helper of more than three operands", nops, 0);
    return sgx_cold_fp(sgx(g), bad, fast, sym, ops, nops, nops, has_desc ? nops + 1 : -1, desc,
                       (int)offsetof(CPU, fpcr), (int)offsetof(CPU, fpsr), 0, 0, t, nt);
}
/* an operand as an XMM register */
static inline int sgx_fx(SgCtx* g, SgD* v) { return sgx_xreg(sgx(g), v); }
/* an operand for a cold path and its tests, in the XMM register the hot path reads */
static inline SgD sgx_fo(SgCtx* g, SgD v) {
    unsigned w = v.w;
    return sgx_xval(w, sgx_fx(g, &v));
}

static inline SgD sgx_fp_binary(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned op) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    static const SgXVForm* const forms[4][2] = {{&sgx_v_addss, &sgx_v_addsd},
                                                {&sgx_v_subss, &sgx_v_subsd},
                                                {&sgx_v_mulss, &sgx_v_mulsd},
                                                {&sgx_v_divss, &sgx_v_divsd}};
    SgD r, ops[2];
    SgXTest t[SGX_TESTS];
    int nt;
    if ((w != 32 && w != 64) || path || op > SNX_FP_DIV) {
        SgD o[2] = {a, b};
        return sgx_fp(g, "snx_x64_fp_binary", path, fpcr, o, 2, 1, a.w | op << 8, a.w);
    }
    nt = sgx_fp_tests(g, t, fpcr, w, 2);
    ops[0] = sgx_fo(g, a);
    ops[1] = sgx_fo(g, b);
    r = sgx_xval(w, sgx_v2(x, forms[op][w == 64], ops[0].x, ops[1].x, 0));
    return sgx_fp_cold_z(g, t, nt, r, "snx_x64_fp_binary", ops, 2, w | op << 8,
                         op == SNX_FP_ADD || op == SNX_FP_SUB ? 0
                         : op == SNX_FP_MUL                   ? 3
                                                              : 1);
}
static inline SgD sg_FPAdd(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_ADD); }
static inline SgD sg_FPSub(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_SUB); }
static inline SgD sg_FPMul(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_MUL); }
static inline SgD sg_FPDiv(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_DIV); }

/* FPMax and FPMin: without a NaN, and without FZ (a denormal flushed), the larger or smaller operand, and of equal
   ones their AND (max: +0 over -0) or OR (min): MAXSD's or MINSD's, which give the second of equal ones, replaced by
   that where CMPEQSD says. The Num forms take the number over a quiet NaN: with a NaN, the helper */
static inline SgD sgx_fp_minmax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned minimum, unsigned numeric) {
    SgX* x = sgx(g);
    unsigned w = a.w, d = w == 64;
    SgD ops[2];
    SgXTest tests[SGX_TESTS];
    int ax, bx, m, t, r, n, nt = 0, k;
    if ((w != 32 && w != 64) || path) {
        SgD o[2] = {a, b};
        return sgx_fp(g, "snx_x64_fp_minmax", path, fpcr, o, 2, 1, a.w | minimum << 8 | numeric << 9, a.w);
    }
    for (k = 1; k <= 2; ++k) nt = sgx_fp_test(g, tests, nt, SGX_T_DENORMAL, k, fpcr, w);
    for (k = 1; k <= 2; ++k) nt = sgx_fp_test(g, tests, nt, SGX_T_NAN, k, fpcr, w);
    ops[0] = sgx_fo(g, a);
    ops[1] = sgx_fo(g, b);
    ax = ops[0].x;
    bx = ops[1].x;
    m = sgx_v2(x, d ? &sgx_v_cmpsd : &sgx_v_cmpss, ax, bx, 0); /* all ones where equal */
    t = sgx_v2(x, minimum ? &sgx_v_orpd : &sgx_v_andpd, ax, bx, 0);
    r = sgx_v2(x, minimum ? (d ? &sgx_v_minsd : &sgx_v_minss) : (d ? &sgx_v_maxsd : &sgx_v_maxss), ax, bx, 0);
    n = sgx_v2(x, &sgx_v_andnpd, m, r, 0); /* the MAXSD where not equal */
    t = sgx_v2(x, &sgx_v_andpd, m, t, 0);
    r = sgx_v2(x, &sgx_v_orpd, n, t, 0);
    return sgx_fp_cold(g, 0, sgx_xval(w, r), "snx_x64_fp_minmax", ops, 2, fpcr, 1, w | minimum << 8 | numeric << 9,
                       tests, nt);
}
static inline SgD sg_FPMax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 0); }
static inline SgD sg_FPMin(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 0); }
static inline SgD sg_FPMaxNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 1); }
static inline SgD sg_FPMinNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 1); }

/* addend + a * b rounded once: VFMADD231 where the host has FMA; a zero result only where the product is a zero */
static inline SgD sg_FPMulAdd(SgCtx* g, SgB path, SgD addend, SgD a, SgD b, SgD fpcr) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD r, ops[3];
    SgXTest t[SGX_TESTS];
    int nt;
    if ((w != 32 && w != 64) || path || !(x->features & SGX_F_FMA)) {
        SgD o[3] = {addend, a, b};
        return sgx_fp(g, "snx_x64_fp_fma", path, fpcr, o, 3, 1, a.w, a.w);
    }
    nt = sgx_fp_tests(g, t, fpcr, w, 3);
    ops[0] = sgx_fo(g, addend);
    ops[1] = sgx_fo(g, a);
    ops[2] = sgx_fo(g, b);
    {
        int in[4] = {ops[0].x, ops[1].x, ops[2].x, 0};
        r = sgx_xval(w, sgx_vins(x, w == 64 ? &sgx_v_vfmadd231sd : &sgx_v_vfmadd231ss, 0, 1, in, 0, 0, SGX_VM_NONE, 0,
                                 0, 0, 0, 0));
    }
    return sgx_fp_cold_z(g, t, nt, r, "snx_x64_fp_fma", ops, 3, w, 6);
}
static inline SgD sg_FPSqrt(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD o, r;
    SgXTest t[SGX_TESTS];
    int nt;
    if ((w != 32 && w != 64) || path) return sgx_fp(g, "snx_x64_fp_unary", path, fpcr, &a, 1, 1, a.w | SNX_FP_SQRT << 8, a.w);
    nt = sgx_fp_tests(g, t, fpcr, w, 1);
    o = sgx_fo(g, a);
    r = sgx_xval(w, sgx_v1(x, w == 64 ? &sgx_v_sqrtsd : &sgx_v_sqrtss, o.x, 0));
    return sgx_fp_cold_z(g, t, nt, r, "snx_x64_fp_unary", &o, 1, w | SNX_FP_SQRT << 8, 1);
}
/* ROUNDSx's immediate for FPRounding's TIEEVEN, POSINF, NEGINF, ZERO (FPCR's: TIEEVEN, which the guard holds it to);
   -1 for TIEAWAY, which x86 doesn't round by */
static inline int sgx_round_imm(int rounding) {
    switch (rounding) {
    case 0:
    case SG_FP_ROUNDING_FPCR: return 0;
    case 1: return 2;
    case 2: return 1;
    case 3: return 3;
    default: return -1;
    }
}
/* FRINTN, P, M, Z, X, I (FRINTA: the helper): ROUNDSx; a NaN, a denormal FZ flushes or FPCR's other modes go cold,
   FRINTX's IXC too until it is set */
static inline SgD sg_FPRoundInt(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, int exact) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    int imm = sgx_round_imm(rounding);
    SgD o, r;
    SgXTest t[SGX_TESTS];
    int nt = 0;
    SnxU32 desc = w | sgx_fp_rm(rounding) << 8 | (SnxU32)!!exact << 12;
    if ((w != 32 && w != 64) || path || imm < 0 || !(x->features & SGX_F_SSE41))
        return sgx_fp(g, "snx_x64_fp_round", path, fpcr, &a, 1, 1, desc, a.w);
    nt = sgx_fp_test(g, t, nt, SGX_T_NAN, 1, fpcr, w);
    nt = sgx_fp_test(g, t, nt, SGX_T_DENORMAL, 1, fpcr, w);
    o = sgx_fo(g, a);
    r = sgx_xval(w, sgx_v1(x, w == 64 ? &sgx_v_roundsd : &sgx_v_roundss, o.x, imm | 8));
    return sgx_fp_cold(g, sgx_fp_bad(g, fpcr, rounding == SG_FP_ROUNDING_FPCR ? SGX_FPCR_RMODE : 0, exact), r,
                       "snx_x64_fp_round", &o, 1, fpcr, 1, desc, t, nt);
}
/* FCVT between singles and doubles: CVTSS2SD (exact, but a denormal single FZ flushes) and CVTSD2SS (a single's roomy
   result, or a zero's: a denormal double gives neither); a NaN goes cold. Halves: the helper */
static inline SgD sg_FPConvert__1(SgCtx* g, SgB path, SgD op, SgD fpcr, long long to_bits) {
    SgX* x = sgx(g);
    unsigned from = op.w, to = (unsigned)to_bits;
    SgD ob, r;
    SgB bad;
    if (to != 16 && to != 32 && to != 64) sg_fail("a conversion to a float of this width", to_bits, 0);
    if (path || !((from == 32 && to == 64) || (from == 64 && to == 32)))
        return sgx_fp(g, "snx_x64_fp_convert", path, fpcr, &op, 1, 1, op.w | (SnxU32)to_bits << 8, to);
    if (to == 64) {
        SgXTest t[SGX_TESTS];
        int nt = sgx_fp_test(g, t, 0, SGX_T_DENORMAL, 1, fpcr, 32);
        nt = sgx_fp_test(g, t, nt, SGX_T_NAN, 1, fpcr, 32);
        ob = sgx_fo(g, op);
        r = sgx_xval(to, sgx_v1(x, &sgx_v_cvtss2sd, ob.x, 0));
        return sgx_fp_cold(g, 0, r, "snx_x64_fp_convert", &ob, 1, fpcr, 1, from | to << 8, t, nt);
    }
    ob = sgx_fp_bits(g, op);
    r = sgx_xval(to, sgx_v1(x, &sgx_v_cvtsd2ss, sgx_fx(g, &op), 0));
    bad = sg_b_or(g, sgx_fp_bad(g, fpcr, SGX_FPCR_RMODE, 1),
                  sg_b_not(g, sg_b_or(g, sgx_fp_roomy(g, sgx_fp_bits(g, r), 32), sgx_fp_zero(g, ob))));
    return sgx_fp_cold(g, bad, r, "snx_x64_fp_convert", &ob, 1, fpcr, 1, from | to << 8, NULL, 0);
}
/* the NZCV nibble (N bit 3 ... V bit 0): UCOMISx's flags, ordered (an unordered pair, which may raise IOC, and a
   denormal FZ flushes go cold): N below, Z equal, C not below, V 0 */
static inline SgD sg_FPCompare(SgCtx* g, SgB path, SgD a, SgD b, int signal, SgD fpcr) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD nib, ops[2];
    SgB n, z, c, un;
    SgXTest t[SGX_TESTS];
    int ins, nt = 0;
    SnxU32 desc = w | (SnxU32)!!signal << 8;
    if ((w != 32 && w != 64) || path) {
        SgD o[2] = {a, b};
        return sgx_fp(g, "snx_x64_fp_compare", path, fpcr, o, 2, 1, desc, 4);
    }
    nt = sgx_fp_test(g, t, nt, SGX_T_DENORMAL, 1, fpcr, w);
    nt = sgx_fp_test(g, t, nt, SGX_T_DENORMAL, 2, fpcr, w);
    ops[0] = sgx_fo(g, a);
    ops[1] = sgx_fo(g, b);
    {
        int in[4] = {ops[0].x, ops[1].x, 0, 0};
        sgx_vins(x, w == 64 ? &sgx_v_ucomisd : &sgx_v_ucomiss, -1, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    ins = x->flags_ins;
    n = sgx_bool(x, SGX_B_CC, ins, 2);   /* B */
    z = sgx_bool(x, SGX_B_CC, ins, 4);   /* E */
    c = sgx_bool(x, SGX_B_CC, ins, 3);   /* AE */
    un = sgx_bool(x, SGX_B_CC, ins, 10); /* P */
    nib = sg_d_concat(g, sg_d_concat(g, sgx_bool_bits(x, n), sgx_bool_bits(x, z)),
                      sg_d_concat(g, sgx_bool_bits(x, c), sgx_const(1, 0)));
    return sg_d_slice(g, sgx_fp_cold(g, un, sg_d_zext(g, nib, 32), "snx_x64_fp_compare", ops, 2, fpcr, 1, desc, t, nt),
                      3, 0);
}
/* SCVTF, UCVTF: CVTSI2Sx of 64 bits (an unsigned word zero extended; an unsigned doubleword of its top bit set, the
   helper), times 2^-fbits (exact: no result is tiny). Exact where the integer fits the significand; else once IXC
   is set, in round to nearest */
static inline SgD sg_FixedToFP(SgCtx* g, SgB path, SgD op, long long fbits, int uns, SgD fpcr, int rounding,
                               long long width) {
    SgX* x = sgx(g);
    unsigned iw = op.w, w = (unsigned)width, p = w == 32 ? 24 : 53;
    SnxU32 desc = op.w | (SnxU32)width << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17;
    SgD v, r;
    SgB fits, bad;
    if (rounding != SG_FP_ROUNDING_FPCR) sg_fail("FixedToFP in a mode of its own: the helper rounds as FPCR says", rounding, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FixedToFP of these fraction bits", fbits, 0);
    if (path || (w != 32 && w != 64) || (iw != 32 && iw != 64))
        return sgx_fp(g, "snx_x64_fp_from_int", path, fpcr, &op, 1, 1, desc, (unsigned)width);
    v = sgx_fp_bits(g, op);
    v = uns || iw == 64 ? sg_d_zext(g, v, 64) : sg_d_sext(g, v, 64);
    {
        int in[4] = {0, sgx_reg(x, &v), 0, 0}, rx;
        rx = sgx_vins(x, w == 64 ? &sgx_v_cvtsi2sd_64 : &sgx_v_cvtsi2ss_64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        if (fbits) {
            SnxU64 scale = w == 64 ? (SnxU64)(1023 - fbits) << 52 : (SnxU64)(127 - fbits) << 23;
            rx = sgx_v2(x, w == 64 ? &sgx_v_mulsd : &sgx_v_mulss, rx, sgx_xconst(x, scale, 0), 0);
        }
        r = sgx_xval(w, rx);
    }
    /* |v| < 2^p: v + 2^p below 2^(p+1) for a signed one, v below 2^p for an unsigned */
    if (uns) fits = sgx_d_ult(g, v, sgx_const(64, 1ull << p));
    else
        fits = sgx_d_ult(g, sg_d_add(g, v, sgx_const(64, 1ull << p)), sgx_const(64, 1ull << (p + 1)));
    bad = sg_b_and(g, sgx_fp_bad(g, fpcr, SGX_FPCR_RMODE, 1), sg_b_not(g, fits));
    if (uns && iw == 64) bad = sg_b_or(g, bad, sg_b_not(g, sgx_d_ult(g, v, sgx_const(64, 1ull << 63))));
    return sgx_fp_cold(g, bad, r, "snx_x64_fp_from_int", &v, 1, fpcr, 1, desc, NULL, 0);
}
/* FCVTZS, FCVTZU and the other modes' (FCVTAS, TIEAWAY: the helper): times 2^fbits, ROUNDSx to the mode, CVTTSx2SI
   to 64 bits; out of range (CVTT's 'indefinite' integer, a negative or too large unsigned one), a NaN, a denormal FZ
   flushes, or IXC still clear goes cold */
static inline SgD sg_FPToFixed(SgCtx* g, SgB path, SgD op, long long fbits, int uns, SgD fpcr, int rounding,
                               long long integer) {
    SgX* x = sgx(g);
    unsigned w = op.w, iw = (unsigned)integer;
    int imm = sgx_round_imm(rounding);
    SnxU32 desc = op.w | (SnxU32)integer << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17 | sgx_fp_rm(rounding) << 24;
    SgD o, r;
    SgB bad;
    SgXTest t[SGX_TESTS];
    int vx, nt;
    if (rounding == SG_FP_ROUNDING_FPCR) sg_fail("FPToFixed in FPCR's mode: the helper takes the encoding's", rounding, 0);
    if (integer != 16 && integer != 32 && integer != 64)
        sg_fail("a conversion to an integer of this width", integer, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FPToFixed of these fraction bits", fbits, 0);
    if (path || (w != 32 && w != 64) || integer == 16 || imm < 0 || (imm != 3 && !(x->features & SGX_F_SSE41)))
        return sgx_fp(g, "snx_x64_fp_to_int", path, fpcr, &op, 1, 1, desc, (unsigned)integer);
    nt = sgx_fp_test(g, t, 0, SGX_T_DENORMAL, 1, fpcr, w);
    o = sgx_fo(g, op);
    vx = o.x;
    if (fbits) {
        SnxU64 scale = w == 64 ? (SnxU64)(1023 + fbits) << 52 : (SnxU64)(127 + fbits) << 23;
        vx = sgx_v2(x, w == 64 ? &sgx_v_mulsd : &sgx_v_mulss, vx, sgx_xconst(x, scale, 0), 0);
    }
    if (imm != 3) vx = sgx_v1(x, w == 64 ? &sgx_v_roundsd : &sgx_v_roundss, vx, imm | 8);
    r = sgx_val(64, sgx_v1(x, w == 64 ? &sgx_v_cvttsd2si_64 : &sgx_v_cvttss2si_64, vx, 0));
    if (uns) /* 0 to 2^iw - 1 (a negative one is above, unsigned) */
        bad = iw == 64 ? sg_b_not(g, sgx_d_ult(g, r, sgx_const(64, 1ull << 63)))
                       : sg_b_not(g, sgx_d_ult(g, r, sgx_const(64, 1ull << 32)));
    else if (iw == 64)
        bad = sg_d_eq(g, r, sgx_const(64, 1ull << 63));
    else /* INT32_MIN to INT32_MAX: r + 2^31 below 2^32 */
        bad = sg_b_not(g, sgx_d_ult(g, sg_d_add(g, r, sgx_const(64, 1ull << 31)), sgx_const(64, 1ull << 32)));
    bad = sg_b_or(g, bad, sgx_fp_bad(g, fpcr, 0, 1));
    r = sgx_fp_cold(g, bad, r, "snx_x64_fp_to_int", &o, 1, fpcr, 1, desc, t, nt);
    return sg_d_slice(g, r, iw - 1, 0);
}
#define SGX_RAX 0
#define SGX_RCX 1
#define SGX_RDX 2
#define SGX_RSI 6
#define SGX_R8 8
#define SGX_R9 9
#define SGX_R10 10
#define SGX_R11 11
/* the register the sequences below take the guest's address in (the memory's base is in R10): RSI, or on Windows, where
   it is the callee's, R9, and R11 beside a pair, which is in R8 and R9 */
static inline int sgx_address_reg(const SgX* x, int pair) { return !SGX_WIN64(x) ? SGX_RSI : pair ? SGX_R11 : SGX_R9; }

/* STXR, STLXR, STXP: cpu.h's stx and stxp inline (SGX_M_STX, SGX_M_STXP: LOCK CMPXCHG, CMPXCHG16B) */
static inline void sg_GuestStoreExclusive(SgCtx* g, SgD address, long long size, SgD value) {
    SgD in[4];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    if (size == 16) {
        const signed char fix[4] = {SGX_R10, (signed char)sgx_address_reg(sgx(g), 1), SGX_R8, SGX_R9};
        in[2] = sg_d_slice(g, value, 63, 0);
        in[3] = sg_d_slice(g, value, 127, 64);
        g->status = sgx_mach(g, SGX_M_STXP, in, fix, 4, 3, 1u << SGX_RCX | 1u << SGX_RDX | 1u << SGX_R11, 1, 32).v;
    } else {
        const signed char fix[3] = {SGX_R10, (signed char)sgx_address_reg(sgx(g), 0), SGX_RDX};
        in[2] = sg_d_zext(g, value, 64);
        g->status = sgx_mach(g, SGX_M_STX, in, fix, 3, sgx_log2(size), 1u << SGX_RCX, 1, 32).v;
    }
    sgx(g)->v[g->status].zab = 1;
}
/* the LSE atomics: LOCK XADD, XCHG, a LOCK CMPXCHG loop for the others (SGX_M_RMW) */
static inline SgD sgx_rmw(SgCtx* g, int op, SgD address, SgD v) {
    const signed char fix[3] = {SGX_R10, (signed char)sgx_address_reg(sgx(g), 0), SGX_RDX};
    unsigned w = v.w;
    SgD in[3];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    in[2] = sg_d_zext(g, v, 64);
    return sg_d_slice(g, sgx_mach(g, SGX_M_RMW, in, fix, 3, op << 2 | sgx_log2(w / 8),
                                  1u << SGX_RCX | 1u << SGX_RDX | 1u << SGX_R11, 1, 64), w - 1, 0);
}
static inline SgD sg_GuestAtomicAdd(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 0, a, v); }
static inline SgD sg_GuestAtomicAnd(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 1, a, v); }
static inline SgD sg_GuestAtomicXor(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 2, a, v); }
static inline SgD sg_GuestAtomicOr(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 3, a, v); }
static inline SgD sg_GuestAtomicSMax(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 4, a, v); }
static inline SgD sg_GuestAtomicSMin(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 5, a, v); }
static inline SgD sg_GuestAtomicUMax(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 6, a, v); }
static inline SgD sg_GuestAtomicUMin(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 7, a, v); }
static inline SgD sg_GuestAtomicSwap(SgCtx* g, SgD a, SgD v) { return sgx_rmw(g, 8, a, v); }
/* CAS: LOCK CMPXCHG, the old value in RAX; CASP of doublewords: LOCK CMPXCHG16B, the old pair in RDX:RAX */
static inline SgD sg_GuestCompareSwap(SgCtx* g, SgD address, SgD expected, SgD desired) {
    unsigned w = expected.w;
    SgD in[6];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    if (w == 128) {
        const signed char fix[6] = {SGX_R10, (signed char)sgx_address_reg(sgx(g), 1), SGX_RAX, SGX_RDX, SGX_R8, SGX_R9};
        SgXIns* i;
        SgD lo;
        in[2] = sg_d_slice(g, expected, 63, 0);
        in[3] = sg_d_slice(g, expected, 127, 64);
        in[4] = sg_d_slice(g, desired, 63, 0);
        in[5] = sg_d_slice(g, desired, 127, 64);
        lo = sgx_mach(g, SGX_M_CASP, in, fix, 6, 0, 1u << SGX_RCX | 1u << SGX_R11, 2, 64);
        i = &sgx(g)->ins[sgx(g)->n - 1];
        return sg_d_concat(g, sgx_val(64, i->def2), lo);
    }
    {
        const signed char fix[4] = {SGX_R10, (signed char)sgx_address_reg(sgx(g), 0), SGX_RAX, SGX_RDX};
        in[2] = sg_d_zext(g, expected, 64);
        in[3] = sg_d_zext(g, desired, 64);
        return sg_d_slice(g, sgx_mach(g, SGX_M_CAS, in, fix, 4, sgx_log2(w / 8), 0, 1, 64), w - 1, 0);
    }
}

/* ---- the machine's actions ---- */

enum { SG_MBReqTypes_Reads, SG_MBReqTypes_Writes, SG_MBReqTypes_All };
/* x86's ordering (Vol. 3A, 10.2.2) is all Arm's barriers ask but that a store waits for no later load: a barrier of
   loads alone or of stores alone is nothing, a full one MFENCE */
static inline void sg_DataMemoryBarrier(SgCtx* g, long long domain, long long types) {
    (void)domain;
    if (types == SG_MBReqTypes_All) sgx_simple(g, SGX_M_FENCE);
}
static inline void sg_DataSynchronizationBarrier(SgCtx* g, long long domain, long long types, long long nxs) {
    (void)domain, (void)nxs;
    if (types == SG_MBReqTypes_All) sgx_simple(g, SGX_M_FENCE);
}
static inline void sg_SpeculativeStoreBypassBarrierToVA(SgCtx* g) { sgx_simple(g, SGX_M_FENCE); }
static inline void sg_SpeculativeStoreBypassBarrierToPA(SgCtx* g) { sgx_simple(g, SGX_M_FENCE); }
static inline void sg_InstructionSynchronizationBarrier(SgCtx* g) { sgx_simple(g, SGX_M_FENCE); }
static inline void sg_ConsumptionOfSpeculativeDataBarrier(SgCtx* g) { (void)g; }
static inline void sg_SynchronizeErrors(SgCtx* g) { (void)g; }
static inline void sg_TakeUnmaskedSErrorInterrupts(SgCtx* g) { (void)g; }
static inline void sg_Hint_DGH(SgCtx* g) { (void)g; }
static inline void sg_Hint_CLRBHB(SgCtx* g) { (void)g; }
static inline void sg_ProfilingSynchronizationBarrier(SgCtx* g) { (void)g; }
static inline void sg_TraceSynchronizationBarrier(SgCtx* g) { (void)g; }
/* YIELD: PAUSE; WFE: the runtime's rt->wfe(state); SEV and SEVL: rt->sev(); WFI goes on */
static inline void sg_Hint_Yield(SgCtx* g) { sgx_simple(g, SGX_M_PAUSE); }
static inline void sg_Hint_WFE(SgCtx* g) { sgx_call(g, NULL, (int)offsetof(RtAPI, wfe), 1, NULL, 0, 0); }
static inline void sg_Hint_WFI(SgCtx* g) { (void)g; }
static inline void sg_SendEvent(SgCtx* g) { sgx_call(g, NULL, (int)offsetof(RtAPI, sev), 0, NULL, 0, 0); }
static inline void sg_SendEventLocal(SgCtx* g) { sg_SendEvent(g); }
/* CRC32 and CRC32C: the reflected update (BitReverse(Poly32Mod2(...)) as README.md's builtins say): CRC32C SSE4.2's
   CRC32 instruction, CRC32 a byte at a time through x64_helpers.c's table (SGX_M_CRC) */
static inline SgD sg_crc32(SgCtx* g, SgD acc_bits, SgD value_bits, long long size, SgS poly) {
    static const signed char fix[2] = {SGX_RAX, SGX_RDX};
    SgD in[2];
    in[0] = sg_d_zext(g, acc_bits, 64);
    in[1] = sg_d_zext(g, value_bits, 64);
    return sg_d_slice(g, sgx_mach(g, SGX_M_CRC, in, fix, 2, size / 8 | (poly.v == 0x1EDC6F41u) << 4,
                                  1u << SGX_RCX | 1u << SGX_RDX | 1u << SGX_R11, 1, 64), 31, 0);
}

/* an x86 load is an acquire one (Vol. 3A, 10.2.2: loads are not reordered with other loads, nor stores with older
   loads); a release store is MFENCE after it, as the sequentially consistent store the LLVM back end makes (Arm's
   store-release orders it with a later load-acquire, which x86's store buffer alone would not) */
static inline void sgx_acquire(SgCtx* g) { (void)g; }
static inline void sgx_release(SgCtx* g, int after) {
    if (after) sgx_simple(g, SGX_M_FENCE);
}

/* ---- the ABI ----
   x86-64 System V: the state's address comes in RDI and stays in RBX (the callee's, saved by the prologue), RSP is
   16-aligned after the prologue (the push of RBX on the return address, then the frame of spill slots), and a call
   out spills what is in a caller-saved register first (sgx_assemble_region). Windows' (SGX_F_WIN64) has the state in
   RCX and the frame's lowest slots the callees'. These are bytes written here: the instruction selection doesn't make a
   function's frame */

enum { SGX_M_NONE };
/* the function emitter's instructions (labels, jumps, calls: x64_fn.h) */
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

static inline void sgx_prologue(SgXAsm* a) {
    SgXBuf* b = a->b;
    sgx_byte(b, 0x53); /* push rbx */
    sgx_byte(b, 0x48); /* mov rbx, rdi, or rcx */
    sgx_byte(b, 0x89);
    sgx_byte(b, SGX_WIN64(a->x) ? 0xCB : 0xFB);
    sgx_byte(b, 0x48); /* sub rsp, imm32: the frame, patched */
    sgx_byte(b, 0x81);
    sgx_byte(b, 0xEC);
    if (a->nframe < 256) a->frame_at[a->nframe++] = b->n;
    sgx_imm(b, 0, 4);
}
static inline void sgx_epilogue(SgXAsm* a) {
    SgXBuf* b = a->b;
    sgx_byte(b, 0x48); /* add rsp, imm32 */
    sgx_byte(b, 0x81);
    sgx_byte(b, 0xC4);
    if (a->nframe < 256) a->frame_at[a->nframe++] = b->n;
    else
        sg_fail("a function of more than 255 epilogues", 0, 0);
    sgx_imm(b, 0, 4);
    sgx_byte(b, 0x5B); /* pop rbx */
}

/* the machine's instructions, their inputs in r[] */
/* ---- the inline sequences' bytes (sgx_mach's: R10 the memory's base, the address in ad, sgx_address_reg's, RBX the
   state) ---- */

/* opcode op (and op | 1 past bytes) of [R10 + ad] with reg, of 1 << sz bytes: its operand-size prefix, REX (W, R, X, B) */
static inline void sgx_m_mem(SgXBuf* b, int lock, unsigned sz, unsigned op8, int reg, int ad) {
    if (lock) sgx_byte(b, 0xF0);
    if (sz == 1) sgx_byte(b, 0x66);
    sgx_byte(b, (unsigned char)(0x41 | (sz == 3 ? 8 : 0) | (reg >> 3) << 2 | (ad >> 3) << 1));
    if (op8 > 0xFF) sgx_byte(b, (unsigned char)(op8 >> 8));
    sgx_byte(b, (unsigned char)((op8 & 0xFF) | (sz ? 1 : 0)));
    sgx_byte(b, (unsigned char)(0x04 | (reg & 7) << 3)); /* [SIB] */
    sgx_byte(b, (unsigned char)(0x02 | (ad & 7) << 3));  /* ad + R10 */
}
/* lea r11, [r10 + ad], then the pair's LOCK CMPXCHG16B there, the new pair R8 and R9. RBX, the low half's, is kept on the
   stack, or on Windows in R8, whose value comes back with it: the stack pointer stays, for what unwinds a fault there */
static inline void sgx_m_pair(SgXBuf* b, int ad, int win64) {
    sgx_byte(b, (unsigned char)(0x4D | (ad >> 3) << 1));
    sgx_bytes(b, "8D 1C"), sgx_byte(b, (unsigned char)(0x02 | (ad & 7) << 3));
    if (win64) /* xchg rbx, r8; mov rcx, r9; lock cmpxchg16b [r11]; xchg rbx, r8 */
        sgx_bytes(b, "4C 87 C3 4C 89 C9 F0 49 0F C7 0B 4C 87 C3");
    else /* push rbx; mov rbx, r8; mov rcx, r9; lock cmpxchg16b [r11]; pop rbx */
        sgx_bytes(b, "53 4C 89 C3 4C 89 C9 F0 49 0F C7 0B 5B");
}
static inline void sgx_jcc32(SgXBuf* b, unsigned cc, size_t* at) {
    sgx_byte(b, 0x0F);
    sgx_byte(b, (unsigned char)(0x80 | cc));
    *at = b->n;
    sgx_imm(b, 0, 4);
}
static inline void sgx_patch32(SgXBuf* b, size_t at) {
    long long d = (long long)b->n - (long long)(at + 4);
    if (at + 4 <= b->cap) {
        b->p[at] = (unsigned char)d, b->p[at + 1] = (unsigned char)(d >> 8), b->p[at + 2] = (unsigned char)(d >> 16);
        b->p[at + 3] = (unsigned char)(d >> 24);
    }
}
static inline void sgx_d32(SgXBuf* b, size_t off) { sgx_imm(b, (long long)off, 4); }
/* the monitor's checks (armed, the address, and for a single its size covering 1 << sz bytes; a pair's exactly),
   failing to *fail */
static inline void sgx_m_monitor(SgXBuf* b, int pair, unsigned sz, int ad, size_t* fail) {
    sgx_bytes(b, "8B 8B"), sgx_d32(b, offsetof(CPU, excl_on)); /* mov ecx, [rbx + excl_on] */
    sgx_bytes(b, "85 C9");                                       /* test ecx, ecx */
    sgx_jcc32(b, 4, &fail[0]);                                   /* jz */
    sgx_byte(b, (unsigned char)(0x48 | (ad >> 3) << 2)), sgx_byte(b, 0x3B); /* cmp ad, [rbx + excl_addr] */
    sgx_byte(b, (unsigned char)(0x83 | (ad & 7) << 3)), sgx_d32(b, offsetof(CPU, excl_addr));
    sgx_jcc32(b, 5, &fail[1]); /* jne */
    if (pair) {
        sgx_bytes(b, "83 BB"), sgx_d32(b, offsetof(CPU, excl_size)), sgx_byte(b, (unsigned char)(sz | 0x10));
        sgx_jcc32(b, 5, &fail[2]); /* cmp dword [rbx + excl_size], sz | 0x10; jne */
    } else {
        sgx_bytes(b, "8B 8B"), sgx_d32(b, offsetof(CPU, excl_size)); /* mov ecx, [rbx + excl_size] */
        sgx_bytes(b, "83 E1 03 83 F9"), sgx_byte(b, (unsigned char)sz); /* and ecx, 3; cmp ecx, sz */
        sgx_jcc32(b, 2, &fail[2]);                                     /* jb */
    }
}
/* status = ZF ? 0 : 1; then the failure's 1; then the monitor disarmed */
static inline void sgx_m_status(SgXBuf* b, const size_t* fail) {
    size_t done;
    sgx_bytes(b, "0F 95 C0 0F B6 C0"); /* setne al; movzx eax, al */
    sgx_byte(b, 0xE9);
    done = b->n;
    sgx_imm(b, 0, 4);
    sgx_patch32(b, fail[0]), sgx_patch32(b, fail[1]), sgx_patch32(b, fail[2]);
    sgx_bytes(b, "B8 01 00 00 00"); /* mov eax, 1 */
    sgx_patch32(b, done);
    sgx_bytes(b, "C7 83"), sgx_d32(b, offsetof(CPU, excl_on)), sgx_imm(b, 0, 4); /* mov dword [rbx + excl_on], 0 */
}
static inline void sgx_machine_inline(SgXAsm* a, SgXIns* i) {
    SgXBuf* b = a->b;
    unsigned sz = (unsigned)(i->imm & 3);
    const int win64 = SGX_WIN64(a->x), ad = sgx_address_reg(a->x, i->op == SGX_M_STXP || i->op == SGX_M_CASP);
    size_t fail[3];
    switch (i->op) {
    case SGX_M_STX: /* cpu.h's stx */
        sgx_m_monitor(b, 0, sz, ad, fail);
        sgx_bytes(b, "48 8B 83"), sgx_d32(b, offsetof(CPU, excl_val)); /* mov rax, [rbx + excl_val] */
        sgx_m_mem(b, 1, sz, 0x0FB0, 2, ad);                            /* lock cmpxchg [r10 + ad], dl.. */
        sgx_m_status(b, fail);
        return;
    case SGX_M_STXP: /* cpu.h's stxp */
        sgx_m_monitor(b, 1, sz, ad, fail);
        sgx_bytes(b, "48 8B 83"), sgx_d32(b, offsetof(CPU, excl_val));
        if (sz == 2) {
            /* the words in one doubleword: ecx = r8d, rdx = r9 << 32 | rcx */
            sgx_bytes(b, "44 89 C1 4C 89 CA 48 C1 E2 20 48 09 CA");
            sgx_m_mem(b, 1, 3, 0x0FB0, 2, ad);
        } else {
            sgx_bytes(b, "48 8B 93"), sgx_d32(b, offsetof(CPU, excl_val2)); /* mov rdx, [rbx + excl_val2] */
            sgx_m_pair(b, ad, win64);
        }
        sgx_m_status(b, fail);
        return;
    case SGX_M_CAS: sgx_m_mem(b, 1, sz, 0x0FB0, 2, ad); return; /* lock cmpxchg [r10 + ad], dl.. */
    case SGX_M_CASP: sgx_m_pair(b, ad, win64); return;
    case SGX_M_RMW: {
        unsigned op = (unsigned)(i->imm >> 2);
        size_t loop;
        if (op == 0 || op == 8) {
            sgx_bytes(b, "48 89 D0"); /* mov rax, rdx; lock xadd / xchg [r10 + ad], al.. */
            if (op == 0) sgx_m_mem(b, 1, sz, 0x0FC0, 0, ad);
            else
                sgx_m_mem(b, 0, sz, 0x86, 0, ad);
            return;
        }
        /* the old value zero extended (movzx eax, byte / word; mov eax / rax) */
        static const char* const ld[4] = {"0F B6 04", "0F B7 04", "8B 04", "8B 04"};
        sgx_byte(b, (unsigned char)(0x41 | (sz == 3 ? 8 : 0) | (ad >> 3) << 1));
        sgx_bytes(b, ld[sz]), sgx_byte(b, (unsigned char)(0x02 | (ad & 7) << 3));
        loop = b->n;
        sgx_bytes(b, "48 89 C1"); /* mov rcx, rax */
        if (op == 1) sgx_bytes(b, "48 21 D1");      /* and rcx, rdx */
        else if (op == 2) sgx_bytes(b, "48 31 D1"); /* xor */
        else if (op == 3) sgx_bytes(b, "48 09 D1"); /* or */
        else {
            /* both extended to 64 bits as signed (4, 5) or unsigned (6, 7) at the size, compared, the larger or smaller */
            static const char* const sx[4] = {"48 0F BE C9 4C 0F BE DA", "48 0F BF C9 4C 0F BF DA", "48 63 C9 4C 63 DA",
                                              "49 89 D3"};
            static const char* const zx[4] = {"0F B6 C9 44 0F B6 DA", "0F B7 C9 44 0F B7 DA", "89 C9 41 89 D3",
                                              "49 89 D3"};
            sgx_bytes(b, op <= 5 ? sx[sz] : zx[sz]);
            sgx_bytes(b, "4C 39 D9"); /* cmp rcx, r11 */
            sgx_bytes(b, op == 4 ? "49 0F 4C CB" : op == 5 ? "49 0F 4F CB" : op == 6 ? "49 0F 42 CB" : "49 0F 47 CB");
        }
        sgx_m_mem(b, 1, sz, 0x0FB0, 1, ad); /* lock cmpxchg [r10 + ad], cl.. */
        sgx_byte(b, 0x0F), sgx_byte(b, 0x85);
        sgx_imm(b, (long long)loop - (long long)(b->n + 4), 4); /* jne loop */
        return;
    }
    case SGX_M_CRC: {
        unsigned bytes = (unsigned)(i->imm & 15), k;
        if (i->imm & 16) { /* crc32 eax, dl / dx / edx; crc32 rax, rdx */
            sgx_bytes(b, bytes == 1 ? "F2 0F 38 F0 C2" : bytes == 2 ? "66 F2 0F 38 F1 C2" : bytes == 4 ? "F2 0F 38 F1 C2" : "F2 48 0F 38 F1 C2");
            return;
        }
        /* the table's address in R11: a relocation of the object's, or the address resolve gives */
        if (a->reloc) {
            sgx_bytes(b, "4C 8D 1D"); /* lea r11, [rip + table] */
            a->reloc(a->machine, b->n, "snx_x64_crc32_table", -4);
            sgx_imm(b, 0, 4);
        } else {
            SnxU64 t = a->resolve ? a->resolve("snx_x64_crc32_table") : 0;
            if (!t) sg_fail("a table nothing resolves", 0, 0);
            sgx_bytes(b, "49 BB"); /* mov r11, imm64 */
            sgx_imm(b, (long long)t, 8);
        }
        for (k = 0; k < bytes; ++k)
            /* mov ecx, eax; xor cl, dl; movzx ecx, cl; shr eax, 8; xor eax, [r11 + rcx*4]; shr rdx, 8 */
            sgx_bytes(b, "89 C1 30 D1 0F B6 C9 C1 E8 08 41 33 04 8B 48 C1 EA 08");
        return;
    }
    default: return;
    }
}

static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgXBuf* b = a->b;
    switch (i->kind) {
    case SGX_K_UNDEF:
        /* state->pc = pc, then rt->undef(state, pc, word): RDI, RSI, EDX (on Windows RCX, RDX, R8D), through RAX =
           state->rt */
        if (a->pin_flush) a->pin_flush(a->machine);
        sgx_byte(b, 0x48); /* mov rax, imm64; mov [rbx + pc], rax */
        sgx_byte(b, 0xB8);
        sgx_imm(b, i->imm, 8);
        sgx_byte(b, 0x48);
        sgx_byte(b, 0x89);
        sgx_byte(b, 0x83);
        sgx_imm(b, (long long)offsetof(CPU, pc), 4);
        sgx_call_room(a);
        sgx_state_arg(a, b);
        sgx_byte(b, 0x48); /* mov rsi (rdx), imm64 */
        sgx_byte(b, SGX_WIN64(a->x) ? 0xBA : 0xBE);
        sgx_imm(b, i->imm, 8);
        sgx_bytes(b, SGX_WIN64(a->x) ? "41 B8" : "BA"); /* mov edx (r8d), imm32 */
        sgx_imm(b, i->disp, 4);
        sgx_byte(b, 0x48); /* mov rax, [rbx + rt] */
        sgx_byte(b, 0x8B);
        sgx_byte(b, 0x83);
        sgx_imm(b, (long long)offsetof(CPU, rt), 4);
        sgx_byte(b, 0xFF); /* call [rax + undef] */
        sgx_byte(b, 0x90);
        sgx_imm(b, (long long)offsetof(RtAPI, undef), 4);
        break;
    case SGX_K_RET:
        /* a step's return of the next pc, in RAX (in[0], fixed there) or a constant */
        if (!i->in[0]) {
            const SgXSel* m = sgx_pick(a->x, sgx_sel_movi_64, sgx_sel_movi_64_n, (SnxU64)i->imm, 64, 1);
            unsigned char rr[4] = {0, 0, 0, 0};
            rr[(int)m->out] = 0;
            m->enc(b, rr, i->imm, 0);
        }
        sgx_epilogue(a);
        sgx_byte(b, 0xC3); /* ret */
        break;
    case SGX_K_MACHINE:
        switch (i->op) {
        case SGX_M_FENCE:
            sgx_byte(b, 0x0F); /* mfence */
            sgx_byte(b, 0xAE);
            sgx_byte(b, 0xF0);
            return;
        case SGX_M_PAUSE:
            sgx_byte(b, 0xF3); /* pause */
            sgx_byte(b, 0x90);
            return;
        case SGX_M_STX:
        case SGX_M_STXP:
        case SGX_M_CAS:
        case SGX_M_CASP:
        case SGX_M_RMW:
        case SGX_M_CRC: sgx_machine_inline(a, i); return;
        case SGX_M_CALL:
        case SGX_M_CALL_RT:
            sgx_call_room(a);
            if (a->pin_cs) a->pin_cs(a->machine, 1); /* the caller-saved pins around the call */
            if (i->w) sgx_state_arg(a, b);
            if (i->op == SGX_M_CALL_RT) {
                sgx_byte(b, 0x48); /* mov rax, [rbx + rt] */
                sgx_byte(b, 0x8B);
                sgx_byte(b, 0x83);
                sgx_imm(b, (long long)offsetof(CPU, rt), 4);
                sgx_byte(b, 0xFF); /* call [rax + offset] */
                sgx_byte(b, 0x90);
                sgx_imm(b, i->disp, 4);
            } else if (a->reloc) {
                sgx_byte(b, 0xE8); /* call rel32, which the object's relocation fills */
                a->reloc(a->machine, b->n, i->sym, -4);
                sgx_imm(b, 0, 4);
            } else {
                SnxU64 at_ = a->resolve ? a->resolve(i->sym) : 0;
                if (!at_) sg_fail("a helper nothing resolves", 0, 0);
                sgx_byte(b, 0x48); /* mov rax, imm64; call rax */
                sgx_byte(b, 0xB8);
                sgx_imm(b, (long long)at_, 8);
                sgx_byte(b, 0xFF);
                sgx_byte(b, 0xD0);
            }
            if (a->pin_cs) a->pin_cs(a->machine, 0);
            return;
        default: sgx_machine_op(a, at, r); return;
        }
    default: break;
    }
    (void)r;
}

/* the function emitter's (x64_fn.h's, after this) where it is included; none in a step's or a block's code */
#ifndef SGX_FN
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r) {
    (void)a, (void)r;
    sg_fail("a machine instruction of no function emitter", at, 0);
}
#endif

