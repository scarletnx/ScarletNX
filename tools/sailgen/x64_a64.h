/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, A64's part (after x64.h): the Sail's registers where cpu.h's CPU keeps them (x[32]: X0
   to X30 and SP; n, z, cf, vf: PSTATE.NZCV, each a 32-bit 0 or 1), its address in RBX for the instruction's code.
   A register read once is the block's value of it after (a load, or what was written: every write is stored at once,
   and the stores a later one overwrites are dropped as the block ends). NZCV written from an instruction's flags is
   four SETcc of them (the selection's map), and ConditionHolds on flags that instruction made, while the x86 flags
   still are them, the condition code the selection gave Arm's condition on its form.

   What isn't an instruction the selection chose is the ABI's, written as bytes here: the prologue (the state's
   address from RDI into RBX, kept as RBX is the callee's), the runtime's UNDEF (cpu.h's RtAPI.undef(state, pc, word))
   and the return (the next pc in RAX) */

#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

struct SgCtx {
    SgX x;
    SnxU32 word;
    SnxU64 pc;
    SnxU64 features; /* the guest's (SNX_A64_FEAT_*) */
    int branched, undefined;
    SgD target; /* BranchTo's: where, and when (0: always) */
    SgB taken;
    int gpr[32];     /* the virtual register of each X register the block knows, 0 for none */
    int vd[32][2];   /* the same of each SIMD&FP register's doublewords (low, high) */
    int origin;      /* the instruction whose flags NZCV was last written from, -1 for none */
    int mem;         /* the virtual register of CPU.mem (guest memory's host address), 0 until read */
    int exclusive[2], exclusive_count, exclusive_next, status; /* a load-exclusive's values, a store's status */
    SnxU64 (*resolve)(const char* sym); /* a helper's address, where the code calls it absolutely (a test's) */
    int start;       /* the instructions before this word's */
};

static inline SgX* sgx(SgCtx* g) { return &g->x; }

/* a guest's feature, a64_decode.h's SNX_A64_FEAT_X (the decoder's profile) */
#define SG_FEAT(x) SNX_A64_##x
static inline int sg_feature(SgCtx* g, SnxU64 feature) { return (g->features & feature) != 0; }

static inline void sg_undefined(SgCtx* g) {
    SgXIns* i = sgx_append(sgx(g), SGX_K_UNDEF);
    i->imm = (long long)g->pc;
    i->disp = (int)g->word;
    i->calls = 1;
    g->undefined = 1;
}
static inline int sg_see(SgCtx* g) {
    if (g->x.n != g->start || g->undefined) sg_fail("a word handed on to the next clause after its own code", g->word, 0);
    return 0;
}
/* BTI's next branch type: no guest of the NX's runs with branch targets guarded */
static inline void sg_BTypeNext_write(SgCtx* g, SgD v) {
    (void)g;
    (void)v;
}

static inline SgD sg_GPR_read(SgCtx* g, long long n) {
    SgX* x = sgx(g);
    if (n < 0 || n > 31) sg_fail("a GPR this back end does not keep", n, 31);
    if (!g->gpr[n]) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)(offsetof(CPU, x) + 8 * (size_t)n);
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->gpr[n] = i->def;
    }
    return sgx_val(64, g->gpr[n]);
}
static inline void sg_GPR_write(SgCtx* g, long long n, SgD v) {
    SgX* x = sgx(g);
    SgXIns* i;
    int r;
    if (n < 0 || n > 31) sg_fail("a GPR this back end does not keep", n, 31);
    if (v.w != 64) sg_fail("a GPR written from other than 64 bits", v.w, 64);
    r = sgx_reg(x, &v);
    i = sgx_append(x, SGX_K_STORE);
    i->w = 64;
    i->in[0] = r;
    i->disp = (int)(offsetof(CPU, x) + 8 * (size_t)n);
    g->gpr[n] = r;
}

/* PSTATE's fields (0 N, 1 Z, 2 C, 3 V): cpu.h's n, z, cf and vf */
enum { SG_PSTATE_N, SG_PSTATE_Z, SG_PSTATE_C, SG_PSTATE_V };
static inline int sgx_flag_offset(long long field) {
    switch (field) {
    case 0: return (int)offsetof(CPU, n);
    case 1: return (int)offsetof(CPU, z);
    case 2: return (int)offsetof(CPU, cf);
    default: return (int)offsetof(CPU, vf);
    }
}
static inline SgD sg_PSTATE_read(SgCtx* g, long long field) {
    SgX* x = sgx(g);
    SgXIns* i;
    if (field < 0 || field > 3) sg_fail("a field of PSTATE this back end does not keep", field, 3);
    i = sgx_append(x, SGX_K_LOAD);
    i->w = 32;
    i->disp = sgx_flag_offset(field);
    i->def = sgx_vreg(x, x->n - 1, 1);
    return sgx_val(1, i->def);
}
static inline void sg_PSTATE_write(SgCtx* g, long long first, long long count, SgD v) {
    SgX* x = sgx(g);
    long long f;
    if (first < 0 || count < 1 || first + count > 4) sg_fail("fields of PSTATE this back end does not keep", first, count);
    if (v.w != (unsigned)count) sg_fail("flags written from a value of another width", count, v.w);
    if (count == 4 && v.nzcv) {
        /* the four from the flags of the instruction that made them, each the condition code that is that flag */
        int from = v.nzcv - 1;
        const SgXSel* fs = x->ins[from].fsel;
        if (x->flags_ins != from) sg_fail("NZCV written after the x86 flags that are them were overwritten", from, 0);
        for (f = 0; f < 4; ++f) {
            int cc = sgx_arm_flag_cc(fs, (int)f);
            SgXIns* i;
            if (cc < 0) {
                /* a flag the form gives as a constant (ANDS's C and V): stored as it */
                SgD k = sgx_const(32, 0);
                SgXIns* st;
                int r = sgx_reg(x, &k);
                st = sgx_append(x, SGX_K_STORE);
                st->w = 32;
                st->in[0] = r;
                st->disp = sgx_flag_offset(f);
                continue;
            }
            i = sgx_append(x, SGX_K_STORE_FLAG);
            i->cc = (unsigned char)cc;
            i->flags_from = from;
            i->disp = sgx_flag_offset(f);
        }
        g->origin = from;
        return;
    }
    for (f = 0; f < count; ++f) {
        SgD bit = sg_d_zext(g, sg_d_slice(g, v, count - 1 - f, count - 1 - f), 32);
        SgXIns* st;
        int r = sgx_reg(x, &bit);
        st = sgx_append(x, SGX_K_STORE);
        st->w = 32;
        st->in[0] = r;
        st->disp = sgx_flag_offset(first + f);
    }
    g->origin = -1;
}

/* ConditionHolds(cond) on the flags NZCV was last written from, while the x86 flags are still them: the condition
   code the selection gave Arm's cond on that form's flags. 0 otherwise (the Sail then reads PSTATE) */
static inline SgB sg_condition_origin(SgCtx* g, SgS cond) {
    SgX* x = sgx(g);
    int cc;
    if (g->origin < 0 || x->flags_ins != g->origin || cond.v >= 14) return 0;
    cc = x->ins[g->origin].fsel->cc_of[cond.v];
    if (cc < 0) return 0;
    return sgx_bool(x, SGX_B_CC, g->origin, cc);
}

/* ---- the machine's instructions: calls, fences ---- */

enum { SGX_M_CALL = 1, SGX_M_CALL_RT, SGX_M_FENCE, SGX_M_PAUSE };
/* the arguments' registers after the state's (RDI) */
static const signed char sgx_arg_regs[6] = {7, 6, 2, 1, 8, 9};

/* a call of the helper sym (x86_64_helpers.c's) or of the runtime's function at rt_offset in RtAPI, the state first
   where state: args its other arguments; its result (RAX) */
static inline SgD sgx_call(SgCtx* g, const char* sym, int rt_offset, int state, SgD* args, int nargs, int result_w) {
    SgX* x = sgx(g);
    int regs[5], k;
    SgXIns* i;
    if (nargs > 5 - (state ? 0 : 0)) sg_fail("a call of more than five arguments", nargs, 0);
    for (k = 0; k < nargs; ++k) regs[k] = sgx_reg(x, &args[k]);
    /* a call changes the flags: a condition on them read after it (FCCMP's, both its compare's path and its flags'
       choice) is first made 0 or 1 in a register */
    sgx_flags_clobbered(x);
    i = sgx_append(x, SGX_K_MACHINE);
    i->op = sym ? SGX_M_CALL : SGX_M_CALL_RT;
    i->sym = sym;
    i->disp = rt_offset;
    i->w = (unsigned char)state;
    i->calls = 1;
    for (k = 0; k < nargs; ++k) {
        i->in[k] = regs[k];
        i->fixed[k] = sgx_arg_regs[k + (state ? 1 : 0)];
    }
    if (result_w) i->def = sgx_vreg(x, x->n - 1, 64);
    /* a call changes the flags; the guest's registers the helpers and the runtime's hints leave as they are */
    x->flags_ins = -1;
    return result_w ? sgx_val((unsigned)result_w, i->def) : sgx_const(1, 0);
}
static inline void sgx_simple(SgCtx* g, int op) {
    SgXIns* i = sgx_append(sgx(g), SGX_K_MACHINE);
    i->op = (unsigned char)op;
}

/* ---- the SIMD&FP registers and floating point ----
   _V's elements are CPU.v's (two doublewords each, the low first), read and written a doubleword at a time; a write
   is of all 128 bits, as nx.sail's register file has it (a scalar's result zero-extended). FPCR is CPU.fpcr. The
   arithmetic the Sail lowers (FPAdd...) calls cpu.h's helpers, the exact ones LLVM's slow paths call, through
   x64_helpers.c's snx_x64_fp_* (the state first, FPSR's flags ORed in there): bits the operation takes as constants
   of the instruction are packed in desc, and a run-time path (FCCMP's) turns into FPCR's bit 32, the helper then
   doing nothing */

static inline int sgx_vd_offset(long long n, int k) { return (int)(offsetof(CPU, v) + 16 * (size_t)n + 8 * (size_t)k); }
static inline SgD sgx_vd_read(SgCtx* g, long long n, int k) {
    SgX* x = sgx(g);
    if (!g->vd[n][k]) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = sgx_vd_offset(n, k);
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->vd[n][k] = i->def;
    }
    return sgx_val(64, g->vd[n][k]);
}
static inline void sgx_vd_write(SgCtx* g, long long n, int k, SgD v) {
    SgX* x = sgx(g);
    int r = sgx_reg(x, &v);
    SgXIns* i = sgx_append(x, SGX_K_STORE);
    i->w = 64;
    i->in[0] = r;
    i->disp = sgx_vd_offset(n, k);
    g->vd[n][k] = r;
}
static inline SgD sg__V_read_bits(SgCtx* g, long long n, long long hi, long long lo) {
    long long w = hi - lo + 1;
    if (n < 0 || n > 31) sg_fail("a SIMD&FP register this back end does not keep", n, 31);
    if (lo == 0 && w <= 64) return sg_d_slice(g, sgx_vd_read(g, n, 0), hi, 0);
    if (lo == 64 && hi <= 127) return sg_d_slice(g, sgx_vd_read(g, n, 1), hi - 64, 0);
    if (lo == 0 && hi == 127) return sg_d_concat(g, sgx_vd_read(g, n, 1), sgx_vd_read(g, n, 0));
    sg_fail("bits of a SIMD&FP register across its doublewords", hi, lo);
}
static inline void sg__V_write(SgCtx* g, long long n, SgD v) {
    SgD lo, hi;
    if (n < 0 || n > 31) sg_fail("a SIMD&FP register this back end does not keep", n, 31);
    if (v.w != 128 || v.wide || v.inv || v.nzcv) sg_fail("a SIMD&FP register written from other than 128 bits", v.w, 128);
    lo = v.v ? sgx_val(64, v.v) : sgx_const(64, v.k);
    hi = v.vh ? sgx_val(64, v.vh) : sgx_const(64, v.kh);
    sgx_vd_write(g, n, 0, lo);
    sgx_vd_write(g, n, 1, hi);
}

