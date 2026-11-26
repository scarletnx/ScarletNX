/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's machine back ends, A64's part that any machine's shares (after the machine's assembly, x64_asm.h or
   arm64_asm.h, and before its own A64 part, x64_a64.h or arm64_a64.h): the Sail's registers where cpu.h's CPU keeps
   them (x[32]: X0 to X30 and SP; n, z, cf, vf: PSTATE.NZCV, each a 32-bit 0 or 1; v[32]; fpcr), its address in the
   machine's SGX_BASE register for the instruction's code. A register read once is the block's value of it after (a
   load, or what was written: every write is stored at once, and the stores a later one overwrites are dropped as the
   block ends). NZCV written from an instruction's flags is four flag stores of them (the selection's map), and
   ConditionHolds on flags that instruction made, while the host's flags still are them, the condition code the
   selection gave Arm's condition on its form. Guest memory is CPU.mem plus the address, a memory operand of the
   machine's move forms; the exclusive monitor's bookkeeping, the branch's operands, and the step's and the block's
   functions (sg_x64_block's) are here; the floating point, the atomics, the barriers and the ABI's bytes are the
   machine's */

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
    signed char vpin[32]; /* the vector register each V register lives in for the whole function, -1 none (pinned) */
    signed char mempin;   /* the host register the guest memory's base (CPU.mem) is in, -1 none (pinned) */
    int vd[32][2];   /* the same of each SIMD&FP register's doublewords (low, high) */
    int vx[32];      /* each SIMD&FP register as a vector value, where the block has it so (one cache or the other) */
    SgB fp_state[2]; /* the hot paths' check of FPCR's mode and FPSR (sgx_fp_bad), once a region: without, with IXC */
    int fp_bound[2]; /* sgx_fp_bound's register for 32 and 64 bits, once a region with fp_state (0: none yet) */
    int fp_dbound[2]; /* sgx_fp_flushed's register for 32 and 64 bits, once a region (0: none yet) */
    SgB fp_fz;        /* sgx_fp_bad's check of FPCR.FZ alone, once a region */
    SgB fp_any;       /* and of FPCR's mode or FZ, or FPSR's IXC clear: the vector guards' hot test */
    int fp_any_bits;  /* the same as sgx_fp_bad_bits' value, its register (0: none yet) */
    int origin;      /* the instruction whose flags NZCV was last written from, -1 for none */
    int mem;         /* the virtual register of CPU.mem (guest memory's host address), 0 until read */
    int base_seen[4]; /* a few recent values of sgx_base's, each checked against what made it */
    unsigned base_next;
    int exclusive[2], exclusive_count, exclusive_next, status; /* a load-exclusive's values, a store's status */
    SgB qc; /* when the instruction so far sets FPSR.QC (sg_FPSR_field_write), 0 for not: stored with its V register */
    int qc_owed; /* a machine's value, any bit of which says FPSR.QC is to be set (mach_codegen.inc's qc_pay), 0 for
                    none */
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
    if (count == 4 && v.nzcv && !x->ins[v.nzcv - 1].fsel) {
        /* a compare's (sgx_cold_cmp's): each flag one of its condition codes stored where the flags are still its,
           first; one of two (Z) or made into a register before, by its bits */
        int from = v.nzcv - 1, made[4] = {0, 0, 0, 0};
        for (f = 0; f < 4; ++f) {
            SgB c = x->ins[from].fb[f][0];
            SgXIns* i;
            if (x->ins[from].fb[f][1] || x->b[c].kind != SGX_B_CC || x->b[c].v != x->flags_ins) continue;
            i = sgx_append(x, SGX_K_STORE_FLAG);
            i->cc = (unsigned char)x->b[c].cc;
            i->flags_from = from;
            i->disp = sgx_flag_offset(f);
            made[f] = 1;
        }
        for (f = 0; f < 4; ++f) {
            SgD bit;
            SgXIns* st;
            int r;
            if (made[f]) continue;
            bit = sgx_cmp_flag_bits(x, from, (int)f);
            bit.w = 32;
            r = sgx_reg(x, &bit);
            st = sgx_append(x, SGX_K_STORE);
            st->w = 32;
            st->in[0] = r;
            st->disp = sgx_flag_offset(f);
        }
        g->origin = -1;
        return;
    }
    if (count == 4 && v.nzcv) {
        /* the four from the flags of the instruction that made them, each the condition code that is that flag */
        int from = v.nzcv - 1;
        const SgXSel* fs = x->ins[from].fsel;
        if (x->flags_ins != from) sg_fail("NZCV written after the host's flags that are them were overwritten", from, 0);
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

/* ConditionHolds(cond) on the flags NZCV was last written from, while the host's flags are still them: the condition
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
    SGX_M_STX,  /* STXR: imm the size's log2; the status */
    SGX_M_STXP, /* STXP: imm 2 (words) or 3; the status */
    SGX_M_CAS,  /* CAS: imm log2; the old value */
    SGX_M_CASP, /* CASP of doublewords: the old pair */
    SGX_M_RMW,  /* LDADD...SWP: imm op << 2 | log2 (x64_helpers.c's snx_x64_rmw's); the old value */
    SGX_M_CRC,  /* CRC32, CRC32C: imm bytes | castagnoli << 4; the result */
    SGX_M_FENCE_LD, /* a barrier of loads alone (a machine whose loads are no acquires) */
    SGX_M_FENCE_ST, /* of stores alone */
    SGX_M_FPENTER,  /* the guest's FPCR into the host's, its FPSR cleared (a machine running the guest's own floating
                       point: arm64's) */
    SGX_M_FPLEAVE,  /* the host's FPSR ORed into the guest's, the host's FPCR its own again */
    SGX_M_SETNZCV   /* the host's flags in[0]'s bits 31-28 (arm64's MSR NZCV: the guest's, for a word that reads them) */
};

/* a call of the helper sym (x64_helpers.c's) or of the runtime's function at rt_offset in RtAPI, the state first
   where state: args its other arguments (in the machine's SGX_ARG_REG); its result (SGX_RET) */
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
    g->fp_fz = g->fp_any = 0, g->fp_any_bits = 0;
    i = sgx_append(x, SGX_K_MACHINE);
    i->op = sym ? SGX_M_CALL : SGX_M_CALL_RT;
    i->sym = sym;
    i->disp = rt_offset;
    i->w = (unsigned char)state;
    i->calls = 1;
    for (k = 0; k < nargs; ++k) {
        i->in[k] = regs[k];
        i->fixed[k] = SGX_ARG_REG(x, k + (state ? 1 : 0));
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
/* V[n] pinned to a vector register: the block's value of it, the register's own (SGX_K_PGET) the first time */
static inline int sgx_vpinned(SgCtx* g, long long n) { return g->pinned && g->vpin[n] >= 0; }
static inline void sgx_vpget(SgCtx* g, long long n) {
    SgX* x = sgx(g);
    SgXIns* i;
    if (g->vx[n]) return;
    i = sgx_append(x, SGX_K_PGET);
    i->imm = SGX_XPIN + g->vpin[n];
    i->def = sgx_xvreg(x, x->n - 1);
    g->vx[n] = i->def;
    g->vd[n][0] = g->vd[n][1] = 0;
}
/* V[n] (pinned) = the vector value r */
static inline void sgx_vpset(SgCtx* g, long long n, int r) {
    SgX* x = sgx(g);
    SgXIns* i = sgx_append(x, SGX_K_PSET);
    i->imm = SGX_XPIN + g->vpin[n];
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
        sgx_vpset(g, n, sgx_vins(x, SGX_VF_INS64, 0, 1, in, k, 0, SGX_VM_NONE, 0, 0, 0, 0, 0));
        return;
    }
    SgXIns* i = sgx_append(x, SGX_K_STORE);
    i->w = 64;
    i->in[0] = r;
    i->disp = sgx_vd_offset(n, k);
    if (g->vx[n]) {
        /* the other doubleword from the vector value, which is no longer the register's */
        if (!g->vd[n][!k]) g->vd[n][!k] = sgx_vd_read(g, n, !k).v;
        g->vx[n] = 0;
    }
    g->vd[n][k] = r;
}
/* _V[n]'s bits hi..lo: of the vector value the block has (a 16-byte load the first time), its low bits or an
   element PSHUFD brings down (sg_d_slice); a register the block wrote as doublewords from those */
static inline SgD sg__V_read_bits(SgCtx* g, long long n, long long hi, long long lo) {
    SgX* x = sgx(g);
    if (n < 0 || n > 31) sg_fail("a SIMD&FP register this back end does not keep", n, 31);
    if (sgx_vpinned(g, n)) sgx_vpget(g, n);
    if (!g->vx[n] && (g->vd[n][0] || g->vd[n][1] || !SGX_HAS_VREGS)) {
        if (lo == 0 && hi < 64) return sg_d_slice(g, sgx_vd_read(g, n, 0), hi, 0);
        if (lo == 64 && hi <= 127) return sg_d_slice(g, sgx_vd_read(g, n, 1), hi - 64, 0);
        if (lo == 0 && hi == 127) return sg_d_concat(g, sgx_vd_read(g, n, 1), sgx_vd_read(g, n, 0));
        sg_fail("bits of a SIMD&FP register across its doublewords", hi, lo);
    }
    if (!g->vx[n]) g->vx[n] = sgx_vins(x, SGX_VF_LD16, 0, 0, NULL, 0, 1, SGX_VM_STATE, 0, 0, sgx_vd_offset(n, 0), 0, 0);
    return sg_d_slice(g, sgx_xval(128, g->vx[n]), hi, lo);
}
/* FPSR[QC] = 1 under path (0: always), a saturating instruction's, once for each element: the condition of them all is
   kept, and FPSR's bit set where it holds as the instruction writes its register (sgx_qc) */
enum { SG_FPSR_QC = 27 };
static inline void sg_FPSR_field_write(SgCtx* g, SgB path, int field, SgD v) {
    if (field != SG_FPSR_QC || !sgx_is_const(&v) || v.k != 1)
        sg_fail("FPSR's field written other than QC set", field, v.w);
    path = path ? path : sg_bool(g, 1);
    g->qc = g->qc ? sg_b_or(g, g->qc, path) : path;
}
static inline void sgx_qc(SgCtx* g) {
    SgX* x = sgx(g);
    SgXIns* i;
    SgD bit;
    int r;
    if (!g->qc) return;
    bit = sg_d_lsl(g, sg_d_zext(g, sgx_bool_bits(x, g->qc), 32), SG_FPSR_QC);
    g->qc = 0;
    i = sgx_append(x, SGX_K_LOAD);
    i->w = 32;
    i->disp = (int)offsetof(CPU, fpsr);
    i->def = sgx_vreg(x, x->n - 1, 32);
    bit = sg_d_or(g, sgx_val(32, i->def), bit);
    r = sgx_reg(x, &bit);
    i = sgx_append(x, SGX_K_STORE);
    i->w = 32;
    i->in[0] = r;
    i->disp = (int)offsetof(CPU, fpsr);
}

/* _V[n] = v, all 128 bits: a vector value by a 16-byte store, general-purpose doublewords by two */
static inline void sg__V_write(SgCtx* g, long long n, SgD v) {
    SgX* x = sgx(g);
    SgD lo, hi;
    sgx_qc(g);
    if (n < 0 || n > 31) sg_fail("a SIMD&FP register this back end does not keep", n, 31);
    if (v.w != 128 || v.wide || v.inv || v.nzcv) sg_fail("a SIMD&FP register written from other than 128 bits", v.w, 128);
    if (sgx_vpinned(g, n)) {
        sgx_vpset(g, n, v.x ? v.x : sgx_xreg(x, &v));
        return;
    }
    if (v.x) {
        int in[4] = {0, v.x, 0, 0};
        sgx_vins(x, SGX_VF_ST16, 0, 0, in, 0, 0, SGX_VM_STATE, 0, 0, sgx_vd_offset(n, 0), 0, 0);
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
/* ---- floating point every machine's back end makes the same way ----
   FPRoundingMode is the back end's own (the mode FPCR holds when the guest runs); FRINT32/64 (FPRoundIntN), FJCVTZS
   and BFCVT are the exact helpers' (x64_helpers.c) */
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
static inline SgD sg_FPRoundIntN(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, long long intsize) {
    return sgx_fp(g, "snx_x64_fp_round_int", path, fpcr, &a, 1, 1,
                  a.w | sgx_fp_rm(rounding) << 8 | (SnxU32)intsize << 16, a.w);
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
/* the reciprocals' estimates and steps, FMULX's product, FRECPX's exponent, and a conversion in a rounding of its own
   (FCVTXN's, to odd): cpu.h's */
static inline SgD sgx_fp_unary_of(SgCtx* g, SgB path, SgD a, SgD fpcr, unsigned op) {
    return sgx_fp(g, "snx_x64_fp_unary", path, fpcr, &a, 1, 1, a.w | op << 8, a.w);
}
static inline SgD sgx_fp_binary_of(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned op) {
    SgD o[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_binary", path, fpcr, o, 2, 1, a.w | op << 8, a.w);
}
static inline SgD sg_FPRecipEstimate(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    return sgx_fp_unary_of(g, path, a, fpcr, SNX_FP_RECPE);
}
static inline SgD sg_FPRSqrtEstimate(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    return sgx_fp_unary_of(g, path, a, fpcr, SNX_FP_RSQRTE);
}
static inline SgD sg_FPRecpX(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    return sgx_fp_unary_of(g, path, a, fpcr, SNX_FP_RECPX);
}
static inline SgD sg_FPMulX(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) {
    return sgx_fp_binary_of(g, path, a, b, fpcr, SNX_FP_MULX);
}
static inline SgD sg_FPRecipStepFused(SgCtx* g, SgB path, SgD a, SgD b) {
    return sgx_fp_binary_of(g, path, a, b, sg_FPCR_read(g), SNX_FP_RECPS);
}
static inline SgD sg_FPRSqrtStepFused(SgCtx* g, SgB path, SgD a, SgD b) {
    return sgx_fp_binary_of(g, path, a, b, sg_FPCR_read(g), SNX_FP_RSQRTS);
}
static inline SgD sg_FPConvert(SgCtx* g, SgB path, SgD op, SgD fpcr, int rounding, long long to_bits) {
    if (to_bits != 16 && to_bits != 32 && to_bits != 64) sg_fail("a conversion to a float of this width", to_bits, 0);
    if (rounding < 0 || rounding > 5) sg_fail("a floating-point rounding the helpers do not take", rounding, 5);
    return sgx_fp(g, "snx_x64_fp_convert", path, fpcr, &op, 1, 1,
                  op.w | (SnxU32)to_bits << 8 | (SnxU32)(rounding + 1) << 16, (unsigned)to_bits);
}
/* URECPE, URSQRTE: of 32 bits, with no flags and no mode */
static inline SgD sgx_estimate32(SgCtx* g, SgD a, int sqrt) {
    SgD args[2];
    args[0] = sg_d_zext(g, a, 64);
    args[1] = sgx_const(32, (SnxU64)sqrt);
    return sg_d_slice(g, sgx_call(g, "snx_x64_estimate", 0, 0, args, 2, 64), 31, 0);
}
static inline SgD sg_UnsignedRecipEstimate(SgCtx* g, SgD a) { return sgx_estimate32(g, a, 0); }
static inline SgD sg_UnsignedRSqrtEstimate(SgCtx* g, SgD a) { return sgx_estimate32(g, a, 1); }
/* FMLAL's product added (path 0: always): the halves as singles, exactly, then the single's fused multiply-add */
static inline SgD sg_FPMulAdd(SgCtx* g, SgB path, SgD addend, SgD a, SgD b, SgD fpcr);
static inline SgD sgx_half_single(SgCtx* g, SgD a, SgD fpcr) {
    SgD args[2];
    args[0] = sg_d_zext(g, a, 64);
    args[1] = sg_d_zext(g, sg_d_slice(g, fpcr, 31, 0), 64);
    return sg_d_slice(g, sgx_call(g, "snx_x64_half", 0, 0, args, 2, 64), 31, 0);
}
static inline SgD sg_FPMulAddH(SgCtx* g, SgB path, SgD addend, SgD a, SgD b, SgD fpcr) {
    SgD x = sgx_half_single(g, a, fpcr), y = sgx_half_single(g, b, fpcr);
    return sg_FPMulAdd(g, path, addend, x, y, fpcr);
}
/* BFDOT's step, which neither reads FPCR (FEAT_EBF16's FPCR.EBF is not kept) nor raises a flag */
static inline SgD sg_BFDotAdd(SgCtx* g, SgB path, SgD sum, SgD a0, SgD a1, SgD b0, SgD b1, SgD fpcr) {
    SgD args[3];
    (void)path, (void)fpcr;
    args[0] = sg_d_zext(g, sum, 64);
    args[1] = sg_d_zext(g, sg_d_concat(g, a1, a0), 64);
    args[2] = sg_d_zext(g, sg_d_concat(g, b1, b0), 64);
    return sg_d_slice(g, sgx_call(g, "snx_x64_bfdot", 0, 0, args, 3, 64), 31, 0);
}
/* SM4's substitution of a byte */
static inline SgD sg_Sbox(SgCtx* g, SgD a) {
    SgD x = sg_d_zext(g, a, 64);
    return sg_d_slice(g, sgx_call(g, "snx_x64_sm4_box", 0, 0, &x, 1, 64), 7, 0);
}
/* AES's SubBytes, MixColumns and their inverses (op 0 to 3), a doubleword at a time: each takes its bytes, or its two
   columns, alone */
static inline SgD sgx_aes(SgCtx* g, SgD a, unsigned op) {
    SgD args[2], half[2];
    int k;
    for (k = 0; k < 2; ++k) {
        args[0] = sg_d_slice(g, a, 64 * k + 63, 64 * k);
        args[1] = sgx_const(32, op);
        half[k] = sgx_call(g, "snx_x64_aes", 0, 0, args, 2, 64);
    }
    return sg_d_concat(g, half[1], half[0]);
}
static inline SgD sg_AESSubBytes(SgCtx* g, SgD a) { return sgx_aes(g, a, 0); }
static inline SgD sg_AESInvSubBytes(SgCtx* g, SgD a) { return sgx_aes(g, a, 1); }
static inline SgD sg_AESMixColumns(SgCtx* g, SgD a) { return sgx_aes(g, a, 2); }
static inline SgD sg_AESInvMixColumns(SgCtx* g, SgD a) { return sgx_aes(g, a, 3); }

/* ---- the floating point of a machine that has no inline code of its own for it (SGX_FP_HELPERS: arm64.h's,
   risc.h's): the exact helpers' for every operation, x64_helpers.c's snx_x64_fp_* (cpu.h's soft float, which raises
   FPSR's flags as FPProcessException does) ---- */

#if SGX_FP_HELPERS

static inline SgD sgx_fp_binary(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned op) {
    SgD o[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_binary", path, fpcr, o, 2, 1, a.w | op << 8, a.w);
}
static inline SgD sg_FPAdd(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_ADD); }
static inline SgD sg_FPSub(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_SUB); }
static inline SgD sg_FPMul(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_MUL); }
static inline SgD sg_FPDiv(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_binary(g, path, a, b, fpcr, SNX_FP_DIV); }
static inline SgD sgx_fp_minmax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr, unsigned minimum, unsigned numeric) {
    SgD o[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_minmax", path, fpcr, o, 2, 1, a.w | minimum << 8 | numeric << 9, a.w);
}
static inline SgD sg_FPMax(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 0); }
static inline SgD sg_FPMin(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 0); }
static inline SgD sg_FPMaxNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 0, 1); }
static inline SgD sg_FPMinNum(SgCtx* g, SgB path, SgD a, SgD b, SgD fpcr) { return sgx_fp_minmax(g, path, a, b, fpcr, 1, 1); }
static inline SgD sg_FPMulAdd(SgCtx* g, SgB path, SgD addend, SgD a, SgD b, SgD fpcr) {
    SgD o[3] = {addend, a, b};
    return sgx_fp(g, "snx_x64_fp_fma", path, fpcr, o, 3, 1, a.w, a.w);
}
static inline SgD sg_FPSqrt(SgCtx* g, SgB path, SgD a, SgD fpcr) {
    return sgx_fp(g, "snx_x64_fp_unary", path, fpcr, &a, 1, 1, a.w | SNX_FP_SQRT << 8, a.w);
}
static inline SgD sg_FPRoundInt(SgCtx* g, SgB path, SgD a, SgD fpcr, int rounding, int exact) {
    return sgx_fp(g, "snx_x64_fp_round", path, fpcr, &a, 1, 1, a.w | sgx_fp_rm(rounding) << 8 | (SnxU32)!!exact << 12,
                  a.w);
}
static inline SgD sg_FPConvert__1(SgCtx* g, SgB path, SgD op, SgD fpcr, long long to_bits) {
    if (to_bits != 16 && to_bits != 32 && to_bits != 64) sg_fail("a conversion to a float of this width", to_bits, 0);
    return sgx_fp(g, "snx_x64_fp_convert", path, fpcr, &op, 1, 1, op.w | (SnxU32)to_bits << 8, (unsigned)to_bits);
}
static inline SgD sg_FPCompare(SgCtx* g, SgB path, SgD a, SgD b, int signal, SgD fpcr) {
    SgD o[2] = {a, b};
    return sgx_fp(g, "snx_x64_fp_compare", path, fpcr, o, 2, 1, a.w | (SnxU32)!!signal << 8, 4);
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
    if (integer != 16 && integer != 32 && integer != 64) sg_fail("a conversion to an integer of this width", integer, 0);
    if (fbits < 0 || fbits > 64) sg_fail("FPToFixed of these fraction bits", fbits, 0);
    return sgx_fp(g, "snx_x64_fp_to_int", path, fpcr, &op, 1, 1,
                  op.w | (SnxU32)integer << 8 | (SnxU32)!!uns << 16 | (SnxU32)fbits << 17 | sgx_fp_rm(rounding) << 24,
                  (unsigned)integer);
}
#endif

