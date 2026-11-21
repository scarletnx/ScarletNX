/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, the part every machine's shares (README.md, "The x86-64 back end"): copied at the top of
   the file sailgen writes after bits.h and before the machine's part (x64_a64.h), which defines SgCtx with an SgX in
   it (sgx(g)) and keeps the guest's registers.

   No LLVM: the generated emitters write x86-64 instructions themselves, chosen by the instruction selection sailgen
   derived from the Intel SDM when it was built (x64_isel.inc: an encoder per selected form, and each primitive's
   candidates). The work is paid for then; at run time an emitter only appends instructions to a short SSA of them
   (the assembly itself, virtual registers for operands: SgXIns), folds what is constant as it appends, and once the
   block is done drops what nothing reads, gives the virtual registers host ones and writes the bytes.

   A value known only when the guest runs is an SgD: bits in a virtual register (or a constant), its width, and how
   many of its register's bits above it are known to be 0 (zab: the selection says of each form whether it leaves its
   result zero extended, and whether it reads only the low bits of an input, so a 32-bit result is used as it is and
   an 8-bit one is zero extended only where something reads it whole). NOT is lazy (a subtraction takes the operand it
   was of), and so is an addition's NZCV: the instruction whose x86 flags are Arm's, by the selection's map (SUBS's C
   is the negation of SUB's CF), until it is written to PSTATE or read. A bool (SgB) is a handle (0 for none, as
   sg_condition_origin gives when no compare made the flags): a constant, a virtual register of 0 or 1, or an x86
   condition code on the flags an instruction left, the selection's condition code for an Arm condition. */

/* ---- the code ---- */

typedef struct {
    unsigned char* p;
    size_t n, cap;
    int disp; /* a memory operand's displacement, for sgx_modrm */
} SgXBuf;

static inline void sgx_byte(SgXBuf* b, unsigned char x) {
    if (b->n < b->cap) b->p[b->n] = x;
    ++b->n;
}
static inline void sgx_imm(SgXBuf* b, long long v, int bytes) {
    int i;
    for (i = 0; i < bytes; ++i) sgx_byte(b, (unsigned char)((unsigned long long)v >> (8 * i)));
}
/* ModRM: a register (0 to 15, mod 11), or the memory at a register plus b->disp (16 + the register: mod 10, a SIB for
   RSP and R12, a 32-bit displacement), Vol. 2A, 2.1.5 */
static inline void sgx_modrm(SgXBuf* b, int reg, int rm) {
    if (rm < 16) {
        sgx_byte(b, (unsigned char)(0xC0 | (reg & 7) << 3 | (rm & 7)));
        return;
    }
    rm -= 16;
    sgx_byte(b, (unsigned char)(0x80 | (reg & 7) << 3 | (rm & 7)));
    if ((rm & 7) == 4) sgx_byte(b, 0x24);
    sgx_imm(b, b->disp, 4);
}

/* the host's features a candidate may need (CPUID's), SgX.features */
enum { SGX_F_BMI1 = 1, SGX_F_BMI2 = 2, SGX_F_LZCNT = 4 };

/* a selected form (x64_isel.inc): its encoder; the feature it needs; its ModRM.rm operand; where each of the
   primitive's inputs goes (the form's operand 0 to 3, or 16 + a fixed register); the inputs it reads whole (clean) and
   those that are immediates; the immediate's fit; where the result is, whether zero extended, and the input it is tied
   to (+1); the fixed registers it writes besides, the operands it needs zeroed before it, the flags it writes (CF PF AF
   ZF SF OF); Arm's flags as its flags (0 CF ... 5 OF, negated by flag_neg's bit); CF the carry in's negation; and each
   Arm condition's condition code on its flags */
typedef struct {
    void (*enc)(SgXBuf* b, const unsigned char* r, long long imm, int cc);
    unsigned feature;
    signed char rm;
    signed char in[4];
    unsigned char clean, imm;
    signed char fit_bits;
    char fit_kind;
    signed char out;
    unsigned char clean_out, tied;
    unsigned clobber, zero, flags;
    signed char flag_of[4];
    unsigned char flag_neg, carry_neg;
    signed char cc_of[16];
} SgXSel;

#include "x64_isel.inc"

/* ---- the SSA of the block's instructions ---- */

enum {
    SGX_K_SEL,        /* a selected form: sel, its inputs in[] by the primitive's order, then the zeroed one's */
    SGX_K_LOAD,       /* def = the 64-bit (w 64) or 32-bit (w 32) field at disp from the state's base */
    SGX_K_STORE,      /* the field at disp = in[0], 64 or 32 bits */
    SGX_K_STORE_FLAG, /* the 32-bit field at disp = condition code cc on flags_from's flags, 0 or 1 */
    SGX_K_SETCC,      /* def = condition code cc on flags_from's flags, 0 or 1 */
    SGX_K_RET,        /* the end: the next pc in[0] (or imm) */
    SGX_K_UNDEF,      /* the runtime's undef(state, pc imm, word disp) */
    SGX_K_MACHINE     /* the machine's own (x64_a64.h's: labels, jumps, calls), op its kind */
};

typedef struct {
    unsigned char kind, cc, w, dead;
    unsigned char op; /* SGX_K_MACHINE's kind of instruction */
    const SgXSel* sel;
    int def;
    int in[5];
    long long imm;
    int disp;
    int flags_from; /* the instruction whose flags a SETCC, STORE_FLAG or cc form reads */
    const SgXSel* fsel; /* whose map of Arm's flags its flags follow (sel's, or the ANDS candidate of its AND) */
    const SgXSel* alt;  /* the same flags without the result (CMP for SUBS), for a result nothing reads */
    signed char fixed[5]; /* the machine's instructions: the host register in[k] must be in, -1 any */
    unsigned char calls;  /* the machine's: it calls out (the caller-saved registers are spilled around it) */
    unsigned char cadd; /* an addition of the constant cadd_k to in[0] (its flags or not): what sgx_optimize folds
                           into the next such addition where nothing reads the flags of either */
    long long cadd_k;
} SgXIns;

typedef struct {
    unsigned char zab;  /* its register's bits from here up are 0 (64: none known) */
    unsigned char op;   /* made by an operation with a constant (SGX_C_*) of width cw, of the value src and k: the
                           chain the next such operation of that width folds into (AND of an AND: one AND of both
                           masks; a 32-bit one zero extends, so not into a 64-bit one) */
    unsigned char cw;
    int def;            /* the instruction defining it */
    int last;           /* the last that reads it */
    signed char reg;    /* the host register, once given */
    short slot;         /* its spill slot, -1 for none */
    signed char xmm;    /* the XMM register it is spilled to, -1 for none */
    int src;
    SnxU64 k;
} SgXV;
enum { SGX_C_NONE, SGX_C_AND, SGX_C_OR, SGX_C_XOR, SGX_C_ADD };

enum { SGX_B_CONST, SGX_B_REG, SGX_B_CC };
typedef struct {
    unsigned char kind, cc;
    int v;  /* SGX_B_CONST: 0 or 1; SGX_B_REG: the virtual register; SGX_B_CC: the instruction whose flags */
} SgXBool;

#define SGX_MAXI 1024
#define SGX_MAXV 2048
#define SGX_MAXB 1024

typedef struct {
    SgXIns ins[SGX_MAXI];
    int n;
    SgXV v[SGX_MAXV];
    int nv;
    SgXBool b[SGX_MAXB];
    int nb;
    unsigned features;
    int flags_ins; /* the instruction whose x86 flags the flags are now, -1 for none */
    int base;      /* the host register the state's address is in (RBX) */
    int unmade;    /* a primitive had no candidate this host can run */
    int pool;      /* the host registers values may be in: the first pool of sgx_pool's (0: all; fewer, for tests of
                      spilling) */
} SgX;

typedef int SgB;

typedef struct {
    unsigned w;
    int v;     /* the virtual register holding bits [0, min(w, 64)), 0 for the constant k */
    SnxU64 k;
    int vh;    /* w > 64: the one of bits [64, w), 0 for the constant kh */
    SnxU64 kh;
    int inv;   /* the value is NOT of v's (not made yet) */
    int nzcv;  /* NZCV not made yet: the instruction whose flags are them, + 1 */
} SgD;

/* an integer known only when the guest runs: its value modulo 2^64 (v, an SgD of 64 bits: what gen.c's (x).v reads),
   or not made yet: the product of a and b (UMULH's, SMULH's high half), or x MOD 2^m (a shift's amount, which the
   shift's x86 form takes as it is: the selection proved its count mask is Arm's MOD datasize) */
enum { SGX_I_VAL, SGX_I_MUL, SGX_I_MOD };
typedef struct {
    SgD v;
    int kind, is_signed;
    unsigned w;
    SgD a, b;
    long long m;
} SgI;

/* the generated code's context: the machine's part defines it, an SgX x in it */
typedef struct SgCtx SgCtx;
static inline SgX* sgx(SgCtx* g);

/* ---- appending ---- */

