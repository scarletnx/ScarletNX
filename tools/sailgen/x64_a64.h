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
    unsigned char gk_known[32]; /* the X registers the block wrote a constant to, and those */
    SnxU64 gk[32];
    signed char pin[32]; /* the host register each X register (31: SP) lives in for the whole function, -1 none:
                            x64_codegen.c's choice (a step's or a block's: none, pinned 0) */
    int pinned;      /* pin[] is in use */
    signed char vpin[32]; /* the XMM register each V register lives in for the whole function, -1 none (pinned) */
    signed char mempin;   /* the host register the guest memory's base (CPU.mem) is in, -1 none (pinned) */
    int vd[32][2];   /* the same of each SIMD&FP register's doublewords (low, high) */
    int vx[32];      /* each SIMD&FP register as an XMM value, where the block has it so (one cache or the other) */
    SgB fp_state[2]; /* the hot paths' check of FPCR's mode and FPSR (sgx_fp_bad), once a region: without, with IXC */
    int fp_bound[2]; /* sgx_fp_bound's register for 32 and 64 bits, once a region with fp_state (0: none yet) */
    int fp_dbound[2]; /* sgx_fp_flushed's register for 32 and 64 bits, once a region (0: none yet) */
    SgB fp_fz;        /* sgx_fp_bad's check of FPCR.FZ alone, once a region */
    int origin;      /* the instruction whose flags NZCV was last written from, -1 for none */
    int mem;         /* the virtual register of CPU.mem (guest memory's host address), 0 until read */
    int exclusive[2], exclusive_count, exclusive_next, status; /* a load-exclusive's values, a store's status */
    SnxU64 (*resolve)(const char* sym); /* a helper's address, where the code calls it absolutely (a test's) */
    int start;       /* the instructions before this word's */
};

static inline SgX* sgx(SgCtx* g) { return &g->x; }
/* x64.h's: nothing of A64's comes between flags and their reader */
static inline void sgx_flags_begin(SgCtx* g) { (void)g; }

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
    if (g->gk_known[n]) return sgx_const(64, g->gk[n]); /* MOVZ, MOVK...: one constant */
    if (!g->gpr[n] && g->pinned && g->pin[n] >= 0) {
        /* a copy of the host register it lives in (an SSA value: the register changes as the guest's does) */
        SgXIns* i = sgx_append(x, SGX_K_PGET);
        i->imm = g->pin[n];
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->gpr[n] = i->def;
    }
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
    g->gk_known[n] = sgx_is_const(&v) != 0;
    g->gk[n] = v.k;
    r = sgx_reg(x, &v);
    if (g->pinned && g->pin[n] >= 0) {
        i = sgx_append(x, SGX_K_PSET);
        i->imm = g->pin[n];
        i->in[0] = r;
        i->side = 1;
        g->gpr[n] = r;
        return;
    }
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
    i->w = 8; /* the flag's low byte (0 or 1): what STORE_FLAG's SETcc wrote */
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

