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
/* ---- each byte's bits moved (GFNI) ----
   GF2P8AFFINEQB: bit i of each byte of a its own bit i + n (VGF_ARITH, past 7 the sign's), bit 7 - i (VGF_REVERSE),
   or bit 0 bit n (VGF_BIT); a bit from past the byte 0. The matrix' row of bit i, in its byte 7 - i, is the mask of the
   bits it adds. Where SSE takes a shift and a mask (a logical shift) it is that: GF2P8AFFINEQB's latency is longer */
enum { VGF_ARITH, VGF_REVERSE, VGF_BIT }; /* VGF_BIT: bit n alone, as bit 0 */
static int vgfni(const SgCtx* g) { return (g->x.features & SGX_F_GFNI) != 0; }
static int vgf(SgCtx* g, int a, int kind, unsigned n) {
    SnxU64 rows = 0;
    for (int i = 0; i < 8; ++i) {
        int from = kind == VGF_REVERSE ? 7 - i : kind == VGF_BIT ? (i ? 8 : (int)n) : i + (int)n;
        if (kind == VGF_ARITH && from > 7) from = 7;
        if (from >= 0 && from < 8) rows |= (SnxU64)1 << (8 * (7 - i) + from);
    }
    int m = kconst(g, rows, rows);
    if (X->features & SGX_F_AVX) {
        int in[4] = {0, a, m, 0};
        return sgx_vins(X, &sgx_v_vgf2p8affineqb, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    return op2(g, &sgx_v_gf2p8affineqb, a, m);
}
/* RBIT of a W or an X: each byte's bits reversed there, then the bytes */
static SgD mg_rbit(Unit* u, SgD v) {
    SgCtx* g = u->g;
    if (!vgfni(g)) return sg_d_bitreverse(g, v);
    unsigned w = v.w;
    int in[4] = {0, sgx_reg(X, &v), 0, 0};
    int r = sgx_vins(X, w == 64 ? &sgx_v_movq_xr : &sgx_v_movd_xr, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    v = sgx_val(w, sgx_v1(X, w == 64 ? &sgx_v_movq_rx : &sgx_v_movd_rx, vgf(g, r, VGF_REVERSE, 0), 0));
    return SGX_OP(X, rev, w, v);
}
/* a << n, n below the element's bits (the bytes by words, their spill masked) */
static int vshl(SgCtx* g, unsigned size, int a, unsigned n) {
    if (n >= 8u << size) return vzero(g);
    if (!n) return a;
    if (!size) return vand(g, opi(g, &sgx_v_psllw_i, a, n), vsplat(g, 0, 0xFFu << n));
    return opi(g, k_sll[size], a, n);
}
/* each lane's sign spread over it */
static int vsign(SgCtx* g, unsigned size, int a) {
    if (k_sra[size]) return opi(g, k_sra[size], a, (8u << size) - 1);
    return vgt(g, size, vzero(g), a, 0);
}
/* a >> n, logical (arith: arithmetic), n up to the element's bits */
static int vshr(SgCtx* g, unsigned size, int a, unsigned n, int arith) {
    unsigned e = 8u << size;
    if (arith) {
        if (n >= e) n = e - 1;
        if (!n) return a;
        if (n == e - 1) return vsign(g, size, a);
        if (!size && vgfni(g)) return vgf(g, a, VGF_ARITH, n);
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
static int vpshufb(SgCtx* g, int a, int b, const int* idx) {
    SnxU64 ma[2] = {0, 0}, mb[2] = {0, 0};
    int any_a = 0, any_b = 0, ra = 0, rb = 0;
    for (int i = 0; i < 16; ++i) {
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
/* whether a form's bytes m (its first operand's byte k as k, its second's as 16 + k, 0 as -1), its operands the
   sources at x and y (a 0, b 16, as idx numbers them), are idx's first len bytes */
static int vshuf_is(const int* idx, int len, const int* m, int x, int y) {
    for (int i = 0; i < len; ++i) {
        int want = idx[i] >= 0 && idx[i] < 32 ? idx[i] : -1;
        int got = m[i] < 0 ? -1 : m[i] < 16 ? x + m[i] : y + m[i] - 16;
        if (want != got) return 0;
    }
    return 1;
}
/* PSHUFD's immediate for idx's first len bytes, each doubleword one of the source's at s (a 0, b 16) whole; -1 for
   none */
static int vshuf_pshufd(const int* idx, int len, int s) {
    int imm = 0, m[16];
    for (int d = 0; d < 4; ++d) {
        int e = d;
        if (4 * d < len) {
            if (idx[4 * d] < s || idx[4 * d] >= s + 16 || (idx[4 * d] - s) % 4) return -1;
            e = (idx[4 * d] - s) / 4;
        }
        imm |= e << (2 * d);
        for (int k = 0; k < 4; ++k) m[4 * d + k] = 4 * e + k;
    }
    return vshuf_is(idx, len, m, s, s) ? imm : -1;
}
/* PBLENDW's immediate for idx's first len bytes, each word the first operand's (at x) or the second's (at y) in its
   place, the second's bytes my (its source's k, or -1: a shifted source); -1 for none */
static int vshuf_blend(const int* idx, int len, int x, int y, const int* my) {
    int imm = 0, m[16];
    for (int w = 0; w < 8; ++w) {
        int from_y = 0;
        if (2 * w < len) {
            int want = idx[2 * w] >= 0 && idx[2 * w] < 32 ? idx[2 * w] : -1;
            from_y = want != x + 2 * w;
        }
        imm |= from_y << w;
        for (int k = 2 * w; k < 2 * w + 2; ++k) m[k] = from_y ? (my[k] < 0 ? -1 : 16 + my[k]) : k;
    }
    return vshuf_is(idx, len, m, x, y) ? imm : -1;
}
/* idx's first len bytes (the rest any) in one host instruction where one has them: PSHUFD, a byte shift, an unpack,
   PALIGNR, SHUFPS, PBLENDW; in two: PBLENDW of one operand shifted, or the low doublewords of a and b
   (PUNPCKLQDQ) under one of the one-source forms or PSHUFB; else PSHUFB of each, ORed. b 0: a alone */
static int vshuf(SgCtx* g, int a, int b, const int* idx, int len) {
    static const SgXVForm* const k_lo[4] = {&sgx_v_punpcklbw, &sgx_v_punpcklwd, &sgx_v_punpckldq, &sgx_v_punpcklqdq};
    static const SgXVForm* const k_hi[4] = {&sgx_v_punpckhbw, &sgx_v_punpckhwd, &sgx_v_punpckhdq, &sgx_v_punpckhqdq};
    int m[16], src[2] = {a, b}, full[16];
    for (int s = 0; s < (b ? 2 : 1); ++s) {
        int imm = vshuf_pshufd(idx, len, 16 * s);
        if (imm >= 0) return imm == 0xE4 ? src[s] : sgx_v1(X, &sgx_v_pshufd, src[s], imm);
        for (int n = 1; n < 16; ++n) {
            for (int i = 0; i < 16; ++i) m[i] = i + n < 16 ? i + n : -1;
            if (vshuf_is(idx, len, m, 16 * s, 16 * s)) return opi(g, &sgx_v_psrldq, src[s], n);
            for (int i = 0; i < 16; ++i) m[i] = i >= n ? i - n : -1;
            if (vshuf_is(idx, len, m, 16 * s, 16 * s)) return opi(g, &sgx_v_pslldq, src[s], n);
        }
    }
    for (int o = 0; b && o < 2; ++o) {
        int x = o ? b : a, y = o ? a : b, xs = o ? 16 : 0, ys = o ? 0 : 16, imm, my[16];
        for (int e = 0; e < 4; ++e) {
            int eb = 1 << e;
            for (int half = 0; half < 2; ++half) {
                for (int i = 0; i < 16; ++i) m[i] = (i / eb & 1 ? 16 : 0) + 8 * half + i / (2 * eb) * eb + i % eb;
                if (vshuf_is(idx, len, m, xs, ys)) return op2(g, (half ? k_hi : k_lo)[e], x, y);
            }
        }
        for (int n = 1; n < 16; ++n) { /* PALIGNR x, y: y's bytes from n, then x's */
            for (int i = 0; i < 16; ++i) m[i] = i + n < 16 ? 16 + i + n : i + n - 16;
            if (vshuf_is(idx, len, m, xs, ys)) return sgx_v2(X, &sgx_v_palignr, x, y, n);
        }
        imm = 0; /* SHUFPS x, y: two of x's doublewords, then two of y's */
        for (int d = 0; d < 4; ++d) {
            int want = 4 * d < len ? idx[4 * d] - (d < 2 ? xs : ys) : d;
            int e = 4 * d < len ? want / 4 : d % 4;
            if (want < 0 || want >= 16) e = -1;
            if (e < 0) {
                imm = -1;
                break;
            }
            imm |= e << (2 * d);
        }
        if (imm >= 0) {
            for (int i = 0; i < 16; ++i) m[i] = (i < 8 ? 0 : 16) + 4 * (imm >> (2 * (i / 4)) & 3) + i % 4;
            if (vshuf_is(idx, len, m, xs, ys)) return sgx_v2(X, &sgx_v_shufps, x, y, imm);
        }
        for (int i = 0; i < 16; ++i) my[i] = i;
        if ((imm = vshuf_blend(idx, len, xs, ys, my)) >= 0) return sgx_v2(X, &sgx_v_pblendw, x, y, imm);
        for (int n = 1; n < 16; ++n) { /* PBLENDW x, y shifted by n bytes up (and below, down) */
            for (int i = 0; i < 16; ++i) my[i] = i >= n ? i - n : -1;
            if ((imm = vshuf_blend(idx, len, xs, ys, my)) >= 0)
                return sgx_v2(X, &sgx_v_pblendw, x, opi(g, &sgx_v_pslldq, y, n), imm);
            for (int i = 0; i < 16; ++i) my[i] = i + n < 16 ? i + n : -1;
            if ((imm = vshuf_blend(idx, len, xs, ys, my)) >= 0)
                return sgx_v2(X, &sgx_v_pblendw, x, opi(g, &sgx_v_psrldq, y, n), imm);
        }
    }
    for (int i = 0; i < 16; ++i) full[i] = i < len && idx[i] >= 0 && idx[i] < 32 ? idx[i] : -1;
    if (b && len <= 8) { /* a's and b's low doublewords in one register, then that one's shuffle */
        int lo = 1, of_a = 0, of_b = 0, one[16];
        for (int i = 0; i < 16; ++i) {
            one[i] = full[i] < 0 ? -1 : full[i] < 16 ? full[i] : full[i] - 8;
            if (full[i] >= 0 && full[i] % 16 >= 8) lo = 0;
            of_a |= full[i] >= 0 && full[i] < 16;
            of_b |= full[i] >= 16;
        }
        if (lo && of_a && of_b) return vshuf(g, op2(g, &sgx_v_punpcklqdq, a, b), 0, one, len);
    }
    return vpshufb(g, a, b, full);
}
/* r's byte i is a's idx[i] (below 16), b's idx[i] - 16 (16 to 31), or 0 (any other) */
static int vperm(SgCtx* g, int a, int b, const int* idx) { return vshuf(g, a, b, idx, 16); }
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
    return vshuf(g, a, b, idx, n * eb);
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

static int shift_imm(SnxU32 word, unsigned* size, unsigned* left, unsigned* right);
static int vabs(SgCtx* g, unsigned size, int a);

/* ---- floating point by lanes ----
   The host's lanes where the LLVM back end's guards (ir_vec_fp.c's fast paths) hold for every lane that is the
   instruction's; else all of them on the cold path through the exact helpers (x64_helpers.c's snx_x64_fp_vec, desc
   as it reads it). sz: 0 singles, 1 doubles; n the lanes */

static const SgXVForm* const k_fadd[2] = {&sgx_v_addps, &sgx_v_addpd};
static const SgXVForm* const k_fsub[2] = {&sgx_v_subps, &sgx_v_subpd};
static const SgXVForm* const k_fmul[2] = {&sgx_v_mulps, &sgx_v_mulpd};
static const SgXVForm* const k_fdiv[2] = {&sgx_v_divps, &sgx_v_divpd};
static const SgXVForm* const k_fmax[2] = {&sgx_v_maxps, &sgx_v_maxpd};
static const SgXVForm* const k_fmin[2] = {&sgx_v_minps, &sgx_v_minpd};
static const SgXVForm* const k_fsqrt[2] = {&sgx_v_sqrtps, &sgx_v_sqrtpd};
static const SgXVForm* const k_fcmp[2] = {&sgx_v_cmpps, &sgx_v_cmppd};
static const SgXVForm* const k_fmask[2] = {&sgx_v_movmskps, &sgx_v_movmskpd};

/* any of the first n lanes of mask m (whole lanes of 4 << sz bytes) set */
static SgB vany(SgCtx* g, int m, unsigned sz, int n) {
    SgD bits = sgx_val(32, sgx_v1(X, k_fmask[sz], m, 0));
    return sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, bits, sgx_const(32, (1u << n) - 1))));
}
/* a lane's floating-point constant */
static int fsplat(SgCtx* g, unsigned sz, SnxU64 bits) { return vsplat(g, sz + 2, bits); }
/* the lanes that are not normal numbers above the smallest: zero, tiny, infinite, NaN (the doubled magnitude, minus
   that of the smallest normal and one, not below the range to infinity's, unsigned: a signed compare of them with
   their tops flipped) */
static int fnot_roomy(SgCtx* g, unsigned sz, int r) {
    unsigned size = sz + 2;
    SnxU64 normal = sz ? 0x0010000000000000ull : 0x00800000u, inf = sz ? 0x7FF0000000000000ull : 0x7F800000u;
    SnxU64 flip = 1ull << ((8u << size) - 1);
    int m2 = vadd(g, size, r, r), t = vsub(g, size, m2, vsplat(g, size, (normal + 1) << 1));
    return vgt(g, size, vxor(g, t, vsplat(g, size, flip)), vsplat(g, size, (((inf - normal - 1) << 1) - 1) ^ flip), 0);
}
static int fzero(SgCtx* g, unsigned sz, int a) { return veq(g, sz + 2, vadd(g, sz + 2, a, a), vzero(g)); }
/* the denormal lanes (the doubled magnitude less 2 below the doubled smallest normal less 2, unsigned: a signed
   compare with their tops flipped) */
static int fdenormal(SgCtx* g, unsigned sz, int a) {
    unsigned size = sz + 2;
    SnxU64 normal = sz ? 0x0010000000000000ull : 0x00800000u, flip = 1ull << ((8u << size) - 1);
    int t = vsub(g, size, vadd(g, size, a, a), vsplat(g, size, 2));
    return vgt(g, size, vsplat(g, size, ((normal << 1) - 2) ^ flip), vxor(g, t, vsplat(g, size, flip)), 0);
}
static int fnan(SgCtx* g, unsigned sz, int a) { return sgx_v2(X, k_fcmp[sz], a, a, 3); /* CMPUNORD */ }
static SgB fstate(SgCtx* g, SnxU32 mask, int ixc) { return sgx_fp_bad(g, sg_FPCR_read(g), mask, ixc); }
/* FPCR.FZ set and a denormal in the first n lanes of a, b or c (0: none), which it flushes, raising IDC: the helpers'
   case (ir_vec_fp.c's unflushed) */
static SgB fflushed(SgCtx* g, unsigned sz, int n, int a, int b, int c) {
    int m = fdenormal(g, sz, a);
    if (b) m = vor(g, m, fdenormal(g, sz, b));
    if (c) m = vor(g, m, fdenormal(g, sz, c));
    return sg_b_and(g, fstate(g, SGX_FPCR_FZ, 0), vany(g, m, sz, n));
}
static int fcold(SgCtx* g, SgB bad, int fast, int a, int b, int d, SnxU32 desc) {
    return sgx_vcold(X, bad, fast, "snx_x64_fp_vec", a, b, d, desc);
}
#define FDESC(kind, sz, n) ((SnxU32)(kind) | (SnxU32)(sz ? 8 : 4) << 4 | (SnxU32)(n) << 8)
/* the LLVM back end's guards in two steps: the hot code tests FPCR's mode and FZ and FPSR's IXC (once a region) and
   that every lane of r is a normal number (nr: fnot_roomy's); the cold path tests the rest before its call
   (x64_asm.h's sgx_cold_vwarm: zacc's bits), back to r where it holds: exact zeros, FZ with no denormal argument */
static int fwarm(SgCtx* g, int r, int nr, unsigned sz, int n, int a, int b, int d, SnxU32 desc, unsigned zacc) {
    /* one test of nr's lane bits ORed with the state's bits, once a region: no condition made of each */
    SgD lanes = sgx_val(32, sgx_v1(X, k_fmask[sz], nr, 0));
    if (!g->fp_any_bits) {
        SgD bits = sgx_fp_bad_bits(g, sg_FPCR_read(g), SGX_FPCR_RMODE | SGX_FPCR_FZ, 1);
        g->fp_any_bits = sgx_reg(X, &bits);
    }
    if (n < (sz ? 2 : 4)) lanes = sg_d_and(g, lanes, sgx_const(32, (1u << n) - 1));
    SgB hot = sg_b_not(g, sg_d_is_zero(g, sg_d_or(g, lanes, sgx_val(32, g->fp_any_bits))));
    return sgx_vcold_warm(X, hot, r, "snx_x64_fp_vec", a, b, d, desc, 0x80 | 0x20 | zacc, (int)offsetof(CPU, fpcr),
                          (int)offsetof(CPU, fpsr), 0);
}
/* the same where the lanes of bad (a NaN, an integer out of range) are the call's: the hot code tests FZ (and where
   ixc, an operation that may be inexact, IXC; the mode with them, as the region has that test already) once a region
   and bad's lanes; the cold path tests bad again, IXC, and the arguments (a, b where not 0) FZ may flush */
static int fwarm_mask(SgCtx* g, int r, int bad, unsigned sz, int n, int a, int b, SnxU32 desc, int ixc) {
    SgB hot =
        sg_b_or(g, ixc ? fstate(g, SGX_FPCR_RMODE | SGX_FPCR_FZ, 1) : fstate(g, SGX_FPCR_FZ, 0), vany(g, bad, sz, n));
    return sgx_vcold_warm(X, hot, r, "snx_x64_fp_vec", a, b, 0, desc,
                          0x80 | 0x40 | (ixc ? 0x20u : 0) | (b ? 2u : 1u) << 3, (int)offsetof(CPU, fpcr),
                          (int)offsetof(CPU, fpsr), bad);
}

/* FADD, FSUB, FMUL, FDIV by lanes; FABD the difference's magnitude */
static int fbinary(SgCtx* g, unsigned op, unsigned sz, int n, int a, int b) {
    const SgXVForm* f = op == SNX_FP_ADD                       ? k_fadd[sz]
                        : op == SNX_FP_MUL || op == SNX_FP_MULX ? k_fmul[sz]
                        : op == SNX_FP_DIV                      ? k_fdiv[sz]
                                                                : k_fsub[sz];
    int r = op2(g, f, a, b);
    /* exact zeros: every one of a sum, a product's of a zero (FMULX's infinity by a zero is the host's NaN: cold), a
       quotient's of a zero; two arguments FZ may flush */
    unsigned zacc = op == SNX_FP_ADD || op == SNX_FP_SUB || op == SNX_FP_ABD ? 1
                    : op == SNX_FP_MUL || op == SNX_FP_MULX                  ? 2
                                                                             : 3;
    r = fwarm(g, r, fnot_roomy(g, sz, r), sz, n, a, b, 0, FDESC(0, sz, n) | (op == SNX_FP_ABD ? SNX_FP_SUB : op) << 12,
              zacc | 2u << 3);
    if (op == SNX_FP_ABD) r = vandn(g, fsplat(g, sz, 1ull << (sz ? 63 : 31)), r);
    return r;
}
/* d + n * m rounded once (sub: d - n * m, n negated first): VFMADD231 where the host has FMA */
static int ffma(SgCtx* g, unsigned sz, int n_, int d, int a, int b, int sub) {
    if (sub) a = vxor(g, a, fsplat(g, sz, 1ull << (sz ? 63 : 31)));
    if (!(X->features & SGX_F_FMA)) return fcold(g, sg_bool(g, 1), vzero(g), d, a, b, FDESC(1, sz, n_));
    int in[4] = {d, a, b, 0};
    int r = sgx_vins(X, sz ? &sgx_v_vfmadd231pd : &sgx_v_vfmadd231ps, 0, 1, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    /* exact zeros where a or b (arguments 2 and 3) is one; d, a and b FZ may flush */
    return fwarm(g, r, fnot_roomy(g, sz, r), sz, n_, d, a, b, FDESC(1, sz, n_), 2 | 4 | 3u << 3);
}
/* FMAX, FMIN (numeric: FMAXNM, FMINNM): MAXPx's lanes, equal ones ANDed (ORed for the minimum); a NaN goes cold */
static int fminmax(SgCtx* g, unsigned sz, int n, int a, int b, int minimum, int numeric) {
    int eq = sgx_v2(X, k_fcmp[sz], a, b, 0), t = minimum ? vor(g, a, b) : vand(g, a, b);
    int r = op2(g, minimum ? k_fmin[sz] : k_fmax[sz], a, b);
    r = vsel(g, eq, t, r);
    return fwarm_mask(g, r, sgx_v2(X, k_fcmp[sz], a, b, 3), sz, n, a, b,
                      FDESC(3, sz, n) | (SnxU32)minimum << 12 | (SnxU32)numeric << 16, 0);
}
static int fsqrt(SgCtx* g, unsigned sz, int n, int a) {
    int r = sgx_v1(X, k_fsqrt[sz], a, 0);
    /* exact zeros where a is one; a FZ may flush */
    return fwarm(g, r, fnot_roomy(g, sz, r), sz, n, a, 0, 0, FDESC(2, sz, n) | SNX_FP_SQRT << 12, 3 | 1u << 3);
}
/* FCMEQ (cmp 0), FCMGE (1), FCMGT (2) of a and b (abs: FACGE, FACGT): CMPPx's EQ, LE and LT the other way; an
   unordered lane goes cold (a signalling NaN, or any NaN for GE and GT, raises IOC) */
static int fcompare(SgCtx* g, unsigned sz, int n, int a, int b, int cmp, int abs) {
    int r;
    if (abs) {
        int m = fsplat(g, sz, sz ? ~(1ull << 63) : 0x7FFFFFFFu);
        a = vand(g, a, m);
        b = vand(g, b, m);
    }
    r = cmp == 0 ? sgx_v2(X, k_fcmp[sz], a, b, 0) : sgx_v2(X, k_fcmp[sz], b, a, cmp == 1 ? 2 : 1);
    return fwarm_mask(g, r, sgx_v2(X, k_fcmp[sz], a, b, 3), sz, n, a, b, FDESC(4, sz, n) | (SnxU32)cmp << 12, 0);
}
static int fround_to(SgCtx* g, unsigned sz, int a, int rm);
/* FCVTZS, FCVTZU and the other modes' (rm as fround_to's, 3 toward zero), to integers of the lanes' width, of a scaled
   by 2^fbits: rounded to an integer in the mode, then CVTTPx's */
static int fto_int(SgCtx* g, unsigned sz, int n, int a, int uns, int fbits, int rm) {
    int x = a, r, bad;
    if (fbits) x = op2(g, k_fmul[sz], x, fsplat(g, sz, sz ? (SnxU64)(1023 + fbits) << 52 : (SnxU64)(127 + fbits) << 23));
    if (rm != 3) x = fround_to(g, sz, x, rm);
    if (!sz) {
        int lo = sgx_v1(X, &sgx_v_cvttps2dq, x, 0);
        if (!uns) {
            r = lo;
            bad = veq(g, 2, lo, vsplat(g, 2, 0x80000000u)); /* out of range or a NaN */
        } else {
            int big = sgx_v2(X, &sgx_v_cmpps, fsplat(g, 0, 0x4F000000u), x, 2); /* 2^31 <= x */
            int hi = vxor(g, sgx_v1(X, &sgx_v_cvttps2dq, op2(g, &sgx_v_subps, x, fsplat(g, 0, 0x4F000000u)), 0),
                          vsplat(g, 2, 0x80000000u));
            r = vsel(g, big, hi, lo);
            bad = vor(g, sgx_v2(X, &sgx_v_cmpps, x, fsplat(g, 0, 0xBF800000u), 2),  /* x <= -1 */
                      sgx_v2(X, &sgx_v_cmpps, fsplat(g, 0, 0x4F800000u), x, 2)); /* 2^32 <= x */
            bad = vor(g, bad, fnan(g, 0, x));
        }
    } else {
        /* the two doubles one at a time (CVTTSD2SI), unsigned ones from 2^63 up cold */
        SgD l0 = sgx_val(64, sgx_v1(X, &sgx_v_cvttsd2si_64, x, 0));
        SgD l1 = sgx_val(64, sgx_v1(X, &sgx_v_cvttsd2si_64, sgx_v1(X, &sgx_v_pshufd, x, 0xEE), 0));
        r = lane_put(g, 3, lane_put(g, 3, vzero(g), 0, l0), 1, l1);
        bad = veq(g, 3, r, vsplat(g, 3, 1ull << 63));
        if (uns) bad = vor(g, bad, vor(g, sgx_v2(X, &sgx_v_cmppd, x, fsplat(g, 1, 0xBFF0000000000000ull), 2),
                                       vgt(g, 3, vzero(g), r, 0)));
    }
    return fwarm_mask(g, r, bad, sz, n, a, 0,
                      FDESC(6, sz, n) | (SnxU32)uns << 12 | (SnxU32)fbits << 16 | (SnxU32)(rm + 1) << 24, 1);
}
/* SCVTF, UCVTF (from integers of the lanes' width), scaled by 2^-fbits: exact where every integer fits the
   significand, else once IXC is set in round to nearest. Unsigned words as their halves (each exact, one rounding
   of the sum) */
static int ffrom_int(SgCtx* g, unsigned sz, int n, int a, int uns, int fbits) {
    int r, fit;
    SnxU64 p = sz ? 53 : 24;
    if (!sz) {
        if (!uns) r = sgx_v1(X, &sgx_v_cvtdq2ps, a, 0);
        else {
            int hi = sgx_v1(X, &sgx_v_cvtdq2ps, vshr(g, 2, a, 16, 0), 0), lo = sgx_v1(X, &sgx_v_cvtdq2ps, vand(g, a, vsplat(g, 2, 0xFFFF)), 0);
            r = op2(g, &sgx_v_addps, op2(g, &sgx_v_mulps, hi, fsplat(g, 0, 0x47800000u)), lo);
        }
    } else {
        SgD l0 = lane_get(g, 3, a, 0), l1 = lane_get(g, 3, a, 1);
        int in[4] = {0, 0, 0, 0}, x0, x1;
        in[1] = sgx_reg(X, &l0);
        x0 = sgx_vins(X, &sgx_v_cvtsi2sd_64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        in[1] = sgx_reg(X, &l1);
        x1 = sgx_vins(X, &sgx_v_cvtsi2sd_64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        r = op2(g, &sgx_v_punpcklqdq, x0, x1);
    }
    if (fbits) r = op2(g, k_fmul[sz], r, fsplat(g, sz, sz ? (SnxU64)(1023 - fbits) << 52 : (SnxU64)(127 - fbits) << 23));
    /* fits: |v| < 2^p (signed: v + 2^p below 2^(p+1), unsigned: v below 2^p) */
    {
        unsigned size = sz + 2;
        SnxU64 flip = 1ull << ((8u << size) - 1);
        int v = uns ? a : vadd(g, size, a, vsplat(g, size, 1ull << p));
        SnxU64 lim = uns ? (1ull << p) : (1ull << (p + 1));
        fit = vgt(g, size, vsplat(g, size, lim ^ flip), vxor(g, v, vsplat(g, size, flip)), 0);
    }
    {
        SgB bad = sg_b_and(g, fstate(g, SGX_FPCR_RMODE, 1), vany(g, vnot(g, fit), sz, n));
        /* an unsigned doubleword from 2^63 up, which CVTSI2SD takes as negative */
        if (sz && uns) bad = sg_b_or(g, bad, vany(g, vgt(g, 3, vzero(g), a, 0), 1, n));
        return fcold(g, bad, r, a, 0, 0, FDESC(7, sz, n) | (SnxU32)uns << 12 | (SnxU32)fbits << 16);
    }
}

/* FRECPS (2 - a b) and FRSQRTS ((3 - a b) / 2) rounded once: the host's FMA, an infinity by a zero and the rest the FMA
   would get wrong (a NaN, a result not normal) going cold through the exact helper, whose special cases are its own */
static int fstep(SgCtx* g, unsigned sz, int n, int a, int b, int sqrt) {
    int t, r, op = sqrt ? SNX_FP_RSQRTS : SNX_FP_RECPS;
    if (!(X->features & SGX_F_FMA))
        return fcold(g, sg_bool(g, 1), vzero(g), a, b, 0, FDESC(0, sz, n) | (SnxU32)op << 12);
    {
        int in[4] = {fsplat(g, sz, sz ? (sqrt ? 0x4008000000000000ull : 0x4000000000000000ull)
                                      : sqrt ? 0x40400000u : 0x40000000u),
                     vxor(g, a, fsplat(g, sz, 1ull << (sz ? 63 : 31))), b, 0};
        t = sgx_vins(X, sz ? &sgx_v_vfmadd231pd : &sgx_v_vfmadd231ps, 0, 1, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    /* halving a result whose half is normal is exact */
    r = sqrt ? op2(g, k_fmul[sz], t, fsplat(g, sz, sz ? 0x3FE0000000000000ull : 0x3F000000u)) : t;
    /* a zero only where a b is exactly 2 (3), so every one exact; a and b FZ may flush */
    return fwarm(g, r, fnot_roomy(g, sz, r), sz, n, a, b, 0, FDESC(0, sz, n) | (SnxU32)op << 12, 1 | 2u << 3);
}
/* FRECPE, FRSQRTE as ir_vec_fp.c's fast_estimate makes them, where every lane is normal with a normal estimate
   (FRSQRTE's positive): the exponent, and the low 8 bits of (2^19 / a' + 1) / 2 (FRSQRTE's: of the root of 2^28 / a'),
   which a single gives exactly for every a' it can take; another lane goes cold */
static int festimate(SgCtx* g, unsigned sz, int n, int a, int sqrt) {
    unsigned size = sz + 2, fb = sz ? 52 : 23, bias = sz ? 1023 : 127;
    int e = vand(g, vshr(g, size, a, fb, 0), vsplat(g, size, 2 * bias + 1));
    int top = vand(g, vshr(g, size, a, fb - 8, 0), vsplat(g, size, 0xFF)), d, x, est, r, bad;
    if (sqrt) {
        SnxU64 least = sz ? 1ull << 52 : 1ull << 23, inf = (SnxU64)(2 * bias + 1) << fb;
        bad = vor(g, vgt(g, size, vsplat(g, size, least), a, 0), vgt(g, size, a, vsplat(g, size, inf - 1), 0));
        d = vadd(g, size, vand(g, top, vsplat(g, size, 0xFE)), vsplat(g, size, 0x101));
        /* twice, an even e's */
        d = vadd(g, size, d, vand(g, d, veq(g, size, vand(g, e, vsplat(g, size, 1)), vzero(g))));
        x = vshr(g, size, vsub(g, size, vsplat(g, size, 3 * bias - 1), e), 1, 0);
    } else {
        bad = vor(g, veq(g, size, e, vzero(g)), vgt(g, size, e, vsplat(g, size, 2 * bias - 2), 0));
        d = vadd(g, size, vshl(g, size, top, 1), vsplat(g, size, 0x201));
        x = vsub(g, size, vsplat(g, size, 2 * bias - 1), e);
    }
    if (sz) d = vor(g, d, vsplat(g, 3, 1ull << 32)); /* the words between: 1, no division by zero */
    est = op2(g, &sgx_v_divps, fsplat(g, 0, sqrt ? 0x4D800000u : 0x49000000u), sgx_v1(X, &sgx_v_cvtdq2ps, d, 0));
    if (sqrt) est = sgx_v1(X, k_fsqrt[0], est, 0);
    est = vshr(g, 2, vadd(g, 2, sgx_v1(X, &sgx_v_cvttps2dq, est, 0), vsplat(g, 2, 1)), 1, 0);
    r = vor(g, vshl(g, size, x, fb), vshl(g, size, vand(g, est, vsplat(g, size, 0xFF)), fb - 8));
    if (!sqrt) r = vor(g, r, vand(g, a, fsplat(g, sz, 1ull << (sz ? 63 : 31))));
    return fcold(g, vany(g, bad, sz, n), r, a, 0, 0,
                 FDESC(2, sz, n) | (SnxU32)(sqrt ? SNX_FP_RSQRTE : SNX_FP_RECPE) << 12);
}
/* a's lanes rounded to integers in mode rm: 0-3 ROUNDPx's (nearest even, plus, minus, zero), 4 ties away
   (sgx_round_away); exact, without the inexact flag */
static int fround_to(SgCtx* g, unsigned sz, int a, int rm) {
    static const unsigned char imm[4] = {8, 10, 9, 11}; /* no exceptions: nearest, up, down, zero */
    return rm == 4 ? sgx_round_away(X, a, sz ? 64 : 32) : sgx_v1(X, sz ? &sgx_v_roundpd : &sgx_v_roundps, a, imm[rm]);
}
/* FRINTN, FRINTP, FRINTM, FRINTZ, FRINTA (rm as fround_to's); a NaN and a value FZ flushes go cold */
static int fround(SgCtx* g, unsigned sz, int n, int a, int rm) {
    int r = fround_to(g, sz, a, rm);
    return fwarm_mask(g, r, fnan(g, sz, a), sz, n, a, 0, FDESC(8, sz, n) | (SnxU32)(rm + 1) << 12, 0);
}

/* the FP instructions by lanes; 0 for the rest */
static int vec_fp(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned q = word >> 30 & 1, sz = word >> 22 & 1, rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31;
    int n = sz ? 2 : q ? 4 : 2, r, scalar = 0;
    switch (insn->op) {
    case SNX_A64_OP_FCMGT_s:
    case SNX_A64_OP_FCMGE_s:
    case SNX_A64_OP_FCMEQ_s:
    case SNX_A64_OP_FABD_s: scalar = 1; break;
    case SNX_A64_OP_FNEG_s:
    case SNX_A64_OP_FABS_s:
        if ((word >> 22 & 3) > 1) return 0; /* a half: the Sail's */
        scalar = 1;
        break;
    case SNX_A64_OP_SCVTF_f:
    case SNX_A64_OP_UCVTF_f:
    case SNX_A64_OP_FCVTZS_f:
    case SNX_A64_OP_FCVTZU_f: scalar = (word & 0xDF800000u) == 0x5F000000u; break; /* AdvSIMD scalar, by immh */
    default: break;
    }
    switch (insn->op) {
    case SNX_A64_OP_FADD_v: case SNX_A64_OP_FSUB_v: case SNX_A64_OP_FMUL_v: case SNX_A64_OP_FDIV_v:
    case SNX_A64_OP_FABD_v: case SNX_A64_OP_FMAX_v: case SNX_A64_OP_FMIN_v: case SNX_A64_OP_FMAXNM_v:
    case SNX_A64_OP_FMINNM_v: case SNX_A64_OP_FMLA_v: case SNX_A64_OP_FMLS_v: case SNX_A64_OP_FMUL_vi:
    case SNX_A64_OP_FMLA_vi: case SNX_A64_OP_FMLS_vi: case SNX_A64_OP_FNEG_v: case SNX_A64_OP_FABS_v:
    case SNX_A64_OP_FSQRT_v: case SNX_A64_OP_FCMEQ_v: case SNX_A64_OP_FCMGE_v: case SNX_A64_OP_FCMGT_v:
    case SNX_A64_OP_FACGE_v: case SNX_A64_OP_FACGT_v: case SNX_A64_OP_FCMEQ0_v: case SNX_A64_OP_FCMGE0_v:
    case SNX_A64_OP_FCMGT0_v: case SNX_A64_OP_FCMLE0_v: case SNX_A64_OP_FCMLT0_v: case SNX_A64_OP_FCVTZS_vi:
    case SNX_A64_OP_FCVTZU_vi: case SNX_A64_OP_SCVTF_vi: case SNX_A64_OP_UCVTF_vi: case SNX_A64_OP_FCVTNS_vi:
    case SNX_A64_OP_FCVTNU_vi: case SNX_A64_OP_FCVTPS_vi: case SNX_A64_OP_FCVTPU_vi: case SNX_A64_OP_FCVTMS_vi:
    case SNX_A64_OP_FCVTMU_vi: case SNX_A64_OP_FCVTAS_vi: case SNX_A64_OP_FCVTAU_vi: case SNX_A64_OP_FMULX_v:
    case SNX_A64_OP_FMULX_vi:
        if (sz && !q) return -1; /* 1D: reserved */
        break;
    case SNX_A64_OP_FCMGT_s: case SNX_A64_OP_FCMGE_s: case SNX_A64_OP_FCMEQ_s: case SNX_A64_OP_FABD_s:
    case SNX_A64_OP_FCVTZS_vf: case SNX_A64_OP_FCVTZU_vf: case SNX_A64_OP_SCVTF_vf: case SNX_A64_OP_UCVTF_vf:
    case SNX_A64_OP_FCVTL_v: case SNX_A64_OP_FCVTN_v: break;
    case SNX_A64_OP_FNEG_s:
    case SNX_A64_OP_FABS_s: break;
    case SNX_A64_OP_SCVTF_f: case SNX_A64_OP_UCVTF_f: case SNX_A64_OP_FCVTZS_f: case SNX_A64_OP_FCVTZU_f:
        if (!scalar) return 0; /* the general-purpose register forms: the Sail's */
        break;
    default: return 0;
    }
    if (scalar) n = 1;
    switch (insn->op) {
    case SNX_A64_OP_FADD_v: r = fbinary(g, SNX_FP_ADD, sz, n, vread(g, rn), vread(g, rm)); break;
    case SNX_A64_OP_FSUB_v: r = fbinary(g, SNX_FP_SUB, sz, n, vread(g, rn), vread(g, rm)); break;
    case SNX_A64_OP_FMUL_v: r = fbinary(g, SNX_FP_MUL, sz, n, vread(g, rn), vread(g, rm)); break;
    case SNX_A64_OP_FMULX_v: r = fbinary(g, SNX_FP_MULX, sz, n, vread(g, rn), vread(g, rm)); break;
    case SNX_A64_OP_FDIV_v: r = fbinary(g, SNX_FP_DIV, sz, n, vread(g, rn), vread(g, rm)); break;
    case SNX_A64_OP_FABD_v:
    case SNX_A64_OP_FABD_s: r = fbinary(g, SNX_FP_ABD, sz, n, vread(g, rn), vread(g, rm)); break;
    case SNX_A64_OP_FMAX_v: r = fminmax(g, sz, n, vread(g, rn), vread(g, rm), 0, 0); break;
    case SNX_A64_OP_FMIN_v: r = fminmax(g, sz, n, vread(g, rn), vread(g, rm), 1, 0); break;
    case SNX_A64_OP_FMAXNM_v: r = fminmax(g, sz, n, vread(g, rn), vread(g, rm), 0, 1); break;
    case SNX_A64_OP_FMINNM_v: r = fminmax(g, sz, n, vread(g, rn), vread(g, rm), 1, 1); break;
    case SNX_A64_OP_FMLA_v:
    case SNX_A64_OP_FMLS_v:
        r = ffma(g, sz, n, vread(g, rd), vread(g, rn), vread(g, rm), insn->op == SNX_A64_OP_FMLS_v);
        break;
    case SNX_A64_OP_FMUL_vi:
    case SNX_A64_OP_FMULX_vi:
    case SNX_A64_OP_FMLA_vi:
    case SNX_A64_OP_FMLS_vi: {
        unsigned h = word >> 11 & 1, l = word >> 21 & 1;
        int e, m;
        if (sz && l) return -1;
        e = sz ? (int)h : (int)(h << 1 | l);
        m = sgx_v1(X, &sgx_v_pshufd, vread(g, rm), sz ? (e ? 0xEE : 0x44) : (unsigned)e * 0x55);
        if (insn->op == SNX_A64_OP_FMUL_vi || insn->op == SNX_A64_OP_FMULX_vi)
            r = fbinary(g, insn->op == SNX_A64_OP_FMULX_vi ? SNX_FP_MULX : SNX_FP_MUL, sz, n, vread(g, rn), m);
        else
            r = ffma(g, sz, n, vread(g, rd), vread(g, rn), m, insn->op == SNX_A64_OP_FMLS_vi);
        break;
    }
    case SNX_A64_OP_FNEG_v:
    case SNX_A64_OP_FNEG_s: r = vxor(g, vread(g, rn), fsplat(g, sz, 1ull << (sz ? 63 : 31))); break;
    case SNX_A64_OP_FABS_v:
    case SNX_A64_OP_FABS_s: r = vandn(g, fsplat(g, sz, 1ull << (sz ? 63 : 31)), vread(g, rn)); break;
    case SNX_A64_OP_FSQRT_v: r = fsqrt(g, sz, n, vread(g, rn)); break;
    case SNX_A64_OP_FCMEQ_v:
    case SNX_A64_OP_FCMEQ_s: r = fcompare(g, sz, n, vread(g, rn), vread(g, rm), 0, 0); break;
    case SNX_A64_OP_FCMGE_v:
    case SNX_A64_OP_FCMGE_s: r = fcompare(g, sz, n, vread(g, rn), vread(g, rm), 1, 0); break;
    case SNX_A64_OP_FCMGT_v:
    case SNX_A64_OP_FCMGT_s: r = fcompare(g, sz, n, vread(g, rn), vread(g, rm), 2, 0); break;
    case SNX_A64_OP_FACGE_v: r = fcompare(g, sz, n, vread(g, rn), vread(g, rm), 1, 1); break;
    case SNX_A64_OP_FACGT_v: r = fcompare(g, sz, n, vread(g, rn), vread(g, rm), 2, 1); break;
    case SNX_A64_OP_FCMEQ0_v: r = fcompare(g, sz, n, vread(g, rn), vzero(g), 0, 0); break;
    case SNX_A64_OP_FCMGE0_v: r = fcompare(g, sz, n, vread(g, rn), vzero(g), 1, 0); break;
    case SNX_A64_OP_FCMGT0_v: r = fcompare(g, sz, n, vread(g, rn), vzero(g), 2, 0); break;
    case SNX_A64_OP_FCMLE0_v: r = fcompare(g, sz, n, vzero(g), vread(g, rn), 1, 0); break;
    case SNX_A64_OP_FCMLT0_v: r = fcompare(g, sz, n, vzero(g), vread(g, rn), 2, 0); break;
    case SNX_A64_OP_FCVTZS_vi:
    case SNX_A64_OP_FCVTZU_vi: r = fto_int(g, sz, n, vread(g, rn), insn->op == SNX_A64_OP_FCVTZU_vi, 0, 3); break;
    case SNX_A64_OP_FCVTNS_vi:
    case SNX_A64_OP_FCVTNU_vi: r = fto_int(g, sz, n, vread(g, rn), insn->op == SNX_A64_OP_FCVTNU_vi, 0, 0); break;
    case SNX_A64_OP_FCVTPS_vi:
    case SNX_A64_OP_FCVTPU_vi: r = fto_int(g, sz, n, vread(g, rn), insn->op == SNX_A64_OP_FCVTPU_vi, 0, 1); break;
    case SNX_A64_OP_FCVTMS_vi:
    case SNX_A64_OP_FCVTMU_vi: r = fto_int(g, sz, n, vread(g, rn), insn->op == SNX_A64_OP_FCVTMU_vi, 0, 2); break;
    case SNX_A64_OP_FCVTAS_vi:
    case SNX_A64_OP_FCVTAU_vi: r = fto_int(g, sz, n, vread(g, rn), insn->op == SNX_A64_OP_FCVTAU_vi, 0, 4); break;
    case SNX_A64_OP_SCVTF_vi:
    case SNX_A64_OP_UCVTF_vi: r = ffrom_int(g, sz, n, vread(g, rn), insn->op == SNX_A64_OP_UCVTF_vi, 0); break;
    case SNX_A64_OP_FCVTZS_vf:
    case SNX_A64_OP_FCVTZU_vf:
    case SNX_A64_OP_SCVTF_vf:
    case SNX_A64_OP_UCVTF_vf:
    case SNX_A64_OP_SCVTF_f:
    case SNX_A64_OP_UCVTF_f:
    case SNX_A64_OP_FCVTZS_f:
    case SNX_A64_OP_FCVTZU_f: {
        unsigned size, left, right;
        int to = insn->op == SNX_A64_OP_FCVTZS_vf || insn->op == SNX_A64_OP_FCVTZU_vf ||
                 insn->op == SNX_A64_OP_FCVTZS_f || insn->op == SNX_A64_OP_FCVTZU_f;
        int un = insn->op == SNX_A64_OP_FCVTZU_vf || insn->op == SNX_A64_OP_UCVTF_vf ||
                 insn->op == SNX_A64_OP_FCVTZU_f || insn->op == SNX_A64_OP_UCVTF_f;
        if (shift_imm(word, &size, &left, &right) < 0 || size < 2 || (size == 3 && !q && !scalar)) return -1;
        sz = size - 2;
        n = scalar ? 1 : sz ? 2 : q ? 4 : 2;
        r = to ? fto_int(g, sz, n, vread(g, rn), un, (int)right, 3) : ffrom_int(g, sz, n, vread(g, rn), un, (int)right);
        break;
    }
    case SNX_A64_OP_FCVTL_v: { /* singles to doubles (FCVTL2: the high pair); halves to singles by F16C */
        int src;
        if (!sz) {
            int nan, f16 = (X->features & SGX_F_F16C) != 0;
            src = q ? sgx_v1(X, &sgx_v_pshufd, vread(g, rn), 0xEE) : vread(g, rn);
            r = f16 ? sgx_v1(X, &sgx_v_vcvtph2ps, src, 0) : vzero(g);
            /* a NaN half (magnitude above 0x7C00), AHP, or no F16C: the helper */
            nan = vgt(g, 1, vand(g, src, vsplat(g, 1, 0x7FFF)), vsplat(g, 1, 0x7C00), 0);
            {
                SgB bad = sg_b_or(g, fstate(g, SGX_FPCR_AHP, 0),
                                  sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, sgx_val(32, sgx_v1(X, &sgx_v_pmovmskb, nan, 0)),
                                                                     sgx_const(32, 0xFF)))));
                if (!f16) bad = sg_bool(g, 1);
                r = fcold(g, bad, r, src, 0, 0, (SnxU32)5 | 2u << 4 | 4u << 8 | 4u << 12);
            }
            vwrite(g, rd, r, 1);
            return 1;
        }
        src = q ? sgx_v1(X, &sgx_v_pshufd, vread(g, rn), 0xEE) : vread(g, rn);
        r = sgx_v1(X, &sgx_v_cvtps2pd, src, 0);
        r = fcold(g, sg_b_or(g, fflushed(g, 0, 2, src, 0, 0), vany(g, fnan(g, 0, src), 0, 2)), r, src, 0, 0,
                  FDESC(5, 0, 2) | 8u << 12);
        vwrite(g, rd, r, 1);
        return 1;
    }
    case SNX_A64_OP_FCVTN_v: { /* doubles to singles (FCVTN2: into the high doubleword); singles to halves by F16C */
        int a, bad;
        SgD bits;
        if (!sz) {
            int f16 = (X->features & SGX_F_F16C) != 0, notroomy, zin;
            SgB cold;
            a = vread(g, rn);
            if (f16) {
                int in[4] = {0, a, 0, 0};
                r = sgx_vins(X, &sgx_v_vcvtps2ph, 0, 0, in, 4, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
            } else
                r = vzero(g);
            /* a half not normal above the smallest (its doubled magnitude less 0x0802 not below 0xEFFE, unsigned),
               unless from a zero single (the range from 0x0802: (0x7C00 - 0x0401) << 1 = 0xEFFE); FPCR's mode, FZ, AHP, IXC clear: the helper */
            {
                int m2 = vadd(g, 1, r, r), t = vsub(g, 1, m2, vsplat(g, 1, 0x0802));
                notroomy = vgt(g, 1, vxor(g, t, vsplat(g, 1, 0x8000)), vsplat(g, 1, (0xEFFEu - 1) ^ 0x8000u), 0);
            }
            zin = op2(g, &sgx_v_packssdw, fzero(g, 0, a), fzero(g, 0, a));
            cold = sg_b_or(g, sg_b_or(g, fstate(g, SGX_FPCR_RMODE | SGX_FPCR_FZ, 1), fstate(g, SGX_FPCR_AHP, 0)),
                           sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, sgx_val(32, sgx_v1(X, &sgx_v_pmovmskb, vandn(g, zin, notroomy), 0)),
                                                              sgx_const(32, 0xFF)))));
            if (!f16) cold = sg_bool(g, 1);
            r = fcold(g, cold, r, a, 0, 0, (SnxU32)5 | 4u << 4 | 4u << 8 | 2u << 12);
            vwrite_narrow(g, rd, r, q);
            return 1;
        }
        a = vread(g, rn);
        r = sgx_v1(X, &sgx_v_cvtpd2ps, a, 0);
        /* lanes not roomy as singles (MOVMSKPS's first two) and not from a zero double (MOVMSKPD's) */
        bits = sg_d_and(g, sgx_val(32, sgx_v1(X, &sgx_v_movmskps, fnot_roomy(g, 0, r), 0)),
                        sg_d_not(g, sgx_val(32, sgx_v1(X, &sgx_v_movmskpd, fzero(g, 1, a), 0))));
        bad = 0;
        /* a denormal double gives a zero single, not roomy: it goes cold whatever FZ is */
        r = fcold(
            g,
            sg_b_or(g, fstate(g, SGX_FPCR_RMODE, 1), sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, bits, sgx_const(32, 3))))),
            r, a, 0, 0, FDESC(5, 1, 2) | 4u << 12);
        (void)bad;
        vwrite_narrow(g, rd, r, q);
        return 1;
    }
    default: return 0;
    }
    if (scalar) sg__V_write(g, rd, sgx_xval(128, sgx_xclear(X, r, sz ? 64 : 32)));
    else
        vwrite(g, rd, r, q);
    return 1;
}

/* ---- saturation ---- */

/* FPSR.QC owed where any bit of v's first bytes is set (the lanes that saturated, or the bits a result differs in from
   the wrapped one): the saturating instructions'. The region's are an OR of PTEST's answers in a general-purpose
   register, no vector register held for it, which mg_qc tests once. A narrower v is tested against a mask of its
   bytes, not shifted to it: two shuffles fewer */
static void set_qc(SgCtx* g, int v, int bytes) {
    SgX* x = X;
    int in[4] = {v, bytes < 16 ? kconst(g, sg_ones(8u * (unsigned)bytes), 0) : v, 0, 0}; /* bytes: 1, 2, 4, 8 or 16 */
    sgx_vins(x, &sgx_v_ptest, -1, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    SgD any = sg_d_zext(g, sgx_bool_bits(x, sgx_bool(x, SGX_B_CC, x->flags_ins, 5)), 32); /* NE: a bit set */
    SgD owed = g->qc_owed ? sg_d_or(g, sgx_val(32, g->qc_owed), any) : any;
    g->qc_owed = sgx_reg(x, &owed);
}
static void mg_qc(Unit* u) {
    SgCtx* g = u->g;
    SgX* x = X;
    SgD bit = sg_d_lsl(g, sgx_val(32, g->qc_owed), 27); /* qc_owed: 0 or 1 */
    SgXIns* load = sgx_append(x, SGX_K_LOAD);
    load->w = 32;
    load->disp = (int)offsetof(CPU, fpsr);
    load->def = sgx_vreg(x, x->n - 1, 32);
    SgD fpsr = sg_d_or(g, sgx_val(32, load->def), bit);
    int r = sgx_reg(x, &fpsr);
    SgXIns* st = sgx_append(x, SGX_K_STORE);
    st->w = 32;
    st->in[0] = r;
    st->disp = (int)offsetof(CPU, fpsr);
}
/* the signed (or unsigned) limits of 8 << size bits */
static SnxU64 smax_of(unsigned size) { return sg_ones((8u << size) - 1); }
static SnxU64 smin_of(unsigned size) { return 1ull << ((8u << size) - 1); }
/* SQADD, UQADD, SQSUB, UQSUB: PADDS and the rest for bytes and halfwords; words and doublewords wrapped, the lanes
   that overflowed (signed: the result's sign unlike both operands' for a sum, unlike a's and like b's for a
   difference; unsigned: carried or borrowed) given the limit. UQSUB of words: the larger of the operands less b.
   *sat: bits set in the lanes that saturated */
static int vqaddsub(SgCtx* g, unsigned size, int a, int b, int uns, int sub, int* sat) {
    static const SgXVForm* const k[4][2] = {{&sgx_v_paddsb, &sgx_v_paddusb}, {&sgx_v_paddsw, &sgx_v_paddusw},
                                            {&sgx_v_psubsb, &sgx_v_psubusb}, {&sgx_v_psubsw, &sgx_v_psubusw}};
    int r = sub ? vsub(g, size, a, b) : vadd(g, size, a, b), ovf, res;
    if (size < 2 || (size == 2 && uns && sub)) {
        res = size < 2 ? op2(g, k[sub * 2 + size][uns], a, b) : vsub(g, 2, vmax(g, 2, a, b, 1, 0), b);
        *sat = vxor(g, res, r);
        return res;
    }
    if (uns) {
        ovf = sub ? vgt(g, size, b, a, 1) : vgt(g, size, a, r, 1);
        *sat = ovf;
        return sub ? vandn(g, ovf, r) : vor(g, r, ovf);
    }
    ovf = sub ? vand(g, vxor(g, a, b), vxor(g, a, r)) : vand(g, vxor(g, a, r), vxor(g, b, r));
    ovf = vsign(g, size, ovf); /* whole lanes */
    *sat = ovf;
    /* the wrapped result's sign spread, XOR the smallest: the largest for a negative one, the smallest for a
       positive */
    return vsel(g, ovf, vxor(g, vsign(g, size, r), vsplat(g, size, smin_of(size))), r);
}
/* SUQADD (uns 0: signed d and unsigned n, saturated as signed) and USQADD (unsigned d, signed n, saturated as
   unsigned): d's sign bit flipped, what is left is UQADD's and SQADD's sum, its sign bit flipped back */
static int vqaddmixed(SgCtx* g, unsigned size, int d, int n, int uns, int* sat) {
    int flip = vsplat(g, size, smin_of(size));
    return vxor(g, vqaddsub(g, size, vxor(g, d, flip), n, !uns, 0, sat), flip);
}
/* SQABS, SQNEG (neg): the smallest's magnitude or negation the largest */
static int vqabsneg(SgCtx* g, unsigned size, int a, int neg, int* sat) {
    if (neg && size < 2) {
        int res = op2(g, size ? &sgx_v_psubsw : &sgx_v_psubsb, vzero(g), a);
        *sat = vxor(g, res, vneg(g, size, a));
        return res;
    }
    int r = neg ? vneg(g, size, a) : vabs(g, size, a);
    int m = veq(g, size, r, vsplat(g, size, smin_of(size)));
    *sat = m;
    return vxor(g, r, m);
}
/* elements of 16 << size bits saturated to 8 << size (to: 0 signed to signed, 1 unsigned to unsigned, 2 signed to
   unsigned), into the low doubleword; *sat the lanes (of the source's) that were out of range */
static int vqnarrow(SgCtx* g, unsigned size, int a, int to, int* sat) {
    unsigned big = size + 1;
    int hi, lo;
    if (to == 1) {
        hi = vgt(g, big, a, vsplat(g, big, sg_ones(8u << size)), 1);
        *sat = hi;
        return vnarrow(g, size, vsel(g, hi, vsplat(g, big, sg_ones(8u << size)), a), 0);
    }
    if (to == 0) {
        hi = vgt(g, big, a, vsplat(g, big, smax_of(size)), 0);
        lo = vgt(g, big, vsplat(g, big, (SnxU64)(-(long long)smin_of(size))), a, 0);
        *sat = vor(g, hi, lo);
        a = vsel(g, hi, vsplat(g, big, smax_of(size)), vsel(g, lo, vsplat(g, big, (SnxU64)(-(long long)smin_of(size))), a));
        return vnarrow(g, size, a, 0);
    }
    hi = vgt(g, big, a, vsplat(g, big, sg_ones(8u << size)), 0);
    lo = vgt(g, big, vzero(g), a, 0);
    *sat = vor(g, hi, lo);
    a = vsel(g, hi, vsplat(g, big, sg_ones(8u << size)), vandn(g, lo, a));
    return vnarrow(g, size, a, 0);
}
/* SQDMULH, SQRDMULH (round): (2 a b (+ 2^(esize-1))) >> esize, saturated where a and b are both the smallest */
static int vqdmulh(SgCtx* g, unsigned size, int a, int b, int round, int* sat) {
    int r, both = vand(g, veq(g, size, a, vsplat(g, size, smin_of(size))), veq(g, size, b, vsplat(g, size, smin_of(size))));
    if (size == 1) {
        if (round) r = op2(g, &sgx_v_pmulhrsw, a, b);
        else /* (a b) >> 15: the high word's bits up one and the low word's top */
            r = vor(g, vshl(g, 1, op2(g, &sgx_v_pmulhw, a, b), 1), vshr(g, 1, op2(g, &sgx_v_pmullw, a, b), 15, 0));
    } else {
        /* the even and odd words' 64-bit products (PMULDQ), (p (+ 2^30)) >> 31's low words */
        int pe = op2(g, &sgx_v_pmuldq, a, b);
        int po = op2(g, &sgx_v_pmuldq, vshr(g, 3, a, 32, 0), vshr(g, 3, b, 32, 0));
        if (round) {
            pe = vadd(g, 3, pe, vsplat(g, 3, 1ull << 30));
            po = vadd(g, 3, po, vsplat(g, 3, 1ull << 30));
        }
        pe = vshr(g, 3, pe, 31, 0);
        po = vshr(g, 3, po, 31, 0);
        r = vor(g, vand(g, pe, vsplat(g, 3, 0xFFFFFFFFu)), vshl(g, 3, po, 32));
    }
    *sat = both;
    return vxor(g, r, both); /* the smallest there: XOR all ones is the largest */
}
/* SQSHL by register, lane by lane: left where the amount is positive (saturating where bits are lost, QC), right
   arithmetic where negative */
static int vqshl_reg(SgCtx* g, unsigned size, int q, int a, int m, int* sat) {
    unsigned e = 8u << size;
    int n = (q ? 16 : 8) >> size, i, r = vzero(g), s_mask = vzero(g);
    for (i = 0; i < n; ++i) {
        SgD v = sg_d_sext(g, lane_get(g, size, a, i), 64), s = sg_d_sext(g, sg_d_slice(g, lane_get(g, size, m, i), 7, 0), 64);
        SgD neg = sg_d_sub(g, sgx_const(64, 0), s), l, back, rr, satv, res;
        SgB left = sgx_d_ult(g, s, sgx_const(64, 1ull << 63)), lost, big;
        /* left: v << min(s, 63) (s >= e: lost unless v is 0), lost where shifting back right differs */
        SgD sl = sg_d_select(g, sgx_d_ult(g, s, sgx_const(64, 64)), s, sgx_const(64, 63));
        l = sg_d_shift_dyn(g, v, sl, 0);
        back = sg_d_shift_dyn(g, l, sl, 2);
        big = sg_b_not(g, sgx_d_ult(g, s, sgx_const(64, e)));
        /* fits e bits: l + 2^(e-1) below 2^e */
        lost = sg_b_or(g, sg_b_not(g, sg_d_eq(g, back, v)),
                       sg_b_not(g, sgx_d_ult(g, sg_d_add(g, l, sgx_const(64, smin_of(size))), sgx_const(64, e == 64 ? 0 : 1ull << e))));
        if (e == 64) lost = sg_b_not(g, sg_d_eq(g, back, v));
        lost = sg_b_and(g, sg_b_or(g, lost, big), sg_b_not(g, sg_d_is_zero(g, v)));
        satv = sg_d_select(g, sgx_d_ult(g, v, sgx_const(64, 1ull << 63)), sgx_const(64, smax_of(size)),
                           sgx_const(64, smin_of(size) | ~sg_ones(e)));
        l = sg_d_select(g, lost, satv, l);
        rr = sg_d_shift_dyn(g, v, sg_d_select(g, sgx_d_ult(g, neg, sgx_const(64, e)), neg, sgx_const(64, e - 1)), 2);
        res = sg_d_select(g, left, l, rr);
        r = lane_put(g, size, r, i, sg_d_slice(g, res, e - 1, 0));
        s_mask = lane_put(g, size, s_mask, i,
                          sg_d_select(g, sg_b_and(g, left, lost), sgx_const(e, sg_ones(e)), sgx_const(e, 0)));
    }
    *sat = s_mask;
    return r;
}

/* ---- shifts by a register's lanes ----
   Left by the signed byte s at the bottom of each lane of m where it is positive, right by -s where it is negative
   (uns: logical), rounded (round: the last bit shifted out added back) or saturated on the left (saturate), as
   ir_vec.c's shl_helper. Left and right are made apart, each 0 where it is the other's lane, and ORed (a 0 amount is
   the value, in both). Words and doublewords by AVX2's shifts, 0 past the lane; bytes and halfwords by products with
   the power of two a table (PSHUFB) has for the amount: left a * 2^s, right the upper half of a * 2^(e + s), whose
   index -1 - s is the amount's complement */
enum { VSH_UNS = 1, VSH_ROUND = 2, VSH_SAT = 4 };

/* each byte's power of two for the amount in i, 2^i (down: 2^(7 - i)), 0 past 7 and for the negative: past the table's
   16 bytes once 0x70 more (saturated) */
static int vpow8(SgCtx* g, int i, int down) {
    return op2(g, &sgx_v_pshufb, kconst(g, down ? 0x0102040810204080ull : 0x8040201008040201ull, 0),
               op2(g, &sgx_v_paddusb, i, vsplat(g, 0, 0x70)));
}
/* each halfword's for the amount in i's low byte, 2^i (down: 2^(15 - i)): the two bytes' tables apart */
static int vpow16(SgCtx* g, int i, int down) {
    SnxU64 up = 0x8040201008040201ull, dn = 0x0102040810204080ull;
    int idx = op2(g, &sgx_v_paddusb, vor(g, i, vsplat(g, 1, 0xFF00)), vsplat(g, 0, 0x70));
    int low = op2(g, &sgx_v_pshufb, down ? kconst(g, 0, dn) : kconst(g, up, 0), idx);
    int high = op2(g, &sgx_v_pshufb, down ? kconst(g, dn, 0) : kconst(g, 0, up), idx);
    return vor(g, low, opi(g, &sgx_v_psllw_i, high, 8));
}
/* a's sign limit, the largest for a positive lane, the smallest for a negative one */
static int vlimit(SgCtx* g, unsigned size, int a) { return vxor(g, vsign(g, size, a), vsplat(g, size, smax_of(size))); }

static int vshift_bytes(SgCtx* g, int a, int m, int flags, int* sat) {
    int uns = flags & VSH_UNS, zero = vzero(g), neg = 0, left, right, i = vnot(g, m), p, q;
    /* right: the even bytes' products and the odd ones', the upper byte of each kept (128 added first to round). Signed
       amounts past 8 shift by 8 where not rounded, which leaves 0 */
    if (!uns && !(flags & VSH_ROUND)) {
        neg = vsign(g, 0, m);
        q = vpow8(g, op2(g, &sgx_v_pminub, i, vsplat(g, 0, 7)), 1);
    } else
        q = vpow8(g, i, 1);
    {
        int ev = uns ? vand(g, a, vsplat(g, 1, 0xFF)) : vshr(g, 1, vshl(g, 1, a, 8), 8, 1), od = vshr(g, 1, a, 8, !uns);
        ev = op2(g, &sgx_v_pmullw, ev, vand(g, q, vsplat(g, 1, 0xFF)));
        od = op2(g, &sgx_v_pmullw, od, vshr(g, 1, q, 8, 0));
        if (flags & VSH_ROUND) {
            ev = vadd(g, 1, ev, vsplat(g, 1, 0x80));
            od = vadd(g, 1, od, vsplat(g, 1, 0x80));
        }
        right = vor(g, vshr(g, 1, ev, 8, 0), vand(g, od, vsplat(g, 1, 0xFF00)));
        if (neg) right = vand(g, right, neg);
    }
    p = vpow8(g, m, 0);
    if (!(flags & VSH_SAT)) { /* left, modulo 256: the odd bytes' product is the halfwords' upper byte */
        int ev = vand(g, op2(g, &sgx_v_pmullw, a, p), vsplat(g, 1, 0xFF));
        int od = op2(g, &sgx_v_pmullw, vand(g, a, vsplat(g, 1, 0xFF00)), vshr(g, 1, p, 8, 0));
        return vor(g, vor(g, ev, od), right);
    }
    {
        /* the products of 16 bits, packed with saturation, which saturates where their upper byte is not 0 (signed:
           where their bits from 7 up are not all the sign). An amount past 7 (p is 0) leaves 0, and the limit where a
           is not 0 */
        int s = uns ? zero : vsign(g, 0, a), far, out_lo, out_hi;
        int lo = op2(g, &sgx_v_pmullw, op2(g, &sgx_v_punpcklbw, a, s), op2(g, &sgx_v_punpcklbw, p, zero));
        int hi = op2(g, &sgx_v_pmullw, op2(g, &sgx_v_punpckhbw, a, s), op2(g, &sgx_v_punpckhbw, p, zero));
        left = op2(g, uns ? &sgx_v_packuswb : &sgx_v_packsswb, lo, hi);
        out_lo = uns ? vshr(g, 1, lo, 8, 0) : vxor(g, vshr(g, 1, lo, 7, 1), vsign(g, 1, lo));
        out_hi = uns ? vshr(g, 1, hi, 8, 0) : vxor(g, vshr(g, 1, hi, 7, 1), vsign(g, 1, hi));
        far = vandn(g, veq(g, 0, a, zero), vgt(g, 0, m, vsplat(g, 0, 7), 0));
        *sat = vor(g, op2(g, uns ? &sgx_v_packuswb : &sgx_v_packsswb, out_lo, out_hi), far);
        return vor(g, vor(g, left, uns ? far : vand(g, far, vlimit(g, 0, a))), right);
    }
}
static int vshift_halves(SgCtx* g, int a, int m, int flags, int* sat) {
    int uns = flags & VSH_UNS, neg = 0, i = vnot(g, m), s = 0, left, right, p, q, high;
    if (!uns) s = vsign(g, 1, a);
    if (!uns && !(flags & VSH_ROUND)) {
        neg = vsign(g, 1, vshl(g, 1, m, 8));
        q = vpow16(g, op2(g, &sgx_v_pminub, i, vsplat(g, 1, 15)), 1);
    } else
        q = vpow16(g, i, 1);
    /* the upper halves of a * q, q of 16 unsigned bits (a signed a's less q where it is negative) */
    right = op2(g, &sgx_v_pmulhuw, a, q);
    if (!uns) right = vsub(g, 1, right, vand(g, q, s));
    if (flags & VSH_ROUND) right = vadd(g, 1, right, vshr(g, 1, op2(g, &sgx_v_pmullw, a, q), 15, 0));
    if (neg) right = vand(g, right, neg);
    p = vpow16(g, m, 0);
    left = op2(g, &sgx_v_pmullw, a, p);
    if (flags & VSH_SAT) {
        /* the product's upper half not its lower's sign (or not 0), or an amount past 15 of an a that is not 0 */
        int far = vandn(g, veq(g, 1, a, vzero(g)), vgt(g, 1, vshl(g, 1, m, 8), vsplat(g, 1, 0x0FFF), 0)), over;
        high = op2(g, &sgx_v_pmulhuw, a, p);
        if (!uns) high = vsub(g, 1, high, vand(g, p, s));
        over = vor(g, far, vnot(g, veq(g, 1, high, uns ? vzero(g) : vsign(g, 1, left))));
        *sat = over;
        left = uns ? vor(g, left, over) : vsel(g, over, vlimit(g, 1, a), left);
    }
    return vor(g, left, right);
}
/* AVX's three-operand form f of a and b */
static int op3(SgCtx* g, const SgXVForm* f, int a, int b) {
    int in[4] = {0, a, b, 0};
    return sgx_vins(X, f, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
/* words' and doublewords' lanes of a shifted right by c's (AVX2): logical, or arithmetic, a doubleword's the logical
   shift of its bits XORed with its sign s */
static int vshrv(SgCtx* g, unsigned size, int a, int c, int uns, int s) {
    if (size == 2) return op3(g, uns ? &sgx_v_vpsrlvd : &sgx_v_vpsravd, a, c);
    if (uns) return op3(g, &sgx_v_vpsrlvq, a, c);
    return vxor(g, op3(g, &sgx_v_vpsrlvq, vxor(g, a, s), c), s);
}
static int vshift_words(SgCtx* g, unsigned size, int a, int m, int flags, int* sat) {
    int uns = flags & VSH_UNS, byte = vsplat(g, size, 0xFF), s = !uns && size == 3 ? vsign(g, 3, a) : 0;
    int cl = vand(g, m, byte), cr = vand(g, vneg(g, size, m), byte), left, right, neg = 0;
    if (!uns || (flags & VSH_SAT)) neg = vsign(g, size, vshl(g, size, m, (8u << size) - 8));
    right = vshrv(g, size, a, cr, uns, s);
    if (flags & VSH_ROUND) /* shifted by one less, the last bit out is its lowest */
        right =
            vadd(g, size, right, vand(g, vshrv(g, size, a, vadd(g, size, cr, vones(g)), uns, s), vsplat(g, size, 1)));
    if (!uns) right = vand(g, right, neg);
    left = op3(g, size == 2 ? &sgx_v_vpsllvd : &sgx_v_vpsllvq, a, cl);
    if (flags & VSH_SAT) { /* what shifting back doesn't give again */
        int back = vshrv(g, size, left, cl, uns, !uns && size == 3 ? vsign(g, 3, left) : 0);
        int over = vandn(g, vor(g, veq(g, size, back, a), neg), vones(g));
        *sat = over;
        left = uns ? vor(g, left, over) : vsel(g, over, vlimit(g, size, a), left);
    }
    return vor(g, left, right);
}
static int vshift(SgCtx* g, unsigned size, int a, int m, int flags, int* sat) {
    return size == 0   ? vshift_bytes(g, a, m, flags, sat)
           : size == 1 ? vshift_halves(g, a, m, flags, sat)
                       : vshift_words(g, size, a, m, flags, sat);
}
/* the shifts by register: VSH_* and whether scalar, or -1 */
static int vshift_of(SnxA64Op op, int* scalar) {
    *scalar = op == SNX_A64_OP_SSHL_s || op == SNX_A64_OP_USHL_s || op == SNX_A64_OP_SRSHL_s ||
              op == SNX_A64_OP_URSHL_s || op == SNX_A64_OP_SQSHL_s || op == SNX_A64_OP_UQSHL_s ||
              op == SNX_A64_OP_SQRSHL_s || op == SNX_A64_OP_UQRSHL_s;
    switch (op) {
        case SNX_A64_OP_SSHL_v:
        case SNX_A64_OP_SSHL_s: return 0;
        case SNX_A64_OP_USHL_v:
        case SNX_A64_OP_USHL_s: return VSH_UNS;
        case SNX_A64_OP_SRSHL_v:
        case SNX_A64_OP_SRSHL_s: return VSH_ROUND;
        case SNX_A64_OP_URSHL_v:
        case SNX_A64_OP_URSHL_s: return VSH_UNS | VSH_ROUND;
        case SNX_A64_OP_SQSHL_v:
        case SNX_A64_OP_SQSHL_s: return VSH_SAT;
        case SNX_A64_OP_UQSHL_v:
        case SNX_A64_OP_UQSHL_s: return VSH_UNS | VSH_SAT;
        case SNX_A64_OP_SQRSHL_v:
        case SNX_A64_OP_SQRSHL_s: return VSH_ROUND | VSH_SAT;
        case SNX_A64_OP_UQRSHL_v:
        case SNX_A64_OP_UQRSHL_s: return VSH_UNS | VSH_ROUND | VSH_SAT;
        default: return -1;
    }
}
/* SQSHL, UQSHL (uns) and SQSHLU (mixed: signed to unsigned) by n, below the lane's bits */
static int vqshl_imm(SgCtx* g, unsigned size, int a, unsigned n, int uns, int mixed, int* sat) {
    int r = vshl(g, size, a, n), over = vnot(g, veq(g, size, vshr(g, size, r, n, !uns && !mixed), a));
    if (mixed) { /* a negative lane is 0, and saturated; the rest unsigned */
        int neg = vsign(g, size, a);
        *sat = vor(g, over, neg);
        return vandn(g, neg, vor(g, r, over));
    }
    *sat = over;
    return uns ? vor(g, r, over) : vsel(g, over, vlimit(g, size, a), r);
}

/* the by-element operand of a halfwords' (H:L:M of V0 to V15) or a words' (H:L) word in every lane */
static int velement(SgCtx* g, unsigned size, SnxU32 word) {
    unsigned h = word >> 11 & 1, l = word >> 21 & 1, m4 = word >> 20 & 1, rm = word >> 16 & 31;
    return size == 1 ? vdup(g, 1, vread(g, rm & 15), (int)(h << 2 | l << 1 | m4))
                     : vdup(g, 2, vread(g, rm), (int)(h << 1 | l));
}
/* SQDMULL: twice the products of the low (hi: high) halves' lanes, of twice their bits. Only the smallest by itself is
   past the largest, and wraps to the smallest of those bits, which no other product gives */
static int vqdmull(SgCtx* g, unsigned size, int a, int b, int hi, int* sat) {
    int p = size == 1 ? op2(g, hi ? &sgx_v_punpckhwd : &sgx_v_punpcklwd, op2(g, &sgx_v_pmullw, a, b),
                            op2(g, &sgx_v_pmulhw, a, b))
                      : op2(g, &sgx_v_pmuldq, vwiden(g, 2, a, hi, 1), vwiden(g, 2, b, hi, 1));
    p = vadd(g, size + 1, p, p);
    int m = veq(g, size + 1, p, vsplat(g, size + 1, smin_of(size + 1)));
    *sat = m;
    return vxor(g, p, m);
}
/* |a - b|'s building blocks and the rest of the lanes' integer arithmetic the instructions below share */
static int vabs(SgCtx* g, unsigned size, int a) {
    if (k_abs[size]) return sgx_v1(X, k_abs[size], a, 0);
    { /* (a ^ s) - s, s the sign */
        int s = vgt(g, 3, vzero(g), a, 0);
        return vsub(g, 3, vxor(g, a, s), s);
    }
}
/* a >> n rounded (half up), logical (arith: arithmetic): the bits shifted out, the top one of them, added back */
static int vrshr(SgCtx* g, unsigned size, int a, unsigned n, int arith) {
    if (!size && vgfni(g)) return vadd(g, 0, vshr(g, 0, a, n, arith), vgf(g, a, VGF_BIT, n - 1));
    return vadd(g, size, vshr(g, size, a, n, arith), vand(g, vshr(g, size, a, n - 1, 0), vsplat(g, size, 1)));
}
/* UADDLP, SADDLP: each pair of lanes added to a lane twice as wide */
static int vaddlp(SgCtx* g, unsigned size, int a, int sign) {
    unsigned e = 8u << size;
    if (size == 1 && sign) return op2(g, &sgx_v_pmaddwd, a, vsplat(g, 1, 1));
    if (size == 2) {
        int hi = vshr(g, 3, a, 32, 0), lo = vand(g, a, vsplat(g, 3, 0xFFFFFFFFu));
        if (sign) { /* PMULDQ by 1: the words of its even lanes sign extended */
            lo = op2(g, &sgx_v_pmuldq, a, vsplat(g, 2, 1));
            hi = op2(g, &sgx_v_pmuldq, hi, vsplat(g, 2, 1));
        }
        return vadd(g, 3, lo, hi);
    }
    return vadd(g, size + 1,
                sign ? vshr(g, size + 1, vshl(g, size + 1, a, e), e, 1) : vand(g, a, vsplat(g, size + 1, sg_ones(e))),
                vshr(g, size + 1, a, e, sign));
}
/* UADDLV, SADDLV: every lane's sum in the low 16 << size bits of the result, the rest of the register not cleared */
static int vaddlv(SgCtx* g, unsigned size, int q, int a, int sign) {
    if (size == 0) { /* the eights' sums (PSADBW); signed bytes by their sign flipped, 128 less each */
        int r = op2(g, &sgx_v_psadbw, sign ? vxor(g, a, vsplat(g, 0, 0x80)) : a, vzero(g));
        if (q) r = vadd(g, 3, r, sgx_v1(X, &sgx_v_pshufd, r, 0xEE));
        return sign ? vsub(g, 3, r, kconst(g, 128u * (q ? 16 : 8), 0)) : r;
    }
    if (size == 1) return vreduce(g, 2, vaddlp(g, 1, a, sign), q ? 4 : 2, red_add, 0, 0);
    { /* words: the two doublewords' sums */
        int t = vadd(g, 3, vwiden(g, 2, a, 0, sign), vwiden(g, 2, a, 1, sign));
        return vadd(g, 3, t, sgx_v1(X, &sgx_v_pshufd, t, 0xEE));
    }
}
/* CLZ by lanes (bytes, halfwords, words): each byte's by nibbles (PSHUFB), then each half's where the upper is all 0 */
static int vclz(SgCtx* g, unsigned size, int a) {
    int t = kconst(g, 0x0101010102020304ull, 0); /* clz of a nibble: 4 3 2 2 1 1 1 1 0 ... */
    int hi = vand(g, vshr(g, 1, a, 4, 0), vsplat(g, 0, 15)), lo = vand(g, a, vsplat(g, 0, 15));
    int r = vadd(g, 0, op2(g, &sgx_v_pshufb, t, hi), vand(g, op2(g, &sgx_v_pshufb, t, lo), veq(g, 0, hi, vzero(g))));
    if (size >= 1) {
        int up = vshr(g, 1, r, 8, 0), low = vand(g, r, vsplat(g, 1, 0xFF));
        r = vadd(g, 1, up, vand(g, low, veq(g, 1, up, vsplat(g, 1, 8))));
    }
    if (size >= 2) {
        int up = vshr(g, 2, r, 16, 0), low = vand(g, r, vsplat(g, 2, 0xFFFF));
        r = vadd(g, 2, up, vand(g, low, veq(g, 2, up, vsplat(g, 2, 16))));
    }
    return r;
}
/* a scalar result (element 0 of 8 << size bits) into V[d], the rest of the register cleared */
static void vwrite_scalar(SgCtx* g, unsigned d, unsigned size, int r) {
    sg__V_write(g, d, sgx_xval(128, sgx_xclear(X, r, 8u << size)));
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

/* the FP instructions the lanes above don't have: by element scalars, pairwise and across lanes, the reciprocal
   estimates and steps and the roundings to an integer; 0 for the rest */
static int vec_fp_more(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned q = word >> 30 & 1, sz = word >> 22 & 1, rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31;
    int n = sz ? 2 : q ? 4 : 2, r, scalar = 0;
    switch (insn->op) {
    case SNX_A64_OP_FMUL_si:
    case SNX_A64_OP_FMLA_si:
    case SNX_A64_OP_FMLS_si: {
        unsigned h = word >> 11 & 1, l = word >> 21 & 1;
        int e = sz ? (int)h : (int)(h << 1 | l), m;
        if (sz && l) return -1;
        scalar = n = 1;
        m = sgx_v1(X, &sgx_v_pshufd, vread(g, rm), sz ? (e ? 0xEE : 0x44) : (unsigned)e * 0x55);
        if (insn->op == SNX_A64_OP_FMUL_si) r = fbinary(g, SNX_FP_MUL, sz, 1, vread(g, rn), m);
        else
            r = ffma(g, sz, 1, vread(g, rd), vread(g, rn), m, insn->op == SNX_A64_OP_FMLS_si);
        break;
    }
    case SNX_A64_OP_FADDP_v:
    case SNX_A64_OP_FMAXP_v:
    case SNX_A64_OP_FMINP_v:
    case SNX_A64_OP_FMAXNMP_v:
    case SNX_A64_OP_FMINNMP_v: {
        int a, b, ev, od;
        if (sz && !q) return -1; /* 1D: reserved */
        a = vread(g, rn), b = vread(g, rm);
        ev = vzip(g, 2, sz + 2, (int)q, a, b);
        od = vzip(g, 3, sz + 2, (int)q, a, b);
        if (insn->op == SNX_A64_OP_FADDP_v)
            r = fbinary(g, SNX_FP_ADD, sz, n, ev, od);
        else
            r = fminmax(g, sz, n, ev, od, insn->op == SNX_A64_OP_FMINP_v || insn->op == SNX_A64_OP_FMINNMP_v,
                        insn->op == SNX_A64_OP_FMAXNMP_v || insn->op == SNX_A64_OP_FMINNMP_v);
        break;
    }
    case SNX_A64_OP_FADDP_s:
    case SNX_A64_OP_FMAXP_s:
    case SNX_A64_OP_FMINP_s:
    case SNX_A64_OP_FMAXNMP_s:
    case SNX_A64_OP_FMINNMP_s: { /* lane 0 with lane 1 */
        int a = vread(g, rn), b = sgx_v1(X, &sgx_v_pshufd, a, sz ? 0xEE : 0x55);
        scalar = n = 1;
        if (insn->op == SNX_A64_OP_FADDP_s)
            r = fbinary(g, SNX_FP_ADD, sz, 1, a, b);
        else
            r = fminmax(g, sz, 1, a, b, insn->op == SNX_A64_OP_FMINP_s || insn->op == SNX_A64_OP_FMINNMP_s,
                        insn->op == SNX_A64_OP_FMAXNMP_s || insn->op == SNX_A64_OP_FMINNMP_s);
        break;
    }
    case SNX_A64_OP_FMAXV_s:
    case SNX_A64_OP_FMINV_s:
    case SNX_A64_OP_FMAXNMV_s:
    case SNX_A64_OP_FMINNMV_s: { /* (0 op 1) op (2 op 3) */
        int a = vread(g, rn), t;
        int mn = insn->op == SNX_A64_OP_FMINV_s || insn->op == SNX_A64_OP_FMINNMV_s;
        int num = insn->op == SNX_A64_OP_FMAXNMV_s || insn->op == SNX_A64_OP_FMINNMV_s;
        t = fminmax(g, 0, 2, sgx_v1(X, &sgx_v_pshufd, a, 0x88), sgx_v1(X, &sgx_v_pshufd, a, 0xDD), mn, num);
        scalar = n = 1;
        r = fminmax(g, 0, 1, t, sgx_v1(X, &sgx_v_pshufd, t, 0x55), mn, num);
        break;
    }
    case SNX_A64_OP_FRECPE_v:
    case SNX_A64_OP_FRECPE_s:
    case SNX_A64_OP_FRSQRTE_v:
    case SNX_A64_OP_FRSQRTE_s: {
        int sqrt = insn->op == SNX_A64_OP_FRSQRTE_v || insn->op == SNX_A64_OP_FRSQRTE_s;
        scalar = insn->op == SNX_A64_OP_FRECPE_s || insn->op == SNX_A64_OP_FRSQRTE_s;
        if (!scalar && sz && !q) return -1;
        if (scalar) n = 1;
        r = festimate(g, sz, n, vread(g, rn), sqrt);
        break;
    }
    case SNX_A64_OP_FRECPS_v:
    case SNX_A64_OP_FRECPS_s:
    case SNX_A64_OP_FRSQRTS_v:
    case SNX_A64_OP_FRSQRTS_s: {
        int sqrt = insn->op == SNX_A64_OP_FRSQRTS_v || insn->op == SNX_A64_OP_FRSQRTS_s;
        scalar = insn->op == SNX_A64_OP_FRECPS_s || insn->op == SNX_A64_OP_FRSQRTS_s;
        if (!scalar && sz && !q) return -1;
        if (scalar) n = 1;
        r = fstep(g, sz, n, vread(g, rn), vread(g, rm), sqrt);
        break;
    }
    case SNX_A64_OP_FRINTN_v:
    case SNX_A64_OP_FRINTP_v:
    case SNX_A64_OP_FRINTM_v:
    case SNX_A64_OP_FRINTZ_v:
    case SNX_A64_OP_FRINTA_v:
        if (sz && !q) return -1;
        r = fround(g, sz, n, vread(g, rn),
                   insn->op == SNX_A64_OP_FRINTN_v   ? 0
                   : insn->op == SNX_A64_OP_FRINTP_v ? 1
                   : insn->op == SNX_A64_OP_FRINTM_v ? 2
                   : insn->op == SNX_A64_OP_FRINTZ_v ? 3
                                                     : 4);
        break;
    default: return 0;
    }
    if (scalar)
        sg__V_write(g, rd, sgx_xval(128, sgx_xclear(X, r, sz ? 64 : 32)));
    else
        vwrite(g, rd, r, q);
    return 1;
}

/* SHA1C, SHA1P, SHA1M by SHA-NI's four rounds (func: their choice, parity and majority, with the constant SHA1RNDS4
   adds, which Vm holds already and so is taken out of it): the state, the words and the first one's E in the other
   order of lanes */
static int vsha1(SgCtx* g, int func, int abcd, int e, int wk) {
    static const SnxU32 k[3] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu};
    int m = sgx_v1(X, &sgx_v_pshufd, vsub(g, 2, wk, vsplat(g, 2, k[func])), 0x1B), t;
    m = vadd(g, 2, m, opi(g, &sgx_v_pslldq, e, 12));
    t = sgx_v2(X, &sgx_v_sha1rnds4, sgx_v1(X, &sgx_v_pshufd, abcd, 0x1B), m, func);
    return sgx_v1(X, &sgx_v_pshufd, t, 0x1B);
}
/* the words of a rotated left by n */
static int vrolw(SgCtx* g, int a, unsigned n) { return vor(g, vshl(g, 2, a, n), vshr(g, 2, a, 32 - n, 0)); }
/* AESMC (AESIMC: inv) of a value AESE (AESD) made, the pair a full round of AES-NI's with a zero key: what that last
   round was of, or 0 */
static int vaes_last(SgCtx* g, int a, int inv) {
    SgX* x = X;
    const SgXIns* i = x->v[a].def >= 0 && x->v[a].def < x->n ? &x->ins[x->v[a].def] : NULL;
    if (!i || i->kind != SGX_K_VEC || i->def != a || i->vf != (inv ? &sgx_v_aesdeclast : &sgx_v_aesenclast)) return 0;
    int key = x->v[i->in[1]].def;
    return key >= 0 && key < x->n && x->ins[key].kind == SGX_K_VEC && x->ins[key].vzero ? i->in[0] : 0;
}

/* the cryptographic extensions' instructions: the host's AES-NI, SHA-NI and SM4 where it has them, else the portable
   helpers (SM4: the Sail's); 0 for the rest */
static int vec_crypto(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31;
    int aes = (X->features & SGX_F_AES) != 0, sha = (X->features & SGX_F_SHA) != 0, r, a, b, c = 0;
    SnxU32 op;
    switch (insn->op) {
    case SNX_A64_OP_AESE:
    case SNX_A64_OP_AESD:
        a = vread(g, rd);
        b = vread(g, rn);
        if (aes) { /* the last round with a zero key: SubBytes and ShiftRows alone */
            r = op2(g, insn->op == SNX_A64_OP_AESD ? &sgx_v_aesdeclast : &sgx_v_aesenclast, vxor(g, a, b), vzero(g));
            break;
        }
        op = insn->op == SNX_A64_OP_AESD ? 0x21 : 0x20;
        goto call;
    case SNX_A64_OP_AESMC:
    case SNX_A64_OP_AESIMC:
        a = vread(g, rn);
        b = 0;
        if (aes) { /* MixColumns: a full round after an inverse last one, both with a zero key */
            int inv = insn->op == SNX_A64_OP_AESIMC, before = vaes_last(g, a, inv);
            r = before ? op2(g, inv ? &sgx_v_aesdec : &sgx_v_aesenc, before, vzero(g))
                : inv  ? sgx_v1(X, &sgx_v_aesimc, a, 0)
                       : op2(g, &sgx_v_aesenc, op2(g, &sgx_v_aesdeclast, a, vzero(g)), vzero(g));
            break;
        }
        op = insn->op == SNX_A64_OP_AESIMC ? 0x23 : 0x22;
        goto call;
    case SNX_A64_OP_SHA1C:
    case SNX_A64_OP_SHA1P:
    case SNX_A64_OP_SHA1M:
        op = insn->op == SNX_A64_OP_SHA1C ? 0 : insn->op == SNX_A64_OP_SHA1P ? 1 : 2;
        if (!sha) goto three;
        r = vsha1(g, (int)op, vread(g, rd), vread(g, rn), vread(g, rm));
        break;
    case SNX_A64_OP_SHA1SU0: /* (Vn:Vd's middle doublewords) ^ Vd ^ Vm */
        a = vread(g, rd);
        r = vxor(g, vxor(g, sgx_v2(X, &sgx_v_palignr, vread(g, rn), a, 8), a), vread(g, rm));
        break;
    case SNX_A64_OP_SHA1H: vwrite_scalar(g, rd, 2, vrolw(g, vread(g, rn), 30)); return 1;
    case SNX_A64_OP_SHA1SU1: /* each word (Vd's ^ Vn's next one) rotated by 1, the last XORed with the first's by 2 */
        r = vrolw(g, vxor(g, vread(g, rd), opi(g, &sgx_v_psrldq, vread(g, rn), 4)), 1);
        r = vxor(g, r, vrolw(g, opi(g, &sgx_v_pslldq, r, 12), 1));
        break;
    case SNX_A64_OP_SHA256H:
    case SNX_A64_OP_SHA256H2:
        op = insn->op == SNX_A64_OP_SHA256H ? 4 : 5;
        if (!sha) goto three;
        { /* two rounds twice on the state's halves in SHA-NI's order, the result's back in the guest's (dynarmic's) */
            int part1 = op == 4, m = vread(g, rm), in[4] = {0, 0, 0, 0}, first, second, r1, r2;
            a = vread(g, part1 ? rn : rd);
            b = vread(g, part1 ? rd : rn);
            first = sgx_v2(X, &sgx_v_shufps, a, b, 0xBB);
            second = sgx_v2(X, &sgx_v_shufps, a, b, 0x11);
            in[0] = first, in[1] = second, in[2] = m;
            r1 = sgx_vins(X, &sgx_v_sha256rnds2, 0, 1, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
            in[0] = second, in[1] = r1, in[2] = sgx_v1(X, &sgx_v_pshufd, m, 0xEE);
            r2 = sgx_vins(X, &sgx_v_sha256rnds2, 0, 1, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
            r = sgx_v2(X, &sgx_v_shufps, r2, r1, part1 ? 0xBB : 0x11);
        }
        break;
    case SNX_A64_OP_SHA256SU0:
        op = 0x12;
        if (!sha) goto two;
        r = op2(g, &sgx_v_sha256msg1, vread(g, rd), vread(g, rn));
        break;
    case SNX_A64_OP_SHA256SU1:
        op = 6;
        if (!sha) goto three;
        a = vread(g, rm);
        r = vadd(g, 2, vread(g, rd), sgx_v2(X, &sgx_v_palignr, a, vread(g, rn), 4));
        r = op2(g, &sgx_v_sha256msg2, r, a);
        break;
    case SNX_A64_OP_SM4E: /* Vd's state by Vn's keys, and Vn's key by Vm's constants: SRC1's and SRC2's */
    case SNX_A64_OP_SM4EKEY:
        if (!(X->features & SGX_F_SM4)) return 0; /* the Sail's */
        a = vread(g, insn->op == SNX_A64_OP_SM4E ? rd : rn);
        b = vread(g, insn->op == SNX_A64_OP_SM4E ? rn : rm);
        r = op3(g, insn->op == SNX_A64_OP_SM4E ? &sgx_v_vsm4rnds4 : &sgx_v_vsm4key4, a, b);
        break;
    default: return 0;
    three: c = vread(g, rm);
    two:
        a = vread(g, rd);
        b = vread(g, rn);
    call:
        r = sgx_vcold(X, sg_bool(g, 1), vzero(g), "snx_x64_crypto", a, b, c, op);
        break;
    }
    vwrite(g, rd, r, 1);
    return 1;
}

static int vec(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    if (insn->feature & SNX_A64_FEAT_FP16) return 0; /* half-precision lanes: the LLVM back end's */
    {
        int f = vec_fp(u, insn, word);
        if (!f) f = vec_fp_more(u, insn, word);
        if (!f) f = vec_crypto(u, insn, word);
        if (f) return f;
    }
    unsigned q = word >> 30 & 1, uns = word >> 29 & 1, size = word >> 22 & 3;
    unsigned rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31;
    int a, b, r;
    {
        int sc, sat = 0, flags = vshift_of(insn->op, &sc);
        if (flags >= 0 && size == 3 && !q && !sc) return -1;
        if (flags >= 0 && sc && size != 3 && !(flags & VSH_SAT)) return -1;
        /* one byte's or halfword's saturating shift: the Sail's in general-purpose registers, as quick */
        if (flags >= 0 && (size < 2 || (X->features & SGX_F_AVX2)) && !(sc && size < 2 && (flags & VSH_SAT))) {
            r = vshift(g, size, vread(g, rn), vread(g, rm), flags, &sat);
            if (flags & VSH_SAT) set_qc(g, sat, sc ? 1 << size : q ? 16 : 8);
            if (sc)
                vwrite_scalar(g, rd, size, r);
            else
                vwrite(g, rd, r, q);
            return 1;
        }
    }
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
    case SNX_A64_OP_CMEQ0_s:
    case SNX_A64_OP_CMGT0_s:
    case SNX_A64_OP_CMGE0_s:
    case SNX_A64_OP_CMLE0_s:
    case SNX_A64_OP_CMLT0_s: {
        int sc = insn->op == SNX_A64_OP_CMEQ0_s || insn->op == SNX_A64_OP_CMGT0_s || insn->op == SNX_A64_OP_CMGE0_s ||
                 insn->op == SNX_A64_OP_CMLE0_s || insn->op == SNX_A64_OP_CMLT0_s;
        if (sc && size != 3) return -1;
        if (size == 3 && !q && !sc) return -1;
        a = vread(g, rn);
        switch (insn->op) {
        case SNX_A64_OP_CMEQ0_v:
        case SNX_A64_OP_CMEQ0_s: r = veq(g, size, a, vzero(g)); break;
        case SNX_A64_OP_CMGT0_v:
        case SNX_A64_OP_CMGT0_s: r = vgt(g, size, a, vzero(g), 0); break;
        case SNX_A64_OP_CMGE0_v:
        case SNX_A64_OP_CMGE0_s: r = vnot(g, vgt(g, size, vzero(g), a, 0)); break;
        case SNX_A64_OP_CMLE0_v:
        case SNX_A64_OP_CMLE0_s: r = vnot(g, vgt(g, size, a, vzero(g), 0)); break;
        default: r = vgt(g, size, vzero(g), a, 0); break;
        }
        vwrite(g, rd, r, sc ? 0 : q);
        return 1;
    }
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
    case SNX_A64_OP_ADDP_s: /* ADDP Dd, Vn.2D: the two doublewords' sum */
        if (size != 3) return -1;
        a = vread(g, rn);
        vwrite(g, rd, vadd(g, 3, a, sgx_v1(X, &sgx_v_pshufd, a, 0xEE)), 0);
        return 1;
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
        vwrite(g, rd, vzip(g, kind, size, q, vread(g, rn), vread(g, rm)), q);
        return 1;
    }
    case SNX_A64_OP_EXT_d:
    case SNX_A64_OP_EXT_q: {
        int idx[16], i, imm4 = (int)(word >> 11 & 15), len = q ? 16 : 8;
        if (!q && imm4 >= 8) return -1;
        for (i = 0; i < 16; ++i) idx[i] = i < len ? (imm4 + i < len ? imm4 + i : 16 + imm4 + i - len) : -1;
        vwrite(g, rd, vshuf(g, vread(g, rn), vread(g, rm), idx, len), q);
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
        vwrite(g, rd, vshuf(g, vread(g, rn), 0, idx, len), q);
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
    case SNX_A64_OP_RBIT_v: { /* each byte's bits reversed: GFNI, or a table of the nibbles' (PSHUFB) */
        a = vread(g, rn);
        if (vgfni(g))
            r = vgf(g, a, VGF_REVERSE, 0);
        else
            r = vor(g,
                    op2(g, &sgx_v_pshufb, kconst(g, 0xE060A020C0408000ull, 0xF070B030D0509010ull),
                        vand(g, a, vsplat(g, 0, 15))),
                    op2(g, &sgx_v_pshufb, kconst(g, 0x0E060A020C040800ull, 0x0F070B030D050901ull),
                        vand(g, opi(g, &sgx_v_psrlw_i, a, 4), vsplat(g, 0, 15))));
        vwrite(g, rd, r, q);
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
    case SNX_A64_OP_RADDHN:
    case SNX_A64_OP_RSUBHN:
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        r = insn->op == SNX_A64_OP_ADDHN || insn->op == SNX_A64_OP_RADDHN ? vadd(g, size + 1, a, b)
                                                                          : vsub(g, size + 1, a, b);
        if (insn->op == SNX_A64_OP_RADDHN || insn->op == SNX_A64_OP_RSUBHN)
            r = vadd(g, size + 1, r, vsplat(g, size + 1, smin_of(size)));
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
    case SNX_A64_OP_SRHADD_v: /* (a | b) - ((a ^ b) >> 1); bytes and halfwords PAVG's, their signs flipped */
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        if (size < 2) {
            int flip = vsplat(g, size, smin_of(size));
            r = vxor(g, op2(g, size ? &sgx_v_pavgw : &sgx_v_pavgb, vxor(g, a, flip), vxor(g, b, flip)), flip);
        } else
            r = vsub(g, size, vor(g, a, b), vshr(g, size, vxor(g, a, b), 1, 1));
        vwrite(g, rd, r, q);
        return 1;
    /* saturating, QC where a lane saturates */
    case SNX_A64_OP_SQADD_v:
    case SNX_A64_OP_UQADD_v:
    case SNX_A64_OP_SQSUB_v:
    case SNX_A64_OP_UQSUB_v: {
        int sat;
        if (size == 3 && !q) return -1;
        r = vqaddsub(g, size, vread(g, rn), vread(g, rm), insn->op == SNX_A64_OP_UQADD_v || insn->op == SNX_A64_OP_UQSUB_v,
                     insn->op == SNX_A64_OP_SQSUB_v || insn->op == SNX_A64_OP_UQSUB_v, &sat);
        set_qc(g, sat, q ? 16 : 8);
        vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SQSHL_vi:
    case SNX_A64_OP_UQSHL_vi:
    case SNX_A64_OP_SQSHLU_vi:
    case SNX_A64_OP_SQSHL_si:
    case SNX_A64_OP_UQSHL_si:
    case SNX_A64_OP_SQSHLU_si: {
        unsigned sz, left, right;
        int sat, scalar = insn->op == SNX_A64_OP_SQSHL_si || insn->op == SNX_A64_OP_UQSHL_si ||
                          insn->op == SNX_A64_OP_SQSHLU_si;
        if (shift_imm(word, &sz, &left, &right) < 0 || (sz == 3 && !q && !scalar)) return -1;
        r = vqshl_imm(g, sz, vread(g, rn), left, insn->op == SNX_A64_OP_UQSHL_vi || insn->op == SNX_A64_OP_UQSHL_si,
                      insn->op == SNX_A64_OP_SQSHLU_vi || insn->op == SNX_A64_OP_SQSHLU_si, &sat);
        set_qc(g, sat, scalar ? 1 << sz : q ? 16 : 8);
        if (scalar)
            vwrite_scalar(g, rd, sz, r);
        else
            vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SUQADD_v:
    case SNX_A64_OP_USQADD_v:
    case SNX_A64_OP_SUQADD_s:
    case SNX_A64_OP_USQADD_s: {
        int sat, scalar = insn->op == SNX_A64_OP_SUQADD_s || insn->op == SNX_A64_OP_USQADD_s;
        if (size == 3 && !q && !scalar) return -1;
        r = vqaddmixed(g, size, vread(g, rd), vread(g, rn),
                       insn->op == SNX_A64_OP_USQADD_v || insn->op == SNX_A64_OP_USQADD_s, &sat);
        set_qc(g, sat, scalar ? 1 << size : q ? 16 : 8);
        if (scalar)
            vwrite_scalar(g, rd, size, r);
        else
            vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SQABS_v:
    case SNX_A64_OP_SQNEG_v:
    case SNX_A64_OP_SQABS_s:
    case SNX_A64_OP_SQNEG_s: {
        int sat, scalar = insn->op == SNX_A64_OP_SQABS_s || insn->op == SNX_A64_OP_SQNEG_s;
        if (size == 3 && !q && !scalar) return -1;
        r = vqabsneg(g, size, vread(g, rn), insn->op == SNX_A64_OP_SQNEG_v || insn->op == SNX_A64_OP_SQNEG_s, &sat);
        set_qc(g, sat, scalar ? 1 << size : q ? 16 : 8);
        if (scalar)
            vwrite_scalar(g, rd, size, r);
        else
            vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SQXTN_v:
    case SNX_A64_OP_UQXTN_v:
    case SNX_A64_OP_SQXTUN_v: {
        int sat;
        if (size == 3) return -1;
        r = vqnarrow(g, size, vread(g, rn), insn->op == SNX_A64_OP_SQXTN_v ? 0 : insn->op == SNX_A64_OP_UQXTN_v ? 1 : 2, &sat);
        set_qc(g, sat, 16);
        vwrite_narrow(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SQSHRN_v:
    case SNX_A64_OP_UQSHRN_v:
    case SNX_A64_OP_SQSHRUN_v: {
        unsigned sz, left, right;
        int sat;
        if (shift_imm(word, &sz, &left, &right) < 0 || sz == 3) return -1;
        a = vshr(g, sz + 1, vread(g, rn), right, insn->op != SNX_A64_OP_UQSHRN_v);
        r = vqnarrow(g, sz, a, insn->op == SNX_A64_OP_SQSHRN_v ? 0 : insn->op == SNX_A64_OP_UQSHRN_v ? 1 : 2, &sat);
        set_qc(g, sat, 16);
        vwrite_narrow(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SQDMULH_v:
    case SNX_A64_OP_SQRDMULH_v:
    case SNX_A64_OP_SQDMULH_vi:
    case SNX_A64_OP_SQRDMULH_vi:
    case SNX_A64_OP_SQDMULH_s:
    case SNX_A64_OP_SQRDMULH_s:
    case SNX_A64_OP_SQDMULH_si:
    case SNX_A64_OP_SQRDMULH_si: {
        int sat, round = insn->op == SNX_A64_OP_SQRDMULH_v || insn->op == SNX_A64_OP_SQRDMULH_vi ||
                         insn->op == SNX_A64_OP_SQRDMULH_s || insn->op == SNX_A64_OP_SQRDMULH_si;
        int scalar = insn->op == SNX_A64_OP_SQDMULH_s || insn->op == SNX_A64_OP_SQRDMULH_s ||
                     insn->op == SNX_A64_OP_SQDMULH_si || insn->op == SNX_A64_OP_SQRDMULH_si;
        if (size != 1 && size != 2) return -1;
        b = insn->op == SNX_A64_OP_SQDMULH_vi || insn->op == SNX_A64_OP_SQRDMULH_vi ||
                    insn->op == SNX_A64_OP_SQDMULH_si || insn->op == SNX_A64_OP_SQRDMULH_si
                ? velement(g, size, word)
                : vread(g, rm);
        r = vqdmulh(g, size, vread(g, rn), b, round, &sat);
        set_qc(g, sat, scalar ? 1 << size : q ? 16 : 8);
        if (scalar)
            vwrite_scalar(g, rd, size, r);
        else
            vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_SQDMULL_v:
    case SNX_A64_OP_SQDMLAL_v:
    case SNX_A64_OP_SQDMLSL_v:
    case SNX_A64_OP_SQDMULL_vi:
    case SNX_A64_OP_SQDMLAL_vi:
    case SNX_A64_OP_SQDMLSL_vi:
    case SNX_A64_OP_SQDMULL_si:
    case SNX_A64_OP_SQDMLAL_si:
    case SNX_A64_OP_SQDMLSL_si: { /* a scalar by register (bit 24 clear) is of element 0, its lane the lowest */
        int sat, sat2,
            scalar = insn->op == SNX_A64_OP_SQDMULL_si || insn->op == SNX_A64_OP_SQDMLAL_si ||
                     insn->op == SNX_A64_OP_SQDMLSL_si;
        int sub =
            insn->op == SNX_A64_OP_SQDMLSL_v || insn->op == SNX_A64_OP_SQDMLSL_vi || insn->op == SNX_A64_OP_SQDMLSL_si;
        int acc = sub || insn->op == SNX_A64_OP_SQDMLAL_v || insn->op == SNX_A64_OP_SQDMLAL_vi ||
                  insn->op == SNX_A64_OP_SQDMLAL_si;
        if (size != 1 && size != 2) return -1;
        b = word >> 24 & 1 ? velement(g, size, word) : vread(g, rm);
        r = vqdmull(g, size, vread(g, rn), b, scalar ? 0 : (int)q, &sat);
        if (acc) {
            r = vqaddsub(g, size + 1, vread(g, rd), r, 0, sub, &sat2);
            sat = vor(g, sat, sat2);
        }
        set_qc(g, sat, scalar ? 2 << size : 16);
        if (scalar)
            vwrite_scalar(g, rd, size + 1, r);
        else
            vwrite(g, rd, r, 1);
        return 1;
    }
    case SNX_A64_OP_SQSHL_v: {
        int sat;
        if (size == 3 && !q) return -1;
        r = vqshl_reg(g, size, (int)q, vread(g, rn), vread(g, rm), &sat);
        set_qc(g, sat, q ? 16 : 8);
        vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_NOT_v: vwrite(g, rd, vnot(g, vread(g, rn)), q); return 1;
    case SNX_A64_OP_UADDLV:
    case SNX_A64_OP_SADDLV:
        if (size == 3 || (size == 2 && !q)) return -1;
        r = vaddlv(g, size, (int)q, vread(g, rn), insn->op == SNX_A64_OP_SADDLV);
        vwrite_scalar(g, rd, size + 1, r);
        return 1;
    case SNX_A64_OP_UADDLP_v:
    case SNX_A64_OP_SADDLP_v:
    case SNX_A64_OP_UADALP_v:
    case SNX_A64_OP_SADALP_v: {
        int acc = insn->op == SNX_A64_OP_UADALP_v || insn->op == SNX_A64_OP_SADALP_v;
        if (size == 3) return -1;
        r = vaddlp(g, size, vread(g, rn), insn->op == SNX_A64_OP_SADDLP_v || insn->op == SNX_A64_OP_SADALP_v);
        if (acc) r = vadd(g, size + 1, vread(g, rd), r);
        vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_UHSUB_v:
    case SNX_A64_OP_SHSUB_v: /* ((a ^ b) >> 1) - (~a & b) */
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        r = vshr(g, size, vxor(g, a, b), 1, insn->op == SNX_A64_OP_SHSUB_v);
        vwrite(g, rd, vsub(g, size, r, vandn(g, a, b)), q);
        return 1;
    case SNX_A64_OP_SRI_v:
    case SNX_A64_OP_SRI_s:
    case SNX_A64_OP_SLI_v:
    case SNX_A64_OP_SLI_s: {
        unsigned sz, left, right;
        int scalar = insn->op == SNX_A64_OP_SRI_s || insn->op == SNX_A64_OP_SLI_s;
        int left_in = insn->op == SNX_A64_OP_SLI_v || insn->op == SNX_A64_OP_SLI_s;
        SnxU64 ones;
        if (shift_imm(word, &sz, &left, &right) < 0 || (sz == 3 && !q && !scalar) || (scalar && sz != 3)) return -1;
        ones = sg_ones(8u << sz);
        a = vread(g, rn);
        /* the bits of the destination the shift leaves alone: its low left (SLI) or its top right (SRI) */
        ones = left_in ? (left ? sg_ones(left) : 0) : ones & ~(right < (8u << sz) ? ones >> right : 0);
        r = left_in ? vshl(g, sz, a, left) : vshr(g, sz, a, right, 0);
        r = vor(g, r, vand(g, vread(g, rd), vsplat(g, sz, ones)));
        vwrite(g, rd, r, scalar ? 0 : q);
        return 1;
    }
    case SNX_A64_OP_SRSHR_v:
    case SNX_A64_OP_URSHR_v:
    case SNX_A64_OP_SRSRA_v:
    case SNX_A64_OP_URSRA_v:
    case SNX_A64_OP_SRSHR_s:
    case SNX_A64_OP_URSHR_s:
    case SNX_A64_OP_SRSRA_s:
    case SNX_A64_OP_URSRA_s: {
        unsigned sz, left, right;
        int scalar = insn->op == SNX_A64_OP_SRSHR_s || insn->op == SNX_A64_OP_URSHR_s ||
                     insn->op == SNX_A64_OP_SRSRA_s || insn->op == SNX_A64_OP_URSRA_s;
        int sign = insn->op == SNX_A64_OP_SRSHR_v || insn->op == SNX_A64_OP_SRSRA_v || insn->op == SNX_A64_OP_SRSHR_s ||
                   insn->op == SNX_A64_OP_SRSRA_s;
        int acc = insn->op == SNX_A64_OP_SRSRA_v || insn->op == SNX_A64_OP_URSRA_v || insn->op == SNX_A64_OP_SRSRA_s ||
                  insn->op == SNX_A64_OP_URSRA_s;
        if (shift_imm(word, &sz, &left, &right) < 0 || (sz == 3 && !q && !scalar) || (scalar && sz != 3)) return -1;
        r = vrshr(g, sz, vread(g, rn), right, sign);
        if (acc) r = vadd(g, sz, vread(g, rd), r);
        vwrite(g, rd, r, scalar ? 0 : q);
        return 1;
    }
    case SNX_A64_OP_RSHRN_v: {
        unsigned sz, left, right;
        if (shift_imm(word, &sz, &left, &right) < 0 || sz == 3) return -1;
        vwrite_narrow(g, rd, vnarrow(g, sz, vrshr(g, sz + 1, vread(g, rn), right, 0), 0), q);
        return 1;
    }
    case SNX_A64_OP_UADDL_v:
    case SNX_A64_OP_SADDL_v:
    case SNX_A64_OP_USUBL_v:
    case SNX_A64_OP_SSUBL_v: {
        int sign = insn->op == SNX_A64_OP_SADDL_v || insn->op == SNX_A64_OP_SSUBL_v;
        int sub = insn->op == SNX_A64_OP_USUBL_v || insn->op == SNX_A64_OP_SSUBL_v;
        if (size == 3) return -1;
        a = vwiden(g, size, vread(g, rn), (int)q, sign);
        b = vwiden(g, size, vread(g, rm), (int)q, sign);
        vwrite(g, rd, sub ? vsub(g, size + 1, a, b) : vadd(g, size + 1, a, b), 1);
        return 1;
    }
    case SNX_A64_OP_USUBW:
    case SNX_A64_OP_SSUBW:
        if (size == 3) return -1;
        b = vwiden(g, size, vread(g, rm), (int)q, insn->op == SNX_A64_OP_SSUBW);
        vwrite(g, rd, vsub(g, size + 1, vread(g, rn), b), 1);
        return 1;
    case SNX_A64_OP_UABD_v:
    case SNX_A64_OP_SABD_v:
    case SNX_A64_OP_UABA_v:
    case SNX_A64_OP_SABA_v: {
        int un = insn->op == SNX_A64_OP_UABD_v || insn->op == SNX_A64_OP_UABA_v;
        if (size == 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        r = vsub(g, size, vmax(g, size, a, b, un, 0), vmax(g, size, a, b, un, 1));
        if (insn->op == SNX_A64_OP_UABA_v || insn->op == SNX_A64_OP_SABA_v) r = vadd(g, size, vread(g, rd), r);
        vwrite(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_UABDL_v:
    case SNX_A64_OP_SABDL_v:
    case SNX_A64_OP_UABAL_v:
    case SNX_A64_OP_SABAL_v: {
        int sign = insn->op == SNX_A64_OP_SABDL_v || insn->op == SNX_A64_OP_SABAL_v;
        if (size == 3) return -1;
        r = vabs(g, size + 1, vsub(g, size + 1, vwiden(g, size, vread(g, rn), (int)q, sign),
                                    vwiden(g, size, vread(g, rm), (int)q, sign)));
        if (insn->op == SNX_A64_OP_UABAL_v || insn->op == SNX_A64_OP_SABAL_v) r = vadd(g, size + 1, vread(g, rd), r);
        vwrite(g, rd, r, 1);
        return 1;
    }
    case SNX_A64_OP_CLZ_v:
    case SNX_A64_OP_CLS_v:
        if (size == 3) return -1;
        a = vread(g, rn);
        if (insn->op == SNX_A64_OP_CLS_v) { /* the bits like the sign, less the sign: those of a ^ (its sign spread) */
            a = vclz(g, size, vxor(g, a, vshr(g, size, a, (8u << size) - 1, 1)));
            vwrite(g, rd, vsub(g, size, a, vsplat(g, size, 1)), q);
            return 1;
        }
        vwrite(g, rd, vclz(g, size, a), q);
        return 1;
    /* doublewords, scalars */
    case SNX_A64_OP_ADD_s:
    case SNX_A64_OP_SUB_s:
    case SNX_A64_OP_CMEQ_s:
    case SNX_A64_OP_CMGT_s:
    case SNX_A64_OP_CMGE_s:
    case SNX_A64_OP_CMHI_s:
    case SNX_A64_OP_CMHS_s:
    case SNX_A64_OP_CMTST_s:
        if (size != 3) return -1;
        a = vread(g, rn);
        b = vread(g, rm);
        switch (insn->op) {
        case SNX_A64_OP_ADD_s: r = vadd(g, 3, a, b); break;
        case SNX_A64_OP_SUB_s: r = vsub(g, 3, a, b); break;
        case SNX_A64_OP_CMEQ_s: r = veq(g, 3, a, b); break;
        case SNX_A64_OP_CMGT_s: r = vgt(g, 3, a, b, 0); break;
        case SNX_A64_OP_CMHI_s: r = vgt(g, 3, a, b, 1); break;
        case SNX_A64_OP_CMGE_s: r = vnot(g, vgt(g, 3, b, a, 0)); break;
        case SNX_A64_OP_CMHS_s: r = vnot(g, vgt(g, 3, b, a, 1)); break;
        default: r = vnot(g, veq(g, 3, vand(g, a, b), vzero(g))); break;
        }
        vwrite(g, rd, r, 0);
        return 1;
    case SNX_A64_OP_SQADD_s:
    case SNX_A64_OP_UQADD_s:
    case SNX_A64_OP_SQSUB_s:
    case SNX_A64_OP_UQSUB_s: {
        int sat, sub = insn->op == SNX_A64_OP_SQSUB_s || insn->op == SNX_A64_OP_UQSUB_s;
        r = vqaddsub(g, size, vread(g, rn), vread(g, rm),
                     insn->op == SNX_A64_OP_UQADD_s || insn->op == SNX_A64_OP_UQSUB_s, sub, &sat);
        set_qc(g, sat, 1 << size);
        vwrite_scalar(g, rd, size, r);
        return 1;
    }
    case SNX_A64_OP_SQXTN_s:
    case SNX_A64_OP_UQXTN_s:
    case SNX_A64_OP_SQXTUN_s: {
        int sat, to = insn->op == SNX_A64_OP_SQXTN_s ? 0 : insn->op == SNX_A64_OP_UQXTN_s ? 1 : 2;
        if (size == 3) return -1;
        r = vqnarrow(g, size, vread(g, rn), to, &sat);
        set_qc(g, sat, 2 << size);
        vwrite_scalar(g, rd, size, r);
        return 1;
    }
    case SNX_A64_OP_SQRSHRN_v:
    case SNX_A64_OP_UQRSHRN_v:
    case SNX_A64_OP_SQRSHRUN_v:
    case SNX_A64_OP_SQSHRN_si:
    case SNX_A64_OP_UQSHRN_si:
    case SNX_A64_OP_SQSHRUN_si:
    case SNX_A64_OP_SQRSHRN_si:
    case SNX_A64_OP_UQRSHRN_si:
    case SNX_A64_OP_SQRSHRUN_si: {
        unsigned sz, left, right;
        int sat, scalar = 0, round = 0, to;
        switch (insn->op) {
        case SNX_A64_OP_SQSHRN_si: scalar = 1, to = 0; break;
        case SNX_A64_OP_UQSHRN_si: scalar = 1, to = 1; break;
        case SNX_A64_OP_SQSHRUN_si: scalar = 1, to = 2; break;
        case SNX_A64_OP_SQRSHRN_si: scalar = 1, round = 1, to = 0; break;
        case SNX_A64_OP_UQRSHRN_si: scalar = 1, round = 1, to = 1; break;
        case SNX_A64_OP_SQRSHRUN_si: scalar = 1, round = 1, to = 2; break;
        case SNX_A64_OP_SQRSHRN_v: round = 1, to = 0; break;
        case SNX_A64_OP_UQRSHRN_v: round = 1, to = 1; break;
        default: round = 1, to = 2; break;
        }
        if (shift_imm(word, &sz, &left, &right) < 0 || sz == 3) return -1;
        a = round ? vrshr(g, sz + 1, vread(g, rn), right, to != 1) : vshr(g, sz + 1, vread(g, rn), right, to != 1);
        r = vqnarrow(g, sz, a, to, &sat);
        set_qc(g, sat, scalar ? 2 << sz : 16);
        if (scalar) vwrite_scalar(g, rd, sz, r);
        else
            vwrite_narrow(g, rd, r, q);
        return 1;
    }
    case SNX_A64_OP_PMULL_p64: /* PMULL, PMULL2 of a doubleword (q: the upper ones): the 128-bit carryless product */
        vwrite(g, rd, sgx_v2(X, &sgx_v_pclmulqdq, vread(g, rn), vread(g, rm), q ? 0x11 : 0x00), 1);
        return 1;
    default: return 0;
    }
}

#undef X
