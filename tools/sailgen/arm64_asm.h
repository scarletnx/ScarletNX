/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's arm64 back end, its assembly (after mach.h, arm64.h's README): mach.h's hooks in the host's own A64 (the
   moves the register allocation makes, the vector registers' slots, a flag's store), the literal pool and the
   frame. Each word is an encoder's of the A64 Sail (a64_enc.inc) */

/* mach.h's: an AND's, ORR's or EOR's bitmask, an addition's or a compare's imm12. The value is the 32-bit immediate
   sign extended the primitives take for an addition (mach.h's sgx_const(32, k)), so it must be one */
static inline int sgx_imm_ok(SgX* x, int kind, SnxU64 k, unsigned w) {
    unsigned n, r, s, i12, sh;
    int sub;
    SnxU64 m = w == 64 ? ~0ull : (1ull << w) - 1, sk;
    (void)x;
    if (kind <= SGX_IMM_LOGIC_XOR) return sga_bitmask(k, w, &n, &r, &s);
    k &= m;
    sk = w < 64 && (k >> (w - 1) & 1) ? k | ~m : k;
    if ((long long)sk != (long long)(int)(SnxU32)sk) return 0;
    return sga_addimm(sk, w, &sub, &i12, &sh);
}

/* ORR Xd, XZR, Xm (ADD Xd, Xn, #0 where SP is one) */
static inline void sgx_mov_rr(SgXAsm* a, int dst, int src) {
    if (dst == src) return;
    if (dst == 31 || src == 31) sga_w(a->b, sge_a64_add_addsub_imm((SnxU32)dst, (SnxU32)src, 0, 0, 1));
    else
        sga_w(a->b, sge_a64_orr_log_shift((SnxU32)dst, SGA_ZR, 0, (SnxU32)src, 0, 1));
}
static inline void sgx_mov_mem(SgXAsm* a, int load, int w, int reg, int base, int disp) {
    sga_ldst(a->b, load, w / 8, reg, base, -1, base == SGX_FRAME ? disp + a->frame_off : disp);
}
/* FMOV Dx, Xg and FMOV Xg, Dx: a spill to a vector register */
static inline void sgx_spill_x(SgXAsm* a, int to_x, int xr, int gr) {
    if (to_x) sga_w(a->b, sge_a64_fmov_float_gen((SnxU32)xr, (SnxU32)gr, 7, 0, 1, 1));
    else
        sga_w(a->b, sge_a64_fmov_float_gen((SnxU32)gr, (SnxU32)xr, 6, 0, 1, 1));
}
/* LDRB: a flag sgx_store_flag stored */
static inline void sgx_load8(SgXAsm* a, int d, int base, int disp) { sga_ldst(a->b, 1, 1, d, base, -1, disp); }
/* CSET W16 of the condition, stored as the field's 32 bits */
static inline void sgx_store_flag(SgXAsm* a, int at, int base, int disp, int cc) {
    (void)at;
    sga_w(a->b, sge_a64_csinc(SGA_X16, SGA_ZR, (SnxU32)cc ^ 1, SGA_ZR, 0));
    sga_ldst(a->b, 0, 4, SGA_X16, base, -1, disp);
}
/* ORR Vd.16B, Vn.16B, Vn.16B */
static inline void sgx_xmov(SgXAsm* a, int d, int s) {
    if (d != s) sga_w(a->b, sge_a64_orr_advsimd_reg((SnxU32)d, (SnxU32)s, (SnxU32)s, 1));
}
/* STR and LDR of Q or D: the frame's slots */
static inline void sgx_xmem(SgXAsm* a, int store, int bytes, int xr, int base, int disp) {
    sga_fp_ldst(a->b, !store, bytes, xr, base, -1, base == SGX_FRAME ? disp + a->frame_off : disp);
}
static inline int sgx_xdown(SgX* x, int v, unsigned lo) { return sgx_v1(x, &sgx_v_down, v, lo); }
static inline int sgx_xconcat(SgX* x, int lo, int hi) {
    int in[4] = {0, lo, hi, 0};
    return sgx_vins(x, &sgx_v_concat, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
static inline int sgx_xclear(SgX* x, int v, unsigned w) {
    if (w >= 128) return v;
    if (w != 64 && w != 32 && w != 16 && w != 8) sg_fail("a vector value cleared above a width not of 8 to 64 bits", w, 0);
    return sgx_v1(x, &sgx_v_clear, v, w);
}

/* the cold paths: none yet (no vector or floating-point inline sequence of this back end goes to one) */
static inline void sgx_assemble_cold(SgXAsm* a, int at) {
    (void)a;
    sg_fail("a cold path, which the arm64 back end does not make yet", at, 0);
}
static inline void sgx_cold_emit(SgXAsm* a) { (void)a; }

/* the literal pool after the code, 16-aligned, and each LDR (literal)'s imm19 (words from it to its entry) */
static inline void sgx_pool_emit(SgXAsm* a) {
    SgXBuf* b = a->b;
    size_t at;
    int k;
    if (!a->npool) {
        a->nrips = 0;
        return;
    }
    while (b->n % 16) sga_w(b, sge_a64_brk(0));
    at = b->n;
    for (k = 0; k < a->npool; ++k) {
        sgx_imm(b, (long long)a->pool[k][0], 8);
        sgx_imm(b, (long long)a->pool[k][1], 8);
    }
    for (k = 0; k < a->nrips; ++k) {
        size_t p = a->rips[k].at;
        long long d = ((long long)(at + 16 * (size_t)a->rips[k].idx) - (long long)p) / 4;
        if (d >= (1 << 18) || d < -(1 << 18)) sg_fail("a literal past LDR's reach", d, 0);
        if (p + 4 <= b->cap) {
            SnxU32 w = (SnxU32)b->p[p] | (SnxU32)b->p[p + 1] << 8 | (SnxU32)b->p[p + 2] << 16 | (SnxU32)b->p[p + 3] << 24;
            w = (w & ~(0x7FFFFu << 5)) | ((SnxU32)d & 0x7FFFFu) << 5;
            b->p[p] = (unsigned char)w, b->p[p + 1] = (unsigned char)(w >> 8), b->p[p + 2] = (unsigned char)(w >> 16);
            b->p[p + 3] = (unsigned char)(w >> 24);
        }
    }
    a->npool = a->nrips = 0;
}

/* the word at at, an ADD or a SUB (bit 30) of an immediate, the frame's size: the same one of i12, shifted or not */
static inline void sga_frame_imm(SgXBuf* b, size_t at, unsigned i12, unsigned sh) {
    SnxU32 w;
    if (at + 4 > b->cap) return;
    w = (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 | (SnxU32)b->p[at + 3] << 24;
    w = (w >> 30 & 1) ? sge_a64_sub_addsub_imm(w & 31, w >> 5 & 31, i12, sh, 1)
                      : sge_a64_add_addsub_imm(w & 31, w >> 5 & 31, i12, sh, 1);
    b->p[at] = (unsigned char)w, b->p[at + 1] = (unsigned char)(w >> 8), b->p[at + 2] = (unsigned char)(w >> 16);
    b->p[at + 3] = (unsigned char)(w >> 24);
}
/* the bytes of the frame's slots, 16-aligned */
static inline unsigned sga_frame_size(const SgXAsm* a) { return ((unsigned)a->slots_max * 8 + 15) & ~15u; }
/* a frame of a page or more, which Windows' prologue reads first (sga_stack_probe) */
#define SGA_FRAME_PROBED(size) ((size) > 4095)
/* the frame's size, patched into the prologue's SUB SP and every epilogue's ADD SP (frame_at: the word), as an imm12 or
   (past 4095 bytes) an imm12 of 4 KiB units */
static inline void sgx_patch_frame(SgXAsm* a) {
    int k;
    unsigned size = sga_frame_size(a), sh = size > 4095, i12 = sh ? (size + 4095) >> 12 : size;
    if (i12 > 4095) sg_fail("a frame past 16 MiB", size, 0);
    for (k = 0; k < a->nframe; ++k) sga_frame_imm(a->b, a->frame_at[k], i12, sh);
}
/* a frame of a page or more on Windows, before its SUB SP: a byte of each page down from SP read, nearest first, so
   each read meets the guard page or one already committed (the system commits a thread's stack by it). X16 and X17 are
   its own. MOV X17, SP; SUB X16, SP, #frame (patched); loop: SUB X17, X17, #4096; LDRB WZR, [X17]; CMP X17, X16;
   B.HI loop. The SUB's place, which frame_at takes (x64_elf.c's a64_prologue knows the words) */
static inline size_t sga_stack_probe(SgXBuf* b) {
    size_t at;
    sga_w(b, sge_a64_add_addsub_imm(SGA_X17, 31, 0, 0, 1));
    at = b->n;
    sga_w(b, sge_a64_sub_addsub_imm(SGA_X16, 31, 0, 0, 1));
    sga_w(b, sge_a64_sub_addsub_imm(SGA_X17, SGA_X17, 1, 1, 1));
    sga_w(b, sge_a64_ldrb_imm_immediate_unsigned(SGA_ZR, SGA_X17, 0));
    sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X17, 0, SGA_X16, 0, 1));
    sga_w(b, sge_a64_b_cond(8, (SnxU32)-3 & 0x7FFFF));
    return at;
}
