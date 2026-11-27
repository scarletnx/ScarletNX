/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's arm64 back end, A64's part of its own (after mach_a64.h, which keeps the guest's registers, its memory and
   its branches): the floating point, the exclusives' stores, the LSE atomics, CRC32, the barriers and the ABI.

   The floating point is the exact helpers' (mach_a64.h's SGX_FP_HELPERS) for every operation: the host computes
   Arm's IEEE arithmetic itself, but only under the guest's FPCR, which the runtime's own C would then run under too,
   so that is left for the code that switches FPCR around the calls out. The exclusives and atomics are the host's own load-exclusive and
   store-exclusive (or the LSE instruction), sequentially consistent as the LLVM back end's host atomics are: a
   store-exclusive is cpu.h's value-based monitor, a compare-and-swap from the value the load-exclusive read. The
   barriers are the guest's own DMB, DSB and ISB, and an acquire load and a release store a DMB beside them.

   The ABI is AAPCS64: the state's address comes in X0 and stays in X19 (the callee's, saved with X29 and X30 by the
   prologue), SP 16-aligned after the prologue's frame of spill slots, a call's result in X0 */

/* ---- the exclusives, the atomics and CRC32: the machine's inline sequences (SGX_K_MACHINE) ----
   Their inputs in fixed registers: the guest memory's base X2, the address X3, the values X4 and X5 (a CAS's expected
   X4, desired X5; a CASP's X4:X5 and X6:X7); their result in X0 (and X1); X1, X6 to X8 their own besides */
#define SGA_M_CLOBBER (1u << 1 | 1u << 6 | 1u << 7 | 1u << 8)