static inline SgD sg_FPCR_read(SgCtx* g) {
    SgX* x = sgx(g);
    SgXIns* i = sgx_append(x, SGX_K_LOAD);
    i->w = 32;
    i->disp = (int)offsetof(CPU, fpcr);
    i->def = sgx_vreg(x, x->n - 1, 32);
    return sgx_val(64, i->def);
}

/* FPCR's 32 bits as a helper takes them, bit 32 set where path doesn't hold */
static inline SgD sgx_fp_fpcr(SgCtx* g, SgB path, SgD fpcr) {
    SgD f = sg_d_zext(g, sg_d_slice(g, fpcr, 31, 0), 64);
    return path ? sg_d_select(g, path, f, sgx_const(64, 1ull << 32)) : f;
}
/* snx_x64_fp_<sym>(state, operands..., fpcr, desc): the value's low w bits */
static inline SgD sgx_fp(SgCtx* g, const char* sym, SgB path, SgD fpcr, SgD* ops, int nops, int has_desc, SnxU32 desc,
                         unsigned w) {
    SgD args[5];
    int k, n = 0;
    for (k = 0; k < nops; ++k) args[n++] = sg_d_zext(g, ops[k], 64);
    args[n++] = sgx_fp_fpcr(g, path, fpcr);
    if (has_desc) args[n++] = sgx_const(32, desc);
    return sg_d_slice(g, sgx_call(g, sym, 0, 1, args, n, 64), (long long)w - 1, 0);
}
static inline SgD sgx_fp_binary(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned op) {
    SgD ops[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_binary", path, fpcr, ops, 2, 1, a.w | op << 8, a.w);
}
static inline SgD sg_FPAdd(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_ADD); }
static inline SgD sg_FPSub(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_SUB); }
static inline SgD sg_FPMul(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_MUL); }
static inline SgD sg_FPDiv(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_DIV); }
static inline SgD sgx_fp_minmax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned minimum, unsigned numeric) {
    SgD ops[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_minmax", path, fpcr, ops, 2, 1, a.w | minimum << 8 | numeric << 9, a.w);
}
static inline SgD sg_FPMax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 0); }
static inline SgD sg_FPMin(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 0); }
static inline SgD sg_FPMaxNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 1); }
static inline SgD sg_FPMinNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 1); }
static inline SgD sg_FPMulAdd(SgCtx* g, SgB path, SgD addend, SgD a, SgD b, SgD fpcr) {
    SgD ops[3] = {addend, a, b};
    return sgx_fp(g, "snx_x64_fp_fma", path, fpcr, ops, 3, 1, a.w, a.w);
}
static inline SgD sg_FPSqrt(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    return sgx_fp(g, "snx_x64_fp_unary", path, fpcr, &a, 1, 1, a.w | SNX_FP_SQRT << 8, a.w);
}
#define SG_FP_ROUNDING_FPCR 6 /* llvm.h's: FPRoundingMode's, past FPRounding's 0-5 */
static inline int sg_FPRoundingMode(SgCtx* g, SgB path, SgD fpcr) {
    (void)g, (void)path, (void)fpcr;
    return SG_FP_ROUNDING_FPCR;
}
/* a rounding the Sail hands a lowered function as desc's rm + 1: FPRounding's 0-4 (TIEEVEN to TIEAWAY), 0 for FPCR's */
static inline SnxU32 sgx_fp_rm(int rounding) {
    if (rounding == SG_FP_ROUNDING_FPCR) return 0;
    if (rounding < 0 || rounding > 4) sg_fail("a floating-point rounding the helpers do not take", rounding, 4);
    return (SnxU32)rounding + 1;
}
static inline SgD sg_FPRoundInt(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, int exact) {
    return sgx_fp(g, "snx_x64_fp_round", path, fpcr, &a, 1, 1, a.w | sgx_fp_rm(rounding) << 8 | (SnxU32)!!exact << 12,
                  a.w);
}
static inline SgD sg_FPRoundIntN(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, long long intsize) {
    return sgx_fp(g, "snx_x64_fp_round_int", path, fpcr, &a, 1, 1,
                  a.w | sgx_fp_rm(rounding) << 8 | (SnxU32)intsize << 16, a.w);
}
static inline SgD sg_FPConvert__1(SgCtx* g, SgB path, SgD op, SgD fpcr, long long to_bits) {
    if (to_bits != 16 && to_bits != 32 && to_bits != 64) sg_fail("a conversion to a float of this width", to_bits, 0);
    return sgx_fp(g, "snx_x64_fp_convert", path, fpcr, &op, 1, 1, op.w | (SnxU32)to_bits << 8, (unsigned)to_bits);
}
/* the NZCV nibble (N bit 3 ... V bit 0) */
static inline SgD sg_FPCompare(SgCtx* g, SgB path, SgD a, SgD b, int signal, SgD fpcr) {
    SgD ops[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_compare", path, fpcr, ops, 2, 1, a.w | (SnxU32)!!signal << 8, 4);
}
static inline SgD sg_FixedToFP(SgCtx* g, SgB path, SgD op, long long fbits, int uns, SgD fpcr, int rounding,
                               long long width) {
    if (rounding != SG_FP_ROUNDING_FPCR) sg_fail("FixedToFP in a mode of its own: the helper rounds as FPCR says", rounding, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FixedToFP of these fraction bits", fbits, 0);
    return sgx_fp(g, "snx_x64_fp_from_int", path, fpcr, &op, 1, 1,
                  op.w | (SnxU32)width << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17, (unsigned)width);
}
static inline SgD sg_FPToFixed(SgCtx* g, SgB path, SgD op, long long fbits, int uns, SgD fpcr, int rounding,
                               long long integer) {
    if (rounding == SG_FP_ROUNDING_FPCR) sg_fail("FPToFixed in FPCR's mode: the helper takes the encoding's", rounding, 0);
    if (integer != 32 && integer != 64) sg_fail("a conversion to an integer of this width", integer, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FPToFixed of these fraction bits", fbits, 0);
    return sgx_fp(g, "snx_x64_fp_to_int", path, fpcr, &op, 1, 1,
                  op.w | (SnxU32)integer << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17 | sgx_fp_rm(rounding) << 24,
                  (unsigned)integer);
}
static inline void sg_FPToFixedJS(SgCtx* g, SgB path, SgD op, SgD fpcr, int is64, long long n, SgD* result, SgD* z) {
    SgD reply;
    if (!is64 || n != 32 || op.w != 64) sg_fail("FPToFixedJS other than a double to 32 bits", n, op.w);
    reply = sgx_fp(g, "snx_x64_fp_jcvt", path, fpcr, &op, 1, 0, 0, 64);
    *result = sg_d_slice(g, reply, 31, 0);
    *z = sg_d_slice(g, reply, 32, 32);
}
static inline SgD sg_FPConvertBF__1(SgCtx* g, SgB path, SgD op, SgD fpcr) {
    return sgx_fp(g, "snx_x64_fp_bfcvt", path, fpcr, &op, 1, 0, 0, 16);
}

/* ---- guest memory: CPU.mem plus the address ---- */

static inline int sgx_mem(SgCtx* g) {
    if (!g->mem) {
        SgX* x = sgx(g);
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)offsetof(CPU, mem);
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->mem = i->def;
    }
    return g->mem;
}
/* the candidate of table whose r/m operand is its input (a load: rm_out 0) or its result (a store) */
static inline const SgXSel* sgx_mem_form(const SgXSel* t, unsigned n, int rm_out) {
    unsigned k;
    for (k = 0; k < n; ++k)
        if (t[k].rm >= 0 && !t[k].feature && !t[k].zero && (rm_out ? t[k].out == t[k].rm : t[k].in[0] == t[k].rm))
            return &t[k];
    sg_fail("no form of a load or a store", (long long)n, rm_out);
}
static inline SgD sgx_load(SgCtx* g, SgD address, int bytes, int disp) {
    SgX* x = sgx(g);
    const SgXSel* s = bytes == 1   ? sgx_mem_form(sgx_sel_zext8_32, sgx_sel_zext8_32_n, 0)
                      : bytes == 2 ? sgx_mem_form(sgx_sel_zext16_32, sgx_sel_zext16_32_n, 0)
                      : bytes == 4 ? sgx_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 0)
                                   : sgx_mem_form(sgx_sel_mov_64, sgx_sel_mov_64_n, 0);
    int a = sgx_reg(x, &address), m = sgx_mem(g);
    SgXIns* i = sgx_append(x, SGX_K_SEL);
    i->sel = s;
    i->fsel = s;
    i->w = (unsigned char)(bytes == 8 ? 64 : 32);
    i->mplace = s->in[0];
    i->in[5] = m;
    i->in[6] = a;
    i->mdisp = disp;
    i->def = sgx_vreg(x, x->n - 1, (unsigned)(8 * bytes));
    return sgx_val((unsigned)(8 * bytes), i->def);
}
static inline void sgx_store(SgCtx* g, SgD address, int bytes, int disp, SgD value) {
    SgX* x = sgx(g);
    const SgXSel* s = bytes == 1   ? sgx_mem_form(sgx_sel_mov_8, sgx_sel_mov_8_n, 1)
                      : bytes == 2 ? sgx_mem_form(sgx_sel_mov_16, sgx_sel_mov_16_n, 1)
                      : bytes == 4 ? sgx_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 1)
                                   : sgx_mem_form(sgx_sel_mov_64, sgx_sel_mov_64_n, 1);
    int v = sgx_reg(x, &value), a = sgx_reg(x, &address), m = sgx_mem(g);
    SgXIns* i = sgx_append(x, SGX_K_SEL);
    i->sel = s;
    i->fsel = s;
    i->w = (unsigned char)(bytes == 8 ? 64 : 32);
    i->mplace = s->out;
    i->in[0] = v;
    i->in[5] = m;
    i->in[6] = a;
    i->mdisp = disp;
    i->side = 1; /* a store: kept though nothing reads a result */
}