static inline SgXIns* sgx_append(SgX* x, int kind) {
    SgXIns* i;
    if (x->n == SGX_MAXI) sg_fail("a block of too many x86 instructions", x->n, SGX_MAXI);
    i = &x->ins[x->n++];
    memset(i, 0, sizeof *i);
    i->kind = (unsigned char)kind;
    i->flags_from = -1;
    memset(i->fixed, -1, sizeof i->fixed);
    return i;
}
static inline int sgx_vreg(SgX* x, int def, unsigned zab) {
    if (x->nv == SGX_MAXV) sg_fail("a block of too many values", x->nv, SGX_MAXV);
    x->v[x->nv].zab = (unsigned char)zab;
    x->v[x->nv].op = SGX_C_NONE;
    x->v[x->nv].def = def;
    x->v[x->nv].last = -1;
    x->v[x->nv].reg = -1;
    x->v[x->nv].slot = -1;
    x->v[x->nv].xmm = -1;
    return x->nv++;
}

static inline SgD sgx_const(unsigned w, SnxU64 k) {
    SgD d;
    memset(&d, 0, sizeof d);
    d.w = w;
    d.k = k & sg_ones(w < 64 ? w : 64);
    return d;
}
static inline SgD sgx_val(unsigned w, int v) {
    SgD d;
    memset(&d, 0, sizeof d);
    d.w = w;
    d.v = v;
    return d;
}
static inline int sgx_is_const(const SgD* d) { return !d->v && !d->inv && !d->nzcv && (d->w <= 64 || !d->vh); }

static inline SgB sgx_bool(SgX* x, int kind, int v, int cc) {
    if (x->nb == SGX_MAXB) sg_fail("a block of too many bools", x->nb, SGX_MAXB);
    if (!x->nb) x->nb = 1; /* 0 is none */
    x->b[x->nb].kind = (unsigned char)kind;
    x->b[x->nb].v = v;
    x->b[x->nb].cc = (unsigned char)cc;
    return x->nb++;
}

/* a candidate the host can run, whose immediate (the constant k of the input that is one) fits */
static inline int sgx_fits(const SgXSel* s, SnxU64 k, unsigned w) {
    long long sk;
    if (!s->fit_bits) return 1;
    if (s->fit_kind == 'S') {
        sk = w < 64 ? (long long)(k << (64 - w)) >> (64 - w) : (long long)k;
        return sk >= -(1ll << (s->fit_bits - 1)) && sk < (1ll << (s->fit_bits - 1));
    }
    return s->fit_bits >= 64 || k < (1ull << s->fit_bits);
}
static inline const SgXSel* sgx_pick(SgX* x, const SgXSel* table, unsigned n, SnxU64 k, unsigned w, int keep_flags) {
    unsigned i;
    for (i = 0; i < n; ++i) {
        const SgXSel* s = &table[i];
        if ((s->feature & x->features) != s->feature || !sgx_fits(s, k, w)) continue;
        if (keep_flags && s->flags) continue;
        return s;
    }
    return NULL;
}

/* a value in a register: a constant moved into one (MOV, which writes no flags), a NOT made */
static inline int sgx_reg(SgX* x, SgD* d);
static inline void sgx_flags_clobbered(SgX* x);

static inline int sgx_movi(SgX* x, SnxU64 k) {
    const SgXSel* s = NULL;
    SgXIns* i;
    int r;
    /* of 32 bits (a 32-bit MOV zero extends), of 32 bits sign extended (movi_s32's K), else all 64 */
    if (k == (SnxU64)(SnxU32)k) s = sgx_pick(x, sgx_sel_movi_32, sgx_sel_movi_32_n, k, 32, 1);
    if (!s && k == (SnxU64)(long long)(int)(SnxU32)k) s = sgx_pick(x, sgx_sel_movi_s32_64, sgx_sel_movi_s32_64_n, k, 64, 1);
    if (!s) s = sgx_pick(x, sgx_sel_movi_64, sgx_sel_movi_64_n, k, 64, 1);
    if (!s) sg_fail("no flag-keeping move of a constant", 0, 0);
    i = sgx_append(x, SGX_K_SEL);
    i->sel = s;
    i->imm = (long long)k;
    i->w = 64;
    r = sgx_vreg(x, x->n - 1, k == (SnxU64)(SnxU32)k ? 32 : 64);
    i->def = r;
    return r;
}

/* appends the candidate s with inputs (registers or constants: the immediate) and the result's width w: its virtual
   register (0 for none). The inputs the form reads whole are zero extended first where they aren't */
static inline SgD sgx_zext(SgX* x, SgD a, unsigned w);
static inline int sgx_emit(SgX* x, const SgXSel* s, SgD* in, int nin, unsigned w, int cc, int flags_from) {
    SgXIns* i;
    int k, regs[4] = {0, 0, 0, 0}, zero = 0;
    long long imm = 0;
    for (k = 0; k < nin; ++k) {
        if (s->imm & (1 << k)) {
            imm = (long long)in[k].k;
            continue;
        }
        if ((s->clean & (1 << k)) && x->v[sgx_reg(x, &in[k])].zab > in[k].w) in[k] = sgx_zext(x, in[k], in[k].w);
        regs[k] = sgx_reg(x, &in[k]);
    }
    if (s->zero) zero = sgx_movi(x, 0); /* MOV, which keeps the flags a cc form reads */
    if (s->flags) {
        /* the flags this writes end the ones before it */
        sgx_flags_clobbered(x);
    }
    i = sgx_append(x, SGX_K_SEL);
    i->sel = s;
    i->imm = imm;
    i->cc = (unsigned char)cc;
    i->w = (unsigned char)w;
    i->flags_from = flags_from;
    i->fsel = s;
    for (k = 0; k < nin; ++k) i->in[k] = regs[k];
    i->in[4] = zero;
    if (s->out >= 0) i->def = sgx_vreg(x, x->n - 1, s->clean_out ? w : 64);
    if (s->flags) x->flags_ins = x->n - 1;
    return i->def;
}

/* the flags of the instruction that made them end: the bools that are condition codes on them are made 0 or 1 in a
   register first (SETcc, which the instructions between keep), in case they are read after (a dropped one costs
   nothing: nothing reads it) */
static inline void sgx_flags_clobbered(SgX* x) {
    int i;
    if (x->flags_ins < 0) return;
    for (i = 1; i < x->nb; ++i)
        if (x->b[i].kind == SGX_B_CC && x->b[i].v == x->flags_ins) {
            SgXIns* s;
            int z = sgx_movi(x, 0);
            s = sgx_append(x, SGX_K_SETCC);
            s->cc = x->b[i].cc;
            s->flags_from = x->flags_ins;
            s->in[0] = z;
            s->def = sgx_vreg(x, x->n - 1, 1);
            x->b[i].kind = SGX_B_REG;
            x->b[i].v = s->def;
        }
    x->flags_ins = -1;
}

static inline SgD sgx_op(SgX* x, const SgXSel* table, unsigned n, SgD* in, int nin, unsigned w) {
    SnxU64 k = 0;
    int i;
    const SgXSel* s;
    for (i = 0; i < nin; ++i)
        if (sgx_is_const(&in[i])) k = in[i].k;
    s = sgx_pick(x, table, n, k, w, 0);
    if (!s) {
        /* none this host can run (CLZ without LZCNT): the word is not made here; a constant stands in meanwhile */
        x->unmade = 1;
        return sgx_const(w, 0);
    }
    return sgx_val(w, sgx_emit(x, s, in, nin, w, 0, -1));
}

/* the candidates of a primitive at a width: sgx_sel_NAME_W */
#define SGX_TABLE(name, w) ((w) == 64 ? sgx_sel_##name##_64 : sgx_sel_##name##_32)
#define SGX_TABLE_N(name, w) ((w) == 64 ? sgx_sel_##name##_64_n : sgx_sel_##name##_32_n)
#define SGX_OP(x, name, w, ...)                                                                                        \
    (sgx_op((x), SGX_TABLE(name, w), SGX_TABLE_N(name, w), (SgD[]){__VA_ARGS__},                                     \
            (int)(sizeof((SgD[]){__VA_ARGS__}) / sizeof(SgD)), (w)))

/* the width a primitive runs at for a value of w bits: 32 or 64 */
static inline unsigned sgx_width(unsigned w) { return w <= 32 ? 32 : 64; }

static inline int sgx_reg(SgX* x, SgD* d) {
    if (d->nzcv) sg_fail("NZCV read as bits before it is written", 0, 0);
    if (d->inv) {
        SgD a = sgx_val(d->w, d->v);
        unsigned w = sgx_width(d->w);
        if (!d->v) a = sgx_const(d->w, d->k);
        *d = SGX_OP(x, not, w, a);
        d->w = a.w;
    }
    if (!d->v) {
        d->v = sgx_movi(x, d->k);
        d->k = 0;
    }
    return d->v;
}

/* ---- dynamic bits ---- */

static inline SgD sg_d_empty(unsigned w) { return sgx_const(w, 0); }
static inline SgD sg_d_from_s(SgS s) { return sgx_const(s.w, s.v); }
static inline SgD sg_d_fill(long long w, int one) { return sgx_const((unsigned)w, one ? ~0ull : 0); }
static inline SgD sg_d_of_int(long long v, long long hi, long long lo) {
    return sgx_const((unsigned)(hi - lo + 1), (SnxU64)v >> lo);
}