static inline void sg_GuestStoreExclusive(SgCtx* g, SgD address, long long size, SgD value) {
    SgD in[4];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    if (size == 16) {
        static const signed char fix[4] = {2, 3, 4, 5};
        in[2] = sg_d_slice(g, value, 63, 0);
        in[3] = sg_d_slice(g, value, 127, 64);
        g->status = sgx_mach(g, SGX_M_STXP, in, fix, 4, 3, SGA_M_CLOBBER, 1, 32).v;
    } else {
        static const signed char fix[3] = {2, 3, 4};
        in[2] = sg_d_zext(g, value, 64);
        g->status = sgx_mach(g, SGX_M_STX, in, fix, 3, sgx_log2(size), SGA_M_CLOBBER, 1, 32).v;
    }
    sgx(g)->v[g->status].zab = 1;
}
static inline SgD sgx_rmw(SgCtx* g, int op, SgD address, SgD v) {
    static const signed char fix[3] = {2, 3, 4};
    unsigned w = v.w;
    SgD in[3];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    in[2] = sg_d_zext(g, v, 64);
    return sg_d_slice(g, sgx_mach(g, SGX_M_RMW, in, fix, 3, op << 2 | sgx_log2(w / 8), SGA_M_CLOBBER, 1, 64), w - 1, 0);
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
static inline SgD sg_GuestCompareSwap(SgCtx* g, SgD address, SgD expected, SgD desired) {
    unsigned w = expected.w;
    SgD in[6];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    if (w == 128) {
        static const signed char fix[6] = {2, 3, 4, 5, 6, 7};
        SgXIns* i;
        SgD lo;
        in[2] = sg_d_slice(g, expected, 63, 0);
        in[3] = sg_d_slice(g, expected, 127, 64);
        in[4] = sg_d_slice(g, desired, 63, 0);
        in[5] = sg_d_slice(g, desired, 127, 64);
        lo = sgx_mach(g, SGX_M_CASP, in, fix, 6, 0, SGA_M_CLOBBER, 2, 64);
        i = &sgx(g)->ins[sgx(g)->n - 1];
        return sg_d_concat(g, sgx_val(64, i->def2), lo);
    }
    {
        static const signed char fix[4] = {2, 3, 4, 5};
        in[2] = sg_d_zext(g, expected, 64);
        in[3] = sg_d_zext(g, desired, 64);
        return sg_d_slice(g, sgx_mach(g, SGX_M_CAS, in, fix, 4, sgx_log2(w / 8), SGA_M_CLOBBER, 1, 64), w - 1, 0);
    }
}
/* CRC32 and CRC32C: the host's own (FEAT_CRC32), else the exact helper (x64_helpers.c's snx_x64_crc32) */
static inline SgD sg_crc32(SgCtx* g, SgD acc_bits, SgD value_bits, long long size, SgS poly) {
    static const signed char fix[2] = {4, 5};
    SgD in[3];
    in[0] = sg_d_zext(g, acc_bits, 64);
    in[1] = sg_d_zext(g, value_bits, 64);
    if (!(sgx(g)->features & SGX_F_CRC32)) {
        in[2] = sgx_const(32, (SnxU64)(size / 8) | (SnxU64)(poly.v == 0x1EDC6F41u) << 4);
        return sg_d_slice(g, sgx_call(g, "snx_x64_crc32", 0, 0, in, 3, 64), 31, 0);
    }
    return sg_d_slice(g, sgx_mach(g, SGX_M_CRC, in, fix, 2, size / 8 | (poly.v == 0x1EDC6F41u) << 4, SGA_M_CLOBBER, 1, 64),
                      31, 0);
}

/* ---- barriers and hints: the guest's own ---- */

enum { SG_MBReqTypes_Reads, SG_MBReqTypes_Writes, SG_MBReqTypes_All };
static inline void sg_DataMemoryBarrier(SgCtx* g, long long domain, long long types) {
    (void)domain;
    sgx_simple(g, types == SG_MBReqTypes_Reads ? SGX_M_FENCE_LD : types == SG_MBReqTypes_Writes ? SGX_M_FENCE_ST : SGX_M_FENCE);
}
static inline void sg_DataSynchronizationBarrier(SgCtx* g, long long domain, long long types, long long nxs) {
    (void)nxs;
    sg_DataMemoryBarrier(g, domain, types);
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
static inline void sg_Hint_Yield(SgCtx* g) { sgx_simple(g, SGX_M_PAUSE); }
static inline void sg_Hint_WFE(SgCtx* g) { sgx_call(g, NULL, (int)offsetof(RtAPI, wfe), 1, NULL, 0, 0); }
static inline void sg_Hint_WFI(SgCtx* g) { (void)g; }
static inline void sg_SendEvent(SgCtx* g) { sgx_call(g, NULL, (int)offsetof(RtAPI, sev), 0, NULL, 0, 0); }
static inline void sg_SendEventLocal(SgCtx* g) { sg_SendEvent(g); }
/* an acquire load: DMB ISHLD after it; a release store: DMB ISH before and after it (sequentially consistent) */
static inline void sgx_acquire(SgCtx* g) { sgx_simple(g, SGX_M_FENCE_LD); }
static inline void sgx_release(SgCtx* g, int after) {
    (void)after;
    sgx_simple(g, SGX_M_FENCE);
}

/* ---- the ABI ---- */

enum { SGX_M_NONE };
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

/* STP X29, X30, [SP, #-32]!; STP X19, X20, [SP, #16]; MOV X29, SP; X19 from X0 (sga_state_in); SUB SP, SP, #frame
   (patched) */
static inline void sgx_prologue(SgXAsm* a) {
    SgXBuf* b = a->b;
    sga_w(b, sge_a64_stp_gen_pre_idx(29, 31, 30, (SnxU32)-4 & 0x7F, 2));
    sga_w(b, sge_a64_stp_gen_general_offset(19, 31, 20, 2, 2));
    sga_w(b, sge_a64_add_addsub_imm(29, 31, 0, 0, 1));
    sga_state_in(b);
    if (a->nframe < 256) a->frame_at[a->nframe++] = b->n;
    sga_w(b, sge_a64_sub_addsub_imm(31, 31, 0, 0, 1));
}
/* ADD SP, SP, #frame (patched); LDP X19, X20, [SP, #16]; LDP X29, X30, [SP], #32 */
static inline void sgx_epilogue(SgXAsm* a) {
    SgXBuf* b = a->b;
    if (a->nframe < 256) a->frame_at[a->nframe++] = b->n;
    else
        sg_fail("a function of more than 255 epilogues", 0, 0);
    sga_w(b, sge_a64_add_addsub_imm(31, 31, 0, 0, 1));
    sga_w(b, sge_a64_ldp_gen_general_offset(19, 31, 20, 2, 2));
    sga_w(b, sge_a64_ldp_gen_post_idx(29, 31, 30, 4, 2));
}

/* a branch's word whose offset (imm19 at bit 5, or B's imm26) is patched once its target is known */
static inline void sga_patch19(SgXBuf* b, size_t at, size_t to) {
    long long d = ((long long)to - (long long)at) / 4;
    SnxU32 w;
    if (at + 4 > b->cap) return;
    w = (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 | (SnxU32)b->p[at + 3] << 24;
    w = (w & ~(0x7FFFFu << 5)) | ((SnxU32)d & 0x7FFFFu) << 5;
    b->p[at] = (unsigned char)w, b->p[at + 1] = (unsigned char)(w >> 8), b->p[at + 2] = (unsigned char)(w >> 16);
    b->p[at + 3] = (unsigned char)(w >> 24);
}
/* LDR Wt or Xt of the state's field at off (X19's) */
static inline void sga_state(SgXBuf* b, int load, int bytes, int rt, size_t off) { sga_ldst(b, load, bytes, rt, SGX_BASE, -1, (int)off); }

/* the load-exclusive and store-exclusive of 1 << sz bytes at [X17] (LDAXRB...LDAXR, STLXRB...STLXR) */
static inline SnxU32 sga_ldaxr(int sz, int rt, int rn) {
    return sz == 0 ? sge_a64_ldaxrb((SnxU32)rt, (SnxU32)rn) : sz == 1 ? sge_a64_ldaxrh((SnxU32)rt, (SnxU32)rn)
                                                            : sge_a64_ldaxr((SnxU32)rt, (SnxU32)rn, (SnxU32)sz);
}
static inline SnxU32 sga_stlxr(int sz, int rs, int rt, int rn) {
    return sz == 0   ? sge_a64_stlxrb((SnxU32)rt, (SnxU32)rn, (SnxU32)rs)
           : sz == 1 ? sge_a64_stlxrh((SnxU32)rt, (SnxU32)rn, (SnxU32)rs)
                     : sge_a64_stlxr((SnxU32)rt, (SnxU32)rn, (SnxU32)rs, (SnxU32)sz);
}
/* the monitor's checks (armed, the address, and the size: a single's covering 1 << sz bytes, a pair's exactly),
   each a branch to the failure, fail[0..2] where to patch it */
static inline void sga_monitor(SgXBuf* b, int pair, unsigned sz, size_t* fail) {
    sga_state(b, 1, 4, 16, offsetof(CPU, excl_on));
    fail[0] = b->n, sga_w(b, sge_a64_cbz(16, 0, 0));
    sga_state(b, 1, 8, 16, offsetof(CPU, excl_addr));
    sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 3, 0, 16, 0, 1)); /* CMP X3, X16 */
    fail[1] = b->n, sga_w(b, sge_a64_b_cond(1, 0));              /* B.NE */
    sga_state(b, 1, 4, 16, offsetof(CPU, excl_size));
    if (pair) {
        sga_w(b, sge_a64_subs_addsub_imm(SGA_ZR, 16, sz | 0x10, 0, 0)); /* CMP W16, #(sz | 0x10) */
        fail[2] = b->n, sga_w(b, sge_a64_b_cond(1, 0));
    } else {
        sga_w(b, sge_a64_and_log_imm(16, 16, 1, 0, 0, 0));            /* AND W16, W16, #3 */
        sga_w(b, sge_a64_subs_addsub_imm(SGA_ZR, 16, sz, 0, 0));        /* CMP W16, #sz */
        fail[2] = b->n, sga_w(b, sge_a64_b_cond(3, 0));                /* B.LO */
    }
}
/* the store's status: 0 where it stored, 1 where a check failed or the memory held another value (then CLREX);
   then the monitor disarmed */
static inline void sga_status(SgXBuf* b, const size_t* fail, size_t differ) {
    size_t done;
    sga_w(b, sge_a64_movz(0, 0, 0, 0));
    done = b->n, sga_w(b, sge_a64_b_uncond(0));
    sga_patch19(b, differ, b->n);
    sga_w(b, sge_a64_clrex(15));
    sga_patch19(b, fail[0], b->n), sga_patch19(b, fail[1], b->n), sga_patch19(b, fail[2], b->n);
    sga_w(b, sge_a64_movz(0, 1, 0, 0));
    {
        long long d = ((long long)b->n - (long long)done) / 4;
        SnxU32 w = sge_a64_b_uncond((SnxU32)d & 0x3FFFFFF);
        if (done + 4 <= b->cap)
            b->p[done] = (unsigned char)w, b->p[done + 1] = (unsigned char)(w >> 8), b->p[done + 2] = (unsigned char)(w >> 16),
            b->p[done + 3] = (unsigned char)(w >> 24);
    }
    sga_state(b, 0, 4, SGA_ZR, offsetof(CPU, excl_on));
}
/* the general-purpose registers each inline sequence below writes, its results' included, X16 and X17 (the
   veneers' scratch, which no value or pin is in) aside: what its sg_ function declares, sgx_machine_declares checks */
static inline unsigned sgx_machine_writes(const SgXIns* i) {
    const unsigned op = (unsigned)(i->imm >> 2);
    switch (i->op) {
        case SGX_M_STX: return 1u << 0 | 1u << 1 | 1u << 6;
        case SGX_M_STXP: return 1u << 0 | 1u << 1 | 1u << 6 | 1u << 7 | 1u << 8;
        case SGX_M_CAS: return 1u << 0 | 1u << 6;
        case SGX_M_CASP: return 1u << 0 | 1u << 1 | 1u << 8;
        case SGX_M_RMW: return 1u << 0 | 1u << 6 | 1u << 7 | (op >= 4 && op <= 7 ? 1u << 8 : 0u);
        case SGX_M_CRC: return 1u << 0;
        default: return 0;
    }
}
static inline void sgx_machine_inline(SgXAsm* a, SgXIns* i) {
    SgXBuf* b = a->b;
    int sz = (int)(i->imm & 3);
    size_t fail[3], loop, differ;
    switch (i->op) {
    case SGX_M_STX: /* cpu.h's stx: the value the load-exclusive read swapped for X4, X0 its status */
        sga_monitor(b, 0, (unsigned)sz, fail);
        sga_w(b, sge_a64_add_addsub_shift(17, 2, 0, 3, 0, 1)); /* X17 = the host address */
        sga_state(b, 1, 8, 16, offsetof(CPU, excl_val));
        if (sz < 3) sga_w(b, sge_a64_ubfm(16, 16, (8u << sz) - 1, 0, 1, 1)); /* compared at the access's size */
        loop = b->n;
        sga_w(b, sga_ldaxr(sz, 1, 17));
        sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 1, 0, 16, 0, 1)); /* CMP X1, X16 */
        differ = b->n, sga_w(b, sge_a64_b_cond(1, 0));
        sga_w(b, sga_stlxr(sz, 6, 4, 17));
        sga_w(b, sge_a64_cbnz(6, (SnxU32)((long long)(loop - b->n) / 4) & 0x7FFFF, 0));
        sga_status(b, fail, differ);
        return;
    case SGX_M_STXP: /* cpu.h's stxp of doublewords (a pair of words is one doubleword's STX): LDAXP and STLXP */
        sga_monitor(b, 1, (unsigned)sz, fail);
        sga_w(b, sge_a64_add_addsub_shift(17, 2, 0, 3, 0, 1));
        sga_state(b, 1, 8, 16, offsetof(CPU, excl_val));
        sga_state(b, 1, 8, 8, offsetof(CPU, excl_val2));
        loop = b->n;
        sga_w(b, sge_a64_ldaxp(1, 17, 7, 1));
        sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 1, 0, 16, 0, 1));
        sga_w(b, sge_a64_ccmp_reg(0, 8, 0, 7, 1));             /* CCMP X7, X8, #0, EQ */
        differ = b->n, sga_w(b, sge_a64_b_cond(1, 0));
        sga_w(b, sge_a64_stlxp(4, 17, 5, 6, 1));
        sga_w(b, sge_a64_cbnz(6, (SnxU32)((long long)(loop - b->n) / 4) & 0x7FFFF, 0));
        sga_status(b, fail, differ);
        return;
    case SGX_M_CAS: { /* CAS: X0 the old value, X5 stored where it was X4 */
        size_t out;
        sga_w(b, sge_a64_add_addsub_shift(17, 2, 0, 3, 0, 1));
        loop = b->n;
        sga_w(b, sga_ldaxr(sz, 0, 17));
        sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 0, 0, 4, 0, sz == 3));
        out = b->n, sga_w(b, sge_a64_b_cond(1, 0));
        sga_w(b, sga_stlxr(sz, 6, 5, 17));
        sga_w(b, sge_a64_cbnz(6, (SnxU32)((long long)(loop - b->n) / 4) & 0x7FFFF, 0));
        {
            size_t skip = b->n;
            sga_w(b, sge_a64_b_uncond(0));
            sga_patch19(b, out, b->n);
            sga_w(b, sge_a64_clrex(15));
            {
                long long d = ((long long)b->n - (long long)skip) / 4;
                SnxU32 w = sge_a64_b_uncond((SnxU32)d & 0x3FFFFFF);
                if (skip + 4 <= b->cap)
                    b->p[skip] = (unsigned char)w, b->p[skip + 1] = (unsigned char)(w >> 8),
                    b->p[skip + 2] = (unsigned char)(w >> 16), b->p[skip + 3] = (unsigned char)(w >> 24);
            }
        }
        return;
    }
    case SGX_M_CASP: { /* CASP of doublewords: X0:X1 the old pair, X6:X7 stored where it was X4:X5 */
        size_t out, skip;
        sga_w(b, sge_a64_add_addsub_shift(17, 2, 0, 3, 0, 1));
        loop = b->n;
        sga_w(b, sge_a64_ldaxp(0, 17, 1, 1));
        sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 0, 0, 4, 0, 1));
        sga_w(b, sge_a64_ccmp_reg(0, 5, 0, 1, 1)); /* CCMP X1, X5, #0, EQ */
        out = b->n, sga_w(b, sge_a64_b_cond(1, 0));
        sga_w(b, sge_a64_stlxp(6, 17, 7, 8, 1));
        sga_w(b, sge_a64_cbnz(8, (SnxU32)((long long)(loop - b->n) / 4) & 0x7FFFF, 0));
        skip = b->n, sga_w(b, sge_a64_b_uncond(0));
        sga_patch19(b, out, b->n);
        sga_w(b, sge_a64_clrex(15));
        {
            long long d = ((long long)b->n - (long long)skip) / 4;
            SnxU32 w = sge_a64_b_uncond((SnxU32)d & 0x3FFFFFF);
            if (skip + 4 <= b->cap)
                b->p[skip] = (unsigned char)w, b->p[skip + 1] = (unsigned char)(w >> 8),
                b->p[skip + 2] = (unsigned char)(w >> 16), b->p[skip + 3] = (unsigned char)(w >> 24);
        }
        return;
    }
    case SGX_M_RMW: { /* the LSE atomics' operations in a load-exclusive loop: X0 the old value, X4 the operand */
        unsigned op = (unsigned)(i->imm >> 2);
        sga_w(b, sge_a64_add_addsub_shift(17, 2, 0, 3, 0, 1));
        loop = b->n;
        sga_w(b, sga_ldaxr(sz, 0, 17));
        switch (op) {
        case 0: sga_w(b, sge_a64_add_addsub_shift(6, 0, 0, 4, 0, 1)); break;
        case 1: sga_w(b, sge_a64_and_log_shift(6, 0, 0, 4, 0, 1)); break;
        case 2: sga_w(b, sge_a64_eor_log_shift(6, 0, 0, 4, 0, 1)); break;
        case 3: sga_w(b, sge_a64_orr_log_shift(6, 0, 0, 4, 0, 1)); break;
        case 8: sga_w(b, sge_a64_orr_log_shift(6, SGA_ZR, 0, 4, 0, 1)); break;
        default: {
            /* both extended to 64 bits at the size (signed for 4 and 5), compared, the larger or smaller */
            unsigned top = (8u << sz) - 1;
            int sgn = op <= 5;
            if (sgn) {
                sga_w(b, sge_a64_sbfm(7, 0, top, 0, 1, 1));
                sga_w(b, sge_a64_sbfm(8, 4, top, 0, 1, 1));
            } else {
                sga_w(b, sge_a64_ubfm(7, 0, top, 0, 1, 1));
                sga_w(b, sge_a64_ubfm(8, 4, top, 0, 1, 1));
            }
            sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 7, 0, 8, 0, 1));
            /* max: the old where GT (signed) or HI; min: where LT or LO */
            sga_w(b, sge_a64_csel(6, 0, op == 4 ? 12 : op == 5 ? 11 : op == 6 ? 8 : 3, 4, 1));
        }
        }
        sga_w(b, sga_stlxr(sz, 7, 6, 17));
        sga_w(b, sge_a64_cbnz(7, (SnxU32)((long long)(loop - b->n) / 4) & 0x7FFFF, 0));
        return;
    }
    case SGX_M_CRC: { /* CRC32{B,H,W,X} or CRC32C* X0 = X4 of X5 */
        unsigned bytes = (unsigned)(i->imm & 15), s = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
        sga_w(b, (i->imm & 16) ? sge_a64_crc32c(0, 4, s, 5, s == 3) : sge_a64_crc32(0, 4, s, 5, s == 3));
        return;
    }
    default: return;
    }
}

