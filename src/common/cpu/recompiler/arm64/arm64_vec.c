/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AdvSIMD part of the arm64 back end (arm64_codegen.c includes this after mach_codegen.inc): the vector helpers
   the SIMD&FP structures' loads and stores take (a register whole, an element in and out of a general-purpose
   register, an element duplicated: UMOV, INS, DUP), on the region's SSA in V registers. vec() returns 1 made, 0 not
   one of these (the function goes to another back end), -1 UNDEF */

#define X sgx(g)

/* V[n] as a vector register */
static int vread(SgCtx* g, unsigned n) {
    SgD d = sg__V_read_bits(g, n, 127, 0);
    return sgx_xreg(X, &d);
}
/* V[d] = r, its high doubleword cleared where q is 0 */
static void vwrite(SgCtx* g, unsigned d, int r, int q) {
    if (!q) r = sgx_xclear(X, r, 64);
    sg__V_write(g, d, sgx_xval(128, r));
}
static int vzero(SgCtx* g) { return sgx_xconst(X, 0, 0); }

/* an element's imm5: its size (0 B ... 3 D) and index */
static SnxU32 v_imm5(unsigned size, int i) { return (SnxU32)i << (size + 1) | 1u << size; }
/* UMOV Wd or Xd, Vn.<T>[imm]: imm packs size << 8 | index */
SGA_VE(umov) {
    unsigned size = (unsigned)(imm >> 8) & 3;
    (void)cc;
    sga_w(b, sge_a64_umov_advsimd(r[0], r[1], v_imm5(size, (int)(imm & 0xFF)), size == 3));
}
/* INS Vd.<T>[imm], Wn or Xn (tied) */
SGA_VE(ins) {
    (void)cc;
    sga_w(b, sge_a64_ins_advsimd_gen(r[0], r[1], v_imm5((unsigned)(imm >> 8) & 3, (int)(imm & 0xFF))));
}
/* DUP Vd.<T>, Vn.<T>[imm], all 128 bits */
SGA_VE(dupe) {
    (void)cc;
    sga_w(b, sge_a64_dup_advsimd_elt_simd(r[0], r[1], v_imm5((unsigned)(imm >> 8) & 3, (int)(imm & 0xFF)), 1));
}
static const SgXVForm sga_v_umov_f = {sga_v_umov, 0, 2, 0, 1, 2, 2, 0, NULL};
static const SgXVForm sga_v_ins_f = {sga_v_ins, 0, 2, 0, 1, 2, 1, 0, NULL};
static const SgXVForm sga_v_dupe_f = {sga_v_dupe, 0, 2, 0, 1, 2, 3, 0, NULL};

/* element i of size (0 B ... 3 D) of a, zero extended in a general-purpose register */
static SgD lane_get(SgCtx* g, unsigned size, int a, int i) {
    int in[4] = {0, a, 0, 0};
    int r = sgx_vins(X, &sga_v_umov_f, 0, 0, in, (long long)size << 8 | i, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    X->v[r].zab = (unsigned char)(8u << size);
    return sgx_val(8u << size, r);
}
/* a with element i of size = v */
static int lane_put(SgCtx* g, unsigned size, int a, int i, SgD v) {
    int in[4] = {a, 0, 0, 0};
    v = sg_d_zext(g, v, 64);
    in[1] = sgx_reg(X, &v);
    return sgx_vins(X, &sga_v_ins_f, 0, 1, in, (long long)size << 8 | i, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
/* element e of size of a in every element */
static int vdup(SgCtx* g, unsigned size, int a, int e) {
    int in[4] = {0, a, 0, 0};
    return sgx_vins(X, &sga_v_dupe_f, 0, 0, in, (long long)size << 8 | e, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}

static int vec(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    (void)u, (void)insn, (void)word;
    return 0;
}

#undef X
