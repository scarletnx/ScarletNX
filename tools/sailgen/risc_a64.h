/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's RISC back ends, A64's part (after mach_a64.h): the exclusives, the atomics, CRC32, the barriers and the
   machine instructions, from risc.h's operations and the machine's branches, calls and frame. The floating point is
   mach_a64.h's exact helpers (SGX_FP_HELPERS). Every barrier is the machine's full one (rk_fence): an acquire load
   one after it, a release store one before and after (sequentially consistent, as the LLVM back end's are).

   The exclusives and atomics are the machine's load-reserved and store-conditional (rk_lr, rk_sc) of 4 or 8 bytes: a
   store-exclusive is cpu.h's value-based monitor, a compare-and-swap of the value the load-exclusive read; a byte's
   or a halfword's works on its aligned word, the other bytes kept as they are, as LLVM's masked atomics do; a pair's
   (16 bytes) the exact helpers' over cpu.h's snx_cas128 (the runtime's RtAPI.cas128 on these hosts), as the LLVM
   back end's code calls it. CRC32 is the exact helper's too */

/* ---- the exclusives, the atomics and CRC32 (SGX_K_MACHINE): inputs in the machine's fixed registers (RK_M_*):
   the guest memory's base RK_M_BASE, the address RK_M_ADDR, the values RK_M_V1 and RK_M_V2 (a CAS's expected and
   desired); the result in SGX_RET; RK_M_T1 to RK_M_T3 their own besides ---- */
#define RK_M_CLOBBER (1u << RK_M_T1 | 1u << RK_M_T2 | 1u << RK_M_T3)
#define RK_M_STX_CLOBBER (RK_M_CLOBBER | 1u << RK_M_V2) /* and RK_M_V2, the reservation's value */

static inline void sg_GuestStoreExclusive(SgCtx* g, SgD address, long long size, SgD value) {
    SgD in[4];
    if (size == 16) {
        /* a pair of doublewords: the runtime's cas128 of the reservation's value, cpu.h's stxp */
        SgD args[3];
        args[0] = address;
        args[1] = sg_d_slice(g, value, 63, 0);
        args[2] = sg_d_slice(g, value, 127, 64);
        g->status = sgx_call(g, "snx_x64_stxp", 0, 1, args, 3, 32).v;
        return;
    } else {
        static const signed char fix[3] = {RK_M_BASE, RK_M_ADDR, RK_M_V1};
        in[0] = sgx_val(64, sgx_mem(g));
        in[1] = address;
        in[2] = sg_d_zext(g, value, 64);
        g->status = sgx_mach(g, SGX_M_STX, in, fix, 3, sgx_log2(size), RK_M_STX_CLOBBER, 1, 32).v;
    }
    sgx(g)->v[g->status].zab = 1;
}
static inline SgD sgx_rmw(SgCtx* g, int op, SgD address, SgD v) {
    static const signed char fix[3] = {RK_M_BASE, RK_M_ADDR, RK_M_V1};
    unsigned w = v.w;
    SgD in[3];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    in[2] = sg_d_zext(g, v, 64);
    return sg_d_slice(g, sgx_mach(g, SGX_M_RMW, in, fix, 3, op << 2 | sgx_log2(w / 8), RK_M_CLOBBER, 1, 64), w - 1, 0);
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
    if (w == 128) {
        /* CASP of doublewords: snx_x64_casp(state, address, expected, desired), the old pair in SGX_RET and SGX_RET2
           (its SnxX64Pair); a call (the caller-saved registers spilled around it) */
        const signed char fix[5] = {sgx_arg_regs[1], sgx_arg_regs[2], sgx_arg_regs[3], sgx_arg_regs[4],
                                    sgx_arg_regs[5]};
        SgD in[5], lo;
        SgXIns* i;
        in[0] = address;
        in[1] = sg_d_slice(g, expected, 63, 0);
        in[2] = sg_d_slice(g, expected, 127, 64);
        in[3] = sg_d_slice(g, desired, 63, 0);
        in[4] = sg_d_slice(g, desired, 127, 64);
        sgx_flags_clobbered(sgx(g));
        lo = sgx_mach(g, SGX_M_CASP, in, fix, 5, 0, 0, 2, 64);
        i = &sgx(g)->ins[sgx(g)->n - 1];
        i->calls = 1;
        i->sym = "snx_x64_casp";
        return sg_d_concat(g, sgx_val(64, i->def2), lo);
    }
    {
        static const signed char fix[4] = {RK_M_BASE, RK_M_ADDR, RK_M_V1, RK_M_V2};
        SgD in[4];
        in[0] = sgx_val(64, sgx_mem(g));
        in[1] = address;
        in[2] = sg_d_zext(g, expected, 64);
        in[3] = sg_d_zext(g, desired, 64);
        return sg_d_slice(g, sgx_mach(g, SGX_M_CAS, in, fix, 4, sgx_log2(w / 8), RK_M_CLOBBER, 1, 64), w - 1, 0);
    }
}
/* CRC32 and CRC32C: the exact helper (x64_helpers.c's table-driven snx_x64_crc32) */
static inline SgD sg_crc32(SgCtx* g, SgD acc_bits, SgD value_bits, long long size, SgS poly) {
    SgD args[3];
    args[0] = sg_d_zext(g, acc_bits, 64);
    args[1] = sg_d_zext(g, value_bits, 64);
    args[2] = sgx_const(32, (SnxU64)(size / 8) | (SnxU64)(poly.v == 0x1EDC6F41u) << 4);
    return sg_d_slice(g, sgx_call(g, "snx_x64_crc32", 0, 0, args, 3, 64), 31, 0);
}

/* ---- barriers and hints ---- */

enum { SG_MBReqTypes_Reads, SG_MBReqTypes_Writes, SG_MBReqTypes_All };
static inline void sg_DataMemoryBarrier(SgCtx* g, long long domain, long long types) {
    (void)domain, (void)types;
    sgx_simple(g, SGX_M_FENCE);
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
static inline void sgx_acquire(SgCtx* g) { sgx_simple(g, SGX_M_FENCE); }
static inline void sgx_release(SgCtx* g, int after) {
    (void)after;
    sgx_simple(g, SGX_M_FENCE);
}

/* ---- the machine instructions ---- */

enum { SGX_M_NONE };
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

static inline int sgx_state_bytes(int disp) {
    return (size_t)disp >= offsetof(CPU, v) && (size_t)disp < offsetof(CPU, v) + sizeof(V128) * 32;
}
/* the state's field at off to or from rt */
static inline void rk_state(SgXBuf* b, int load, int bytes, int rt, size_t off) {
    rk_ldst(b, load, bytes, 0, rt, SGX_BASE, -1, (int)off);
}
/* a load-reserved/store-conditional loop at the host address (RK_M_T1: the base plus the address) of the aligned 4
   or 8 bytes holding the access of 1 << sz bytes: old = the access's value (zero extended) into a0; then new = f(old)
   of op (0-8: RMW's operations on a3, 9: desired where old is expected, else no store) stored where it took. Its
   temporaries RK_M_T1 to RK_M_T3, RK_S1, RK_S2 and RK_SA; the inputs as they were */
static inline void risc_atomic(SgXBuf* b, int sz, int op, int expected, int desired) {
    int bytes = 1 << sz, word = bytes < 4 ? 4 : bytes, shift = 0, value = RK_M_V1;
    size_t loop, out = 0, done;
    rk_add(b, RK_M_T1, RK_M_BASE, RK_M_ADDR, 1);
    if (bytes < 4) {
        /* the word: the address's low bits its byte offset, the access a field of it at (offset * 8); a big-endian
           host's word has the offset's bytes at the other end (an aligned field's: offset ^ (4 - bytes)) */
        rk_andi(b, RK_M_T3, RK_M_T1, 3);
        if (RK_BIG_ENDIAN) rk_xori(b, RK_M_T3, RK_M_T3, 4 - bytes);
        rk_slli(b, RK_M_T3, RK_M_T3, 3, 1);
        rk_andi(b, RK_M_T1, RK_M_T1, -4);
        shift = 1;
    }
    loop = b->n;
    rk_lr(b, RK_S1, RK_M_T1, word);
    /* old: the access's bits of the word, zero extended */
    if (shift) {
        rk_srl(b, SGX_RET, RK_S1, RK_M_T3, 1);
        rk_zext(b, SGX_RET, SGX_RET, 8 * bytes);
    } else if (bytes == 4)
        rk_zext(b, SGX_RET, RK_S1, 32);
    else
        rk_mv(b, SGX_RET, RK_S1);
    /* a big-endian host's: the guest's number of the bytes */
    if (RK_BIG_ENDIAN && bytes > 1) rk_bswap(b, SGX_RET, SGX_RET, bytes);
    /* new into RK_M_T2 */
    switch (op) {
    case 0: rk_add(b, RK_M_T2, SGX_RET, value, 1); break;
    case 1: rk_and(b, RK_M_T2, SGX_RET, value); break;
    case 2: rk_xor(b, RK_M_T2, SGX_RET, value); break;
    case 3: rk_or(b, RK_M_T2, SGX_RET, value); break;
    case 8: rk_mv(b, RK_M_T2, value); break;
    case 9:
        /* the expected at the access's size against old: no store where they differ */
        if (bytes < 8) rk_zext(b, RK_S2, expected, 8 * bytes);
        else
            rk_mv(b, RK_S2, expected);
        rk_xor(b, RK_S2, RK_S2, SGX_RET);
        out = rk_bz(b, 1, RK_S2);
        rk_mv(b, RK_M_T2, desired);
        break;
    default: {
        /* max and min: both extended at the size (signed for 4 and 5), compared, the larger or smaller */
        int sgn = op <= 5, bits = 8 * bytes;
        if (bits < 64) {
            if (sgn) rk_sext(b, RK_S2, SGX_RET, bits), rk_sext(b, RK_M_T2, value, bits);
            else
                rk_zext(b, RK_S2, SGX_RET, bits), rk_zext(b, RK_M_T2, value, bits);
        } else
            rk_mv(b, RK_S2, SGX_RET), rk_mv(b, RK_M_T2, value);
        /* lt = S2 < T2; max: lt ? T2 : old; min: lt ? old : T2 */
        if (sgn) rk_slt(b, RK_SA, RK_S2, RK_M_T2);
        else
            rk_sltu(b, RK_SA, RK_S2, RK_M_T2);
        if (op == 5 || op == 7) rk_xori(b, RK_SA, RK_SA, 1);
        rk_neg(b, RK_SA, RK_SA, 1);
        rk_xor(b, RK_S2, RK_M_T2, SGX_RET);
        rk_and(b, RK_S2, RK_S2, RK_SA);
        rk_xor(b, RK_M_T2, SGX_RET, RK_S2);
        break;
    }
    }
    /* and new's bytes in the host's order */
    if (RK_BIG_ENDIAN && bytes > 1) rk_bswap(b, RK_M_T2, RK_M_T2, bytes);
    if (shift) {
        /* the word with the field replaced: (word & ~(mask << off)) | ((new & mask) << off) */
        rk_li(b, RK_S2, bytes == 1 ? 0xffu : 0xffffu);
        rk_and(b, RK_M_T2, RK_M_T2, RK_S2);
        rk_sll(b, RK_M_T2, RK_M_T2, RK_M_T3, 1);
        rk_sll(b, RK_S2, RK_S2, RK_M_T3, 1);
        rk_xori(b, RK_S2, RK_S2, -1);
        rk_and(b, RK_S1, RK_S1, RK_S2);
        rk_or(b, RK_M_T2, RK_M_T2, RK_S1);
    }
    rk_sc(b, RK_S2, RK_M_T1, RK_M_T2, word);
    rk_patch(b, rk_bz(b, 1, RK_S2), loop);
    if (out) {
        done = rk_j(b);
        rk_patch(b, out, b->n);
        rk_fence(b); /* the reservation given up with the old value read: an acquire as the store would have been */
        rk_patch(b, done, b->n);
    }
}
/* cpu.h's stx: the monitor armed at this address and size (a single's covering 1 << sz bytes), then a CAS of the
   value the load-exclusive read for a3; a0 the status (0 stored, 1 not); the monitor disarmed */
static inline void risc_stx(SgXBuf* b, int sz) {
    size_t fail[3], skip;
    int k;
    rk_state(b, 1, 4, RK_S1, offsetof(CPU, excl_on));
    fail[0] = rk_bz(b, 0, RK_S1);
    rk_state(b, 1, 8, RK_S1, offsetof(CPU, excl_addr));
    rk_xor(b, RK_S1, RK_S1, RK_M_ADDR);
    fail[1] = rk_bz(b, 1, RK_S1);
    rk_state(b, 1, 4, RK_S1, offsetof(CPU, excl_size));
    rk_andi(b, RK_S1, RK_S1, 3);
    rk_li(b, RK_S2, (SnxU64)sz);
    rk_sltu(b, RK_S1, RK_S1, RK_S2);
    fail[2] = rk_bz(b, 1, RK_S1);
    /* the CAS: expected the reservation's value (a4) at the access's size, desired a3 */
    rk_state(b, 1, 8, RK_M_V2, offsetof(CPU, excl_val));
    risc_atomic(b, sz, 9, RK_M_V2, RK_M_V1);
    /* a0 = the old value; stored where it was the expected */
    if (sz < 3) rk_zext(b, RK_M_V2, RK_M_V2, 8 << sz);
    rk_xor(b, SGX_RET, SGX_RET, RK_M_V2);
    rk_snez(b, SGX_RET, SGX_RET);
    skip = rk_j(b);
    for (k = 0; k < 3; ++k) rk_patch(b, fail[k], b->n);
    rk_li(b, SGX_RET, 1);
    rk_patch(b, skip, b->n);
    rk_li(b, RK_S1, 0);
    rk_state(b, 0, 4, RK_S1, offsetof(CPU, excl_on));
}
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgXBuf* b = a->b;
    switch (i->kind) {
    case SGX_K_UNDEF:
        /* state->pc = pc, then rt->undef(state, pc, word) */
        if (a->pin_flush) a->pin_flush(a->machine);
        rk_li(b, sgx_arg_regs[1], (SnxU64)i->imm);
        rk_state(b, 0, 8, sgx_arg_regs[1], offsetof(CPU, pc));
        rk_mv(b, sgx_arg_regs[0], SGX_BASE);
        rk_li(b, sgx_arg_regs[2], (SnxU64)(SnxU32)i->disp);
        rk_state(b, 1, 8, RK_SA, offsetof(CPU, rt));
        rk_ldst(b, 1, 8, 0, RK_SA, RK_SA, -1, (int)offsetof(RtAPI, undef));
        rk_call_reg(b, RK_SA);
        break;
    case SGX_K_RET:
        if (!i->in[0]) rk_li(b, SGX_RET, (SnxU64)i->imm);
        sgx_epilogue(a);
        rk_ret(b);
        break;
    case SGX_K_MACHINE:
        switch (i->op) {
        case SGX_M_FENCE:
        case SGX_M_FENCE_LD:
        case SGX_M_FENCE_ST: rk_fence(b); return;
        case SGX_M_PAUSE: rk_nop(b); return;
        case SGX_M_STX: risc_stx(b, (int)(i->imm & 3)); return;
        case SGX_M_CAS: risc_atomic(b, (int)(i->imm & 3), 9, RK_M_V1, RK_M_V2); return;
        case SGX_M_RMW: risc_atomic(b, (int)(i->imm & 3), (int)(i->imm >> 2), 0, 0); return;
        case SGX_M_CASP:
        case SGX_M_CALL:
        case SGX_M_CALL_RT:
            if (a->pin_cs) a->pin_cs(a->machine, 1);
            if (i->w || i->op == SGX_M_CASP) rk_mv(b, sgx_arg_regs[0], SGX_BASE);
            if (i->op == SGX_M_CALL_RT) {
                rk_state(b, 1, 8, RK_SA, offsetof(CPU, rt));
                rk_ldst(b, 1, 8, 0, RK_SA, RK_SA, -1, i->disp);
                rk_call_reg(b, RK_SA);
            } else if (a->reloc) {
                size_t lit = rk_call_reloc(b);
                a->reloc(a->machine, lit, i->sym, 0);
            } else {
                SnxU64 to = a->resolve ? a->resolve(i->sym) : 0;
                if (!to) sg_fail("a helper nothing resolves", 0, 0);
                rk_li(b, RK_SA, to);
                rk_call_reg(b, RK_SA);
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