/* ---- the machine's inline sequences: inputs in fixed registers ---- */

static inline int sgx_mem(SgCtx* g);
/* the machine instruction op of inputs in[k] (in registers fix[k]), imm, the registers it changes besides (clobber)
   and its results (SGX_RET, and SGX_RET2 where two) */
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
        int s = g->base_seen[k];
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
    g->base_seen[g->base_next++ & 3] = r.v;
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

/* the machine's ordering of an acquire load after it and of a release store (before and after it: the sequentially
   consistent store the LLVM back end makes, which orders it with a later load-acquire too) */
static inline void sgx_acquire(SgCtx* g);
static inline void sgx_release(SgCtx* g, int after);
/* GuestMemRead(address, size, acquire): 16 bytes are two of 8 */
static inline SgD sg_GuestMemRead(SgCtx* g, SgD address, long long size, int acquire) {
    SgD v;
    if (address.w != 64) sg_fail("a guest address of other than 64 bits", address.w, 64);
    if (size == 16) v = sg_d_concat(g, sgx_load(g, address, 8, 8), sgx_load(g, address, 8, 0));
    else {
        if (size != 1 && size != 2 && size != 4 && size != 8) sg_fail("a guest access of this size", size, 0);
        v = sgx_load(g, address, (int)size, 0);
    }
    if (acquire) sgx_acquire(g);
    return v;
}
/* GuestMemWrite(address, size, release, value) */
static inline void sg_GuestMemWrite(SgCtx* g, SgD address, long long size, int release, SgD value) {
    if (address.w != 64) sg_fail("a guest address of other than 64 bits", address.w, 64);
    if (value.w != 8u * (unsigned)size) sg_fail("a guest write of a value of another size", size, value.w);
    if (release) sgx_release(g, 0);
    if (size == 16) {
        sgx_store(g, address, 8, 0, sg_d_slice(g, value, 63, 0));
        sgx_store(g, address, 8, 8, sg_d_slice(g, value, 127, 64));
    } else
        sgx_store(g, address, (int)size, 0, value);
    if (release) sgx_release(g, 1);
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
/* LDXR, LDAXR, LDXP: cpu.h's ldx and ldxp inline (an acquire's ordering the Sail's GuestMemRead gives): the value, and the monitor armed at
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
    sgx_acquire(g); /* LDAXR's: the Sail's GuestLoadExclusive doesn't say which, so every one is an acquire */
    sgx_state_store(g, (int)offsetof(CPU, excl_addr), 64, address);
    sgx_state_store(g, (int)offsetof(CPU, excl_on), 32, sgx_const(32, 1));
    g->exclusive_next = 0;
}
static inline SgD sg_GuestExclusiveValue(SgCtx* g, SgD address, long long size) {
    (void)address;
    if (g->exclusive_next >= g->exclusive_count) sg_fail("an exclusive read with no load-exclusive before it", size, 0);
    return sg_d_slice(g, sgx_val(64, g->exclusive[g->exclusive_next++]), 8 * size - 1, 0);
}
static inline SgD sg_GuestExclusiveStatus(SgCtx* g) {
    if (!g->status) sg_fail("an exclusive status with no store-exclusive before it", 0, 0);
    return sgx_val(1, g->status);
}
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
/* BranchTo(target, ...): where the instruction goes, under the run-time condition when (0: always) */
static inline void sg_branch_to(SgCtx* g, SgD target, SgB when) {
    if (g->branched) sg_fail("an instruction that branches twice", (long long)g->word, 0);
    if (target.w != 64) sg_fail("a branch to a target of other than 64 bits", target.w, 64);
    g->branched = 1;
    g->target = target;
    g->taken = when;
}