/* x64.h's: the state's words of the registers (bits 0-30: X0-X30, 31 SP) and flags (32-35: N Z C V) dead names */
static inline int sgx_dead_words(SnxU64 dead, SnxU32 vdead, unsigned short* words) {
    int n = 0, r, k;
    for (r = 0; r < 32; ++r)
        if (vdead >> r & 1)
            for (k = 0; k < 4; ++k) words[n++] = (unsigned short)(((offsetof(CPU, v) + 16 * (size_t)r) >> 2) + (unsigned)k);
    for (r = 0; r < 32; ++r)
        if (dead >> r & 1) {
            unsigned w = (unsigned)(offsetof(CPU, x) + 8 * (size_t)r) >> 2;
            words[n++] = (unsigned short)w;
            words[n++] = (unsigned short)(w + 1);
        }
    for (r = 0; r < 4; ++r)
        if (dead >> (32 + r) & 1) words[n++] = (unsigned short)((unsigned)sgx_flag_offset(r) >> 2);
    return n;
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

enum {
    SGX_M_CALL = 1, SGX_M_CALL_RT, SGX_M_FENCE, SGX_M_PAUSE,
    /* inline, their inputs in fixed registers (sgx_mach): the guest's memory base in R10, the address in RSI */
    SGX_M_STX,  /* STXR: value RDX; imm the size's log2; status in RAX */
    SGX_M_STXP, /* STXP: values R8, R9; imm 2 (words) or 3; status in RAX */
    SGX_M_CAS,  /* CAS: expected RAX, desired RDX; imm log2; the old value in RAX */
    SGX_M_CASP, /* CASP of doublewords: expected RDX:RAX, desired R9:R8; the old pair in RDX:RAX */
    SGX_M_RMW,  /* LDADD...SWP: value RDX; imm op << 2 | log2 (x64_helpers.c's snx_x64_rmw's); the old value in RAX */
    SGX_M_CRC   /* CRC32, CRC32C: the accumulator RAX, value RDX; imm bytes | castagnoli << 4; the result in RAX */
};
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
       choice) is first made 0 or 1 in a register; and it may change FPCR (MSR's snx_msr): the FP state check again */
    sgx_flags_clobbered(x);
    g->fp_state[0] = g->fp_state[1] = 0;
    g->fp_bound[0] = g->fp_bound[1] = 0;
    g->fp_dbound[0] = g->fp_dbound[1] = 0;
    g->fp_fz = 0;
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
/* V[n] pinned to an XMM register: the block's value of it, the register's own (SGX_K_PGET) the first time */
static inline int sgx_vpinned(SgCtx* g, long long n) { return g->pinned && g->vpin[n] >= 0; }
static inline void sgx_vpget(SgCtx* g, long long n) {
    SgX* x = sgx(g);
    SgXIns* i;
    if (g->vx[n]) return;
    i = sgx_append(x, SGX_K_PGET);
    i->imm = 16 + g->vpin[n];
    i->def = sgx_xvreg(x, x->n - 1);
    g->vx[n] = i->def;
    g->vd[n][0] = g->vd[n][1] = 0;
}
/* V[n] (pinned) = the XMM value r */
static inline void sgx_vpset(SgCtx* g, long long n, int r) {
    SgX* x = sgx(g);
    SgXIns* i = sgx_append(x, SGX_K_PSET);
    i->imm = 16 + g->vpin[n];
    i->in[0] = r;
    i->side = 1;
    g->vx[n] = r;
    g->vd[n][0] = g->vd[n][1] = 0;
}
static inline SgD sgx_vd_read(SgCtx* g, long long n, int k) {
    SgX* x = sgx(g);
    if (sgx_vpinned(g, n)) sgx_vpget(g, n);
    if (g->vx[n]) {
        SgD d = sg_d_slice(g, sgx_xval(128, g->vx[n]), 64 * k + 63, 64 * k);
        sgx_reg(x, &d);
        return d;
    }
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
    if (sgx_vpinned(g, n)) {
        /* the doubleword into the register's value: PINSRQ */
        int in[4];
        sgx_vpget(g, n);
        in[0] = g->vx[n], in[1] = r, in[2] = in[3] = 0;
        sgx_vpset(g, n, sgx_vins(x, &sgx_v_pinsrq, 0, 1, in, k, 0, SGX_VM_NONE, 0, 0, 0, 0, 0));
        return;
    }
    SgXIns* i = sgx_append(x, SGX_K_STORE);
    i->w = 64;
    i->in[0] = r;
    i->disp = sgx_vd_offset(n, k);
    if (g->vx[n]) {
        /* the other doubleword from the XMM value, which is no longer the register's */
        if (!g->vd[n][!k]) g->vd[n][!k] = sgx_vd_read(g, n, !k).v;
        g->vx[n] = 0;
    }
    g->vd[n][k] = r;
}
/* _V[n]'s bits hi..lo: of the XMM value the block has (MOVDQU from the state the first time), its low bits or an
   element PSHUFD brings down (sg_d_slice); a register the block wrote as doublewords from those */
static inline SgD sg__V_read_bits(SgCtx* g, long long n, long long hi, long long lo) {
    SgX* x = sgx(g);
    if (n < 0 || n > 31) sg_fail("a SIMD&FP register this back end does not keep", n, 31);
    if (sgx_vpinned(g, n)) sgx_vpget(g, n);
    if (!g->vx[n] && (g->vd[n][0] || g->vd[n][1])) {
        if (lo == 0 && hi < 64) return sg_d_slice(g, sgx_vd_read(g, n, 0), hi, 0);
        if (lo == 64 && hi <= 127) return sg_d_slice(g, sgx_vd_read(g, n, 1), hi - 64, 0);
        if (lo == 0 && hi == 127) return sg_d_concat(g, sgx_vd_read(g, n, 1), sgx_vd_read(g, n, 0));
        sg_fail("bits of a SIMD&FP register across its doublewords", hi, lo);
    }
    if (!g->vx[n]) g->vx[n] = sgx_vins(x, &sgx_v_movdqu_ld, 0, 0, NULL, 0, 1, SGX_VM_STATE, 0, 0, sgx_vd_offset(n, 0), 0, 0);
    return sg_d_slice(g, sgx_xval(128, g->vx[n]), hi, lo);
}
/* _V[n] = v, all 128 bits: an XMM value by MOVDQU, general-purpose doublewords by two MOVs */
static inline void sg__V_write(SgCtx* g, long long n, SgD v) {
    SgX* x = sgx(g);
    SgD lo, hi;
    if (n < 0 || n > 31) sg_fail("a SIMD&FP register this back end does not keep", n, 31);
    if (v.w != 128 || v.wide || v.inv || v.nzcv) sg_fail("a SIMD&FP register written from other than 128 bits", v.w, 128);
    if (sgx_vpinned(g, n)) {
        sgx_vpset(g, n, v.x ? v.x : sgx_xreg(x, &v));
        return;
    }
    if (v.x) {
        int in[4] = {0, v.x, 0, 0};
        sgx_vins(x, &sgx_v_movdqu_st, 0, 0, in, 0, 0, SGX_VM_STATE, 0, 0, sgx_vd_offset(n, 0), 0, 0);
        g->vx[n] = v.x;
        g->vd[n][0] = g->vd[n][1] = 0;
        return;
    }
    lo = v.v ? sgx_val(64, v.v) : sgx_const(64, v.k);
    hi = v.vh ? sgx_val(64, v.vh) : sgx_const(64, v.kh);
    g->vx[n] = 0;
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
/* ---- the hot paths ----
   Each operation is the SSE instruction's, and the exact helper's (snx_x64_fp_*) on a cold path where the result
   may not be the console's: the guards are the LLVM back end's (llvm.h's sg_fp_fast_*). FPCR rounds to nearest
   (DN matters to NaNs only, which go cold), under FZ no operand is a denormal (sgx_fp_flushed: many games run with FZ
   set, and a guard of FZ clear sent all of their arithmetic to the helpers), FPSR's IXC is set already where the
   operation may be inexact (the inline code doesn't raise it), and the result is a normal number above the smallest
   (no underflow, overflow or NaN: no flag but IXC, and nothing FZ flushes) or a zero that is exact. The host's MXCSR
   is its default, round to nearest without FTZ or DAZ, as the runtime keeps it */

#define SGX_FPCR_RMODE (3u << 22)
#define SGX_FPCR_FZ (1u << 24)
#define SGX_FPCR_AHP (1u << 26)
#define SGX_FPSR_IXC (1u << 4)
/* FPCR's mask bits set, or (ixc) FPSR's IXC clear: the hot path may not be the console's. Made once a region for
   FPCR's mode and FPSR's values, and for FZ alone (an MSR or a call forgets them: sgx_call) */
static inline SgB sgx_fp_bad(SgCtx* g, SgD fpcr, SnxU32 mask, int ixc) {
    SgX* x = sgx(g);
    SgD f, bad;
    if (mask == SGX_FPCR_RMODE && g->fp_state[ixc]) return g->fp_state[ixc];
    if (mask == SGX_FPCR_FZ && !ixc && g->fp_fz) return g->fp_fz;
    f = sg_d_and(g, sg_d_zext(g, sg_d_slice(g, fpcr, 31, 0), 32), sgx_const(32, mask));
    if (ixc) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        SgD fpsr;
        i->w = 32;
        i->disp = (int)offsetof(CPU, fpsr);
        i->def = sgx_vreg(x, x->n - 1, 32);
        fpsr = sgx_val(32, i->def);
        f = sg_d_or(g, f, sg_d_xor(g, sg_d_and(g, fpsr, sgx_const(32, SGX_FPSR_IXC)), sgx_const(32, SGX_FPSR_IXC)));
    }
    bad = f;
    {
        SgB b = sg_b_not(g, sg_d_is_zero(g, bad));
        if (mask == SGX_FPCR_RMODE) g->fp_state[ixc] = b;
        if (mask == SGX_FPCR_FZ && !ixc) g->fp_fz = b;
        return b;
    }
}
/* a float's bits (w 32 or 64) */
static inline SgD sgx_fp_bits(SgCtx* g, SgD v) {
    SgD d = v;
    sgx_reg(sgx(g), &d);
    return d;
}
/* |bits| in (the smallest normal, infinity): neither zero, tiny, infinite nor a NaN */
static inline SgB sgx_fp_roomy(SgCtx* g, SgD bits, unsigned w) {
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull, inf = w == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
    SgD m = sg_d_lsl(g, bits, 1); /* the magnitude, doubled: the sign shifted out */
    SgD t = sg_d_sub(g, m, sgx_const(w, (normal + 1) << 1));
    return sgx_d_ult(g, t, sgx_const(w, (inf - normal - 1) << 1));
}
/* the hot path's test of a result r (w 32 or 64): its doubled magnitude less the smallest normal's (and 1 more) below
   the bound, unsigned: a normal number above the smallest, below infinity. The bound is 0 where FPCR rounds otherwise
   or FPSR's IXC is clear: a register made once a region (sgx_fp_bound), so the test costs a MOVD, LEA, CMP and Jcc
   and no load of FPCR or FPSR an operation */
static inline SgD sgx_fp_bound(SgCtx* g, SgD fpcr, unsigned w) {
    SgX* x = sgx(g);
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull, inf = w == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
    SgD b;
    if (g->fp_bound[w == 64]) return sgx_val(w, g->fp_bound[w == 64]);
    b = sg_d_select(g, sgx_fp_bad(g, fpcr, SGX_FPCR_RMODE, 1), sgx_const(w, 0), sgx_const(w, (inf - normal - 1) << 1));
    sgx_reg(x, &b);
    g->fp_bound[w == 64] = b.v;
    return b;
}
static inline SgB sgx_fp_hot(SgCtx* g, SgD r, SgD fpcr, unsigned w) {
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull;
    SgD bound = sgx_fp_bound(g, fpcr, w), rb = sgx_fp_bits(g, r);
    return sgx_d_ult(g, sg_d_sub(g, sg_d_lsl(g, rb, 1), sgx_const(w, (normal + 1) << 1)), bound);
}
/* a denormal operand v (w 32 or 64) that FPCR.FZ flushes, raising IDC: the helpers' case (llvm.h's
   sg_fp_unflushed). Its doubled magnitude less 2 below the bound, unsigned: the doubled smallest normal less 2 where
   FZ is set, 0 where it is clear (never), a register made once a region, so the test costs a LEA, CMP and Jcc */
static inline SgB sgx_fp_flushed(SgCtx* g, SgD fpcr, SgD v, unsigned w) {
    SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull;
    SgD b;
    if (g->fp_dbound[w == 64])
        b = sgx_val(w, g->fp_dbound[w == 64]);
    else {
        b = sg_d_select(g, sgx_fp_bad(g, fpcr, SGX_FPCR_FZ, 0), sgx_const(w, (normal << 1) - 2), sgx_const(w, 0));
        sgx_reg(sgx(g), &b);
        g->fp_dbound[w == 64] = b.v;
    }
    return sgx_d_ult(g, sg_d_sub(g, sg_d_lsl(g, sgx_fp_bits(g, v), 1), sgx_const(w, 2)), b);
}
/* sgx_fp_hot of r, and no operand FZ flushes */
static inline SgB sgx_fp_hot_of(SgCtx* g, SgD r, SgD fpcr, unsigned w, const SgD* ops, int nops) {
    SgB ok = sgx_fp_hot(g, r, fpcr, w);
    int k;
    for (k = 0; k < nops; ++k) ok = sg_b_and(g, ok, sg_b_not(g, sgx_fp_flushed(g, fpcr, ops[k], w)));
    return ok;
}
/* the cold path of an operation sgx_fp_hot tests: the helper's call where not ok, but a zero result the hot path's
   where FPCR rounds to nearest, under FZ no operand is a denormal (llvm.h's sg_fp_unflushed: many games run with FZ
   set and compute zeros), and (zargs, a mask of ops) one of those operands is a zero (none: any zero is exact, as a
   sum's is) */
static inline SgD sgx_fp_cold_z(SgCtx* g, SgB ok, SgD fast, const char* sym, SgD* ops, int nops, SnxU32 desc,
                                unsigned zargs) {
    return sgx_cold_fp(sgx(g), sg_b_not(g, ok), fast, sym, ops, nops, nops, nops + 1, desc, (int)offsetof(CPU, fpcr),
                       (int)offsetof(CPU, fpsr), 0xC0 | zargs, SGX_FPCR_RMODE);
}
static inline SgB sgx_fp_zero(SgCtx* g, SgD bits) { return sg_d_is_zero(g, sg_d_lsl(g, bits, 1)); }
static inline SgB sgx_fp_nan(SgCtx* g, SgD bits, unsigned w) {
    SnxU64 inf = w == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
    /* magnitude > infinity: infinity's doubled magnitude < the doubled bits', unsigned */
    return sgx_d_ult(g, sgx_const(w, inf << 1), sg_d_lsl(g, bits, 1));
}
/* the arguments of an snx_x64_fp_* helper (x64_helpers.c): the operands' bits, FPCR, the constants packed */
static inline SgD sgx_fp_cold(SgCtx* g, SgB bad, SgD fast, const char* sym, SgD* ops, int nops, SgD fpcr, int has_desc,
                              SnxU32 desc) {
    (void)fpcr;
    if (nops > 3) sg_fail("an FP helper of more than three operands", nops, 0);
    return sgx_cold_fp(sgx(g), bad, fast, sym, ops, nops, nops, has_desc ? nops + 1 : -1, desc,
                       (int)offsetof(CPU, fpcr), (int)offsetof(CPU, fpsr), 0, 0);
}
/* an operand as an XMM register */
static inline int sgx_fx(SgCtx* g, SgD* v) { return sgx_xreg(sgx(g), v); }

static inline SgD sgx_fp_binary(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned op) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    static const SgXVForm* const forms[4][2] = {{&sgx_v_addss, &sgx_v_addsd},
                                                {&sgx_v_subss, &sgx_v_subsd},
                                                {&sgx_v_mulss, &sgx_v_mulsd},
                                                {&sgx_v_divss, &sgx_v_divsd}};
    SgD ab, bb, r, rb, ops[2];
    SgB ok;
    if ((w != 32 && w != 64) || path || op > SNX_FP_DIV) {
        SgD o[2] = {a, b};
        return sgx_fp(g, "snx_x64_fp_binary", path, fpcr, o, 2, 1, a.w | op << 8, a.w);
    }
    (void)ab, (void)bb, (void)rb;
    r = sgx_xval(w, sgx_v2(x, forms[op][w == 64], sgx_fx(g, &a), sgx_fx(g, &b), 0));
    ops[0] = a;
    ops[1] = b;
    ok = sgx_fp_hot_of(g, r, fpcr, w, ops, 2);
    return sgx_fp_cold_z(g, ok, r, "snx_x64_fp_binary", ops, 2, w | op << 8,
                         op == SNX_FP_ADD || op == SNX_FP_SUB ? 0 : op == SNX_FP_MUL ? 3 : 1);
}
static inline SgD sg_FPAdd(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_ADD); }
static inline SgD sg_FPSub(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_SUB); }
static inline SgD sg_FPMul(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_MUL); }
static inline SgD sg_FPDiv(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_DIV); }