static inline SgD sgx_zext(SgX* x, SgD a, unsigned w) {
    int r = sgx_reg(x, &a);
    unsigned aw = a.w;
    if (x->v[r].zab <= aw) {
        a.w = w;
        return a;
    }
    if (aw == 32) a = SGX_OP(x, zext32, 64, a);
    else if (aw == 8)
        a = SGX_OP(x, zext8, 32, a);
    else if (aw == 16)
        a = SGX_OP(x, zext16, 32, a);
    else if (aw < 32)
        a = SGX_OP(x, and_i, 32, a, sgx_const(32, sg_ones(aw)));
    else
        a = SGX_OP(x, lsr_i, 64, SGX_OP(x, lsl_i, 64, a, sgx_const(6, 64 - aw)), sgx_const(6, 64 - aw));
    x->v[a.v].zab = (unsigned char)aw;
    a.w = w;
    return a;
}

static inline SgD sg_d_zext(SgCtx* g, SgD a, long long w) {
    if (sgx_is_const(&a)) return sgx_const((unsigned)w, a.k);
    if (w > 64) {
        SgD r = sgx_zext(sgx(g), a, 64);
        r.w = (unsigned)w;
        r.vh = 0;
        r.kh = 0;
        return r;
    }
    return sgx_zext(sgx(g), a, (unsigned)w);
}

static inline SgD sg_d_sext(SgCtx* g, SgD a, long long w) {
    SgX* x = sgx(g);
    unsigned aw = a.w;
    if (sgx_is_const(&a)) {
        SnxU64 k = a.k;
        if (aw < 64 && (k >> (aw - 1)) & 1) k |= ~sg_ones(aw);
        return sgx_const((unsigned)w, k);
    }
    if (w > 64) sg_fail("a sign extension past 64 bits", w, 64);
    if (aw == (unsigned)w) return a;
    if (aw == 8) a = w <= 32 ? SGX_OP(x, sext8, 32, a) : SGX_OP(x, sext8, 64, a);
    else if (aw == 16)
        a = w <= 32 ? SGX_OP(x, sext16, 32, a) : SGX_OP(x, sext16, 64, a);
    else if (aw == 32)
        a = SGX_OP(x, sext32, 64, a);
    else {
        unsigned pw = sgx_width((unsigned)w);
        a = SGX_OP(x, asr_i, pw, SGX_OP(x, lsl_i, pw, a, sgx_const(6, pw - aw)), sgx_const(6, pw - aw));
    }
    a.w = (unsigned)w;
    return a;
}

static inline SgD sg_d_slice(SgCtx* g, SgD a, long long hi, long long lo) {
    SgX* x = sgx(g);
    unsigned w = (unsigned)(hi - lo + 1);
    if (a.w > 64) {
        SgD l = a.v ? sgx_val(64, a.v) : sgx_const(64, a.k), h = a.vh ? sgx_val(a.w - 64, a.vh) : sgx_const(a.w - 64, a.kh);
        if (lo >= 64) return sg_d_slice(g, h, hi - 64, lo - 64);
        if (hi < 64) return sg_d_slice(g, l, hi, lo);
        /* across the two: the low part's high bits and the high part's low ones (EXTR) */
        {
            SgD a1 = sg_d_slice(g, l, 63, lo), b1 = sg_d_zext(g, sg_d_slice(g, h, hi - 64, 0), 64);
            SgD sh = SGX_OP(x, lsl_i, 64, b1, sgx_const(6, (SnxU64)(64 - lo)));
            SgD r = SGX_OP(x, or, 64, sg_d_zext(g, a1, 64), sh);
            r.w = w;
            return r;
        }
    }
    if (a.nzcv) {
        /* a flag of NZCV not made yet (bit 3 N, 2 Z, 1 C, 0 V): its condition code */
        sg_fail("a slice of NZCV", hi, lo);
    }
    if (sgx_is_const(&a)) return sgx_const(w, a.k >> lo);
    if (lo == 0 && !a.inv) {
        a.w = w;
        return a;
    }
    if (a.inv && lo == 0) {
        a.w = w;
        return a;
    }
    sgx_reg(x, &a);
    a = SGX_OP(x, lsr_i, sgx_width(a.w), a, sgx_const(6, (SnxU64)lo));
    a.w = w;
    return a;
}

static inline SgD sg_d_concat(SgCtx* g, SgD h, SgD l) {
    SgX* x = sgx(g);
    unsigned w = h.w + l.w;
    if (w > 64) {
        SgD r;
        if (l.w != 64) sg_fail("a concatenation past 64 bits of a low part not of 64", l.w, h.w);
        memset(&r, 0, sizeof r);
        r.w = w;
        if (sgx_is_const(&l)) r.k = l.k;
        else
            r.v = sgx_reg(x, &l);
        if (sgx_is_const(&h)) r.kh = h.k;
        else
            r.vh = sgx_reg(x, &h);
        return r;
    }
    if (sgx_is_const(&h) && sgx_is_const(&l)) return sgx_const(w, h.k << l.w | l.k);
    if (sgx_is_const(&h) && !h.k) return sg_d_zext(g, l, w);
    {
        unsigned pw = sgx_width(w);
        SgD hs = sgx_is_const(&h) ? sgx_const(pw, h.k << l.w) : SGX_OP(x, lsl_i, pw, sg_d_zext(g, h, pw), sgx_const(6, l.w));
        SgD r = sgx_is_const(&l) && !l.k ? hs : SGX_OP(x, or, pw, hs, sg_d_zext(g, l, pw));
        r.w = w;
        return r;
    }
}

static inline SgD sg_d_not(SgCtx* g, SgD a) {
    (void)g;
    if (sgx_is_const(&a)) return sgx_const(a.w, ~a.k);
    a.inv = !a.inv;
    return a;
}

static inline SgD sgx_logic(SgCtx* g, int op, SgD a, SgD b) {
    SgX* x = sgx(g);
    unsigned w = a.w, pw = sgx_width(w);
    if (a.w > 64) sg_fail("a logical operation past 64 bits", a.w, 64);
    if (sgx_is_const(&a) && sgx_is_const(&b))
        return sgx_const(w, op == 0 ? a.k & b.k : op == 1 ? a.k | b.k : a.k ^ b.k);
    if (sgx_is_const(&a)) {
        SgD t = a;
        a = b;
        b = t;
    }
    if (sgx_is_const(&b)) {
        SnxU64 ones = sg_ones(w);
        if (op == 0 && !b.k) return sgx_const(w, 0);
        if (op == 0 && b.k == ones) return a;
        if (op != 0 && !b.k) return a;
        if (op == 2 && b.k == ones) return sg_d_not(g, a);
    }
    if (op == 0 && b.inv && !a.inv) {
        /* BIC: A AND NOT B (ANDN where the host has it) */
        SgD nb = b;
        nb.inv = 0;
        if (sgx_pick(x, SGX_TABLE(bic, pw), SGX_TABLE_N(bic, pw), 0, pw, 0)) {
            SgD r = SGX_OP(x, bic, pw, a, nb);
            r.w = w;
            return r;
        }
    }
    if (sgx_is_const(&b) && a.v && !a.inv && x->v[a.v].op == SGX_C_AND + op && x->v[a.v].cw == pw) {
        /* a chain of the same operation with constants, folded at run time: x AND k1 AND k2 = x AND (k1 AND k2), and
           so for OR and XOR (what made a, dropped as the block ends if nothing else reads it) */
        SgXV* av = &x->v[a.v];
        SnxU64 k = op == 0 ? av->k & b.k : op == 1 ? av->k | b.k : av->k ^ b.k;
        SgD src = sgx_val(a.w, av->src);
        return sgx_logic(g, op, src, sgx_const(w, k));
    }
    {
        SgD r;
        int imm = sgx_is_const(&b) && sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, b.k, pw);
        if (op == 0) r = imm ? SGX_OP(x, and_i, pw, a, b) : SGX_OP(x, and, pw, a, b);
        else if (op == 1)
            r = imm ? SGX_OP(x, or_i, pw, a, b) : SGX_OP(x, or, pw, a, b);
        else
            r = imm ? SGX_OP(x, xor_i, pw, a, b) : SGX_OP(x, xor, pw, a, b);
        if (sgx_is_const(&b) && a.v && !a.inv && r.v) {
            x->v[r.v].op = (unsigned char)(SGX_C_AND + op);
            x->v[r.v].cw = (unsigned char)pw;
            x->v[r.v].src = a.v;
            x->v[r.v].k = b.k;
        }
        r.w = w;
        return r;
    }
}
static inline SgD sg_d_and(SgCtx* g, SgD a, SgD b) { return sgx_logic(g, 0, a, b); }
static inline SgD sg_d_or(SgCtx* g, SgD a, SgD b) { return sgx_logic(g, 1, a, b); }
static inline SgD sg_d_xor(SgCtx* g, SgD a, SgD b) { return sgx_logic(g, 2, a, b); }