/* the machine's instructions (UNDEF, RET, calls, barriers, the inline sequences), their inputs in r[] */
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgXBuf* b = a->b;
    switch (i->kind) {
    case SGX_K_UNDEF:
        /* state->pc = pc, then rt->undef(state, pc, word): X0, X1, W2, through X16 = state->rt */
        if (a->pin_flush) a->pin_flush(a->machine);
        sga_movi(b, 1, (SnxU64)i->imm, 1);
        sga_state(b, 0, 8, 1, offsetof(CPU, pc));
        sga_state_out(b);
        sga_movi(b, 2, (SnxU64)(SnxU32)i->disp, 0);
        sga_state(b, 1, 8, 16, offsetof(CPU, rt));
        sga_ldst(b, 1, 8, 16, 16, -1, (int)offsetof(RtAPI, undef));
        sga_w(b, sge_a64_blr(16));
        break;
    case SGX_K_RET:
        /* a step's return of the next pc, in X0 (in[0], fixed there) or a constant */
        if (!i->in[0]) sga_movi(b, 0, (SnxU64)i->imm, 1);
        sgx_epilogue(a);
        sga_w(b, sge_a64_ret(30));
        break;
    case SGX_K_MACHINE:
        switch (i->op) {
        case SGX_M_FENCE: sga_w(b, sge_a64_dmb(11)); return;    /* DMB ISH */
        case SGX_M_FENCE_LD: sga_w(b, sge_a64_dmb(9)); return; /* DMB ISHLD */
        case SGX_M_FENCE_ST: sga_w(b, sge_a64_dmb(10)); return; /* DMB ISHST */
        case SGX_M_PAUSE: sga_w(b, sge_a64_yield()); return;
        case SGX_M_FPENTER: /* LDR W16, [X19, #fpcr]; MSR FPCR, X16; MSR FPSR, XZR */
            sga_state(b, 1, 4, 16, offsetof(CPU, fpcr));
            sga_w(b, sge_a64_msr_reg(16, 0, 4, 4, 3, 1));
            sga_w(b, sge_a64_msr_reg(SGA_ZR, 1, 4, 4, 3, 1));
            return;
        case SGX_M_SETNZCV: sga_w(b, sge_a64_msr_reg(r[0], 0, 2, 4, 3, 1)); return; /* MSR NZCV, Xr */
        case SGX_M_FPLEAVE: /* MRS X16, FPSR; the guest's FPSR |= it; MSR FPCR, XZR (the host's own: 0) */
            sga_w(b, sge_a64_mrs(16, 1, 4, 4, 3, 1));
            sga_state(b, 1, 4, 17, offsetof(CPU, fpsr));
            sga_w(b, sge_a64_orr_log_shift(17, 17, 0, 16, 0, 0));
            sga_state(b, 0, 4, 17, offsetof(CPU, fpsr));
            sga_w(b, sge_a64_msr_reg(SGA_ZR, 0, 4, 4, 3, 1));
            return;
        case SGX_M_STX:
        case SGX_M_STXP:
        case SGX_M_CAS:
        case SGX_M_CASP:
        case SGX_M_RMW:
        case SGX_M_CRC:
            sgx_machine_declares(a, at, sgx_machine_writes(i));
            sgx_machine_inline(a, i);
            return;
        case SGX_M_CALL:
        case SGX_M_CALL_RT:
            if (a->pin_cs) a->pin_cs(a->machine, 1);
            if (i->w) sga_state_out(b); /* X0, the state */
            if (i->op == SGX_M_CALL_RT) {
                sga_state(b, 1, 8, 16, offsetof(CPU, rt));
                sga_ldst(b, 1, 8, 16, 16, -1, i->disp);
                sga_w(b, sge_a64_blr(16));
            } else if (a->reloc) {
                /* BL, which the object's relocation (CALL26) fills */
                a->reloc(a->machine, b->n, i->sym, 0);
                sga_w(b, sge_a64_bl(0));
            } else {
                SnxU64 to = a->resolve ? a->resolve(i->sym) : 0;
                if (!to) sg_fail("a helper nothing resolves", 0, 0);
                sga_movi(b, 16, to, 1);
                sga_w(b, sge_a64_blr(16));
            }
            if (a->pin_cs) a->pin_cs(a->machine, 0);
            return;
        default: sgx_machine_op(a, at, r); return;
        }
    default: break;
    }
    (void)r;
}

#ifndef SGX_FN
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r) {
    (void)a, (void)r;
    sg_fail("a machine instruction of no function emitter", at, 0);
}
#endif