/* FPMax and FPMin: without a NaN, and without FZ (a denormal flushed), the larger or smaller operand, and of equal
   ones their AND (max: +0 over -0) or OR (min): MAXSD's or MINSD's, which give the second of equal ones, replaced by
   that where CMPEQSD says. The Num forms take the number over a quiet NaN: with a NaN, the helper */
static inline SgD sgx_fp_minmax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned minimum, unsigned numeric) {
    SgX* x = sgx(g);
    unsigned w = a.w, d = w == 64;
    SgD ab, bb, ops[2];
    int ax, bx, m, t, r, n;
    SgB bad;
    if ((w != 32 && w != 64) || path) {
        SgD o[2] = {a, b};
        return sgx_fp(g, "snx_x64_fp_minmax", path, fpcr, o, 2, 1, a.w | minimum << 8 | numeric << 9, a.w);
    }
    ab = sgx_fp_bits(g, a);
    bb = sgx_fp_bits(g, b);
    ax = sgx_fx(g, &a);
    bx = sgx_fx(g, &b);
    m = sgx_v2(x, d ? &sgx_v_cmpsd : &sgx_v_cmpss, ax, bx, 0); /* all ones where equal */
    t = sgx_v2(x, minimum ? &sgx_v_orpd : &sgx_v_andpd, ax, bx, 0);
    r = sgx_v2(x, minimum ? (d ? &sgx_v_minsd : &sgx_v_minss) : (d ? &sgx_v_maxsd : &sgx_v_maxss), ax, bx, 0);
    n = sgx_v2(x, &sgx_v_andnpd, m, r, 0); /* the MAXSD where not equal */
    t = sgx_v2(x, &sgx_v_andpd, m, t, 0);
    r = sgx_v2(x, &sgx_v_orpd, n, t, 0);
    bad = sg_b_or(g, sg_b_or(g, sgx_fp_flushed(g, fpcr, ab, w), sgx_fp_flushed(g, fpcr, bb, w)),
                  sg_b_or(g, sgx_fp_nan(g, ab, w), sgx_fp_nan(g, bb, w)));
    ops[0] = ab;
    ops[1] = bb;
    return sgx_fp_cold(g, bad, sgx_xval(w, r), "snx_x64_fp_minmax", ops, 2, fpcr, 1, w | minimum << 8 | numeric << 9);
}
static inline SgD sg_FPMax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 0); }
static inline SgD sg_FPMin(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 0); }
static inline SgD sg_FPMaxNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 1); }
static inline SgD sg_FPMinNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 1); }