/* GuestMemRead(address, size, acquire): an x86 load is an acquire one (Vol. 3A, 10.2.2: loads are not reordered with
   other loads, nor stores with older loads); 16 bytes are two of 8 */
static inline SgD sg_GuestMemRead(SgCtx* g, SgD address, long long size, int acquire) {
    (void)acquire;
    if (address.w != 64) sg_fail("a guest address of other than 64 bits", address.w, 64);
    if (size == 16) return sg_d_concat(g, sgx_load(g, address, 8, 8), sgx_load(g, address, 8, 0));
    if (size != 1 && size != 2 && size != 4 && size != 8) sg_fail("a guest access of this size", size, 0);
    return sgx_load(g, address, (int)size, 0);
}
/* GuestMemWrite(address, size, release, value): a release store is MFENCE after it, as the sequentially consistent
   store the LLVM back end makes (Arm's store-release orders it with a later load-acquire, which x86's store buffer
   alone would not) */
static inline void sg_GuestMemWrite(SgCtx* g, SgD address, long long size, int release, SgD value) {
    if (address.w != 64) sg_fail("a guest address of other than 64 bits", address.w, 64);
    if (value.w != 8u * (unsigned)size) sg_fail("a guest write of a value of another size", size, value.w);
    if (size == 16) {
        sgx_store(g, address, 8, 0, sg_d_slice(g, value, 63, 0));
        sgx_store(g, address, 8, 8, sg_d_slice(g, value, 127, 64));
    } else
        sgx_store(g, address, (int)size, 0, value);
    if (release) sgx_simple(g, SGX_M_FENCE);
}

