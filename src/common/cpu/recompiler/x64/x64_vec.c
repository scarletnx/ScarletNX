/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AdvSIMD instructions of the x86-64 back end (x64_codegen.c includes this after sailgen's emitters): what the
   LLVM back end's ir_vec.c does, on the region's SSA in XMM registers. The SIMD&FP registers are the block's XMM
   values (x64_a64.h's sg__V_read_bits, sg__V_write); each operation is SSE forms of x64.ops' enc lines, a byte
   shuffle (PSHUFB, its masks in the function's constant pool) where the lanes move, general-purpose code lane by lane
   where x86 has no form (a shift of each lane by its own amount). Every one is held against the LLVM back end by
   recompiler_backends. vec() returns 1 made, 0 not one of these (the function goes to LLVM), -1 UNDEF */

#define X sgx(g)

/* V[n] as an XMM register */
static int vread(SgCtx* g, unsigned n) {
    SgD d = sg__V_read_bits(g, n, 127, 0);
    return sgx_xreg(X, &d);
}
/* V[d] = r, its high doubleword cleared where q is 0 */
static void vwrite(SgCtx* g, unsigned d, int r, int q) {
    if (!q) r = sgx_xclear(X, r, 64);
    sg__V_write(g, d, sgx_xval(128, r));
}
static int op2(SgCtx* g, const SgXVForm* f, int a, int b) { return sgx_v2(X, f, a, b, 0); }
/* a shift-by-immediate form (PSLLW xmm1, imm8): a's register, shifted */
static int opi(SgCtx* g, const SgXVForm* f, int a, long long imm) {
    int in[4] = {a, 0, 0, 0};
    return sgx_vins(X, f, 0, 1, in, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
static int kconst(SgCtx* g, SnxU64 lo, SnxU64 hi) { return sgx_xconst(X, lo, hi); }
static int vzero(SgCtx* g) { return kconst(g, 0, 0); }
static int vones(SgCtx* g) { return kconst(g, ~0ull, ~0ull); }
/* each element of 8 << size bits the constant k */
static SnxU64 splat64(unsigned size, SnxU64 k) {
    unsigned e = 8u << size, i;
    SnxU64 r = 0;
    k &= sg_ones(e);
    for (i = 0; i < 64; i += e) r |= k << i;
    return r;
}
static int vsplat(SgCtx* g, unsigned size, SnxU64 k) {
    SnxU64 v = splat64(size, k);
    return kconst(g, v, v);
}
static int vnot(SgCtx* g, int a) { return op2(g, &sgx_v_pxor, a, vones(g)); }
static int vand(SgCtx* g, int a, int b) { return op2(g, &sgx_v_pand, a, b); }
static int vor(SgCtx* g, int a, int b) { return op2(g, &sgx_v_por, a, b); }
static int vxor(SgCtx* g, int a, int b) { return op2(g, &sgx_v_pxor, a, b); }
/* ~a & b */
static int vandn(SgCtx* g, int a, int b) { return op2(g, &sgx_v_pandn, a, b); }
/* m ? a : b, m of whole lanes */
static int vsel(SgCtx* g, int m, int a, int b) { return vor(g, vand(g, m, a), vandn(g, m, b)); }

static const SgXVForm* const k_add[4] = {&sgx_v_paddb, &sgx_v_paddw, &sgx_v_paddd, &sgx_v_paddq};
static const SgXVForm* const k_sub[4] = {&sgx_v_psubb, &sgx_v_psubw, &sgx_v_psubd, &sgx_v_psubq};
static const SgXVForm* const k_eq[4] = {&sgx_v_pcmpeqb, &sgx_v_pcmpeqw, &sgx_v_pcmpeqd, &sgx_v_pcmpeqq};
static const SgXVForm* const k_gt[4] = {&sgx_v_pcmpgtb, &sgx_v_pcmpgtw, &sgx_v_pcmpgtd, &sgx_v_pcmpgtq};
static const SgXVForm* const k_maxs[4] = {&sgx_v_pmaxsb, &sgx_v_pmaxsw, &sgx_v_pmaxsd, NULL};
static const SgXVForm* const k_mins[4] = {&sgx_v_pminsb, &sgx_v_pminsw, &sgx_v_pminsd, NULL};
static const SgXVForm* const k_maxu[4] = {&sgx_v_pmaxub, &sgx_v_pmaxuw, &sgx_v_pmaxud, NULL};
static const SgXVForm* const k_minu[4] = {&sgx_v_pminub, &sgx_v_pminuw, &sgx_v_pminud, NULL};
static const SgXVForm* const k_sll[4] = {NULL, &sgx_v_psllw_i, &sgx_v_pslld_i, &sgx_v_psllq_i};
static const SgXVForm* const k_srl[4] = {NULL, &sgx_v_psrlw_i, &sgx_v_psrld_i, &sgx_v_psrlq_i};
static const SgXVForm* const k_sra[4] = {NULL, &sgx_v_psraw_i, &sgx_v_psrad_i, NULL};
static const SgXVForm* const k_abs[4] = {&sgx_v_pabsb, &sgx_v_pabsw, &sgx_v_pabsd, NULL};
static const SgXVForm* const k_sx[3] = {&sgx_v_pmovsxbw, &sgx_v_pmovsxwd, &sgx_v_pmovsxdq};
static const SgXVForm* const k_zx[3] = {&sgx_v_pmovzxbw, &sgx_v_pmovzxwd, &sgx_v_pmovzxdq};

static int vadd(SgCtx* g, unsigned size, int a, int b) { return op2(g, k_add[size], a, b); }
static int vsub(SgCtx* g, unsigned size, int a, int b) { return op2(g, k_sub[size], a, b); }
static int vneg(SgCtx* g, unsigned size, int a) { return vsub(g, size, vzero(g), a); }
static int veq(SgCtx* g, unsigned size, int a, int b) { return op2(g, k_eq[size], a, b); }
/* a > b, signed; unsigned through the sign bits flipped */
static int vgt(SgCtx* g, unsigned size, int a, int b, int uns) {
    if (uns) {
        int f = vsplat(g, size, 1ull << ((8u << size) - 1));
        a = vxor(g, a, f);
        b = vxor(g, b, f);
    }
    return op2(g, k_gt[size], a, b);
}
/* the larger (or smaller) of a and b */
static int vmax(SgCtx* g, unsigned size, int a, int b, int uns, int minimum) {
    const SgXVForm* f = minimum ? (uns ? k_minu : k_mins)[size] : (uns ? k_maxu : k_maxs)[size];
    if (f) return op2(g, f, a, b);
    {
        int m = vgt(g, size, a, b, uns);
        return minimum ? vsel(g, m, b, a) : vsel(g, m, a, b);
    }
}
/* a << n, n below the element's bits (the bytes by words, their spill masked) */
static int vshl(SgCtx* g, unsigned size, int a, unsigned n) {
    if (n >= 8u << size) return vzero(g);
    if (!n) return a;
    if (!size) return vand(g, opi(g, &sgx_v_psllw_i, a, n), vsplat(g, 0, 0xFFu << n));
    return opi(g, k_sll[size], a, n);
}
/* a >> n, logical (arith: arithmetic), n up to the element's bits */
static int vshr(SgCtx* g, unsigned size, int a, unsigned n, int arith) {
    unsigned e = 8u << size;
    if (arith) {
        if (n >= e) n = e - 1;
        if (!n) return a;
        if (k_sra[size]) return opi(g, k_sra[size], a, n);
        /* (a ^ m) >> n - (m >> n), m the sign bit: the logical shift sign extended */
        {
            int m = vsplat(g, size, 1ull << (e - 1)), t = vshr(g, size, vxor(g, a, m), n, 0);
            return vsub(g, size, t, vsplat(g, size, (1ull << (e - 1)) >> n));
        }
    }
    if (n >= e) return vzero(g);
    if (!n) return a;
    if (!size) return vand(g, opi(g, &sgx_v_psrlw_i, a, n), vsplat(g, 0, 0xFFu >> n));
    return opi(g, k_srl[size], a, n);
}

/* ---- byte shuffles ----
   r's byte i is a's idx[i] (below 16), b's idx[i] - 16 (16 to 31), or 0 (any other): PSHUFB of each, ORed */
static int vperm(SgCtx* g, int a, int b, const int* idx) {
    SnxU64 ma[2] = {0, 0}, mb[2] = {0, 0};
    int i, any_a = 0, any_b = 0, ra = 0, rb = 0;
    for (i = 0; i < 16; ++i) {
        unsigned ka = 0x80, kb = 0x80;
        if (idx[i] >= 0 && idx[i] < 16) ka = (unsigned)idx[i], any_a = 1;
        else if (idx[i] >= 16 && idx[i] < 32)
            kb = (unsigned)idx[i] - 16, any_b = 1;
        ma[i / 8] |= (SnxU64)ka << (8 * (i % 8));
        mb[i / 8] |= (SnxU64)kb << (8 * (i % 8));
    }
    if (any_a) ra = op2(g, &sgx_v_pshufb, a, kconst(g, ma[0], ma[1]));
    if (any_b) rb = op2(g, &sgx_v_pshufb, b, kconst(g, mb[0], mb[1]));
    if (!any_a && !any_b) return vzero(g);
    if (!any_b) return ra;
    if (!any_a) return rb;
    return vor(g, ra, rb);
}
/* element e (of eb bytes) of the pair a:b (n elements each) as idx: a's, then b's */
static void put_elem(int* idx, int at, int eb, int from_b, int e) {
    int k;
    for (k = 0; k < eb; ++k) idx[at * eb + k] = (from_b ? 16 : 0) + e * eb + k;
}
/* the permutes, of 8 << size bits and q's length: ZIP1 (kind 0), ZIP2, UZP1, UZP2, TRN1, TRN2 */
static int vzip(SgCtx* g, int kind, unsigned size, int q, int a, int b) {
    int idx[16], i, eb = 1 << size, n = (q ? 16 : 8) / eb;
    for (i = 0; i < 16; ++i) idx[i] = -1;
    for (i = 0; i < n; ++i)
        switch (kind) {
        case 0: put_elem(idx, i, eb, i & 1, i / 2); break;
        case 1: put_elem(idx, i, eb, i & 1, n / 2 + i / 2); break;
        case 2: put_elem(idx, i, eb, i >= n / 2, 2 * (i % (n / 2))); break;
        case 3: put_elem(idx, i, eb, i >= n / 2, 2 * (i % (n / 2)) + 1); break;
        case 4: put_elem(idx, i, eb, i & 1, (i & ~1)); break;
        default: put_elem(idx, i, eb, i & 1, (i & ~1) + 1); break;
        }
    return vperm(g, a, b, idx);
}
/* the low half of each element of 16 << size bits (high: the high half), into the low doubleword */
static int vnarrow(SgCtx* g, unsigned size, int a, int high) {
    int idx[16], i, k, eb = 1 << size;
    for (i = 0; i < 16; ++i) idx[i] = -1;
    for (i = 0; i < 8 / eb; ++i)
        for (k = 0; k < eb; ++k) idx[i * eb + k] = i * 2 * eb + (high ? eb : 0) + k;
    return vperm(g, a, 0, idx);
}
/* a narrow result r into V[d]: the low doubleword (q 0, the upper cleared), or the high one, the low kept (the '2'
   forms) */
static void vwrite_narrow(SgCtx* g, unsigned d, int r, int q) {
    if (!q) {
        vwrite(g, d, r, 0);
        return;
    }
    vwrite(g, d, op2(g, &sgx_v_punpcklqdq, vread(g, d), r), 1);
}
/* the elements of a's low doubleword (hi: its high one) extended to twice their bits */
static int vwiden(SgCtx* g, unsigned size, int a, int hi, int sign) {
    if (hi) a = sgx_v1(X, &sgx_v_pshufd, a, 0xEE);
    return sgx_v1(X, (sign ? k_sx : k_zx)[size], a, 0);
}
/* element e of a (of 8 << size bits) in every lane */
static int vdup(SgCtx* g, unsigned size, int a, int e) {
    int idx[16], i, eb = 1 << size;
    for (i = 0; i < 16; ++i) idx[i] = e * eb + i % eb;
    return vperm(g, a, 0, idx);
}
/* a * b of 8 << size bits (the bytes by words: the even bytes' products and the odd ones' moved up) */
static int vmul(SgCtx* g, unsigned size, int a, int b) {
    if (size == 1) return op2(g, &sgx_v_pmullw, a, b);
    if (size == 2) return op2(g, &sgx_v_pmulld, a, b);
    if (size == 0) {
        int even = vand(g, op2(g, &sgx_v_pmullw, a, b), vsplat(g, 1, 0xFF));
        int odd = op2(g, &sgx_v_pmullw, opi(g, &sgx_v_psrlw_i, a, 8), opi(g, &sgx_v_psrlw_i, b, 8));
        return vor(g, even, opi(g, &sgx_v_psllw_i, odd, 8));
    }
    sg_fail("a vector product of doublewords", size, 0);
}
/* the operation folded over a's first n elements: element 0 */
static int vreduce(SgCtx* g, unsigned size, int a, int n, int (*op)(SgCtx*, unsigned, int, int, int, int), int uns,
                   int minimum) {
    int eb = 1 << size;
    while (n > 1) {
        int idx[16], i;
        for (i = 0; i < 16; ++i) idx[i] = i + n / 2 * eb < 16 ? i + n / 2 * eb : -1;
        a = op(g, size, a, vperm(g, a, 0, idx), uns, minimum);
        n /= 2;
    }
    return a;
}
static int red_add(SgCtx* g, unsigned size, int a, int b, int uns, int minimum) {
    (void)uns, (void)minimum;
    return vadd(g, size, a, b);
}

/* ---- the lanes one at a time: what x86 has no form for ---- */

static const SgXVForm* const k_pextr[4] = {&sgx_v_pextrb, &sgx_v_pextrw, &sgx_v_pextrd, &sgx_v_pextrq};
static const SgXVForm* const k_pinsr[4] = {&sgx_v_pinsrb, &sgx_v_pinsrw, &sgx_v_pinsrd, &sgx_v_pinsrq};
/* lane i of a (8 << size bits) as a general-purpose value */
static SgD lane_get(SgCtx* g, unsigned size, int a, int i) {
    int in[4] = {0, a, 0, 0};
    return sgx_val(8u << size, sgx_vins(X, k_pextr[size], 0, 0, in, i, 0, SGX_VM_NONE, 0, 0, 0, 0, 0));
}
/* a with lane i v */
static int lane_put(SgCtx* g, unsigned size, int a, int i, SgD v) {
    int in[4] = {0, 0, 0, 0};
    v = sg_d_zext(g, v, size == 3 ? 64 : 32);
    in[0] = a;
    in[1] = sgx_reg(X, &v);
    return sgx_vins(X, k_pinsr[size], 0, 1, in, i, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
/* SSHL, USHL by register: each lane by the signed byte at its bottom in m: left where positive, right where
   negative (arithmetic for SSHL), 0 (or the sign, SSHL's right) where the amount is the lane's bits or more */
static int vshl_reg(SgCtx* g, unsigned size, int q, int a, int m, int sign) {
    unsigned e = 8u << size;
    int n = (q ? 16 : 8) >> size, i, r = vzero(g);
    for (i = 0; i < n; ++i) {
        SgD v = lane_get(g, size, a, i), s = sg_d_sext(g, sg_d_slice(g, lane_get(g, size, m, i), 7, 0), 64), res;
        SgD v64 = sign ? sg_d_sext(g, v, 64) : sg_d_zext(g, v, 64);
        SgD neg = sg_d_sub(g, sgx_const(64, 0), s);
        SgB left = sgx_d_ult(g, s, sgx_const(64, 1ull << 63)); /* s >= 0 */
        SgB lbig = sg_b_not(g, sgx_d_ult(g, s, sgx_const(64, e)));
        SgB rbig = sg_b_not(g, sgx_d_ult(g, neg, sgx_const(64, e)));
        SgD l = sg_d_shift_dyn(g, v64, sg_d_and(g, s, sgx_const(64, 63)), 0);
        SgD rr = sg_d_shift_dyn(g, v64, sg_d_and(g, neg, sgx_const(64, 63)), sign ? 2 : 1);
        SgD rfill = sign ? sg_d_shift_dyn(g, v64, sgx_const(64, 63), 2) : sgx_const(64, 0);
        l = sg_d_select(g, lbig, sgx_const(64, 0), l);
        rr = sg_d_select(g, rbig, rfill, rr);
        res = sg_d_select(g, left, l, rr);
        r = lane_put(g, size, r, i, sg_d_slice(g, res, e - 1, 0));
    }
    return r;
}

/* ---- the instructions ---- */

/* immh:immb's element size (the highest bit of immh) and shift: left (immh:immb - esize) or right (2 * esize -
   immh:immb); -1 for immh 0 */
static int shift_imm(SnxU32 word, unsigned* size, unsigned* left, unsigned* right) {
    unsigned immh = word >> 19 & 15, immhb = word >> 16 & 127, s;
    if (!immh) return -1;
    for (s = 3; !(immh >> s & 1); --s) {}
    *size = s;
    *left = immhb - (8u << s);
    *right = 2 * (8u << s) - immhb;
    return 0;
}

static int vec(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned q = word >> 30 & 1, uns = word >> 29 & 1, size = word >> 22 & 3;
    unsigned rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31;
    int a, b, r;
    switch (insn->op) {
    /* bitwise, the whole register */
    case SNX_A64_OP_AND_v: vwrite(g, rd, vand(g, vread(g, rn), vread(g, rm)), q); return 1;
    case SNX_A64_OP_BIC_v: vwrite(g, rd, vandn(g, vread(g, rm), vread(g, rn)), q); return 1;
    case SNX_A64_OP_ORR_v: vwrite(g, rd, vor(g, vread(g, rn), vread(g, rm)), q); return 1;
    case SNX_A64_OP_ORN_v: vwrite(g, rd, vor(g, vread(g, rn), vnot(g, vread(g, rm))), q); return 1;
    case SNX_A64_OP_EOR_v: vwrite(g, rd, vxor(g, vread(g, rn), vread(g, rm)), q); return 1;
    case SNX_A64_OP_BSL_v: { /* d = m ^ ((n ^ m) & d) */
        int d = vread(g, rd), m = vread(g, rm);
        vwrite(g, rd, vxor(g, m, vand(g, vxor(g, vread(g, rn), m), d)), q);
        return 1;
    }
    case SNX_A64_OP_BIT_v: { /* d = d ^ ((d ^ n) & m) */
        int d = vread(g, rd);
        vwrite(g, rd, vxor(g, d, vand(g, vxor(g, d, vread(g, rn)), vread(g, rm))), q);
        return 1;
    }
    case SNX_A64_OP_BIF_v: { /* d = d ^ ((d ^ n) & ~m) */
        int d = vread(g, rd);
        vwrite(g, rd, vxor(g, d, vandn(g, vread(g, rm), vxor(g, d, vread(g, rn)))), q);
        return 1;
    }
    /* integer arithmetic, compares, minimum and maximum, lane by lane */
    case SNX_A64_OP_ADD_v:
    case SNX_A64_OP_SUB_v:
        if (size == 3 && !q) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        vwrite(g, rd, insn->op == SNX_A64_OP_ADD_v ? vadd(g, size, a, b) : vsub(g, size, a, b), q);
        return 1;
    case SNX_A64_OP_NEG_v:
    case SNX_A64_OP_ABS_v:
        if (size == 3 && !q) return -1;
        a = vread(g, rn);
        if (insn->op == SNX_A64_OP_NEG_v) r = vneg(g, size, a);
        else if (k_abs[size])
            r = sgx_v1(X, k_abs[size], a, 0);
        else { /* (a ^ s) - s, s the sign */
            int s = vgt(g, 3, vzero(g), a, 0);
            r = vsub(g, 3, vxor(g, a, s), s);
        }
        vwrite(g, rd, r, q);
        return 1;
    case SNX_A64_OP_MUL_v:
    case SNX_A64_OP_MLA_v:
    case SNX_A64_OP_MLS_v:
        if (size == 3) return -1;
        r = vmul(g, size, vread(g, rn), vread(g, rm));
        if (insn->op == SNX_A64_OP_MLA_v) r = vadd(g, size, vread(g, rd), r);
        else if (insn->op == SNX_A64_OP_MLS_v)
            r = vsub(g, size, vread(g, rd), r);
        vwrite(g, rd, r, q);
        return 1;
    case SNX_A64_OP_MUL_vi:
    case SNX_A64_OP_MLA_vi:
    case SNX_A64_OP_MLS_vi: {
        /* by element: H:L:M of halfwords (Rm 0-15), H:L of words */
        unsigned h = word >> 11 & 1, l = word >> 21 & 1, m4 = word >> 20 & 1;
        int e;
        if (size == 1) {
            e = (int)(h << 2 | l << 1 | m4);
            rm &= 15;
        } else if (size == 2)
            e = (int)(h << 1 | l);
        else
            return -1;
        r = vmul(g, size, vread(g, rn), vdup(g, size, vread(g, rm), e));
        if (insn->op == SNX_A64_OP_MLA_vi) r = vadd(g, size, vread(g, rd), r);
        else if (insn->op == SNX_A64_OP_MLS_vi)
            r = vsub(g, size, vread(g, rd), r);
        vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_CMEQ_v:
    case SNX_A64_OP_CMGT_v:
    case SNX_A64_OP_CMGE_v:
    case SNX_A64_OP_CMHI_v:
    case SNX_A64_OP_CMHS_v:
    case SNX_A64_OP_CMTST_v:
        if (size == 3 && !q) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        switch (insn->op) {
        case SNX_A64_OP_CMEQ_v: r = veq(g, size, a, b); break;
        case SNX_A64_OP_CMGT_v: r = vgt(g, size, a, b, 0); break;
        case SNX_A64_OP_CMHI_v: r = vgt(g, size, a, b, 1); break;
        case SNX_A64_OP_CMGE_v: r = vnot(g, vgt(g, size, b, a, 0)); break;
        case SNX_A64_OP_CMHS_v: r = vnot(g, vgt(g, size, b, a, 1)); break;
        default: r = vnot(g, veq(g, size, vand(g, a, b), vzero(g))); break;
        }
        vwrite(g, rd, r, q);
        return 1;
    case SNX_A64_OP_CMEQ0_v:
    case SNX_A64_OP_CMGT0_v:
    case SNX_A64_OP_CMGE0_v:
    case SNX_A64_OP_CMLE0_v:
    case SNX_A64_OP_CMLT0_v:
        if (size == 3 && !q) return -1;
        a = vread(g, rn);
        switch (insn->op) {
        case SNX_A64_OP_CMEQ0_v: r = veq(g, size, a, vzero(g)); break;
        case SNX_A64_OP_CMGT0_v: r = vgt(g, size, a, vzero(g), 0); break;
        case SNX_A64_OP_CMGE0_v: r = vnot(g, vgt(g, size, vzero(g), a, 0)); break;
        case SNX_A64_OP_CMLE0_v: r = vnot(g, vgt(g, size, a, vzero(g), 0)); break;
        default: r = vgt(g, size, vzero(g), a, 0); break;
        }
        vwrite(g, rd, r, q);
        return 1;
    case SNX_A64_OP_SMAX_v:
    case SNX_A64_OP_SMIN_v:
    case SNX_A64_OP_UMAX_v:
    case SNX_A64_OP_UMIN_v: {
        int mn = insn->op == SNX_A64_OP_SMIN_v || insn->op == SNX_A64_OP_UMIN_v;
        int un = insn->op == SNX_A64_OP_UMAX_v || insn->op == SNX_A64_OP_UMIN_v;
        if (size == 3) return -1;
        vwrite(g, rd, vmax(g, size, vread(g, rn), vread(g, rm), un, mn), q);
        return 1;
    }
    /* pairwise: the even elements of n:m (concatenated) with the odd ones */
    case SNX_A64_OP_ADDP_v:
    case SNX_A64_OP_SMAXP_v:
    case SNX_A64_OP_SMINP_v:
    case SNX_A64_OP_UMAXP_v:
    case SNX_A64_OP_UMINP_v: {
        int ev, od;
        if (size == 3 && (!q || insn->op != SNX_A64_OP_ADDP_v)) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        ev = vzip(g, 2, size, q, a, b);
        od = vzip(g, 3, size, q, a, b);
        if (insn->op == SNX_A64_OP_ADDP_v) r = vadd(g, size, ev, od);
        else
            r = vmax(g, size, ev, od, insn->op == SNX_A64_OP_UMAXP_v || insn->op == SNX_A64_OP_UMINP_v,
                     insn->op == SNX_A64_OP_SMINP_v || insn->op == SNX_A64_OP_UMINP_v);
        vwrite(g, rd, r, q);
        return 1;
    }
    /* across the lanes: element 0, the rest of the register cleared */
    case SNX_A64_OP_ADDV:
    case SNX_A64_OP_SMAXV:
    case SNX_A64_OP_SMINV:
    case SNX_A64_OP_UMAXV:
    case SNX_A64_OP_UMINV: {
        int n = (q ? 16 : 8) >> size;
        if (size == 3 || (size == 2 && !q)) return -1;
        a = vread(g, rn);
        if (insn->op == SNX_A64_OP_ADDV) r = vreduce(g, size, a, n, red_add, 0, 0);
        else
            r = vreduce(g, size, a, n, (int (*)(SgCtx*, unsigned, int, int, int, int))vmax,
                        insn->op == SNX_A64_OP_UMAXV || insn->op == SNX_A64_OP_UMINV,
                        insn->op == SNX_A64_OP_SMINV || insn->op == SNX_A64_OP_UMINV);
        sg__V_write(g, rd, sgx_xval(128, sgx_xclear(X, r, 8u << size)));
        return 1;
    }
    /* the permutes */
    case SNX_A64_OP_ZIP1:
    case SNX_A64_OP_ZIP2:
    case SNX_A64_OP_UZP1:
    case SNX_A64_OP_UZP2:
    case SNX_A64_OP_TRN1:
    case SNX_A64_OP_TRN2: {
        int kind = insn->op == SNX_A64_OP_ZIP1   ? 0
                   : insn->op == SNX_A64_OP_ZIP2 ? 1
                   : insn->op == SNX_A64_OP_UZP1 ? 2
                   : insn->op == SNX_A64_OP_UZP2 ? 3
                   : insn->op == SNX_A64_OP_TRN1 ? 4
                                                 : 5;
        if (size == 3 && !q) return -1;
        vwrite(g, rd, vzip(g, kind, size, q, vread(g, rn), vread(g, rm)), 1);
        return 1;
    }
    case SNX_A64_OP_EXT_q: {
        int idx[16], i, imm4 = (int)(word >> 11 & 15), len = q ? 16 : 8;
        if (!q && imm4 >= 8) return -1;
        for (i = 0; i < 16; ++i) idx[i] = i < len ? (imm4 + i < len ? imm4 + i : 16 + imm4 + i - len) : -1;
        vwrite(g, rd, vperm(g, vread(g, rn), vread(g, rm), idx), 1);
        return 1;
    }
    case SNX_A64_OP_REV16_v:
    case SNX_A64_OP_REV32_v:
    case SNX_A64_OP_REV64_v: {
        int idx[16], i, cb = insn->op == SNX_A64_OP_REV16_v ? 2 : insn->op == SNX_A64_OP_REV32_v ? 4 : 8;
        int eb = 1 << size, len = q ? 16 : 8;
        if (eb >= cb) return -1;
        for (i = 0; i < 16; ++i) {
            int c = i / cb * cb, e = (i - c) / eb, k = (i - c) % eb;
            idx[i] = i < len ? c + (cb / eb - 1 - e) * eb + k : -1;
        }
        vwrite(g, rd, vperm(g, vread(g, rn), 0, idx), 1);
        return 1;
    }
    case SNX_A64_OP_CNT_v: { /* each byte's ones: a table of the nibbles' (PSHUFB) */
        int lo, hi, t;
        if (size) return -1;
        a = vread(g, rn);
        t = kconst(g, 0x0302020102010100ull, 0x0403030203020201ull);
        lo = vand(g, a, vsplat(g, 0, 15));
        hi = vand(g, opi(g, &sgx_v_psrlw_i, a, 4), vsplat(g, 0, 15));
        vwrite(g, rd, vadd(g, 0, op2(g, &sgx_v_pshufb, t, lo), op2(g, &sgx_v_pshufb, t, hi)), q);
        return 1;
    }
    case SNX_A64_OP_TBL_TBX: {
        /* each table register's bytes where the index (less 16 a register) is below 16: PADDUSB of 0x70 makes the
           others' top bit set, which PSHUFB gives 0 for. TBX keeps the destination's byte where the index is past
           every table */
        unsigned len = (word >> 13 & 3) + 1, tbx = word >> 12 & 1, j;
        int idx = vread(g, rm), acc = 0;
        for (j = 0; j < len; ++j) {
            int t = vread(g, (rn + j) & 31), k = j ? vsub(g, 0, idx, vsplat(g, 0, 16 * j)) : idx;
            int s = op2(g, &sgx_v_pshufb, t, op2(g, &sgx_v_paddusb, k, vsplat(g, 0, 0x70)));
            acc = acc ? vor(g, acc, s) : s;
        }
        if (tbx) { /* the index below 16 * len: the table's byte, else the destination's */
            int in = veq(g, 0, op2(g, &sgx_v_pminub, idx, vsplat(g, 0, 16 * len - 1)), idx);
            acc = vsel(g, in, acc, vread(g, rd));
        }
        vwrite(g, rd, acc, q);
        return 1;
    }
    /* shifts by an immediate */
    case SNX_A64_OP_SHL_v:
    case SNX_A64_OP_USHR_v:
    case SNX_A64_OP_SSHR_v:
    case SNX_A64_OP_USRA_v:
    case SNX_A64_OP_SSRA_v:
    case SNX_A64_OP_SHL_s:
    case SNX_A64_OP_USHR_s:
    case SNX_A64_OP_SSHR_s: {
        unsigned sz, left, right;
        int scalar = insn->op == SNX_A64_OP_SHL_s || insn->op == SNX_A64_OP_USHR_s || insn->op == SNX_A64_OP_SSHR_s;
        if (shift_imm(word, &sz, &left, &right) < 0 || (sz == 3 && !q && !scalar) || (scalar && sz != 3)) return -1;
        if (scalar) q = 0;
        a = vread(g, rn);
        switch (insn->op) {
        case SNX_A64_OP_SHL_v:
        case SNX_A64_OP_SHL_s: r = vshl(g, sz, a, left); break;
        case SNX_A64_OP_USHR_v:
        case SNX_A64_OP_USHR_s: r = vshr(g, sz, a, right, 0); break;
        case SNX_A64_OP_SSHR_v:
        case SNX_A64_OP_SSHR_s: r = vshr(g, sz, a, right, 1); break;
        case SNX_A64_OP_USRA_v: r = vadd(g, sz, vread(g, rd), vshr(g, sz, a, right, 0)); break;
        default: r = vadd(g, sz, vread(g, rd), vshr(g, sz, a, right, 1)); break;
        }
        vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SHRN_v: { /* the low halves of the elements shifted right */
        unsigned sz, left, right;
        if (shift_imm(word, &sz, &left, &right) < 0 || sz == 3) return -1;
        /* immh names the destination's elements; the source's are twice them, the shift up to their bits */
        vwrite_narrow(g, rd, vnarrow(g, sz, vshr(g, sz + 1, vread(g, rn), right, 0), 0), q);
        return 1;
    }
    case SNX_A64_OP_XTN:
        if (size == 3) return -1;
        vwrite_narrow(g, rd, vnarrow(g, size, vread(g, rn), 0), q);
        return 1;
    case SNX_A64_OP_SSHLL_v:
    case SNX_A64_OP_USHLL_v: {
        unsigned sz, left, right;
        if (shift_imm(word, &sz, &left, &right) < 0 || sz == 3) return -1;
        r = vwiden(g, sz, vread(g, rn), (int)q, insn->op == SNX_A64_OP_SSHLL_v);
        vwrite(g, rd, vshl(g, sz + 1, r, left), 1);
        return 1;
    }
    case SNX_A64_OP_SHLL_v:
        if (size == 3) return -1;
        vwrite(g, rd, vshl(g, size + 1, vwiden(g, size, vread(g, rn), (int)q, 0), 8u << size), 1);
        return 1;
    /* shifts by a register's lanes */
    case SNX_A64_OP_USHL_v:
    case SNX_A64_OP_SSHL_v:
        if (size == 3 && !q) return -1;
        vwrite(g, rd, vshl_reg(g, size, (int)q, vread(g, rn), vread(g, rm), insn->op == SNX_A64_OP_SSHL_v), q);
        return 1;
    /* widening */
    case SNX_A64_OP_UADDW:
    case SNX_A64_OP_SADDW:
        if (size == 3) return -1;
        vwrite(g, rd, vadd(g, size + 1, vread(g, rn), vwiden(g, size, vread(g, rm), (int)q, !uns)), 1);
        return 1;
    case SNX_A64_OP_UMULL_v:
    case SNX_A64_OP_SMULL_v:
    case SNX_A64_OP_UMLAL_v:
    case SNX_A64_OP_SMLAL_v:
    case SNX_A64_OP_UMLSL_v:
    case SNX_A64_OP_SMLSL_v:
    case SNX_A64_OP_UMULL_vi:
    case SNX_A64_OP_SMULL_vi:
    case SNX_A64_OP_UMLAL_vi:
    case SNX_A64_OP_SMLAL_vi:
    case SNX_A64_OP_UMLSL_vi:
    case SNX_A64_OP_SMLSL_vi: {
        int byelem = insn->op == SNX_A64_OP_UMULL_vi || insn->op == SNX_A64_OP_SMULL_vi ||
                     insn->op == SNX_A64_OP_UMLAL_vi || insn->op == SNX_A64_OP_SMLAL_vi ||
                     insn->op == SNX_A64_OP_UMLSL_vi || insn->op == SNX_A64_OP_SMLSL_vi;
        int acc = insn->op == SNX_A64_OP_UMLAL_v || insn->op == SNX_A64_OP_SMLAL_v ||
                  insn->op == SNX_A64_OP_UMLAL_vi || insn->op == SNX_A64_OP_SMLAL_vi;
        int dec = insn->op == SNX_A64_OP_UMLSL_v || insn->op == SNX_A64_OP_SMLSL_v ||
                  insn->op == SNX_A64_OP_UMLSL_vi || insn->op == SNX_A64_OP_SMLSL_vi;
        int sign = !uns, an, bm, p;
        if (size == 3 || (byelem && size == 0)) return -1;
        an = vwiden(g, size, vread(g, rn), (int)q, sign);
        if (byelem) {
            unsigned h = word >> 11 & 1, l = word >> 21 & 1, m4 = word >> 20 & 1;
            int e = size == 1 ? (int)(h << 2 | l << 1 | m4) : (int)(h << 1 | l);
            if (size == 1) rm &= 15;
            bm = vwiden(g, size, vdup(g, size, vread(g, rm), e), 0, sign);
        } else
            bm = vwiden(g, size, vread(g, rm), (int)q, sign);
        if (size == 2) p = op2(g, sign ? &sgx_v_pmuldq : &sgx_v_pmuludq, an, bm);
        else
            p = vmul(g, size + 1, an, bm);
        if (acc) p = vadd(g, size + 1, vread(g, rd), p);
        else if (dec)
            p = vsub(g, size + 1, vread(g, rd), p);
        vwrite(g, rd, p, 1);
        return 1;
    }
    case SNX_A64_OP_ADDHN:
    case SNX_A64_OP_SUBHN:
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        r = insn->op == SNX_A64_OP_ADDHN ? vadd(g, size + 1, a, b) : vsub(g, size + 1, a, b);
        vwrite_narrow(g, rd, vnarrow(g, size, r, 1), q);
        return 1;
    /* halving */
    case SNX_A64_OP_UHADD_v:
    case SNX_A64_OP_SHADD_v:
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        /* (a & b) + ((a ^ b) >> 1) */
        vwrite(g, rd, vadd(g, size, vand(g, a, b), vshr(g, size, vxor(g, a, b), 1, insn->op == SNX_A64_OP_SHADD_v)), q);
        return 1;
    case SNX_A64_OP_URHADD_v:
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        if (size == 0) r = op2(g, &sgx_v_pavgb, a, b);
        else if (size == 1)
            r = op2(g, &sgx_v_pavgw, a, b);
        else /* (a | b) - ((a ^ b) >> 1) */
            r = vsub(g, size, vor(g, a, b), vshr(g, size, vxor(g, a, b), 1, 0));
        vwrite(g, rd, r, q);
        return 1;
    default: return 0;
    }
}

#undef X