static inline SgD sgx_addsub(SgCtx* g, SgD a, SgD b, int sub) {
    SgX* x = sgx(g);
    unsigned w = a.w, pw = sgx_width(w);
    SgD r;
    if (sgx_is_const(&a) && sgx_is_const(&b)) return sgx_const(w, sub ? a.k - b.k : a.k + b.k);
    if (sgx_is_const(&b) && !b.k) return a;
    if (!sub && sgx_is_const(&a) && !a.k) return b;
    if (sgx_is_const(&b)) {
        SnxU64 k = sub ? (SnxU64)0 - b.k : b.k;
        if (a.v && !a.inv && x->v[a.v].op == SGX_C_ADD && x->v[a.v].cw == pw) {
            /* (x + k1) + k2 = x + (k1 + k2) */
            SgD src = sgx_val(a.w, x->v[a.v].src);
            return sgx_addsub(g, src, sgx_const(w, x->v[a.v].k + k), 0);
        }
        if (sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, k, pw)) {
            r = SGX_OP(x, add_i, pw, a, sgx_const(32, k));
            if (a.v && !a.inv && r.v) {
                x->v[r.v].op = SGX_C_ADD;
                x->v[r.v].cw = (unsigned char)pw;
                x->v[r.v].src = a.v;
                x->v[r.v].k = k;
            }
            r.w = w;
            return r;
        }
    }
    if (!sub && sgx_is_const(&a)) {
        SgD t = a;
        a = b;
        b = t;
    }
    if (sub && sgx_is_const(&a) && !a.k) r = SGX_OP(x, neg, pw, b);
    else
        r = sub ? SGX_OP(x, sub, pw, a, b) : SGX_OP(x, add, pw, a, b);
    r.w = w;
    return r;
}
static inline SgD sg_d_add(SgCtx* g, SgD a, SgD b) { return sgx_addsub(g, a, b, 0); }
static inline SgD sg_d_sub(SgCtx* g, SgD a, SgD b) { return sgx_addsub(g, a, b, 1); }
static inline SgD sg_d_add_int(SgCtx* g, SgD a, long long n) { return sgx_addsub(g, a, sgx_const(a.w, (SnxU64)n), 0); }

static inline SgD sgx_shift_i(SgCtx* g, int kind, SgD a, long long n) {
    SgX* x = sgx(g);
    unsigned w = a.w, pw = sgx_width(w);
    SgD r;
    if (!n) return a;
    if (sgx_is_const(&a)) {
        SnxU64 k = a.k, m = sg_ones(w);
        if (kind == 0) return sgx_const(w, n >= 64 ? 0 : k << n);
        if (kind == 1) return sgx_const(w, n >= 64 ? 0 : k >> n);
        if (kind == 2) {
            long long s = w < 64 ? (long long)(k << (64 - w)) >> (64 - w) : (long long)k;
            return sgx_const(w, (SnxU64)(s >> (n >= 64 ? 63 : n)));
        }
        return sgx_const(w, ((k >> (n % w)) | (k << ((w - n % w) % w))) & m);
    }
    if (w != pw && kind != 0) {
        /* a narrower value: its bits above made what the shift brings in (zeros, or its sign) first */
        a = kind == 2 ? sg_d_sext(g, a, pw) : kind == 1 ? sg_d_zext(g, a, pw) : a;
        if (kind == 3) sg_fail("a rotation of a value not of 32 or 64 bits", w, n);
    }
    switch (kind) {
    case 0: r = SGX_OP(x, lsl_i, pw, a, sgx_const(pw == 64 ? 6 : 5, (SnxU64)n)); break;
    case 1: r = SGX_OP(x, lsr_i, pw, a, sgx_const(pw == 64 ? 6 : 5, (SnxU64)n)); break;
    case 2: r = SGX_OP(x, asr_i, pw, a, sgx_const(pw == 64 ? 6 : 5, (SnxU64)n)); break;
    default: r = SGX_OP(x, ror_i, pw, a, sgx_const(pw == 64 ? 6 : 5, (SnxU64)n)); break;
    }
    r.w = w;
    return r;
}
static inline SgD sg_d_lsl(SgCtx* g, SgD a, long long n) { return sgx_shift_i(g, 0, a, n); }
static inline SgD sg_d_lsr(SgCtx* g, SgD a, long long n) { return sgx_shift_i(g, 1, a, n); }
static inline SgD sg_d_asr(SgCtx* g, SgD a, long long n) { return sgx_shift_i(g, 2, a, n); }
static inline SgD sg_d_ror(SgCtx* g, SgD a, long long n) { return sgx_shift_i(g, 3, a, n); }

/* LSLV, LSRV, ASRV, RORV: the amount UInt(operand2) MOD datasize, which the x86 forms selected for them take as it
   is (their count mask, the selection proved, is that MOD) */
static inline SgD sg_d_shift_dyn(SgCtx* g, SgD a, SgD amount, int kind) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    SgD r;
    if (w != 32 && w != 64) sg_fail("a shift by a register of a value not of 32 or 64 bits", w, kind);
    if (sgx_is_const(&amount)) return sgx_shift_i(g, kind, a, (long long)(amount.k & (w - 1)));
    switch (kind) {
    case 0: r = SGX_OP(x, lslv, w, a, amount); break;
    case 1: r = SGX_OP(x, lsrv, w, a, amount); break;
    case 2: r = SGX_OP(x, asrv, w, a, amount); break;
    default: r = SGX_OP(x, rorv, w, a, amount); break;
    }
    return r;
}

static inline void sg_d_set_slice(SgCtx* g, SgD* a, long long hi, long long lo, SgD v) {
    unsigned w = a->w, n = (unsigned)(hi - lo + 1);
    SnxU64 mask = sg_ones(n) << lo;
    SgD keep = sg_d_and(g, *a, sgx_const(w, ~mask));
    SgD put = sg_d_lsl(g, sg_d_zext(g, v, w), lo);
    *a = sg_d_or(g, keep, put);
}

static inline SgD sg_d_replicate(SgCtx* g, SgD a, long long n) {
    SgD r = a;
    long long i;
    for (i = 1; i < n; ++i) r = sg_d_concat(g, r, a);
    return r;
}

/* ---- bools ---- */

static inline SgB sg_bool(SgCtx* g, int v) { return sgx_bool(sgx(g), SGX_B_CONST, v != 0, 0); }

/* a bool's value as a register of 0 or 1 */
static inline SgD sgx_bool_bits(SgX* x, SgB c) {
    SgXBool* b = &x->b[c];
    if (b->kind == SGX_B_CONST) return sgx_const(1, (SnxU64)b->v);
    if (b->kind == SGX_B_CC) {
        int z = sgx_movi(x, 0);
        SgXIns* s = sgx_append(x, SGX_K_SETCC);
        s->cc = b->cc;
        s->flags_from = b->v;
        s->in[0] = z;
        s->def = sgx_vreg(x, x->n - 1, 1);
        b = &x->b[c];
        b->kind = SGX_B_REG;
        b->v = s->def;
    }
    return sgx_val(1, b->v);
}

/* a bool as a condition code on the flags as they are now: a register's tested where it is one */
static inline int sgx_bool_cc(SgX* x, SgB c, int* from) {
    SgXBool* b = &x->b[c];
    if (b->kind == SGX_B_CC && b->v == x->flags_ins) {
        *from = b->v;
        return b->cc;
    }
    {
        SgD v = sgx_bool_bits(x, c);
        const SgXSel* s;
        SgD in[2];
        in[0] = v;
        in[1] = v;
        s = sgx_pick(x, sgx_sel_tst_32, sgx_sel_tst_32_n, 0, 32, 0);
        sgx_emit(x, s, in, 2, 32, 0, -1);
        *from = x->flags_ins;
        return s->cc_of[SGX_ARM_NE];
    }
}

static inline SgB sg_b_not(SgCtx* g, SgB a) {
    SgX* x = sgx(g);
    SgXBool b = x->b[a];
    if (b.kind == SGX_B_CONST) return sgx_bool(x, SGX_B_CONST, !b.v, 0);
    if (b.kind == SGX_B_CC) return sgx_bool(x, SGX_B_CC, b.v, b.cc ^ 1); /* x86's condition codes pair by bit 0 */
    return sgx_bool(x, SGX_B_REG, SGX_OP(x, xor_i, 32, sgx_val(1, b.v), sgx_const(32, 1)).v, 0);
}
static inline SgB sgx_bool_op(SgCtx* g, int op, SgB a, SgB b) {
    SgX* x = sgx(g);
    SgXBool ba = x->b[a], bb = x->b[b];
    if (ba.kind == SGX_B_CONST) return op ? (ba.v ? a : b) : (ba.v ? b : a);
    if (bb.kind == SGX_B_CONST) return op ? (bb.v ? b : a) : (bb.v ? a : b);
    {
        SgD va = sgx_bool_bits(x, a), vb = sgx_bool_bits(x, b);
        SgD r = op ? SGX_OP(x, or, 32, va, vb) : SGX_OP(x, and, 32, va, vb);
        return sgx_bool(x, SGX_B_REG, r.v, 0);
    }
}
static inline SgB sg_b_and(SgCtx* g, SgB a, SgB b) { return sgx_bool_op(g, 0, a, b); }
static inline SgB sg_b_or(SgCtx* g, SgB a, SgB b) { return sgx_bool_op(g, 1, a, b); }
static inline SgB sg_b_eq(SgCtx* g, int ne, SgB a, SgB b) {
    SgX* x = sgx(g);
    SgD va = sgx_bool_bits(x, a), vb = sgx_bool_bits(x, b);
    SgD r = SGX_OP(x, xor, 32, va, vb);
    r.w = 1;
    if (!ne) r = SGX_OP(x, xor_i, 32, r, sgx_const(32, 1));
    return sgx_bool(x, SGX_B_REG, r.v, 0);
}