static inline int sgx_log2(long long size) { return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3; }
static inline void sg_GuestLoadExclusive(SgCtx* g, SgD address, long long size) {
    SgD args[2];
    args[0] = address;
    if (size == 16) {
        args[1] = sgx_const(32, 3);
        g->exclusive[0] = sgx_call(g, "snx_x64_ldxp", 0, 1, args, 2, 64).v;
        {
            SgXIns* i = sgx_append(sgx(g), SGX_K_LOAD);
            i->w = 64;
            i->disp = (int)offsetof(CPU, excl_val2);
            i->def = sgx_vreg(sgx(g), sgx(g)->n - 1, 64);
            g->exclusive[1] = i->def;
        }
        g->exclusive_count = 2;
    } else {
        args[1] = sgx_const(32, (SnxU64)sgx_log2(size));
        g->exclusive[0] = sgx_call(g, "snx_x64_ldx", 0, 1, args, 2, 64).v;
        g->exclusive_count = 1;
    }
    g->exclusive_next = 0;
}
static inline SgD sg_GuestExclusiveValue(SgCtx* g, SgD address, long long size) {
    (void)address;
    if (g->exclusive_next >= g->exclusive_count) sg_fail("an exclusive read with no load-exclusive before it", size, 0);
    return sg_d_slice(g, sgx_val(64, g->exclusive[g->exclusive_next++]), 8 * size - 1, 0);
}
static inline void sg_GuestStoreExclusive(SgCtx* g, SgD address, long long size, SgD value) {
    SgD args[4];
    args[0] = address;
    if (size == 16) {
        args[1] = sgx_const(32, 3);
        args[2] = sg_d_slice(g, value, 63, 0);
        args[3] = sg_d_slice(g, value, 127, 64);
        g->status = sgx_call(g, "snx_x64_stxp", 0, 1, args, 4, 32).v;
    } else {
        args[1] = sgx_const(32, (SnxU64)sgx_log2(size));
        args[2] = sg_d_zext(g, value, 64);
        g->status = sgx_call(g, "snx_x64_stx", 0, 1, args, 3, 32).v;
    }
    sgx(g)->v[g->status].zab = 1;
}
static inline SgD sg_GuestExclusiveStatus(SgCtx* g) {
    if (!g->status) sg_fail("an exclusive status with no store-exclusive before it", 0, 0);
    return sgx_val(1, g->status);
}
static inline SgD sgx_rmw(SgCtx* g, int op, SgD address, SgD v) {
    SgD args[3];
    unsigned w = v.w;
    args[0] = address;
    args[1] = sg_d_zext(g, v, 64);
    args[2] = sgx_const(32, (SnxU64)(op << 2 | sgx_log2(w / 8)));
    return sg_d_slice(g, sgx_call(g, "snx_x64_rmw", 0, 1, args, 3, 64), w - 1, 0);
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
    SgD args[5];
    unsigned w = expected.w;
    args[0] = address;
    if (w == 128) {
        SgD lo, hi;
        SgXIns* i;
        args[1] = sg_d_slice(g, expected, 63, 0);
        args[2] = sg_d_slice(g, expected, 127, 64);
        args[3] = sg_d_slice(g, desired, 63, 0);
        args[4] = sg_d_slice(g, desired, 127, 64);
        lo = sgx_call(g, "snx_x64_cas128", 0, 1, args, 5, 64);
        i = sgx_append(sgx(g), SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)offsetof(CPU, excl_val2);
        i->def = sgx_vreg(sgx(g), sgx(g)->n - 1, 64);
        hi = sgx_val(64, i->def);
        return sg_d_concat(g, hi, lo);
    }
    args[1] = sg_d_zext(g, expected, 64);
    args[2] = sg_d_zext(g, desired, 64);
    args[3] = sgx_const(32, (SnxU64)sgx_log2(w / 8));
    return sg_d_slice(g, sgx_call(g, "snx_x64_cas", 0, 1, args, 4, 64), w - 1, 0);
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
/* CLREX: the local monitor, CPU.excl_on, 0 */
static inline void sg_ClearExclusiveLocal(SgCtx* g) {
    SgX* x = sgx(g);
    SgD z = sgx_const(32, 0);
    int r = sgx_reg(x, &z);
    SgXIns* st = sgx_append(x, SGX_K_STORE);
    st->w = 32;
    st->in[0] = r;
    st->disp = (int)offsetof(CPU, excl_on);
}
static inline void sg_BTypeCompatible_write(SgCtx* g, SgB v) {
    (void)g;
    (void)v;
}
/* CRC32 and CRC32C: the helper's reflected update (BitReverse(Poly32Mod2(...)) as README.md's builtins say) */
static inline SgD sg_crc32(SgCtx* g, SgD acc_bits, SgD value_bits, long long size, SgS poly) {
    SgD args[4];
    args[0] = sg_d_zext(g, acc_bits, 64);
    args[1] = sg_d_zext(g, value_bits, 64);
    args[2] = sgx_const(32, (SnxU64)(size / 8));
    args[3] = sgx_const(32, poly.v == 0x1EDC6F41u);
    return sg_d_slice(g, sgx_call(g, "snx_x64_crc32", 0, 0, args, 4, 64), 31, 0);
}

/* BranchTo(target, ...): where the instruction goes, under the run-time condition when (0: always) */
static inline void sg_branch_to(SgCtx* g, SgD target, SgB when) {
    if (g->branched) sg_fail("an instruction that branches twice", (long long)g->word, 0);
    if (target.w != 64) sg_fail("a branch to a target of other than 64 bits", target.w, 64);
    g->branched = 1;
    g->target = target;
    g->taken = when;
}

/* ---- the ABI ----
   x86-64 System V: the state's address comes in RDI and stays in RBX (the callee's, saved by the prologue), RSP is
   16-aligned after the prologue (the push of RBX on the return address, then the frame of spill slots), and a call
   out spills what is in a caller-saved register first (sgx_assemble_region). These are bytes written here: the
   instruction selection doesn't make a function's frame */

enum { SGX_M_NONE };
/* the function emitter's instructions (labels, jumps, calls: x64_fn.h) */
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

static inline void sgx_prologue(SgXAsm* a) {
    SgXBuf* b = a->b;
    sgx_byte(b, 0x53); /* push rbx */
    sgx_byte(b, 0x48); /* mov rbx, rdi */
    sgx_byte(b, 0x89);
    sgx_byte(b, 0xFB);
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
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgXBuf* b = a->b;
    switch (i->kind) {
    case SGX_K_UNDEF:
        /* state->pc = pc, then rt->undef(state, pc, word): RDI, RSI, EDX, through RAX = state->rt */
        sgx_byte(b, 0x48); /* mov rax, imm64; mov [rbx + pc], rax */
        sgx_byte(b, 0xB8);
        sgx_imm(b, i->imm, 8);
        sgx_byte(b, 0x48);
        sgx_byte(b, 0x89);
        sgx_byte(b, 0x83);
        sgx_imm(b, (long long)offsetof(CPU, pc), 4);
        sgx_byte(b, 0x48); /* mov rdi, rbx */
        sgx_byte(b, 0x89);
        sgx_byte(b, 0xDF);
        sgx_byte(b, 0x48); /* mov rsi, imm64 */
        sgx_byte(b, 0xBE);
        sgx_imm(b, i->imm, 8);
        sgx_byte(b, 0xBA); /* mov edx, imm32 */
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
        case SGX_M_CALL:
        case SGX_M_CALL_RT:
            if (i->w) {
                sgx_byte(b, 0x48); /* mov rdi, rbx: the state */
                sgx_byte(b, 0x89);
                sgx_byte(b, 0xDF);
            }
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

/* the dispatcher sailgen writes after this */
static int sg_decode(SgCtx* g);

/* words in a row as one function, u64 block(CPU*) returning the next pc, their instructions one SSA: a register read
   once, flags one instruction left read by the next as a condition code (CMP then B.cond is CMP and CMOVcc), the stores
   a later one overwrites dropped. Every word but the last runs on to the next: a branch or an UNDEF ends the block
   (the words after it are left out: count is how many it took). Its size in code, 0 when a word isn't taken, needs a
   primitive this host has no form of, or the code needs more than cap bytes */
static size_t sg_x64_block(SgCtx* g, const SnxU32* words, size_t* count, SnxU64 pc, SnxU64 features,
                           unsigned host_features, unsigned char* code, size_t cap) {
    SgXBuf b;
    SgXAsm a;
    SgXIns* r;
    size_t i;
    int pool = g->x.pool;
    SnxU64 (*resolve)(const char*) = g->resolve;
    memset(g, 0, sizeof *g);
    g->resolve = resolve;
    g->features = features;
    g->origin = -1;
    g->x.features = host_features;
    g->x.flags_ins = -1;
    g->x.base = 3; /* RBX */
    g->x.nv = 1;   /* 0 is none */
    g->x.nb = 1;
    g->x.pool = pool;
    for (i = 0; i < *count; ++i) {
        g->word = words[i];
        g->pc = pc + 4 * i;
        g->start = g->x.n;
        if (!sg_decode(g) || g->x.unmade) return 0;
        if (g->branched || g->undefined) {
            ++i;
            break;
        }
    }
    *count = i;
    pc = g->pc;
    r = NULL;
    if (g->branched && !g->undefined) {
        SgD next = g->taken ? sg_d_select(g, g->taken, g->target, sgx_const(64, pc + 4)) : g->target;
        int v = sgx_reg(&g->x, &next);
        r = sgx_append(&g->x, SGX_K_RET);
        r->in[0] = v;
        r->fixed[0] = 0; /* RAX */
    } else {
        r = sgx_append(&g->x, SGX_K_RET);
        r->imm = (long long)(g->undefined ? pc : pc + 4);
    }
    b.p = code;
    b.n = 0;
    b.cap = cap;
    b.disp = 0;
    b.index = -1;
    memset(&a, 0, sizeof a);
    a.b = &b;
    a.resolve = g->resolve;
    sgx_prologue(&a);
    sgx_assemble_region(&a, &g->x);
    sgx_patch_frame(&a);
    return b.n <= cap ? b.n : 0;
}

/* one word's code as a step, u64 step(CPU*) returning the next pc (the word's own after UNDEF): sg_x64_block of it */
static size_t sg_x64_step(SgCtx* g, SnxU32 word, SnxU64 pc, SnxU64 features, unsigned host_features,
                          unsigned char* code, size_t cap) {
    size_t one = 1;
    return sg_x64_block(g, &word, &one, pc, features, host_features, code, cap);
}