/* addend + a * b rounded once: VFMADD231 where the host has FMA; a zero result only where the product is a zero */
static inline SgD sg_FPMulAdd(SgCtx* g, SgB path, SgD addend, SgD a, SgD b, SgD fpcr) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD cb, ab, bb, r, rb, ops[3];
    SgB ok;
    if ((w != 32 && w != 64) || path || !(x->features & SGX_F_FMA)) {
        SgD o[3] = {addend, a, b};
        return sgx_fp(g, "snx_x64_fp_fma", path, fpcr, o, 3, 1, a.w, a.w);
    }
    (void)cb, (void)ab, (void)bb, (void)rb;
    {
        int in[4] = {sgx_fx(g, &addend), sgx_fx(g, &a), sgx_fx(g, &b), 0};
        r = sgx_xval(w, sgx_vins(x, w == 64 ? &sgx_v_vfmadd231sd : &sgx_v_vfmadd231ss, 0, 1, in, 0, 0, SGX_VM_NONE, 0,
                                 0, 0, 0, 0));
    }
    ops[0] = addend;
    ops[1] = a;
    ops[2] = b;
    ok = sgx_fp_hot_of(g, r, fpcr, w, ops, 3);
    return sgx_fp_cold_z(g, ok, r, "snx_x64_fp_fma", ops, 3, w, 6);
}
static inline SgD sg_FPSqrt(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD ab, r, rb;
    SgB ok;
    if ((w != 32 && w != 64) || path) return sgx_fp(g, "snx_x64_fp_unary", path, fpcr, &a, 1, 1, a.w | SNX_FP_SQRT << 8, a.w);
    (void)ab, (void)rb;
    r = sgx_xval(w, sgx_v1(x, w == 64 ? &sgx_v_sqrtsd : &sgx_v_sqrtss, sgx_fx(g, &a), 0));
    ok = sgx_fp_hot_of(g, r, fpcr, w, &a, 1);
    return sgx_fp_cold_z(g, ok, r, "snx_x64_fp_unary", &a, 1, w | SNX_FP_SQRT << 8, 1);
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
/* ROUNDSx's immediate for FPRounding's TIEEVEN, POSINF, NEGINF, ZERO (FPCR's: TIEEVEN, which the guard holds it to);
   -1 for TIEAWAY, which x86 doesn't round by */
static inline int sgx_round_imm(int rounding) {
    switch (rounding) {
    case 0:
    case SG_FP_ROUNDING_FPCR: return 0;
    case 1: return 2;
    case 2: return 1;
    case 3: return 3;
    default: return -1;
    }
}
/* FRINTN, P, M, Z, X, I (FRINTA: the helper): ROUNDSx; a NaN, a denormal FZ flushes or FPCR's other modes go cold,
   FRINTX's IXC too until it is set */
static inline SgD sg_FPRoundInt(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, int exact) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    int imm = sgx_round_imm(rounding);
    SgD ab, r;
    SgB bad;
    SnxU32 desc = w | sgx_fp_rm(rounding) << 8 | (SnxU32)!!exact << 12;
    if ((w != 32 && w != 64) || path || imm < 0 || !(x->features & SGX_F_SSE41))
        return sgx_fp(g, "snx_x64_fp_round", path, fpcr, &a, 1, 1, desc, a.w);
    ab = sgx_fp_bits(g, a);
    r = sgx_xval(w, sgx_v1(x, w == 64 ? &sgx_v_roundsd : &sgx_v_roundss, sgx_fx(g, &a), imm | 8));
    bad = sg_b_or(g, sgx_fp_bad(g, fpcr, rounding == SG_FP_ROUNDING_FPCR ? SGX_FPCR_RMODE : 0, exact),
                  sg_b_or(g, sgx_fp_nan(g, ab, w), sgx_fp_flushed(g, fpcr, ab, w)));
    return sgx_fp_cold(g, bad, r, "snx_x64_fp_round", &ab, 1, fpcr, 1, desc);
}
static inline SgD sg_FPRoundIntN(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, long long intsize) {
    return sgx_fp(g, "snx_x64_fp_round_int", path, fpcr, &a, 1, 1,
                  a.w | sgx_fp_rm(rounding) << 8 | (SnxU32)intsize << 16, a.w);
}
/* FCVT between singles and doubles: CVTSS2SD (exact, but a denormal single FZ flushes) and CVTSD2SS (a single's roomy
   result, or a zero's: a denormal double gives neither); a NaN goes cold. Halves: the helper */
static inline SgD sg_FPConvert__1(SgCtx* g, SgB path, SgD op, SgD fpcr, long long to_bits) {
    SgX* x = sgx(g);
    unsigned from = op.w, to = (unsigned)to_bits;
    SgD ob, r;
    SgB bad;
    if (to != 16 && to != 32 && to != 64) sg_fail("a conversion to a float of this width", to_bits, 0);
    if (path || !((from == 32 && to == 64) || (from == 64 && to == 32)))
        return sgx_fp(g, "snx_x64_fp_convert", path, fpcr, &op, 1, 1, op.w | (SnxU32)to_bits << 8, to);
    ob = sgx_fp_bits(g, op);
    r = sgx_xval(to, sgx_v1(x, to == 64 ? &sgx_v_cvtss2sd : &sgx_v_cvtsd2ss, sgx_fx(g, &op), 0));
    if (to == 64)
        bad = sg_b_or(g, sgx_fp_flushed(g, fpcr, ob, 32), sgx_fp_nan(g, ob, 32));
    else {
        SgD rb = sgx_fp_bits(g, r);
        bad = sg_b_or(g, sgx_fp_bad(g, fpcr, SGX_FPCR_RMODE, 1),
                      sg_b_not(g, sg_b_or(g, sgx_fp_roomy(g, rb, 32), sgx_fp_zero(g, ob))));
    }
    return sgx_fp_cold(g, bad, r, "snx_x64_fp_convert", &ob, 1, fpcr, 1, from | to << 8);
}
/* the NZCV nibble (N bit 3 ... V bit 0): UCOMISx's flags, ordered (an unordered pair, which may raise IOC, and a
   denormal FZ flushes go cold): N below, Z equal, C not below, V 0 */