/* c ? a : b of bits: CMOVcc on c's condition code */
static inline SgD sg_d_select(SgCtx* g, SgB c, SgD a, SgD b) {
    SgX* x = sgx(g);
    SgXBool cb = x->b[c];
    unsigned w = a.w, pw = sgx_width(w);
    const SgXSel* s;
    SgD in[3];
    int from, cc, r;
    if (cb.kind == SGX_B_CONST) return cb.v ? a : b;
    if (w > 64) sg_fail("a choice of bits past 64", w, 64);
    sgx_reg(x, &a);
    sgx_reg(x, &b);
    cc = sgx_bool_cc(x, c, &from);
    s = sgx_pick(x, SGX_TABLE(csel, pw), SGX_TABLE_N(csel, pw), 0, pw, 0);
    in[0] = a;
    in[1] = b;
    in[2] = sgx_const(1, 0);
    r = sgx_emit(x, s, in, 2, pw, cc, from);
    {
        SgD d = sgx_val(w, r);
        return d;
    }
}
static inline SgB sg_b_select(SgCtx* g, SgB c, SgB a, SgB b) {
    SgX* x = sgx(g);
    SgD r = sg_d_select(g, c, sgx_bool_bits(x, a), sgx_bool_bits(x, b));
    return sgx_bool(x, SGX_B_REG, sgx_reg(x, &r), 0);
}

/* a == b of bits: CMP (or TEST of a value with itself against 0), and the condition code the selection gives Arm's EQ
   on its flags */
static inline SgB sg_d_eq(SgCtx* g, SgD a, SgD b) {
    SgX* x = sgx(g);
    unsigned w = a.w, pw = sgx_width(w);
    const SgXSel* s;
    SgD in[2];
    if (sgx_is_const(&a) && sgx_is_const(&b)) return sgx_bool(x, SGX_B_CONST, a.k == b.k, 0);
    if (sgx_is_const(&a)) {
        SgD t = a;
        a = b;
        b = t;
    }
    if (w != pw) {
        a = sgx_zext(x, a, pw);
        if (!sgx_is_const(&b)) b = sgx_zext(x, b, pw);
    }
    if (a.inv || b.inv) {
        sgx_reg(x, &a);
        if (!sgx_is_const(&b)) sgx_reg(x, &b);
    }
    if (sgx_is_const(&b) && !b.k) {
        in[0] = a;
        in[1] = a;
        s = sgx_pick(x, SGX_TABLE(tst, pw), SGX_TABLE_N(tst, pw), 0, pw, 0);
    } else if (sgx_is_const(&b) && sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, b.k, pw)) {
        in[0] = a;
        in[1] = sgx_const(32, b.k);
        s = sgx_pick(x, SGX_TABLE(cmp_i, pw), SGX_TABLE_N(cmp_i, pw), b.k, pw, 0);
    } else {
        in[0] = a;
        in[1] = b;
        s = sgx_pick(x, SGX_TABLE(cmp, pw), SGX_TABLE_N(cmp, pw), 0, pw, 0);
    }
    sgx_emit(x, s, in, 2, pw, 0, -1);
    return sgx_bool(x, SGX_B_CC, x->flags_ins, s->cc_of[SGX_ARM_EQ]);
}
static inline SgB sg_d_is_zero(SgCtx* g, SgD a) { return sg_d_eq(g, a, sgx_const(a.w, 0)); }

/* ---- integers ---- */

static inline SgI sgx_int(SgD v, int is_signed) {
    SgI i;
    memset(&i, 0, sizeof i);
    i.v = v;
    i.kind = SGX_I_VAL;
    i.is_signed = is_signed;
    i.w = v.w;
    return i;
}
static inline SgI sg_i_uint(SgCtx* g, SgD x) { return sgx_int(sg_d_zext(g, x, 64), 0); }
static inline SgI sg_i_sint(SgCtx* g, SgD x) { return sgx_int(sg_d_sext(g, x, 64), 1); }
static inline SgI sg_i_const(SgCtx* g, long long v) {
    (void)g;
    return sgx_int(sgx_const(64, (SnxU64)v), 1);
}
/* made modulo 2^64: what an integer's slice of its low bits reads */
static inline SgD sgx_i_val(SgCtx* g, SgI* i) {
    SgX* x = sgx(g);
    if (i->kind == SGX_I_MUL && sgx_is_const(&i->a) && sgx_is_const(&i->b)) {
        i->v = sgx_const(64, i->a.k * i->b.k);
        i->kind = SGX_I_VAL;
    } else if (i->kind == SGX_I_MUL) {
        i->v = SGX_OP(x, mul, 64, i->a, i->b);
        i->kind = SGX_I_VAL;
    } else if (i->kind == SGX_I_MOD)
        i->v = sg_d_zext(g, sg_d_slice(g, i->v, i->m - 1, 0), 64), i->kind = SGX_I_VAL;
    return i->v;
}
static inline SgI sg_i_add(SgCtx* g, SgI a, SgI b) { return sgx_int(sg_d_add(g, sgx_i_val(g, &a), sgx_i_val(g, &b)), a.is_signed); }
static inline SgI sg_i_sub(SgCtx* g, SgI a, SgI b) { return sgx_int(sg_d_sub(g, sgx_i_val(g, &a), sgx_i_val(g, &b)), a.is_signed); }
static inline SgI sg_i_mul(SgCtx* g, SgI a, SgI b) {
    SgI r;
    memset(&r, 0, sizeof r);
    r.kind = SGX_I_MUL;
    r.a = sgx_i_val(g, &a);
    r.b = sgx_i_val(g, &b);
    r.is_signed = a.is_signed || b.is_signed;
    r.w = 64;
    return r;
}

/* the product of two constants, 128 bits: its high half (signed or not) */
static inline SnxU64 sgx_mulh(SnxU64 a, SnxU64 b, int is_signed) {
    SnxU64 a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    SnxU64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    SnxU64 mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    SnxU64 hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    if (is_signed) {
        /* the unsigned product less 2^64 times each negative operand's other operand */
        if ((long long)a < 0) hi -= b;
        if ((long long)b < 0) hi -= a;
    }
    return hi;
}
/* x MOD m, m a power of two: a shift's amount (kept so for sg_d_shift_dyn's x86 form, which masks it) */
static inline SgI sg_i_mod(SgCtx* g, SgI a, long long m) {
    long long k = 0;
    (void)g;
    if (m <= 0 || (m & (m - 1))) sg_fail("a MOD by other than a power of two", m, 0);
    while ((1ll << k) != m) ++k;
    if (a.kind == SGX_I_VAL && sgx_is_const(&a.v)) return sgx_int(sgx_const(64, a.v.k & (SnxU64)(m - 1)), a.is_signed);
    a.kind = SGX_I_MOD;
    a.m = k;
    return a;
}
static inline SgD sg_i_slice(SgCtx* g, SgI i, long long hi, long long lo) {
    SgX* x = sgx(g);
    if (i.kind == SGX_I_MUL && lo == 64 && hi == 127) {
        if (sgx_is_const(&i.a) && sgx_is_const(&i.b)) return sgx_const(64, sgx_mulh(i.a.k, i.b.k, i.is_signed));
        return i.is_signed ? SGX_OP(x, smulh, 64, i.a, i.b) : SGX_OP(x, umulh, 64, i.a, i.b);
    }
    if (i.kind == SGX_I_MOD && lo == 0 && hi < 64) {
        SgD d = i.v;
        if (hi + 1 >= (long long)i.m) return sg_d_zext(g, sg_d_slice(g, d, i.m - 1, 0), hi + 1);
    }
    if (hi >= 64) sg_fail("an integer's bits past 64", hi, lo);
    return sg_d_slice(g, sgx_i_val(g, &i), hi, lo);
}
/* a shift's amount, as gen.c passes it ((amount).v): a MOD left for the x86 form's count mask */
#define SGX_AMOUNT(i) ((i).v)

/* CLZ where the host has no LZCNT: a binary search of shifts, a test and CMOVcc each step (the shifted value and
   the count's sum made before the test, so both choices read its flags), then 1 more where the top bit is still 0
   (w for 0) */
static inline SgD sgx_clz_search(SgCtx* g, SgD a) {
    unsigned w = a.w, step;
    SgD n = sgx_const(w, 0);
    for (step = w / 2; step >= 1; step /= 2) {
        SgD top = sg_d_lsr(g, a, w - step), up = sg_d_lsl(g, a, step), more = sg_d_add_int(g, n, step);
        SgB none = sg_d_is_zero(g, top);
        n = sg_d_select(g, none, more, n);
        a = sg_d_select(g, none, up, a);
    }
    return sg_d_add(g, n, sg_d_xor(g, sg_d_lsr(g, a, w - 1), sgx_const(w, 1)));
}

static inline SgI sg_i_clz(SgCtx* g, SgD a) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    if (w != 32 && w != 64) sg_fail("CLZ of a value not of 32 or 64 bits", w, 0);
    if (!sgx_pick(x, SGX_TABLE(clz, w), SGX_TABLE_N(clz, w), 0, w, 0)) return sgx_int(sg_d_zext(g, sgx_clz_search(g, a), 64), 0);
    return sgx_int(sg_d_zext(g, SGX_OP(x, clz, w, a), 64), 0);
}

