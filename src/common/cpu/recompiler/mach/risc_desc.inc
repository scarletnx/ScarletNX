/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The RISC back ends' share of an AOT module's descriptor (native.h's SnxNativeDesc, nx/loader/module_x64.c writing
   the data): its few functions from risc.h's operations and the machine's branches, its literal and its return,
   included by a machine's code generator after mach/risc_codegen.inc (MG_DESC the table's name, MG_DESC_NAME the
   machine's). They have no frame, so they keep to the registers no call keeps: the first four arguments', the
   assembler's (RK_SA) and the scratch of code before a frame (RK_ENTRY_S1); a literal keeps the link
   (rk_leaf_literal) */
#ifndef RK_ENTRY_S1
#define RK_ENTRY_S1 RK_S1
#endif

/* the function's code from b, 0 where it fit */
static int desc_done(SnxNativeFn* f, const SgXBuf* b) {
    if (b->n > b->cap) return -1;
    f->size = b->n;
    return 0;
}
/* d = the address the literal holds (sym + addend), its relocation the function's next */
static void desc_address(SnxNativeFn* f, SgXBuf* b, int d, const char* sym, long long addend) {
    f->rel[f->nrel].at = rk_leaf_literal(b, RK_SA);
    f->rel[f->nrel].sym = sym;
    f->rel[f->nrel].type = 0;
    f->rel[f->nrel++].addend = addend;
    rk_ldst(b, 1, 8, 0, d, RK_SA, -1, 0);
}
#define DESC_BEGIN(f) SgXBuf b = {.p = (f)->code, .cap = sizeof (f)->code, .index = -1}; (f)->nrel = 0

static int desc_returns(SnxNativeFn* f, const char* sym) {
    DESC_BEGIN(f);
    desc_address(f, &b, SGX_RET, sym, 0);
    rk_ret(&b);
    return desc_done(f, &b);
}

static int desc_module_fn(SnxNativeFn* f, size_t count, uint64_t base, size_t fn_at, size_t ptr_at) {
    DESC_BEGIN(f);
    int src = sgx_arg_regs[0], dst = sgx_arg_regs[1], fns = sgx_arg_regs[2], t = sgx_arg_regs[3], n = RK_ENTRY_S1;
    size_t skip, loop;
    if (fn_at > 0x7FFFFFFF || ptr_at > 0x7FFFFFFF) return -1;
    desc_address(f, &b, src, "ENT_OFF", 0);
    desc_address(f, &b, dst, "ENT_ADDR", 0);
    desc_address(f, &b, fns, "FNS", 0);
    rk_li(&b, n, (SnxU64)count);
    skip = rk_bz(&b, 0, n);
    loop = b.n;
    rk_ldst(&b, 1, 4, 0, t, src, -1, 0); /* ENT_ADDR[i] = base + ENT_OFF[i] */
    rk_addi(&b, t, t, (long long)base, 1);
    rk_ldst(&b, 0, 8, 0, t, dst, -1, 0);
    rk_ldst(&b, 1, 4, 0, t, src, -1, (int)fn_at); /* ENT_PTR[i] = FNS[ENT_FN[i]] */
    rk_slli(&b, t, t, 3, 1);
    rk_add(&b, t, t, fns, 1);
    rk_ldst(&b, 1, 8, 0, t, t, -1, 0);
    rk_ldst(&b, 0, 8, 0, t, dst, -1, (int)ptr_at);
    rk_addi(&b, src, src, 4, 1);
    rk_addi(&b, dst, dst, 8, 1);
    rk_addi(&b, n, n, -1, 1);
    if (rk_patch(&b, rk_bz(&b, 1, n), loop) || rk_patch(&b, skip, b.n)) return -1;
    desc_address(f, &b, SGX_RET, "MOD", 0);
    rk_ret(&b);
    return desc_done(f, &b);
}

static int desc_import_dyn(SnxNativeFn* f) {
    DESC_BEGIN(f);
    int c = sgx_arg_regs[0], e = sgx_arg_regs[1], t2 = sgx_arg_regs[2], t3 = sgx_arg_regs[3];
    size_t miss[3];
    rk_ldst(&b, 1, 8, 0, t2, c, -1, (int)offsetof(CPU, code_lo));
    rk_ldst(&b, 1, 8, 0, t3, c, -1, (int)offsetof(CPU, code_hi));
    rk_sub(&b, t3, t3, t2, 1);
    rk_sub(&b, t2, e, t2, 1);
    rk_sltu(&b, RK_SA, t2, t3); /* e - code_lo < code_hi - code_lo */
    miss[0] = rk_bz(&b, 0, RK_SA);
    rk_andi(&b, t3, e, 3);
    miss[1] = rk_bz(&b, 1, t3);
    rk_slli(&b, t2, t2, 1, 1); /* (e - code_lo) / 4 * 8 */
    rk_ldst(&b, 1, 8, 0, t3, c, -1, (int)offsetof(CPU, disp));
    rk_add(&b, t2, t2, t3, 1);
    rk_ldst(&b, 1, 8, 0, t2, t2, -1, 0);
    miss[2] = rk_bz(&b, 0, t2);
    rk_jump_reg(&b, t2);
    for (int k = 0; k < 3; ++k)
        if (rk_patch(&b, miss[k], b.n)) return -1;
    rk_ldst(&b, 1, 8, 0, t2, c, -1, (int)offsetof(CPU, rt));
    rk_ldst(&b, 1, 8, 0, t2, t2, -1, (int)offsetof(RtAPI, miss));
    rk_jump_reg(&b, t2);
    return desc_done(f, &b);
}

static int desc_tail_slot(SnxNativeFn* f, const char* sym, long long addend) {
    DESC_BEGIN(f);
    desc_address(f, &b, RK_SA, sym, addend);
    rk_ldst(&b, 1, 8, 0, RK_SA, RK_SA, -1, 0);
    rk_jump_reg(&b, RK_SA);
    return desc_done(f, &b);
}
/* the machine's tail call by its relocation (MG_R_TAIL: a literal's, or a PC-relative pair's) */
static int desc_tail_to(SnxNativeFn* f, const char* sym) {
    DESC_BEGIN(f);
    f->rel[0].at = rk_tail_reloc(&b);
    f->rel[0].sym = sym;
    f->rel[0].addend = 0;
    f->rel[0].type = MG_R_TAIL;
    f->nrel = 1;
    return desc_done(f, &b);
}

const SnxNativeDesc MG_DESC = {MG_DESC_NAME,   MG_EM,          MG_ELF_FLAGS,    MG_R_ABS64,     MG_ELF_BIG,
                               desc_returns,   desc_module_fn, desc_import_dyn, desc_tail_slot, desc_tail_to};