/* the dispatcher sailgen writes after this */
static int sg_decode(SgCtx* g);
/* the machine's prologue and epilogue of a step's or a block's function */
static inline void sgx_prologue(SgXAsm* a);
static inline void sgx_epilogue(SgXAsm* a);

/* words in a row as one function, u64 block(CPU*) returning the next pc, their instructions one SSA: a register read
   once, flags one instruction left read by the next as a condition code (CMP then B.cond is CMP and CMOVcc), the stores
   a later one overwrites dropped. Every word but the last runs on to the next: a branch, an UNDEF or an instruction
   that leaves the next too little of the block's room ends it (the words after it are left out: count is how many it
   took). Its size in code, 0 when a word isn't taken, needs a primitive this host has no form of, or the code needs
   more than cap bytes */
static size_t sg_x64_block(SgCtx* g, const SnxU32* words, size_t* count, SnxU64 pc, SnxU64 features,
                           unsigned host_features, unsigned char* code, size_t cap) {
    SgXBuf b;
    SgXAsm a;
    SgXIns* r;
    size_t i;
    int pool = g->x.pool, xpool = g->x.xpool;
    SnxU64 (*resolve)(const char*) = g->resolve;
    memset(g, 0, sizeof *g);
    g->resolve = resolve;
    g->features = features;
    g->origin = -1;
    g->x.features = host_features;
    g->x.flags_ins = -1;
    g->x.base = SGX_BASE;
    g->x.nv = 1;   /* 0 is none */
    g->x.nb = 1;
    g->x.pool = pool;
    g->x.xpool = xpool;
    for (i = 0; i < *count; ++i) {
        if (i && (g->x.n > SGX_MAXI / 4 || g->x.nv > SGX_MAXV / 4 || g->x.nb > SGX_MAXB / 4)) break;
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
        r->fixed[0] = SGX_RET;
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
    a.x = &g->x;
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