static inline SgD sg_FPCompare(SgCtx* g, SgB path, SgD a, SgD b, int signal, SgD fpcr) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD ab, bb, nib, ops[2];
    SgB n, z, c, un, bad;
    int ins;
    SnxU32 desc = w | (SnxU32)!!signal << 8;
    if ((w != 32 && w != 64) || path) {
        SgD o[2] = {a, b};
        return sgx_fp(g, "snx_x64_fp_compare", path, fpcr, o, 2, 1, desc, 4);
    }
    ab = sgx_fp_bits(g, a);
    bb = sgx_fp_bits(g, b);
    {
        int in[4] = {sgx_fx(g, &a), sgx_fx(g, &b), 0, 0};
        sgx_vins(x, w == 64 ? &sgx_v_ucomisd : &sgx_v_ucomiss, -1, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    ins = x->flags_ins;
    n = sgx_bool(x, SGX_B_CC, ins, 2);   /* B */
    z = sgx_bool(x, SGX_B_CC, ins, 4);   /* E */
    c = sgx_bool(x, SGX_B_CC, ins, 3);   /* AE */
    un = sgx_bool(x, SGX_B_CC, ins, 10); /* P */
    nib = sg_d_concat(g, sg_d_concat(g, sgx_bool_bits(x, n), sgx_bool_bits(x, z)),
                      sg_d_concat(g, sgx_bool_bits(x, c), sgx_const(1, 0)));
    bad = sg_b_or(g, un, sg_b_or(g, sgx_fp_flushed(g, fpcr, ab, w), sgx_fp_flushed(g, fpcr, bb, w)));
    ops[0] = ab;
    ops[1] = bb;
    return sg_d_slice(g, sgx_fp_cold(g, bad, sg_d_zext(g, nib, 32), "snx_x64_fp_compare", ops, 2, fpcr, 1, desc), 3, 0);
}
/* SCVTF, UCVTF: CVTSI2Sx of 64 bits (an unsigned word zero extended; an unsigned doubleword of its top bit set, the
   helper), times 2^-fbits (exact: no result is tiny). Exact where the integer fits the significand; else once IXC
   is set, in round to nearest */
static inline SgD sg_FixedToFP(SgCtx* g, SgB path, SgD op, long long fbits, int uns, SgD fpcr, int rounding,
                               long long width) {
    SgX* x = sgx(g);
    unsigned iw = op.w, w = (unsigned)width, p = w == 32 ? 24 : 53;
    SnxU32 desc = op.w | (SnxU32)width << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17;
    SgD v, r;
    SgB fits, bad;
    if (rounding != SG_FP_ROUNDING_FPCR) sg_fail("FixedToFP in a mode of its own: the helper rounds as FPCR says", rounding, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FixedToFP of these fraction bits", fbits, 0);
    if (path || (w != 32 && w != 64) || (iw != 32 && iw != 64))
        return sgx_fp(g, "snx_x64_fp_from_int", path, fpcr, &op, 1, 1, desc, (unsigned)width);
    v = sgx_fp_bits(g, op);
    v = uns || iw == 64 ? sg_d_zext(g, v, 64) : sg_d_sext(g, v, 64);
    {
        int in[4] = {0, sgx_reg(x, &v), 0, 0}, rx;
        rx = sgx_vins(x, w == 64 ? &sgx_v_cvtsi2sd_64 : &sgx_v_cvtsi2ss_64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        if (fbits) {
            SnxU64 scale = w == 64 ? (SnxU64)(1023 - fbits) << 52 : (SnxU64)(127 - fbits) << 23;
            rx = sgx_v2(x, w == 64 ? &sgx_v_mulsd : &sgx_v_mulss, rx, sgx_xconst(x, scale, 0), 0);
        }
        r = sgx_xval(w, rx);
    }
    /* |v| < 2^p: v + 2^p below 2^(p+1) for a signed one, v below 2^p for an unsigned */
    if (uns) fits = sgx_d_ult(g, v, sgx_const(64, 1ull << p));
    else
        fits = sgx_d_ult(g, sg_d_add(g, v, sgx_const(64, 1ull << p)), sgx_const(64, 1ull << (p + 1)));
    bad = sg_b_and(g, sgx_fp_bad(g, fpcr, SGX_FPCR_RMODE, 1), sg_b_not(g, fits));
    if (uns && iw == 64) bad = sg_b_or(g, bad, sg_b_not(g, sgx_d_ult(g, v, sgx_const(64, 1ull << 63))));
    return sgx_fp_cold(g, bad, r, "snx_x64_fp_from_int", &v, 1, fpcr, 1, desc);
}
/* FCVTZS, FCVTZU and the other modes' (FCVTAS, TIEAWAY: the helper): times 2^fbits, ROUNDSx to the mode, CVTTSx2SI
   to 64 bits; out of range (CVTT's 'indefinite' integer, a negative or too large unsigned one), a NaN, a denormal FZ
   flushes, or IXC still clear goes cold */
static inline SgD sg_FPToFixed(SgCtx* g, SgB path, SgD op, long long fbits, int uns, SgD fpcr, int rounding,
                               long long integer) {
    SgX* x = sgx(g);
    unsigned w = op.w, iw = (unsigned)integer;
    int imm = sgx_round_imm(rounding);
    SnxU32 desc = op.w | (SnxU32)integer << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17 | sgx_fp_rm(rounding) << 24;
    SgD ob, r;
    SgB bad;
    int vx;
    if (rounding == SG_FP_ROUNDING_FPCR) sg_fail("FPToFixed in FPCR's mode: the helper takes the encoding's", rounding, 0);
    if (integer != 32 && integer != 64) sg_fail("a conversion to an integer of this width", integer, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FPToFixed of these fraction bits", fbits, 0);
    if (path || (w != 32 && w != 64) || imm < 0 || (imm != 3 && !(x->features & SGX_F_SSE41)))
        return sgx_fp(g, "snx_x64_fp_to_int", path, fpcr, &op, 1, 1, desc, (unsigned)integer);
    ob = sgx_fp_bits(g, op);
    vx = sgx_fx(g, &op);
    if (fbits) {
        SnxU64 scale = w == 64 ? (SnxU64)(1023 + fbits) << 52 : (SnxU64)(127 + fbits) << 23;
        vx = sgx_v2(x, w == 64 ? &sgx_v_mulsd : &sgx_v_mulss, vx, sgx_xconst(x, scale, 0), 0);
    }
    if (imm != 3) vx = sgx_v1(x, w == 64 ? &sgx_v_roundsd : &sgx_v_roundss, vx, imm | 8);
    r = sgx_val(64, sgx_v1(x, w == 64 ? &sgx_v_cvttsd2si_64 : &sgx_v_cvttss2si_64, vx, 0));
    if (uns) /* 0 to 2^iw - 1 (a negative one is above, unsigned) */
        bad = iw == 64 ? sg_b_not(g, sgx_d_ult(g, r, sgx_const(64, 1ull << 63)))
                       : sg_b_not(g, sgx_d_ult(g, r, sgx_const(64, 1ull << 32)));
    else if (iw == 64)
        bad = sg_d_eq(g, r, sgx_const(64, 1ull << 63));
    else /* INT32_MIN to INT32_MAX: r + 2^31 below 2^32 */
        bad = sg_b_not(g, sgx_d_ult(g, sg_d_add(g, r, sgx_const(64, 1ull << 31)), sgx_const(64, 1ull << 32)));
    bad = sg_b_or(g, bad, sg_b_or(g, sgx_fp_bad(g, fpcr, 0, 1), sgx_fp_flushed(g, fpcr, ob, w)));
    r = sgx_fp_cold(g, bad, r, "snx_x64_fp_to_int", &ob, 1, fpcr, 1, desc);
    return sg_d_slice(g, r, iw - 1, 0);
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

/* ---- the machine's inline sequences: inputs in fixed registers ---- */

static inline int sgx_mem(SgCtx* g);
/* the machine instruction op of inputs in[k] (in registers fix[k]), imm, the registers it changes besides (clobber)
   and its results (RAX, and RDX where two) */
static inline SgD sgx_mach(SgCtx* g, int op, SgD* in, const signed char* fix, int n, long long imm, unsigned clobber,
                           int results, unsigned w) {
    SgX* x = sgx(g);
    int regs[6], k;
    SgXIns* i;
    for (k = 0; k < n; ++k) regs[k] = sgx_reg(x, &in[k]);
    i = sgx_append(x, SGX_K_MACHINE);
    i->op = (unsigned char)op;
    i->imm = imm;
    i->mclobber = (unsigned short)clobber;
    for (k = 0; k < n; ++k) {
        i->in[k] = regs[k];
        i->fixed[k] = fix[k];
    }
    if (results) i->def = sgx_vreg(x, x->n - 1, 64);
    if (results > 1) i->def2 = sgx_vreg(x, x->n - 1, 64);
    i->side = 1; /* memory, or the monitor: kept */
    x->flags_ins = -1;
    return results ? sgx_val(w, i->def) : sgx_const(1, 0);
}
#define SGX_RAX 0
#define SGX_RCX 1
#define SGX_RDX 2
#define SGX_RSI 6
#define SGX_R8 8
#define SGX_R9 9
#define SGX_R10 10
#define SGX_R11 11

/* ---- guest memory: CPU.mem plus the address ---- */

static inline int sgx_mem(SgCtx* g) {
    if (!g->mem && g->pinned && g->mempin >= 0) {
        /* the pinned register's own: read-only, never a PSET */
        SgX* x = sgx(g);
        SgXIns* i = sgx_append(x, SGX_K_PGET);
        i->imm = g->mempin;
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->mem = i->def;
    }
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
/* a guest address that is another value plus a constant (an LDR's offset, an ADD before it): that value, the constant
   into the access's displacement ([base + value + disp], the same 64-bit sum) */
static inline void sgx_addr(SgX* x, SgD* address, int* disp) {
    SgXV* v;
    long long d;
    if (!address->v || address->inv || sgx_is_const(address)) return;
    v = &x->v[address->v];
    if (v->op != SGX_C_ADD || v->cw != 64 || !v->src) return;
    d = (long long)v->k + *disp;
    if (d != (long long)(int)d) return;
    *address = sgx_val(64, v->src);
    *disp = (int)d;
}
/* the base register of a guest access at address (+ disp): the memory's base, or, where the address is a sum of two
   registers (a register offset), the memory's base plus the one that is a pinned register's (or the first), made once
   a region, and the other the index: [mem + x0 + x25] and [mem + x0 + x24] share mem + x0 */
static inline int sgx_base(SgCtx* g, SgD* address, int* disp) {
    SgX* x = sgx(g);
    int m = sgx_mem(g), a, b, k, d;
    static _Thread_local int seen[4];
    static _Thread_local unsigned next;
    SgXV* v;
    SgD r;
    sgx_addr(x, address, disp);
    if (!address->v || address->inv || sgx_is_const(address)) return m;
    v = &x->v[address->v];
    if (v->op != SGX_C_SUM || v->cw != 64) return m;
    a = v->src, b = (int)v->k;
    if (x->v[b].def >= 0 && x->ins[x->v[b].def].kind == SGX_K_PGET &&
        (x->v[a].def < 0 || x->ins[x->v[a].def].kind != SGX_K_PGET)) {
        int t = a;
        a = b;
        b = t;
    }
    for (k = 0; k < 4; ++k) {
        int s = seen[k];
        if (s <= 0 || s >= x->nv) continue;
        d = x->v[s].def;
        if (d >= 0 && d < x->n && x->ins[d].kind == SGX_K_SEL && x->ins[d].def == s &&
            x->ins[d].sel == &sgx_sel_lea_64[0] && x->ins[d].in[0] == m && x->ins[d].in[1] == a) {
            *address = sgx_val(64, b);
            return s;
        }
    }
    r = SGX_OP(x, lea, 64, sgx_val(64, m), sgx_val(64, a));
    if (!r.v) return m;
    seen[next++ & 3] = r.v;
    *address = sgx_val(64, b);
    return r.v;
}
static inline SgD sgx_load(SgCtx* g, SgD address, int bytes, int disp) {
    SgX* x = sgx(g);
    int base = sgx_base(g, &address, &disp);
    const SgXSel* s = bytes == 1   ? sgx_mem_form(sgx_sel_zext8_32, sgx_sel_zext8_32_n, 0)
                      : bytes == 2 ? sgx_mem_form(sgx_sel_zext16_32, sgx_sel_zext16_32_n, 0)
                      : bytes == 4 ? sgx_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 0)
                                   : sgx_mem_form(sgx_sel_mov_64, sgx_sel_mov_64_n, 0);
    int a = sgx_reg(x, &address), m = base;
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
    int base = sgx_base(g, &address, &disp);
    const SgXSel* s = bytes == 1   ? sgx_mem_form(sgx_sel_mov_8, sgx_sel_mov_8_n, 1)
                      : bytes == 2 ? sgx_mem_form(sgx_sel_mov_16, sgx_sel_mov_16_n, 1)
                      : bytes == 4 ? sgx_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 1)
                                   : sgx_mem_form(sgx_sel_mov_64, sgx_sel_mov_64_n, 1);
    int v = sgx_reg(x, &value), a = sgx_reg(x, &address), m = base;
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
/* a state field of w bits (32 or 64) = v */
static inline void sgx_state_store(SgCtx* g, int disp, unsigned w, SgD v) {
    SgX* x = sgx(g);
    int r = sgx_reg(x, &v);
    SgXIns* st = sgx_append(x, SGX_K_STORE);
    st->w = (unsigned char)w;
    st->in[0] = r;
    st->disp = disp;
}
/* LDXR, LDAXR, LDXP: cpu.h's ldx and ldxp inline (x86's loads are acquires): the value, and the monitor armed at
   the address with it (a pair of doublewords: both, the size's pair flag) */
static inline void sg_GuestLoadExclusive(SgCtx* g, SgD address, long long size) {
    SgD v;
    if (size == 16) {
        SgD v1 = sgx_load(g, address, 8, 0), v2 = sgx_load(g, address, 8, 8);
        sgx_state_store(g, (int)offsetof(CPU, excl_val), 64, v1);
        sgx_state_store(g, (int)offsetof(CPU, excl_val2), 64, v2);
        sgx_state_store(g, (int)offsetof(CPU, excl_size), 32, sgx_const(32, 3 | 0x10));
        g->exclusive[0] = sgx_reg(sgx(g), &v1);
        g->exclusive[1] = sgx_reg(sgx(g), &v2);
        g->exclusive_count = 2;
    } else {
        v = sg_d_zext(g, sgx_load(g, address, (int)size, 0), 64);
        sgx_state_store(g, (int)offsetof(CPU, excl_val), 64, v);
        sgx_state_store(g, (int)offsetof(CPU, excl_size), 32, sgx_const(32, (SnxU64)sgx_log2(size)));
        g->exclusive[0] = sgx_reg(sgx(g), &v);
        g->exclusive_count = 1;
    }
    sgx_state_store(g, (int)offsetof(CPU, excl_addr), 64, address);
    sgx_state_store(g, (int)offsetof(CPU, excl_on), 32, sgx_const(32, 1));
    g->exclusive_next = 0;
}
static inline SgD sg_GuestExclusiveValue(SgCtx* g, SgD address, long long size) {
    (void)address;
    if (g->exclusive_next >= g->exclusive_count) sg_fail("an exclusive read with no load-exclusive before it", size, 0);
    return sg_d_slice(g, sgx_val(64, g->exclusive[g->exclusive_next++]), 8 * size - 1, 0);
}
/* STXR, STLXR, STXP: cpu.h's stx and stxp inline (SGX_M_STX, SGX_M_STXP: LOCK CMPXCHG, CMPXCHG16B) */
static inline void sg_GuestStoreExclusive(SgCtx* g, SgD address, long long size, SgD value) {
    SgD in[4];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    if (size == 16) {
        static const signed char fix[4] = {SGX_R10, SGX_RSI, SGX_R8, SGX_R9};
        in[2] = sg_d_slice(g, value, 63, 0);
        in[3] = sg_d_slice(g, value, 127, 64);
        g->status = sgx_mach(g, SGX_M_STXP, in, fix, 4, 3, 1u << SGX_RCX | 1u << SGX_RDX | 1u << SGX_R11, 1, 32).v;
    } else {
        static const signed char fix[3] = {SGX_R10, SGX_RSI, SGX_RDX};
        in[2] = sg_d_zext(g, value, 64);
        g->status = sgx_mach(g, SGX_M_STX, in, fix, 3, sgx_log2(size), 1u << SGX_RCX, 1, 32).v;
    }
    sgx(g)->v[g->status].zab = 1;
}
static inline SgD sg_GuestExclusiveStatus(SgCtx* g) {
    if (!g->status) sg_fail("an exclusive status with no store-exclusive before it", 0, 0);
    return sgx_val(1, g->status);
}
/* the LSE atomics: LOCK XADD, XCHG, a LOCK CMPXCHG loop for the others (SGX_M_RMW) */
static inline SgD sgx_rmw(SgCtx* g, int op, SgD address, SgD v) {
    static const signed char fix[3] = {SGX_R10, SGX_RSI, SGX_RDX};
    unsigned w = v.w;
    SgD in[3];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    in[2] = sg_d_zext(g, v, 64);
    return sg_d_slice(g, sgx_mach(g, SGX_M_RMW, in, fix, 3, op << 2 | sgx_log2(w / 8),
                                  1u << SGX_RCX | 1u << SGX_RDX | 1u << SGX_R11, 1, 64), w - 1, 0);
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
/* CAS: LOCK CMPXCHG, the old value in RAX; CASP of doublewords: LOCK CMPXCHG16B, the old pair in RDX:RAX */
static inline SgD sg_GuestCompareSwap(SgCtx* g, SgD address, SgD expected, SgD desired) {
    unsigned w = expected.w;
    SgD in[6];
    in[0] = sgx_val(64, sgx_mem(g));
    in[1] = address;
    if (w == 128) {
        static const signed char fix[6] = {SGX_R10, SGX_RSI, SGX_RAX, SGX_RDX, SGX_R8, SGX_R9};
        SgXIns* i;
        SgD lo;
        in[2] = sg_d_slice(g, expected, 63, 0);
        in[3] = sg_d_slice(g, expected, 127, 64);
        in[4] = sg_d_slice(g, desired, 63, 0);
        in[5] = sg_d_slice(g, desired, 127, 64);
        lo = sgx_mach(g, SGX_M_CASP, in, fix, 6, 0, 1u << SGX_RCX | 1u << SGX_R11, 2, 64);
        i = &sgx(g)->ins[sgx(g)->n - 1];
        return sg_d_concat(g, sgx_val(64, i->def2), lo);
    }
    {
        static const signed char fix[4] = {SGX_R10, SGX_RSI, SGX_RAX, SGX_RDX};
        in[2] = sg_d_zext(g, expected, 64);
        in[3] = sg_d_zext(g, desired, 64);
        return sg_d_slice(g, sgx_mach(g, SGX_M_CAS, in, fix, 4, sgx_log2(w / 8), 0, 1, 64), w - 1, 0);
    }
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
/* CRC32 and CRC32C: the reflected update (BitReverse(Poly32Mod2(...)) as README.md's builtins say): CRC32C SSE4.2's
   CRC32 instruction, CRC32 a byte at a time through x64_helpers.c's table (SGX_M_CRC) */
static inline SgD sg_crc32(SgCtx* g, SgD acc_bits, SgD value_bits, long long size, SgS poly) {
    static const signed char fix[2] = {SGX_RAX, SGX_RDX};
    SgD in[2];
    in[0] = sg_d_zext(g, acc_bits, 64);
    in[1] = sg_d_zext(g, value_bits, 64);
    return sg_d_slice(g, sgx_mach(g, SGX_M_CRC, in, fix, 2, size / 8 | (poly.v == 0x1EDC6F41u) << 4,
                                  1u << SGX_RCX | 1u << SGX_RDX | 1u << SGX_R11, 1, 64), 31, 0);
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
/* ---- the inline sequences' bytes (sgx_mach's: R10 the memory's base, RSI the address, RBX the state) ---- */

/* opcode op (and op | 1 past bytes) of [R10 + RSI] with reg, of 1 << sz bytes: its operand-size prefix, REX (W, B) */
static inline void sgx_m_r10rsi(SgXBuf* b, int lock, unsigned sz, unsigned op8, int reg) {
    if (lock) sgx_byte(b, 0xF0);
    if (sz == 1) sgx_byte(b, 0x66);
    sgx_byte(b, (unsigned char)(0x41 | (sz == 3 ? 8 : 0) | (reg >> 3) << 2));
    if (op8 > 0xFF) sgx_byte(b, (unsigned char)(op8 >> 8));
    sgx_byte(b, (unsigned char)((op8 & 0xFF) | (sz ? 1 : 0)));
    sgx_byte(b, (unsigned char)(0x04 | (reg & 7) << 3)); /* [SIB] */
    sgx_byte(b, 0x32);                                   /* RSI + R10 */
}
static inline void sgx_jcc32(SgXBuf* b, unsigned cc, size_t* at) {
    sgx_byte(b, 0x0F);
    sgx_byte(b, (unsigned char)(0x80 | cc));
    *at = b->n;
    sgx_imm(b, 0, 4);
}
static inline void sgx_patch32(SgXBuf* b, size_t at) {
    long long d = (long long)b->n - (long long)(at + 4);
    if (at + 4 <= b->cap) {
        b->p[at] = (unsigned char)d, b->p[at + 1] = (unsigned char)(d >> 8), b->p[at + 2] = (unsigned char)(d >> 16);
        b->p[at + 3] = (unsigned char)(d >> 24);
    }
}
static inline void sgx_d32(SgXBuf* b, size_t off) { sgx_imm(b, (long long)off, 4); }
/* the monitor's checks (armed, the address, and for a single its size covering 1 << sz bytes; a pair's exactly),
   failing to *fail */
static inline void sgx_m_monitor(SgXBuf* b, int pair, unsigned sz, size_t* fail) {
    sgx_bytes(b, "8B 8B"), sgx_d32(b, offsetof(CPU, excl_on)); /* mov ecx, [rbx + excl_on] */
    sgx_bytes(b, "85 C9");                                       /* test ecx, ecx */
    sgx_jcc32(b, 4, &fail[0]);                                   /* jz */
    sgx_bytes(b, "48 3B B3"), sgx_d32(b, offsetof(CPU, excl_addr)); /* cmp rsi, [rbx + excl_addr] */
    sgx_jcc32(b, 5, &fail[1]);                                      /* jne */
    if (pair) {
        sgx_bytes(b, "83 BB"), sgx_d32(b, offsetof(CPU, excl_size)), sgx_byte(b, (unsigned char)(sz | 0x10));
        sgx_jcc32(b, 5, &fail[2]); /* cmp dword [rbx + excl_size], sz | 0x10; jne */
    } else {
        sgx_bytes(b, "8B 8B"), sgx_d32(b, offsetof(CPU, excl_size)); /* mov ecx, [rbx + excl_size] */
        sgx_bytes(b, "83 E1 03 83 F9"), sgx_byte(b, (unsigned char)sz); /* and ecx, 3; cmp ecx, sz */
        sgx_jcc32(b, 2, &fail[2]);                                     /* jb */
    }
}
/* status = ZF ? 0 : 1; then the failure's 1; then the monitor disarmed */
static inline void sgx_m_status(SgXBuf* b, const size_t* fail) {
    size_t done;
    sgx_bytes(b, "0F 95 C0 0F B6 C0"); /* setne al; movzx eax, al */
    sgx_byte(b, 0xE9);
    done = b->n;
    sgx_imm(b, 0, 4);
    sgx_patch32(b, fail[0]), sgx_patch32(b, fail[1]), sgx_patch32(b, fail[2]);
    sgx_bytes(b, "B8 01 00 00 00"); /* mov eax, 1 */
    sgx_patch32(b, done);
    sgx_bytes(b, "C7 83"), sgx_d32(b, offsetof(CPU, excl_on)), sgx_imm(b, 0, 4); /* mov dword [rbx + excl_on], 0 */
}
static inline void sgx_machine_inline(SgXAsm* a, SgXIns* i) {
    SgXBuf* b = a->b;
    unsigned sz = (unsigned)(i->imm & 3);
    size_t fail[3];
    switch (i->op) {
    case SGX_M_STX: /* cpu.h's stx */
        sgx_m_monitor(b, 0, sz, fail);
        sgx_bytes(b, "48 8B 83"), sgx_d32(b, offsetof(CPU, excl_val)); /* mov rax, [rbx + excl_val] */
        sgx_m_r10rsi(b, 1, sz, 0x0FB0, 2);                              /* lock cmpxchg [r10 + rsi], dl.. */
        sgx_m_status(b, fail);
        return;
    case SGX_M_STXP: /* cpu.h's stxp */
        sgx_m_monitor(b, 1, sz, fail);
        sgx_bytes(b, "48 8B 83"), sgx_d32(b, offsetof(CPU, excl_val));
        if (sz == 2) {
            /* the words in one doubleword: ecx = r8d, rdx = r9 << 32 | rcx */
            sgx_bytes(b, "44 89 C1 4C 89 CA 48 C1 E2 20 48 09 CA");
            sgx_m_r10rsi(b, 1, 3, 0x0FB0, 2);
        } else {
            sgx_bytes(b, "48 8B 93"), sgx_d32(b, offsetof(CPU, excl_val2)); /* mov rdx, [rbx + excl_val2] */
            /* lea r11, [r10 + rsi]; push rbx; mov rbx, r8; mov rcx, r9; lock cmpxchg16b [r11]; pop rbx */
            sgx_bytes(b, "4D 8D 1C 32 53 4C 89 C3 4C 89 C9 F0 49 0F C7 0B 5B");
        }
        sgx_m_status(b, fail);
        return;
    case SGX_M_CAS: sgx_m_r10rsi(b, 1, sz, 0x0FB0, 2); return; /* lock cmpxchg [r10 + rsi], dl.. */
    case SGX_M_CASP: /* lea r11, [r10 + rsi]; push rbx; mov rbx, r8; mov rcx, r9; lock cmpxchg16b [r11]; pop rbx */
        sgx_bytes(b, "4D 8D 1C 32 53 4C 89 C3 4C 89 C9 F0 49 0F C7 0B 5B");
        return;
    case SGX_M_RMW: {
        unsigned op = (unsigned)(i->imm >> 2);
        size_t loop;
        if (op == 0 || op == 8) {
            sgx_bytes(b, "48 89 D0"); /* mov rax, rdx; lock xadd / xchg [r10 + rsi], al.. */
            if (op == 0) sgx_m_r10rsi(b, 1, sz, 0x0FC0, 0);
            else
                sgx_m_r10rsi(b, 0, sz, 0x86, 0);
            return;
        }
        /* the old value zero extended (movzx eax, byte / word; mov eax / rax) */
        static const char* const ld[4] = {"41 0F B6 04 32", "41 0F B7 04 32", "41 8B 04 32", "49 8B 04 32"};
        sgx_bytes(b, ld[sz]);
        loop = b->n;
        sgx_bytes(b, "48 89 C1"); /* mov rcx, rax */
        if (op == 1) sgx_bytes(b, "48 21 D1");      /* and rcx, rdx */
        else if (op == 2) sgx_bytes(b, "48 31 D1"); /* xor */
        else if (op == 3) sgx_bytes(b, "48 09 D1"); /* or */
        else {
            /* both extended to 64 bits as signed (4, 5) or unsigned (6, 7) at the size, compared, the larger or smaller */
            static const char* const sx[4] = {"48 0F BE C9 4C 0F BE DA", "48 0F BF C9 4C 0F BF DA", "48 63 C9 4C 63 DA",
                                              "49 89 D3"};
            static const char* const zx[4] = {"0F B6 C9 44 0F B6 DA", "0F B7 C9 44 0F B7 DA", "89 C9 41 89 D3",
                                              "49 89 D3"};
            sgx_bytes(b, op <= 5 ? sx[sz] : zx[sz]);
            sgx_bytes(b, "4C 39 D9"); /* cmp rcx, r11 */
            sgx_bytes(b, op == 4 ? "49 0F 4C CB" : op == 5 ? "49 0F 4F CB" : op == 6 ? "49 0F 42 CB" : "49 0F 47 CB");
        }
        sgx_m_r10rsi(b, 1, sz, 0x0FB0, 1); /* lock cmpxchg [r10 + rsi], cl.. */
        sgx_byte(b, 0x0F), sgx_byte(b, 0x85);
        sgx_imm(b, (long long)loop - (long long)(b->n + 4), 4); /* jne loop */
        return;
    }
    case SGX_M_CRC: {
        unsigned bytes = (unsigned)(i->imm & 15), k;
        if (i->imm & 16) { /* crc32 eax, dl / dx / edx; crc32 rax, rdx */
            sgx_bytes(b, bytes == 1 ? "F2 0F 38 F0 C2" : bytes == 2 ? "66 F2 0F 38 F1 C2" : bytes == 4 ? "F2 0F 38 F1 C2" : "F2 48 0F 38 F1 C2");
            return;
        }
        /* the table's address in R11: a relocation of the object's, or the address resolve gives */
        if (a->reloc) {
            sgx_bytes(b, "4C 8D 1D"); /* lea r11, [rip + table] */
            a->reloc(a->machine, b->n, "snx_x64_crc32_table", -4);
            sgx_imm(b, 0, 4);
        } else {
            SnxU64 t = a->resolve ? a->resolve("snx_x64_crc32_table") : 0;
            if (!t) sg_fail("a table nothing resolves", 0, 0);
            sgx_bytes(b, "49 BB"); /* mov r11, imm64 */
            sgx_imm(b, (long long)t, 8);
        }
        for (k = 0; k < bytes; ++k)
            /* mov ecx, eax; xor cl, dl; movzx ecx, cl; shr eax, 8; xor eax, [r11 + rcx*4]; shr rdx, 8 */
            sgx_bytes(b, "89 C1 30 D1 0F B6 C9 C1 E8 08 41 33 04 8B 48 C1 EA 08");
        return;
    }
    default: return;
    }
}

static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgXBuf* b = a->b;
    switch (i->kind) {
    case SGX_K_UNDEF:
        /* state->pc = pc, then rt->undef(state, pc, word): RDI, RSI, EDX, through RAX = state->rt */
        if (a->pin_flush) a->pin_flush(a->machine);
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
        case SGX_M_STX:
        case SGX_M_STXP:
        case SGX_M_CAS:
        case SGX_M_CASP:
        case SGX_M_RMW:
        case SGX_M_CRC: sgx_machine_inline(a, i); return;
        case SGX_M_CALL:
        case SGX_M_CALL_RT:
            if (a->pin_cs) a->pin_cs(a->machine, 1); /* the caller-saved pins around the call */
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
            if (a->pin_cs) a->pin_cs(a->machine, 0);
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
    sgx_cold_emit(&a);
    sgx_pool_emit(&a);
    sgx_patch_frame(&a);
    free(a.cold.p), free(a.pool), free(a.rips), free(a.cjumps), free(a.ccalls);
    return b.n <= cap ? b.n : 0;
}

/* one word's code as a step, u64 step(CPU*) returning the next pc (the word's own after UNDEF): sg_x64_block of it */
static size_t sg_x64_step(SgCtx* g, SnxU32 word, SnxU64 pc, SnxU64 features, unsigned host_features,
                          unsigned char* code, size_t cap) {
    size_t one = 1;
    return sg_x64_block(g, &word, &one, pc, features, host_features, code, cap);
}