/* ---- flags ----
   AddWithCarry and a logical result's NZCV are the flags an instruction left, read through its candidate's map of
   Arm's flags as x86's (fsel), until they are written (the machine's sg_PSTATE_write) or read */

static inline SgD sgx_nzcv(SgX* x, int ins) {
    SgD d;
    memset(&d, 0, sizeof d);
    d.w = 4;
    d.nzcv = ins + 1;
    (void)x;
    return d;
}

/* the condition code on fsel's flags that is Arm's flag f (0 N ... 3 V) */
static inline int sgx_arm_flag_cc(const SgXSel* fsel, int f) {
    int j = fsel->flag_of[f];
    if (j < 0) return -1;
    return (fsel->flag_neg >> f) & 1 ? sgx_flag_ncc[j] : sgx_flag_cc[j];
}

static inline void sg_add_with_carry(SgCtx* g, SgD a, SgD b, SgD cin, SgD* result, SgD* nzcv) {
    SgX* x = sgx(g);
    unsigned w = a.w;
    const SgXSel *s, *alt;
    SgD in[2];
    int r;
    if (w != 32 && w != 64) sg_fail("AddWithCarry of a width not 32 or 64", w, 0);
    if (!sgx_is_const(&cin)) sg_fail("AddWithCarry with a run-time carry in: not made by this back end yet", w, 0);
    in[0] = a;
    if (sgx_is_const(&b)) {
        /* an immediate: a + k, or a - k where b is NOT k and the carry in 1 (ADD's and SUB's imm8 and imm32 forms with
           their flags; CMP's where nothing reads the result) */
        SnxU64 m = sg_ones(w), k = cin.k ? ~b.k & m : b.k & m;
        if (sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, k, w)) {
            in[1] = sgx_const(32, k);
            s = cin.k ? sgx_pick(x, SGX_TABLE(subs_i, w), SGX_TABLE_N(subs_i, w), k, w, 0)
                      : sgx_pick(x, SGX_TABLE(adds_i, w), SGX_TABLE_N(adds_i, w), k, w, 0);
            alt = cin.k ? sgx_pick(x, SGX_TABLE(cmp_i, w), SGX_TABLE_N(cmp_i, w), k, w, 0) : NULL;
            if (s) {
                SgXIns* e;
                r = sgx_emit(x, s, in, 2, w, 0, -1);
                e = &x->ins[x->n - 1];
                e->alt = alt;
                e->cadd = 1;
                e->cadd_k = (long long)(cin.k ? ((SnxU64)0 - k) & m : k);
                *result = sgx_val(w, r);
                *nzcv = sgx_nzcv(x, x->n - 1);
                return;
            }
        }
    }
    if (cin.k) {
        /* a + NOT(b') + 1 = a - b': SUBS, the operand b was NOT of (or NOT b made) */
        SgD o = b;
        if (sgx_is_const(&o)) o = sgx_const(w, ~o.k);
        else
            o.inv = !o.inv;
        in[1] = o;
        s = sgx_pick(x, SGX_TABLE(subs, w), SGX_TABLE_N(subs, w), 0, w, 0);
        alt = sgx_pick(x, SGX_TABLE(cmp, w), SGX_TABLE_N(cmp, w), 0, w, 0);
    } else {
        in[1] = b;
        s = sgx_pick(x, SGX_TABLE(adds, w), SGX_TABLE_N(adds, w), 0, w, 0);
        alt = NULL;
    }
    r = sgx_emit(x, s, in, 2, w, 0, -1);
    x->ins[x->n - 1].alt = alt;
    *result = sgx_val(w, r);
    *nzcv = sgx_nzcv(x, x->n - 1);
}

/* [r[N-1]] @ IsZeroBit(r) @ 0b00: the flags of ANDS. Where the AND that made r is the last to write the flags and the
   selection lists its form for ANDS too, they are its own; else TEST r, r */
static inline SgD sg_d_logic_flags(SgCtx* g, SgD r) {
    SgX* x = sgx(g);
    unsigned w = r.w;
    if ((w == 32 || w == 64) && r.v && !r.inv && x->flags_ins >= 0 && x->v[r.v].def == x->flags_ins) {
        SgXIns* i = &x->ins[x->flags_ins];
        const SgXSel* t = SGX_TABLE(ands, w);
        unsigned k, n = SGX_TABLE_N(ands, w);
        for (k = 0; k < n; ++k)
            if (i->sel && t[k].enc == i->sel->enc && t[k].in[0] == i->sel->in[0] && t[k].in[1] == i->sel->in[1]) {
                i->fsel = &t[k];
                return sgx_nzcv(x, x->flags_ins);
            }
    }
    {
        SgD in[2];
        const SgXSel* s;
        unsigned pw = sgx_width(w);
        if (w != pw) r = sgx_zext(x, r, pw);
        in[0] = r;
        in[1] = r;
        s = sgx_pick(x, SGX_TABLE(tst, pw), SGX_TABLE_N(tst, pw), 0, pw, 0);
        sgx_emit(x, s, in, 2, pw, 0, -1);
        return sgx_nzcv(x, x->n - 1);
    }
}

/* ---- the block's end: dropping what nothing reads, registers, bytes ---- */

/* the instructions nothing reads are dropped; one whose result nothing reads but whose flags something does becomes
   its flags-only form (SUBS's result to the zero register: CMP) */
static inline void sgx_optimize(SgX* x) {
    unsigned char* vlive = (unsigned char*)calloc((size_t)x->nv + 1, 1);
    unsigned char* flive = (unsigned char*)calloc((size_t)x->n + 1, 1);
    int i, k;
    if (!vlive || !flive) sg_fail("out of memory", 0, 0);
    /* a store a later one to the same place overwrites, with no load of it and no call of the runtime (which reads the
       state) between, is dropped: a flag an instruction of the block set and a later one set again, a register
       written twice */
    for (i = 0; i < x->n; ++i) {
        SgXIns* s = &x->ins[i];
        if (s->kind != SGX_K_STORE && s->kind != SGX_K_STORE_FLAG) continue;
        for (k = i + 1; k < x->n; ++k) {
            SgXIns* t = &x->ins[k];
            if (t->kind == SGX_K_UNDEF || t->kind == SGX_K_MACHINE || (t->kind == SGX_K_LOAD && t->disp == s->disp))
                break;
            if ((t->kind == SGX_K_STORE || t->kind == SGX_K_STORE_FLAG) && t->disp == s->disp &&
                (t->kind == SGX_K_STORE_FLAG ? 32 : t->w) >= (s->kind == SGX_K_STORE_FLAG ? 32 : s->w)) {
                s->kind = SGX_K_SEL; /* dead: what made its value is dropped too where nothing else reads it */
                s->sel = NULL;
                s->dead = 1;
                s->in[0] = 0;
                s->flags_from = -1;
                break;
            }
        }
    }
    for (i = x->n - 1; i >= 0; --i) {
        SgXIns* s = &x->ins[i];
        int root = s->kind == SGX_K_STORE || s->kind == SGX_K_STORE_FLAG || s->kind == SGX_K_RET || s->kind == SGX_K_UNDEF ||
                   s->kind == SGX_K_MACHINE;
        int live = !s->dead && (root || (s->def && vlive[s->def]) || flive[i]);
        if (!live) {
            s->dead = 1;
            continue;
        }
        if (s->kind == SGX_K_SEL && s->def && !vlive[s->def] && s->alt) {
            s->sel = s->alt;
            s->def = 0;
        }
        for (k = 0; k < 5; ++k)
            if (s->in[k]) vlive[s->in[k]] = 1;
        if (s->flags_from >= 0) flive[s->flags_from] = 1;
    }
    /* constant additions in a row whose flags nothing reads: x + k1 + k2 is x + (k1 + k2) (ADD #1, ADD #2, SUB #3 is
       one ADD), the first dropped where nothing else reads it. Only here, where what reads the flags is known: an ADDS
       of the sum would set them from the last addition alone */
    {
        int changed = 0;
        for (i = 0; i < x->n; ++i) {
            SgXIns* s = &x->ins[i];
            SgXIns* p;
            int d;
            long long sum;
            if (s->dead || !s->cadd || flive[i] || !s->in[0]) continue;
            d = x->v[s->in[0]].def;
            if (d < 0 || d >= i) continue;
            p = &x->ins[d];
            if (p->dead || !p->cadd || flive[d] || p->w != s->w || !p->in[0]) continue;
            sum = (long long)(((SnxU64)p->cadd_k + (SnxU64)s->cadd_k) & sg_ones(s->w));
            if (!sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, (SnxU64)sum, s->w)) continue;
            if (!sum && s->def && (s->w == 64 || x->v[p->in[0]].zab <= s->w)) {
                /* a sum of 0: the value is the first's operand, which every later reader takes instead, where its
                   register is as zero extended as the sum's was (a 32-bit addition's result is, and its readers
                   counted on it) */
                int j, from = s->def, to = p->in[0];
                for (j = i + 1; j < x->n; ++j)
                    for (k = 0; k < 5; ++k)
                        if (x->ins[j].in[k] == from) x->ins[j].in[k] = to;
                s->dead = 1;
                changed = 1;
                continue;
            }
            /* the sum by an addition's form (a subtraction's immediate was the negation) */
            {
                const SgXSel* t = sgx_pick(x, SGX_TABLE(add_i, s->w), SGX_TABLE_N(add_i, s->w), (SnxU64)sum, s->w, 0);
                if (!t) continue;
                s->sel = t;
                s->fsel = t;
                s->alt = NULL;
                s->in[0] = p->in[0];
                s->imm = sum;
                s->cadd_k = sum;
                changed = 1;
            }
        }
        if (changed) {
            /* what the folds left unread: liveness again */
            memset(vlive, 0, (size_t)x->nv + 1);
            for (i = x->n - 1; i >= 0; --i) {
                SgXIns* s = &x->ins[i];
                int root = s->kind == SGX_K_STORE || s->kind == SGX_K_STORE_FLAG || s->kind == SGX_K_RET ||
                           s->kind == SGX_K_UNDEF || s->kind == SGX_K_MACHINE;
                if (s->dead) continue;
                if (!(root || (s->def && vlive[s->def]) || flive[i])) {
                    s->dead = 1;
                    continue;
                }
                for (k = 0; k < 5; ++k)
                    if (s->in[k]) vlive[s->in[k]] = 1;
            }
        }
    }
    for (i = 0; i < x->nv; ++i) x->v[i].last = -1;
    for (i = 0; i < x->n; ++i) {
        if (x->ins[i].dead) continue;
        for (k = 0; k < 5; ++k)
            if (x->ins[i].in[k]) x->v[x->ins[i].in[k]].last = i;
    }
    free(vlive);
    free(flive);
}

