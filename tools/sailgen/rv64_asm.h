/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's riscv64 back end, its frame (after mach.h, before risc_asm.h): a step's or a block's prologue and
   epilogue, the spill slots' room patched in once the code is done */

/* ADDI sp, -32; SD ra, s0 and s1; ADDI s0, sp, 32; MV s1, a0 (the state); then sp -= the frame (LUI and ADDIW of
   t6, patched: frame_at the LUI; SUB sp) */
static inline void sgx_prologue(SgXAsm* a) {
    SgXBuf* b = a->b;
    rv_w(b, sge_rv64_addi(RV_SP, RV_SP, rv_i12(-32)));
    rv_w(b, sge_rv64_sd(RV_RA, RV_SP, 24));
    rv_w(b, sge_rv64_sd(RV_S0, RV_SP, 16));
    rv_w(b, sge_rv64_sd(SGX_BASE, RV_SP, 8));
    rv_w(b, sge_rv64_addi(RV_S0, RV_SP, 32));
    rk_mv(b, SGX_BASE, sgx_arg_regs[0]);
    if (a->nframe < 256) a->frame_at[a->nframe++] = b->n;
    rv_w(b, sge_rv64_lui(RK_SA, 0));
    rv_w(b, sge_rv64_addiw(RK_SA, RK_SA, 0));
    rv_w(b, sge_rv64_sub(RV_SP, RV_SP, RK_SA));
}
/* sp back from s0 (no size to patch); the saved registers; ADDI sp, 32 */
static inline void sgx_epilogue(SgXAsm* a) {
    SgXBuf* b = a->b;
    rv_w(b, sge_rv64_addi(RV_SP, RV_S0, rv_i12(-32)));
    rv_w(b, sge_rv64_ld(SGX_BASE, RV_SP, 8));
    rv_w(b, sge_rv64_ld(RV_S0, RV_SP, 16));
    rv_w(b, sge_rv64_ld(RV_RA, RV_SP, 24));
    rv_w(b, sge_rv64_addi(RV_SP, RV_SP, 32));
}
/* the frame's size (the slots, 16-aligned) into each prologue's LUI and ADDIW */
static inline void sgx_patch_frame(SgXAsm* a) {
    long long size = ((long long)a->slots_max * 8 + 15) & ~15ll, lo = (long long)((SnxU64)size << 52) >> 52;
    SnxU32 hi20 = (SnxU32)(((size + 0x800) >> 12) & 0xfffff);
    int k;
    for (k = 0; k < a->nframe; ++k) {
        size_t at = a->frame_at[k];
        if (at + 8 > a->b->cap) continue;
        rv_word_put(a->b, at, sge_rv64_lui(RK_SA, hi20));
        rv_word_put(a->b, at + 4, sge_rv64_addiw(RK_SA, RK_SA, rv_i12(lo)));
    }
}
