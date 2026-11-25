/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's RISC back ends, their assembly (after mach.h): the frame and mach.h's hooks from risc.h's operations and the
   machine's frame. No vector register holds a value (SGX_HAS_VREGS 0), so the vector hooks are never reached;
   nor are the cold paths or the constant pool, which only vector code makes */

/* a step's or a block's frame: the machine's function frame (rk_fn_prologue, rk_fn_epilogue), its size patched in once
   the code is done (rk_fn_patch_frame) */
static inline void sgx_prologue(SgXAsm* a) {
    size_t at = rk_fn_prologue(a->b, 0);
    if (a->nframe < 256) a->frame_at[a->nframe++] = at;
}
static inline void sgx_epilogue(SgXAsm* a) { rk_fn_epilogue(a->b, 0); }
static inline void sgx_patch_frame(SgXAsm* a) {
    int k;
    for (k = 0; k < a->nframe; ++k)
        if (!rk_fn_patch_frame(a->b, a->frame_at[k], (long long)a->slots_max * 8))
            sg_fail("a frame too large for the machine's", a->slots_max, 0);
}

/* an immediate: any for a logical operation (the machine makes what its encoding can't hold), the 32-bit one sign
   extended the primitives take for an addition and a compare */
static inline int sgx_imm_ok(SgX* x, int kind, SnxU64 k, unsigned w) {
    SnxU64 m = w == 64 ? ~0ull : (1ull << w) - 1, sk;
    (void)x;
    if (kind <= SGX_IMM_LOGIC_XOR) return 1;
    k &= m;
    sk = w < 64 && (k >> (w - 1) & 1) ? k | ~m : k;
    return (long long)sk == (long long)(int)(SnxU32)sk;
}

static inline void sgx_mov_rr(SgXAsm* a, int dst, int src) { rk_mv(a->b, dst, src); }
/* the guest's state field at disp holds guest bytes (A64's vector registers: guest memory's order), not a number */
static inline int sgx_state_bytes(int disp);
static inline void sgx_mov_mem(SgXAsm* a, int load, int w, int reg, int base, int disp) {
    if (RK_BIG_ENDIAN && base == SGX_BASE && sgx_state_bytes(disp)) rk_gldst(a->b, load, w / 8, 0, reg, base, -1, disp);
    else
        rk_ldst(a->b, load, w / 8, 0, reg, base, -1, disp);
}
/* a flag's 0 or 1: its 32-bit field whole (sgx_store_flag's), whose low byte a big-endian host has at its end */
static inline void sgx_load8(SgXAsm* a, int d, int base, int disp) { rk_ldst(a->b, 1, 4, 0, d, base, -1, disp); }
/* the condition's 0 or 1 (risc_cond in RK_S1), stored as the field's 32 bits */
static inline void sgx_store_flag(SgXAsm* a, int at, int base, int disp, int cc) {
    (void)at;
    risc_cond(a->b, RK_S1, cc);
    rk_ldst(a->b, 0, 4, 0, RK_S1, base, -1, disp);
}

static inline void sgx_spill_x(SgXAsm* a, int to_x, int xr, int gr) {
    (void)a, (void)to_x, (void)xr, (void)gr;
    sg_fail("a spill to a vector register, which this machine keeps no value in", gr, xr);
}
static inline void sgx_xmov(SgXAsm* a, int d, int s) {
    (void)a;
    sg_fail("a vector register's copy on a machine without vector values", d, s);
}
static inline void sgx_xmem(SgXAsm* a, int store, int bytes, int xr, int base, int disp) {
    (void)a, (void)store, (void)bytes, (void)base, (void)disp;
    sg_fail("a vector register's slot on a machine without vector values", xr, 0);
}
static inline int sgx_xdown(SgX* x, int v, unsigned lo) {
    (void)x;
    sg_fail("a vector element on a machine without vector values", v, lo);
    return 0;
}
static inline int sgx_xconcat(SgX* x, int lo, int hi) {
    (void)x;
    sg_fail("a vector of doublewords on a machine without vector values", lo, hi);
    return 0;
}
static inline int sgx_xclear(SgX* x, int v, unsigned w) {
    (void)x;
    sg_fail("a vector value cleared on a machine without vector values", v, w);
    return 0;
}
static inline void sgx_assemble_cold(SgXAsm* a, int at) {
    (void)a;
    sg_fail("a cold path, which the RISC back ends do not make", at, 0);
}
static inline void sgx_cold_emit(SgXAsm* a) { (void)a; }
static inline void sgx_pool_emit(SgXAsm* a) {
    if (a->npool) sg_fail("a constant pool on a machine without vector values", a->npool, 0);
    a->nrips = 0;
}