/* the host registers values may be in: those a call may change (RAX RCX RDX RSI RDI R8-R11); RBX holds the state's
   address. The machine's part writes what an ABI needs (its prologue, the runtime's calls, the flow) */
static const signed char sgx_pool[] = {6, 7, 8, 9, 10, 11, 0, 2, 1}; /* the fixed-purpose ones (RAX, RDX, RCX: MUL, CL) last */
#define SGX_CALLER_SAVED 0x0FC7u /* RAX RCX RDX RSI RDI R8-R11 */
#define SGX_RSP 4

/* the assembler of a function: its bytes, its spill slots ([RSP + 8 * k], the frame the prologue makes and the
   epilogues give back, its size patched in once the function's code is done) and the machine's labels and
   relocations (x64_a64.h's) */
typedef struct SgXAsm {
    SgX* x;
    SgXBuf* b;
    int owner[16]; /* the virtual register a host one holds, 0 for none, -1 a copy made for one instruction */
    int slots, slots_max;
    int xmm_owner[16];    /* the value an XMM register holds spilled, 0 for none */
    int* next_call;       /* for each instruction, the next one at or after it that calls out (x->n: none) */
    size_t frame_at[256]; /* where an imm32 of the frame's size is */
    int nframe;
    void* machine;        /* the machine's part's own (its labels, its relocations) */
} SgXAsm;

static inline const SgXSel* sgx_find(const SgXSel* t, unsigned n, int rm_is_out, int w_rm) {
    unsigned i;
    (void)w_rm;
    for (i = 0; i < n; ++i) {
        if (t[i].rm < 0 || t[i].feature) continue;
        if (rm_is_out ? t[i].out == t[i].rm : t[i].in[0] == t[i].rm) return &t[i];
    }
    sg_fail("no MOV form for a load or a store", rm_is_out, 0);
}

static inline void sgx_mov_rr(SgXAsm* a, int dst, int src) {
    const SgXSel* s;
    unsigned char r[4];
    if (dst == src) return;
    s = sgx_find(sgx_sel_mov_64, sgx_sel_mov_64_n, 0, 0);
    r[(int)s->out] = (unsigned char)dst;
    r[(int)s->in[0]] = (unsigned char)src;
    s->enc(a->b, r, 0, 0);
}
/* a register to or from memory at base + disp (64 bits, or 32: a 32-bit load zero extends) */
static inline void sgx_mov_mem(SgXAsm* a, int load, int w, int reg, int base, int disp) {
    const SgXSel* m = w == 64 ? sgx_find(sgx_sel_mov_64, sgx_sel_mov_64_n, !load, 0)
                              : sgx_find(sgx_sel_mov_32, sgx_sel_mov_32_n, !load, 0);
    unsigned char r[4] = {0, 0, 0, 0};
    if (load) {
        r[(int)m->out] = (unsigned char)reg;
        r[(int)m->in[0]] = (unsigned char)(16 + base);
    } else {
        r[(int)m->out] = (unsigned char)(16 + base);
        r[(int)m->in[0]] = (unsigned char)reg;
    }
    a->b->disp = disp;
    m->enc(a->b, r, 0, 0);
}

/* MOVQ xmm, r64 (66 REX.W 0F 6E /r) and MOVQ r64, xmm (66 REX.W 0F 7E /r): a spill to an XMM register, as bytes (the
   selection reads general-purpose forms only) */
static inline void sgx_movq(SgXBuf* b, int to_xmm, int xmm, int gpr) {
    sgx_byte(b, 0x66);
    sgx_byte(b, (unsigned char)(0x48 | (xmm >> 3) << 2 | (gpr >> 3)));
    sgx_byte(b, 0x0F);
    sgx_byte(b, to_xmm ? 0x6E : 0x7E);
    sgx_byte(b, (unsigned char)(0xC0 | (xmm & 7) << 3 | (gpr & 7)));
}

/* a value spilled where no register is free: to a free XMM register where nothing calls out before it is last read
   (every XMM register is caller-saved), else to its slot of the frame, given once (an SSA value never changes, so a
   second spill stores nothing) */
static inline void sgx_spill_at(SgXAsm* a, int r, int at) {
    int v = a->owner[r], k;
    SgXV* vv = &a->x->v[v];
    if (vv->slot < 0 && vv->xmm < 0 && a->next_call && at >= 0 && a->next_call[at] > vv->last)
        for (k = 0; k < 16; ++k) {
            if (a->xmm_owner[k] > 0 && a->x->v[a->xmm_owner[k]].last >= at) continue;
            if (a->xmm_owner[k] > 0) a->x->v[a->xmm_owner[k]].xmm = -1;
            a->xmm_owner[k] = v;
            vv->xmm = (signed char)k;
            sgx_movq(a->b, 1, k, r);
            break;
        }
    vv->reg = -1;
    a->owner[r] = 0;
    if (vv->xmm >= 0) return;
    a->owner[r] = v;
    vv->reg = (signed char)r;
    {
        /* the frame */
        if (vv->slot < 0) {
            vv->slot = (short)a->slots++;
            if (a->slots > a->slots_max) a->slots_max = a->slots;
            sgx_mov_mem(a, 0, 64, r, SGX_RSP, 8 * vv->slot);
        }
    }
    vv->reg = -1;
    a->owner[r] = 0;
}
static inline void sgx_spill(SgXAsm* a, int r) { sgx_spill_at(a, r, -1); }

/* a host register free at instruction at, none of avoid's (a mask: the registers the instruction reads or writes):
   one whose value nothing reads after, or else the one whose value is read latest, spilled */
static inline int sgx_free_reg(SgXAsm* a, int at, unsigned avoid) {
    unsigned k, n = a->x->pool && (unsigned)a->x->pool < sizeof sgx_pool ? (unsigned)a->x->pool : sizeof sgx_pool;
    int best = -1;
    for (k = 0; k < n; ++k) {
        int r = sgx_pool[k];
        if (avoid & (1u << r)) continue;
        if (!a->owner[r] || (a->owner[r] > 0 && a->x->v[a->owner[r]].last < at)) {
            if (a->owner[r] > 0) a->x->v[a->owner[r]].reg = -1;
            a->owner[r] = 0;
            return r;
        }
    }
    for (k = 0; k < n; ++k) {
        int r = sgx_pool[k];
        if ((avoid & (1u << r)) || a->owner[r] <= 0) continue;
        if (best < 0 || a->x->v[a->owner[r]].last > a->x->v[a->owner[best]].last) best = r;
    }
    if (best < 0) sg_fail("no host register to spill", at, (long long)avoid);
    sgx_spill_at(a, best, at);
    return best;
}

/* value v in a register (reloaded from its slot where it was spilled), not one of avoid's */
static inline int sgx_in_reg(SgXAsm* a, int v, int at, unsigned avoid) {
    SgXV* vv = &a->x->v[v];
    int r;
    if (vv->reg >= 0) return vv->reg;
    if (vv->slot < 0 && vv->xmm < 0) sg_fail("a value in neither a register nor a slot", v, at);
    r = sgx_free_reg(a, at, avoid);
    vv = &a->x->v[v];
    if (vv->xmm >= 0) sgx_movq(a->b, 0, vv->xmm, r);
    else
        sgx_mov_mem(a, 1, 64, r, SGX_RSP, 8 * vv->slot);
    a->owner[r] = v;
    a->x->v[v].reg = (signed char)r;
    return r;
}

/* moves what host register r holds elsewhere, where it is still read after at */
static inline void sgx_evict(SgXAsm* a, int r, int at, unsigned avoid) {
    int v = a->owner[r];
    if (v > 0 && a->x->v[v].last > at) {
        int to = sgx_free_reg(a, at, avoid | (1u << r));
        sgx_mov_rr(a, to, r);
        a->x->v[v].reg = (signed char)to;
        a->owner[to] = v;
    }
    a->owner[r] = 0;
}

/* the inputs of instruction at in registers, none moved by another's reload: the mask of the registers they are in */
static inline unsigned sgx_inputs_in_regs(SgXAsm* a, int at, unsigned avoid) {
    SgXIns* i = &a->x->ins[at];
    unsigned held = 0;
    int k;
    for (k = 0; k < 5; ++k)
        if (i->in[k]) held |= 1u << sgx_in_reg(a, i->in[k], at, avoid | held);
    return held;
}

static inline void sgx_assemble_sel(SgXAsm* a, int at) {
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    const SgXSel* s = i->sel;
    unsigned char r[4] = {0, 0, 0, 0};
    int k, out_reg = -1, tied_in = s->tied ? s->tied - 1 : -1;
    unsigned c, fixed = s->clobber, held;
    /* the fixed registers it reads and writes: what is moved out of one goes to none of them */
    for (k = 0; k < 4; ++k)
        if (s->in[k] >= 16) fixed |= 1u << (s->in[k] - 16);
    if (s->out >= 16) fixed |= 1u << (s->out - 16);
    held = sgx_inputs_in_regs(a, at, fixed);
    /* the inputs in their places: a fixed register's copied in first (what it held moved out of the way), then the
       others, which that may have moved */
    for (k = 0; k < 4; ++k) {
        int v = i->in[k], p = s->in[k];
        if (!v || p < 16) continue;
        if (x->v[v].reg != p - 16) {
            sgx_evict(a, p - 16, at - 1, fixed | held);
            sgx_mov_rr(a, p - 16, x->v[v].reg);
            a->owner[p - 16] = -1;
        }
    }
    for (k = 0; k < 4; ++k) {
        int v = i->in[k], p = s->in[k];
        if (v && p >= 0 && p < 16) r[p] = (unsigned char)x->v[v].reg;
    }
    /* the fixed registers it writes */
    for (c = 0; c < 16; ++c)
        if (s->clobber & (1u << c)) {
            int keep = 0;
            for (k = 0; k < 4; ++k)
                if (i->in[k] && x->v[i->in[k]].reg == (int)c && x->v[i->in[k]].last == at) keep = 1;
            if (!keep || a->owner[c] > 0) sgx_evict(a, (int)c, at, fixed | held);
        }
    /* the result: in its fixed register, in the register of the input (or the zeroed value) it overwrites, or free */
    if (s->out >= 0) {
        int tv = tied_in >= 0 ? i->in[tied_in] : s->zero ? i->in[4] : 0;
        if (s->out >= 16) {
            out_reg = s->out - 16;
            if (a->owner[out_reg] > 0 && a->owner[out_reg] != i->def) sgx_evict(a, out_reg, at, fixed | held);
        } else if (tv) {
            int from = x->v[tv].reg, dies = x->v[tv].last == at;
            for (k = 0; k < 4; ++k)
                if (k != tied_in && i->in[k] == tv) dies = 0;
            if (dies) {
                out_reg = from;
                a->owner[from] = 0;
                x->v[tv].reg = -1;
            } else {
                out_reg = sgx_free_reg(a, at, fixed | held);
                sgx_mov_rr(a, out_reg, from);
            }
            r[(int)s->out] = (unsigned char)out_reg;
        } else {
            out_reg = sgx_free_reg(a, at, fixed | held);
            r[(int)s->out] = (unsigned char)out_reg;
        }
    }
    s->enc(a->b, r, i->imm, i->cc);
    for (c = 0; c < 16; ++c)
        if (a->owner[c] == -1) a->owner[c] = 0;
    if (out_reg >= 0 && i->def) {
        if (a->owner[out_reg] > 0 && a->owner[out_reg] != i->def) a->x->v[a->owner[out_reg]].reg = -1;
        a->owner[out_reg] = i->def;
        x->v[i->def].reg = (signed char)out_reg;
    }
}

/* the machine's part: the instructions of its own kind (SGX_K_MACHINE, SGX_K_UNDEF, SGX_K_RET), their inputs in
   registers r[] (in[k]'s), which it may only read; the prologue and the epilogues */
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r);

/* one region of code (the instructions in x since the last region): dropped what nothing reads, its values given
   registers, its bytes written after what a's buffer holds. No value lives from one region into the next */
static inline void sgx_assemble_region(SgXAsm* a, SgX* x) {
    int i, k;
    SgXBuf* b = a->b;
    a->x = x;
    memset(a->owner, 0, sizeof a->owner);
    memset(a->xmm_owner, 0, sizeof a->xmm_owner);
    a->slots = 0;
    for (i = 0; i < x->nv; ++i) {
        x->v[i].reg = -1;
        x->v[i].slot = -1;
        x->v[i].xmm = -1;
    }
    sgx_optimize(x);
    a->next_call = (int*)malloc(sizeof(int) * (size_t)(x->n + 1));
    if (!a->next_call) sg_fail("out of memory", 0, 0);
    a->next_call[x->n] = x->n;
    for (i = x->n - 1; i >= 0; --i)
        a->next_call[i] = !x->ins[i].dead && (x->ins[i].calls || x->ins[i].kind == SGX_K_MACHINE) ? i : a->next_call[i + 1];
    for (i = 0; i < x->n; ++i) {
        SgXIns* s = &x->ins[i];
        unsigned char r[5] = {0, 0, 0, 0, 0};
        if (s->dead) continue;
        switch (s->kind) {
        case SGX_K_SEL: sgx_assemble_sel(a, i); break;
        case SGX_K_LOAD: {
            int d = sgx_free_reg(a, i, 0);
            sgx_mov_mem(a, 1, s->w == 64 ? 64 : 32, d, x->base, s->disp);
            a->owner[d] = s->def;
            x->v[s->def].reg = (signed char)d;
            break;
        }
        case SGX_K_STORE:
            sgx_mov_mem(a, 0, s->w == 64 ? 64 : 32, sgx_in_reg(a, s->in[0], i, 0), x->base, s->disp);
            break;
        case SGX_K_STORE_FLAG: {
            /* SETcc on the field's low byte: the others are 0 already, the field being 0 or 1 */
            const SgXSel* m = &sgx_sel_cset_32[0];
            r[(int)m->out] = (unsigned char)(16 + x->base);
            b->disp = s->disp;
            m->enc(b, r, 0, s->cc);
            break;
        }
        case SGX_K_SETCC: {
            const SgXSel* m = &sgx_sel_cset_32[0];
            int z = s->in[0], d = sgx_in_reg(a, z, i, 0);
            if (x->v[z].last != i) {
                int to = sgx_free_reg(a, i, 1u << d);
                sgx_mov_rr(a, to, d);
                d = to;
            } else {
                a->owner[d] = 0;
                x->v[z].reg = -1;
            }
            r[(int)m->out] = (unsigned char)d;
            m->enc(b, r, 0, s->cc);
            a->owner[d] = s->def;
            x->v[s->def].reg = (signed char)d;
            break;
        }
        default: {
            /* the machine's: its inputs in their fixed registers (imm's mask: in[k] wants host register fixed[k]),
               the caller-saved ones spilled first where it calls out */
            unsigned held = 0;
            for (k = 0; k < 5; ++k)
                if (s->in[k] && s->fixed[k] >= 0) {
                    int fr = s->fixed[k];
                    int v = s->in[k], from = sgx_in_reg(a, v, i, held | (1u << fr));
                    if (from != fr) {
                        if (a->owner[fr] > 0) sgx_evict(a, fr, i - 1, held | (1u << fr));
                        sgx_mov_rr(a, fr, from);
                        a->owner[fr] = -1;
                    }
                    held |= 1u << fr;
                    r[k] = (unsigned char)fr;
                } else if (s->in[k]) {
                    r[k] = (unsigned char)sgx_in_reg(a, s->in[k], i, held);
                    held |= 1u << r[k];
                }
            if (s->calls)
                for (k = 0; k < 16; ++k)
                    if ((SGX_CALLER_SAVED & (1u << k)) && a->owner[k] > 0 && x->v[a->owner[k]].last > i) sgx_spill(a, k);
            sgx_machine_ins(a, i, r);
            for (k = 0; k < 16; ++k)
                if (a->owner[k] == -1 || (s->calls && (SGX_CALLER_SAVED & (1u << k)))) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
            if (s->def) {
                /* its result is in RAX */
                a->owner[0] = s->def;
                x->v[s->def].reg = 0;
            }
            break;
        }
        }
    }
    free(a->next_call);
    a->next_call = NULL;
    x->n = 0;
    x->nv = 1;
    x->nb = 1;
    x->flags_ins = -1;
    x->unmade = 0;
}

/* the frame's size, patched into the prologue and every epilogue once the function is done: its slots, 16-aligned
   (RSP is 16-aligned after the prologue's push of RBX, for calls) */
static inline void sgx_patch_frame(SgXAsm* a) {
    int k, size = (a->slots_max * 8 + 15) & ~15;
    for (k = 0; k < a->nframe; ++k) {
        size_t at = a->frame_at[k];
        if (at + 4 <= a->b->cap) {
            a->b->p[at] = (unsigned char)size;
            a->b->p[at + 1] = (unsigned char)(size >> 8);
            a->b->p[at + 2] = (unsigned char)(size >> 16);
            a->b->p[at + 3] = (unsigned char)(size >> 24);
        }
    }
}
