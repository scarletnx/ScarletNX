/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's machine back end, what every host machine's shares (README.md, "The x86-64 back end"): after the
   machine's encodings and selection (x64.h: SgXBuf, SgXSel, SgXVForm, the sgx_sel_* tables and the SGX_* constants
   it lists) and before its assembly (x64_asm.h: the functions declared under "the machine's"), which it calls.

   A value known only when the guest runs is an SgD: bits in a virtual register (or a constant), its width, and how
   many of its register's bits above it are known to be 0 (zab: the selection says of each form whether it leaves its
   result zero extended, and whether it reads only the low bits of an input, so a 32-bit result is used as it is and
   an 8-bit one is zero extended only where something reads it whole). NOT is lazy (a subtraction takes the operand it
   was of), and so is an addition's NZCV: the instruction whose x86 flags are Arm's, by the selection's map (SUBS's C
   is the negation of SUB's CF), until it is written to PSTATE or read. A bool (SgB) is a handle (0 for none, as
   sg_condition_origin gives when no compare made the flags): a constant, a virtual register of 0 or 1, or an x86
   condition code on the flags an instruction left, the selection's condition code for an Arm condition. */


/* what a machine may say of itself (x64.h, arm64.h, risc.h), its default where it says nothing: the bits its
   condition codes carry besides the Arm condition, which a flag's code takes from the selection that set the flags
   (SGX_CC_EXTRA: risc.h's kind of virtual flags); a division of two operands whose divisor mach.h makes 1 where it is 0
   or -1 under the most negative value (SGX_DIV_2OP, not x86's RDX:RAX); vector registers for values (SGX_HAS_VREGS:
   without them a SIMD&FP register is two doublewords in general-purpose ones); the floating point all helpers'
   (SGX_FP_HELPERS) */
#ifndef SGX_CC_EXTRA
#define SGX_CC_EXTRA(fsel) 0
#endif
#ifndef SGX_DIV_2OP
#define SGX_DIV_2OP 0
#endif
#ifndef SGX_HAS_VREGS
#define SGX_HAS_VREGS 1
#endif
#ifndef SGX_FP_HELPERS
#define SGX_FP_HELPERS 0 /* mach_a64.h's exact helpers for all the floating point, not the machine's own code */
#endif
/* a machine of two calling conventions (x64.h's): the registers values may be in, a call's argument n's register (past
   the machine's registers, SGX_NREG + a slot of the frame, where the convention has it on the stack), the frame's first
   slot for values (those below are the convention's) and the vector registers no value may be in as the callee's */
#ifndef SGX_POOL
#define SGX_POOL(a) sgx_pool
#define SGX_POOL_N(a) sizeof sgx_pool
#endif
#ifndef SGX_ARG_REG
#define SGX_ARG_REG(x, n) sgx_arg_regs[n]
#endif
#ifndef SGX_A32_ARG /* AArch32's calls' after the state, its exclusive store's registers and a bus access's memory's base */
#define SGX_A32_ARG(x, n) sgx_a32_args[n]
#define SGX_A32_STREX_REGS(x) sgx_a32_strex
#define SGX_A32_BUS_MEM(x) sgx_a32_args[3]
#endif
#ifndef SGX_SLOT0
#define SGX_SLOT0(a) 0
#endif
#ifndef SGX_XCALLEE
#define SGX_XCALLEE(a) 0u
#endif

/* ---- the SSA of the block's instructions ---- */

enum {
    SGX_K_SEL,        /* a selected form: sel, its inputs in[] by the primitive's order, then the zeroed one's */
    SGX_K_LOAD,       /* def = the 64-bit (w 64) or 32-bit (w 32) field at disp from the state's base */
    SGX_K_STORE,      /* the field at disp = in[0], 64 or 32 bits */
    SGX_K_STORE_FLAG, /* the 32-bit field at disp = condition code cc on flags_from's flags, 0 or 1 */
    SGX_K_SETCC,      /* def = condition code cc on flags_from's flags, 0 or 1 */
    SGX_K_RET,        /* the end: the next pc in[0] (or imm) */
    SGX_K_UNDEF,      /* the runtime's undef(state, pc imm, word disp) */
    SGX_K_MACHINE,    /* the machine's own (x64_a64.h's: labels, jumps, calls), op its kind */
    SGX_K_VEC,        /* an SSE form (vf): its operand k is in[k] (none for the result's unless vtied, or the memory's) */
    SGX_K_COLD,       /* def = cc on flags_from's flags ? sym(state, in[1..w]) : in[0], the call out of the hot code */
    SGX_K_PGET,       /* def = the host register imm (a guest register the function keeps there: x64_codegen.c) */
    SGX_K_PSET        /* the host register imm = in[0] */
};

/* a machine instruction's inputs at most (CASP's six): its fixed registers, the loops that place them and the
   registers an encoding reads are this long */
#define SGX_MACHINE_IN 6
/* every instruction's inputs: the passes over them read this many */
#define SGX_INS_IN     8
/* the conditions of a cold path that are tests of their own (sgx_cold_fp's), a jump each: twice the bits of a float v (w
   32 or 64, its sign shifted out) less a constant, compared unsigned with a limit made once a region */
enum {
    SGX_T_NORMAL,   /* cold unless 2v less 2 and twice the smallest normal is below the limit */
    SGX_T_DENORMAL, /* cold where 2v less 2 is below the limit */
    SGX_T_NAN       /* cold where 2v is above twice infinity (no limit) */
};
#define SGX_TESTS 4
typedef struct {
    int kind;
    int of;    /* the float: 0 the value tied, 1 + k argument k */
    int limit; /* the virtual register of the limit, 0 for none */
    unsigned w;
} SgXTest;
typedef struct {
    unsigned char kind, cc, w, dead;
    unsigned char op; /* SGX_K_MACHINE's kind of instruction */
    const SgXSel* sel;
    int def;
    int def2;  /* a call's second result, in RDX */
    int in[SGX_INS_IN]; /* the primitive's inputs (0-3), the zeroed value (4), a memory operand's base and index (5, 6);
                           a cold path's: the value tied (0), the call's arguments (1-5), its tests' limits (6, 7) */
    long long imm;
    int disp;
    signed char mplace; /* the form's operand that is the memory [in[5] + in[6] + mdisp], -1 for none */
    int mdisp;
    const char* sym;    /* SGX_K_MACHINE's call of a helper: its name */
    int flags_from; /* the instruction whose flags a SETCC, STORE_FLAG or cc form reads */
    const SgXSel* fsel; /* whose map of Arm's flags its flags follow (sel's, or the ANDS candidate of its AND) */
    const SgXSel* alt;  /* the same flags without the result (CMP for SUBS), for a result nothing reads */
    signed char fixed[SGX_MACHINE_IN]; /* the machine's instructions: the host register in[k] must be in, -1 any */
    unsigned char side;   /* a selected form that changes memory (a guest store): kept though nothing reads it */
    unsigned char calls;  /* the machine's: it calls out (the caller-saved registers are spilled around it) */
    unsigned char cadd; /* an addition of the constant cadd_k to in[0] (its flags or not): what sgx_optimize folds
                           into the next such addition where nothing reads the flags of either */
    long long cadd_k;
    /* SGX_K_VEC: the form; its result's operand (-1 none: a store, a compare), which reads in[vout] first when vtied;
       vzero: the result is made 0 first (PXOR of itself), no input; the memory operand (mplace) is the state's
       [base + mdisp] (mstate), the constant pool's kv (mrip), or guest memory [in[5] + in[6] + mdisp]; vflags: it
       writes the flags (UCOMISS, PTEST); vfold: an AVX twin, whose operand 2 may be memory (sgx_fold_loads) */
    unsigned mclobber; /* SGX_K_MACHINE's: the host registers it changes besides its results' (SGX_RET, SGX_RET2) */
    const SgXVForm* vf;
    signed char vout;
    unsigned char vtied, vzero, mstate, mrip, vflags, vfold;
    SnxU64 kv[2];
    /* SGX_K_COLD of sgx_cold_fp: its call reads and writes the state's 32-bit fields at disp (FPCR) and mdisp (FPSR)
       alone (cstate); argument cfpcr - 1 is the field at disp, ckat - 1 the constant imm, both loaded on the cold path
       alone; cw32: the arguments (bits 0-4) and the result (7) of 32 bits in XMM registers; zacc: sgx_cold_fp's */
    unsigned char cstate, cfpcr, ckat, cw32, zacc;
    /* and its tests: what kind of, the float in[of], the limit in[limit] (6 or 7, 0 for none), the width */
    unsigned char ntests;
    struct {
        unsigned char kind, of, limit, w;
    } test[SGX_TESTS];
    unsigned char jdead; /* SGX_K_MACHINE: a jump, kv[1] and kv[0] what is dead where it goes (sgx_dead_words') */
    /* a compare whose flags are no fsel's (sgx_cold_cmp's): Arm's flag f (0 N ... 3 V) the AND of the bools (SgB)
       fb[f][0] and fb[f][1] (0: none), condition codes on its flags until those change (then SETcc's registers) */
    int fb[4][2];
} SgXIns;

typedef struct {
    unsigned char cls;  /* 0 a general-purpose register's, 1 an XMM register's (128 bits) */
    unsigned char zab;  /* its register's bits from here up are 0 (64: none known) */
    unsigned char op;   /* made by an operation with a constant (SGX_C_*) of width cw, of the value src and k: the
                           chain the next such operation of that width folds into (AND of an AND: one AND of both
                           masks; a 32-bit one zero extends, so not into a 64-bit one) */
    unsigned char cw;
    int def;            /* the instruction defining it */
    int last;           /* the last that reads it */
    signed char reg;    /* the host register (an XMM one for cls 1), once given */
    short slot;         /* its spill slot, -1 for none */
    signed char xmm;    /* the XMM register it is spilled to, -1 for none */
    unsigned char pinned; /* SGX_K_PGET's: it is in the pinned host register itself, which no instruction may
                             overwrite as its result (a tied form copies it first) */
    int src;
    SnxU64 k;
    int g0, g1; /* cls 1: its low and high doublewords as general-purpose values, once taken out (sgx_flat) */
} SgXV;
enum { SGX_C_NONE, SGX_C_AND, SGX_C_OR, SGX_C_XOR, SGX_C_ADD, SGX_C_SUM }; /* SUM: src + the value k, registers */

enum { SGX_B_CONST, SGX_B_REG, SGX_B_CC };
typedef struct {
    unsigned char kind, cc;
    int v;  /* SGX_B_CONST: 0 or 1; SGX_B_REG: the virtual register; SGX_B_CC: the instruction whose flags */
} SgXBool;

typedef struct {
    unsigned w;
    int v;     /* the virtual register holding bits [0, min(w, 64)), 0 for the constant k */
    SnxU64 k;
    int vh;    /* w > 64: the one of bits [64, w), 0 for the constant kh */
    SnxU64 kh;
    int inv;   /* the value is NOT of v's (not made yet) */
    int wide;  /* bits past 64 this back end doesn't make (CRC32's operands as the Sail concatenates them, which the
                  lowered builtin doesn't read): an error where something reads them */
    int nzcv;  /* NZCV not made yet: the instruction whose flags are them, + 1 */
    int x;     /* the value is the low w bits of this XMM virtual register (the bits above unknown): floating point and
                  vectors, which the operations of general-purpose registers take out of it first (sgx_flat) */
} SgD;

#define SGX_MAXI 4096
#define SGX_STATE_WORDS 1024 /* the state's 4-byte words dead-store elimination tracks (CPU's first 4 KiB) */
/* the guest's (x64_a64.h's): the state's words masks of dead registers and flags (dead) and V registers (vdead) name,
   at most 200 of them, into words[]; their count */
static inline int sgx_dead_words(SnxU64 dead, SnxU32 vdead, unsigned short* words);
#define SGX_MAXV 8192
#define SGX_MAXB 4096
#define SGX_MAXDW 8
#define SGX_WIDE 256

/* an integer known only when the guest runs, exactly (SgI's node): the operation it is of other integers, or bits, and
   the doublewords of its value in two's complement that something has read so far (sgx_int_dw makes them). bits: how
   many hold it, its sign's included; pos: it is never negative, so bits - 1 hold it and zeros are past them */
#define SGX_IDW    4 /* 256 bits at most */
#define SGX_MAXINT 1024
enum {
    SGX_IN_BITS, SGX_IN_ADD, SGX_IN_SUB, SGX_IN_MUL, SGX_IN_SHL, SGX_IN_SHR, SGX_IN_SHLV, SGX_IN_SHRV, SGX_IN_SELECT
};
typedef struct {
    unsigned id; /* its number, which its place in the ring (SgX.ints) is another's where they differ */
    unsigned char op, pos;
    unsigned char made, carried; /* the doublewords made (bit k the k-th's), and whether the carry is */
    short bits;
    unsigned a, b; /* what it is of */
    int c;         /* SGX_IN_SELECT: the condition, of a where it holds */
    long long k;   /* SGX_IN_SHL, SGX_IN_SHR: the amount */
    SgD n;         /* SGX_IN_SHLV, SGX_IN_SHRV: the amount, of 64 bits, from 0 to 255 */
    SgD dw[SGX_IDW], carry; /* ADD's and SUB's carry (borrow) out of the first, 0 or 1 of 64 bits */
} SgXInt;

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
    int xpool;     /* and the vector registers: the first xpool (0: all) */
    SnxU64 end_dead; /* the guest's state nothing reads past the region's end (sgx_dead_words' mask), for the store
                        pass; 0 for none known */
    SnxU32 end_vdead; /* and its V registers */
    /* ---- the emitters' and the passes' own scratch, here and not a thread's, as every back end a program holds has its
       own ---- */
    /* values over 128 bits (two SIMD&FP registers concatenated, TBL's four, up to 512 bits): their doublewords, the
       lowest first, an SgD's wide - 1 indexing them; a ring, as they live for an instruction's emission */
    SgD wide[SGX_WIDE][SGX_MAXDW];
    unsigned nwide;
    SgXInt ints[SGX_MAXINT]; /* the exact integers, a ring as wide's is: an instruction's */
    unsigned nints;
    /* a few recent values, for one made again (each is checked against the instruction that made it, which a new region
       has replaced): vector forms of registers alone, the pool's loads, sums of two registers */
    int vec_seen[16], pool_seen[8], sum_seen[8];
    unsigned vec_next, pool_next, sum_next;
    /* sgx_optimize's: what is read, of values and of flags; the state's words a later store covers, marked by now */
    unsigned char vlive[SGX_MAXV + 1], flive[SGX_MAXI + 1];
    unsigned stamp[SGX_STATE_WORDS], now;
    int next_call[SGX_MAXI + 1]; /* sgx_assemble_region's (SgXAsm's) */
} SgX;

typedef int SgB;



/* an integer known only when the guest runs: its value modulo 2^64 (v, an SgD of 64 bits: what gen.c's (x).v reads),
   or not made yet: the product of a and b (UMULH's, SMULH's high half), or x MOD 2^m (a shift's amount, which the
   shift's x86 form takes as it is: the selection proved its count mask is Arm's MOD datasize). What needs more of it
   than that (a saturation, a shift right) reads its exact value (node) */
enum { SGX_I_VAL, SGX_I_MUL, SGX_I_MOD };
typedef struct {
    SgD v;
    int kind, is_signed;
    int uint; /* UInt's value itself, 0 to 2^64 - 1: compared unsigned (sg_i_cmp) */
    unsigned w;
    SgD a, b;
    long long m;
    unsigned node; /* the exact integer (SgXInt) it is, 0 for one its 64 bits are, as signed */
} SgI;

/* ---- the machine's values (x64_asm.h's for x86-64) ----
   whether the constant k of w bits is an immediate the primitives of a kind take (SGX_IMM_*: an AND's, an ORR's, an
   EOR's; an addition's without and with its flags; a compare's), which their tables' candidates then fit; a vector
   value's element at bit lo (32-bit aligned, or 64) as the low bits of another; two doublewords as one vector value
   (lo the low) */
enum { SGX_IMM_LOGIC, SGX_IMM_LOGIC_OR, SGX_IMM_LOGIC_XOR, SGX_IMM_ADD, SGX_IMM_ADDS, SGX_IMM_SUBS, SGX_IMM_CMP };
static inline int sgx_imm_ok(SgX* x, int kind, SnxU64 k, unsigned w);
static inline int sgx_xdown(SgX* x, int v, unsigned lo);
static inline int sgx_xconcat(SgX* x, int lo, int hi);

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
    i->mplace = -1;
    memset(i->fixed, -1, sizeof i->fixed);
    return i;
}
static inline int sgx_vreg(SgX* x, int def, unsigned zab) {
    if (x->nv == SGX_MAXV) sg_fail("a block of too many values", x->nv, SGX_MAXV);
    x->v[x->nv].cls = 0;
    x->v[x->nv].g0 = x->v[x->nv].g1 = 0;
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
static inline int sgx_is_const(const SgD* d) {
    return !d->v && !d->x && !d->inv && !d->nzcv && !d->wide && (d->w <= 64 || !d->vh);
}

/* ---- XMM values: the SSE forms' (x64.ops' enc lines) ---- */

static inline void sgx_flags_clobbered(SgX* x);

static inline int sgx_xvreg(SgX* x, int def) {
    int r = sgx_vreg(x, def, 64);
    x->v[r].cls = 1;
    return r;
}
static inline SgD sgx_xval(unsigned w, int v) {
    SgD d;
    memset(&d, 0, sizeof d);
    d.w = w;
    d.x = v;
    return d;
}

/* where an SSE form's memory operand is: none, guest memory [in[5] + in[6] + disp], the state's [base + disp], or
   the constant pool's 16 bytes (lo, hi) */
enum { SGX_VM_NONE, SGX_VM_GUEST, SGX_VM_STATE, SGX_VM_POOL };
/* appends f: in[k] the virtual register of operand k (an XMM one where f's xmask says, else general-purpose); out
   the result's operand (-1: none, the form writes the flags), tied when it is read first (in[out] then its old
   value), zero when it is made 0 first instead. A memory operand memk (mode not NONE) is read, or written where it
   is out: a store, kept though nothing reads it. The result's virtual register (0 for none) */
/* an SSE form of registers alone already made in the region with the same inputs and immediate: its value (a few
   recent ones, each checked against the instruction that made it, which a new region has replaced) */
static inline int sgx_vcse_find(SgX* x, const SgXVForm* f, int out, int tied, const int* in, long long imm) {
    int k, j;
    for (k = 0; k < 16; ++k) {
        int v = x->vec_seen[k], d;
        SgXIns* s;
        if (v <= 0 || v >= x->nv) continue;
        d = x->v[v].def;
        if (d < 0 || d >= x->n) continue;
        s = &x->ins[d];
        if (s->kind != SGX_K_VEC || s->def != v || s->vf != f || s->vout != out || s->vtied != (tied > 0) ||
            s->vzero || s->mplace >= 0 || s->imm != imm || s->dead)
            continue;
        for (j = 0; j < f->nops && j < 4; ++j)
            if ((j == out && !(tied > 0) ? 0 : (in ? in[j] : 0)) != s->in[j]) break;
        if (j == f->nops || j == 4) return v;
    }
    return 0;
}
static inline int sgx_vins(SgX* x, const SgXVForm* f, int out, int tied, const int* in, long long imm, int memk,
                           int mode, int base, int index, int disp, SnxU64 lo, SnxU64 hi) {
    SgXIns* i;
    int k;
    if (tied > 0 && mode == SGX_VM_NONE && f->vex && (x->features & SGX_F_AVX) && out == 0 && f->nops == 2 && in) {
        /* the AVX twin: a op= b is c = a op b, no copy of a where a is read after. isel.c makes each one
           'VMNEM xmm1, xmm2, xmm3/m', so b may be memory there */
        int vin[4] = {0, in[0], in[1], 0}, n = x->n;
        int v = sgx_vins(x, f->vex, 0, 0, vin, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        if (x->n > n) x->ins[x->n - 1].vfold = 1;
        return v;
    }
    if (mode == SGX_VM_NONE && out >= 0 && tied >= 0 && in) {
        int v = sgx_vcse_find(x, f, out, tied, in, imm);
        if (v) return v;
    }
    if (f->feature & ~x->features & ~(unsigned)SGX_F_GPR)
        sg_fail("an SSE form this host's processor lacks", (long long)f->feature, (long long)x->features);
    if (out < 0) sgx_flags_clobbered(x); /* a compare's flags end the ones before: their conditions kept first */
    i = sgx_append(x, SGX_K_VEC);
    i->vf = f;
    i->vout = (signed char)out;
    i->vtied = (unsigned char)(tied != 0);
    i->vzero = (unsigned char)(tied < 0);
    if (tied < 0) i->vtied = 0;
    i->imm = imm;
    for (k = 0; k < f->nops && k < 4; ++k) i->in[k] = in ? in[k] : 0;
    if (out >= 0 && !i->vtied) i->in[out] = 0;
    if (mode != SGX_VM_NONE) {
        i->mplace = (signed char)memk;
        i->in[memk] = 0;
        i->mdisp = disp;
        if (mode == SGX_VM_GUEST) {
            i->in[5] = base;
            i->in[6] = index;
        } else if (mode == SGX_VM_STATE)
            i->mstate = 1;
        else {
            i->mrip = 1;
            i->kv[0] = lo;
            i->kv[1] = hi;
        }
    }
    if (out < 0) {
        /* a compare: its flags are the ones now */
        i->vflags = 1;
        x->flags_ins = x->n - 1;
        return 0;
    }
    if (mode != SGX_VM_NONE && out == memk) {
        i->side = 1;
        return 0;
    }
    i->def = (f->xmask >> out & 1) ? sgx_xvreg(x, x->n - 1) : sgx_vreg(x, x->n - 1, 64);
    if (mode == SGX_VM_NONE && tied >= 0 && in) x->vec_seen[x->vec_next++ & 15] = i->def;
    return i->def;
}
/* f of registers only: result operand 0 (tied: the two-operand SSE forms), the operands' registers a, b, c */
static inline int sgx_v2(SgX* x, const SgXVForm* f, int a, int b, long long imm) {
    int in[4] = {a, b, 0, 0};
    return sgx_vins(x, f, 0, 1, in, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
/* f's result operand 0 written, not read: PSHUFD, MOVQ r64, xmm, CVTTSD2SI... of the register b */
static inline int sgx_v1(SgX* x, const SgXVForm* f, int b, long long imm) {
    int in[4] = {0, b, 0, 0};
    return sgx_vins(x, f, 0, 0, in, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
/* 16 bytes from the constant pool (lo the low doubleword) */
static inline int sgx_xconst(SgX* x, SnxU64 lo, SnxU64 hi) {
    /* the region's loads of the pool, a few: one a constant (a value it holds is checked against the instruction that
       made it, which a new region has replaced) */
    int k, v;
    if (!lo && !hi) return sgx_vins(x, SGX_VF_ZERO, 0, -1, NULL, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    for (k = 0; k < 8; ++k) {
        int d;
        v = x->pool_seen[k];
        if (v <= 0 || v >= x->nv) continue;
        d = x->v[v].def;
        if (d >= 0 && d < x->n && x->ins[d].kind == SGX_K_VEC && x->ins[d].mrip && x->ins[d].def == v &&
            x->ins[d].kv[0] == lo && x->ins[d].kv[1] == hi)
            return v;
    }
    v = sgx_vins(x, SGX_VF_LD16, 0, 0, NULL, 0, 1, SGX_VM_POOL, 0, 0, 0, lo, hi);
    x->pool_seen[x->pool_next++ & 7] = v;
    return v;
}

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

static inline SgD sgx_nzcv_bits(SgX* x, int ins);
/* an XMM value as general-purpose registers (MOVD, MOVQ; PEXTRQ for bits past 64), each taken out once */
static inline void sgx_flat(SgX* x, SgD* d) {
    int v = d->x;
    unsigned w = d->w;
    if (!v) return;
    if (!x->v[v].g0) {
        int in[4] = {0, v, 0, 0};
        x->v[v].g0 = sgx_vins(x, w <= 32 ? SGX_VF_X2G32 : SGX_VF_X2G64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        x->v[x->v[v].g0].zab = (unsigned char)(w <= 32 ? 32 : 64);
    } else if (w > 32 && x->v[x->v[v].g0].zab == 32) {
        int in[4] = {0, v, 0, 0};
        x->v[v].g0 = sgx_vins(x, SGX_VF_X2G64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    memset(d, 0, sizeof *d);
    d->w = w;
    d->v = x->v[v].g0;
    if (w > 64) {
        if (!x->v[v].g1) {
            int in[4] = {0, v, 0, 0};
            x->v[v].g1 = sgx_vins(x, SGX_VF_XHI2G, 0, 0, in, 1, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        }
        d->vh = x->v[v].g1;
    }
}

static inline int sgx_reg(SgX* x, SgD* d) {
    sgx_flat(x, d);
    if (d->wide) sg_fail("bits past 64 of a concatenation this back end doesn't make, read", d->w, 0);
    if (d->nzcv) *d = sgx_nzcv_bits(x, d->nzcv - 1);
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

/* any value in an XMM register (its bits above w unknown, but a constant's 0): a constant from the pool (PXOR for 0),
   general-purpose registers by MOVQ (and PINSRQ for the high doubleword) */
static inline int sgx_xreg(SgX* x, SgD* d) {
    int r;
    if (d->x) return d->x;
    if (d->w > 64) {
        SgD lo, hi;
        if (d->wide || d->nzcv) sg_fail("an XMM value of these bits", d->w, 0);
        if (sgx_is_const(d)) r = sgx_xconst(x, d->k, d->kh);
        else {
            lo = d->v ? sgx_val(64, d->v) : sgx_const(64, d->k);
            hi = d->vh ? sgx_val(d->w - 64, d->vh) : sgx_const(d->w - 64, d->kh);
            if (d->inv) sg_fail("an XMM value of a NOT past 64 bits", d->w, 0);
            if (sgx_is_const(&lo) && sgx_is_const(&hi)) r = sgx_xconst(x, lo.k, hi.k);
            else {
                int in[4] = {0, sgx_reg(x, &lo), 0, 0};
                r = sgx_vins(x, SGX_VF_G2X64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
                if (!sgx_is_const(&hi) || hi.k) {
                    in[0] = r;
                    in[1] = sgx_reg(x, &hi);
                    r = sgx_vins(x, SGX_VF_INS64, 0, 1, in, 1, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
                }
            }
        }
    } else if (sgx_is_const(d))
        r = sgx_xconst(x, d->k, 0);
    else {
        int in[4] = {0, sgx_reg(x, d), 0, 0};
        r = sgx_vins(x, d->w <= 32 ? SGX_VF_G2X32 : SGX_VF_G2X64, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    *d = sgx_xval(d->w, r);
    return r;
}
/* a vector value's bits w and up, to 128, made 0 (the machine's: sgx_xclear) */
static inline int sgx_xclear(SgX* x, int v, unsigned w);

/* ---- dynamic bits ---- */

static inline SgD sg_d_empty(unsigned w) { return sgx_const(w, 0); }
static inline SgD sg_d_from_s(SgS s) {
    SgD d = sgx_const(s.w, s.v);
    d.kh = s.vh;
    return d;
}
static inline SgD sg_d_fill(long long w, int one) {
    SgD d = sgx_const((unsigned)w, one ? ~0ull : 0);
    if (one && w > 64) d.kh = sg_ones((unsigned)w - 64);
    return d;
}
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

/* ---- values over 64 bits as doublewords ---- */
static inline unsigned sgx_ndw(unsigned w) { return (w + 63) / 64; }
static inline SgD sgx_half(SgD d, int k);
static inline SgD sg_d_or(SgCtx* g, SgD a, SgD b);
static inline SgD sg_d_lsl(SgCtx* g, SgD a, long long n);
static inline SgD sg_d_lsr(SgCtx* g, SgD a, long long n);
static inline SgD sg_d_not(SgCtx* g, SgD a);
static inline SgD sg_d_zext(SgCtx* g, SgD a, long long w);
static inline SgD sg_d_slice(SgCtx* g, SgD a, long long hi, long long lo);
/* a's doublewords, the lowest first, each 64 bits wide and the last zero above a's bits */
static inline void sgx_dws(SgCtx* g, SgD a, SgD* out) {
    unsigned n = sgx_ndw(a.w), k;
    sgx_flat(sgx(g), &a);
    if (a.wide) {
        int inv = a.inv;
        memcpy(out, sgx(g)->wide[a.wide - 1], n * sizeof *out);
        if (inv) {
            for (k = 0; k < n; ++k) out[k] = sg_d_not(g, out[k]);
            if (a.w % 64) out[n - 1] = sg_d_zext(g, sg_d_slice(g, out[n - 1], a.w % 64 - 1, 0), 64);
        }
        return;
    }
    if (a.w <= 64) {
        out[0] = sg_d_zext(g, a, 64);
        return;
    }
    if (a.w > 128 && (a.v || a.vh || a.k || a.kh)) sg_fail("bits past 128 that are not Zeros", a.w, 0);
    for (k = 2; k < n; ++k) out[k] = sgx_const(64, 0); /* sg_d_fill's Zeros */
    if (a.w > 128) a.w = 128;
    out[0] = sgx_half(a, 0);
    out[1] = sg_d_zext(g, sgx_half(a, 1), 64);
}
/* the 64 bits of the doublewords dw (n of them) from bit pos (negative or past them: zeros) */
static inline SgD sgx_dw_at(SgCtx* g, const SgD* dw, unsigned n, long long pos) {
    long long q = pos >= 0 ? pos / 64 : -((-pos + 63) / 64), r = pos - 64 * q;
    SgD lo = q >= 0 && q < (long long)n ? dw[q] : sgx_const(64, 0);
    SgD hi = q + 1 >= 0 && q + 1 < (long long)n ? dw[q + 1] : sgx_const(64, 0);
    if (!r) return lo;
    return sg_d_or(g, sg_d_lsr(g, lo, r), sg_d_lsl(g, hi, 64 - r));
}
/* a value of w bits from its doublewords (the last's bits past w ignored) */
static inline SgD sgx_of_dws(SgCtx* g, unsigned w, SgD* dw) {
    unsigned n = sgx_ndw(w);
    SgD r;
    if (w % 64) dw[n - 1] = sg_d_zext(g, sg_d_slice(g, dw[n - 1], w % 64 - 1, 0), 64);
    if (w <= 64) {
        r = sg_d_slice(g, dw[0], w - 1, 0);
        return r;
    }
    memset(&r, 0, sizeof r);
    r.w = w;
    if (w <= 128) {
        SgD lo = dw[0], hi = dw[1];
        if (sgx_is_const(&lo)) r.k = lo.k;
        else
            r.v = sgx_reg(sgx(g), &lo);
        if (sgx_is_const(&hi)) r.kh = hi.k;
        else
            r.vh = sgx_reg(sgx(g), &hi);
        return r;
    }
    {
        unsigned slot = sgx(g)->nwide++ % SGX_WIDE;
        memcpy(sgx(g)->wide[slot], dw, n * sizeof *dw);
        r.wide = (int)slot + 1;
        return r;
    }
}

static inline SgD sg_d_zext(SgCtx* g, SgD a, long long w) {
    if (a.x && a.w <= 64 && w > 64) return sgx_xval((unsigned)w, sgx_xclear(sgx(g), a.x, a.w));
    if (a.x && a.w > 64) {
        a.w = (unsigned)w;
        return a;
    }
    sgx_flat(sgx(g), &a);
    if (a.w == (unsigned)w) return a;
    if (w > 128 || a.wide || (a.w > 64 && a.inv)) {
        /* over 128 bits: its doublewords and zeros */
        SgD d[SGX_MAXDW];
        unsigned k;
        if (w > 64 * SGX_MAXDW) sg_fail("an extension past 512 bits", a.w, w);
        sgx_dws(g, a, d);
        for (k = sgx_ndw(a.w); k < sgx_ndw((unsigned)w); ++k) d[k] = sgx_const(64, 0);
        return sgx_of_dws(g, (unsigned)w, d);
    }
    if (a.w > 64) {
        /* past 64 bits already: its high part's bits above it are 0, as a register's or a constant's are */
        a.w = (unsigned)w;
        return a;
    }
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

static inline SgD sgx_shift_i(SgCtx* g, int kind, SgD a, long long n);
static inline SgD sg_d_concat(SgCtx* g, SgD h, SgD l);
static inline SgD sg_d_slice(SgCtx* g, SgD a, long long hi, long long lo);
static inline SgD sg_d_sext(SgCtx* g, SgD a, long long w) {
    sgx_flat(sgx(g), &a);

    SgX* x = sgx(g);
    unsigned aw = a.w;
    if (w > 64 && aw != (unsigned)w) {
        /* past a doubleword: the low one a's sign extended (or a's own), the high part its sign's copies (or a's
           high part sign extended) */
        SgD lo = aw <= 64 ? sg_d_sext(g, a, 64) : sg_d_slice(g, a, 63, 0);
        SgD hi = aw <= 64 ? sg_d_slice(g, sgx_shift_i(g, 2, lo, 63), (long long)w - 65, 0)
                          : sg_d_sext(g, sg_d_slice(g, a, aw - 1, 64), w - 64);
        return sg_d_concat(g, hi, lo);
    }
    if (sgx_is_const(&a)) {
        SnxU64 k = a.k;
        if (aw < 64 && (k >> (aw - 1)) & 1) k |= ~sg_ones(aw);
        return sgx_const((unsigned)w, k);
    }
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

static inline SgD sgx_cmp_flag_bits(SgX* x, int ins, int f);
static inline SgD sg_d_slice(SgCtx* g, SgD a, long long hi, long long lo) {
    SgX* x = sgx(g);
    unsigned w = (unsigned)(hi - lo + 1);
    if (a.x) {
        /* an XMM value's low bits are themselves (of a float's or a vector's width: others go to general-purpose
           registers); an aligned element of 32 or 64 bits PSHUFD's to the bottom */
        if (lo == 0 && (w == 16 || w == 32 || w == 64 || w == 128)) {
            a.w = w;
            return a;
        }
        if ((lo == 64 && hi <= 127) || (lo % 32 == 0 && hi < lo + 32 && hi <= 127)) {
            return sgx_xval(w, sgx_xdown(x, a.x, (unsigned)lo));
        }
        sgx_flat(x, &a);
    }
    if (a.w > 64) {
        /* each doubleword of the slice: the source's 64 bits from there, the last cut to the slice's width */
        SgD in[SGX_MAXDW], out[SGX_MAXDW];
        unsigned j;
        if (lo == 0 && (unsigned)hi + 1 == a.w) return a;
        sgx_dws(g, a, in);
        for (j = 0; j < sgx_ndw(w); ++j) out[j] = sgx_dw_at(g, in, sgx_ndw(a.w), lo + 64ll * j);
        return sgx_of_dws(g, w, out);
    }
    if (a.nzcv && !x->ins[a.nzcv - 1].fsel) {
        /* a compare's flags (bit 3 N, 2 Z, 1 C, 0 V): those of the slice alone, the top one first */
        SgD r = sgx_const(32, 0);
        long long k;
        for (k = hi; k >= lo; --k) {
            SgD f = sgx_cmp_flag_bits(x, a.nzcv - 1, (int)(3 - k));
            f.w = 32;
            r = sgx_is_const(&r) ? f : SGX_OP(x, or, 32, SGX_OP(x, lsl_i, 32, r, sgx_const(5, 1)), f);
        }
        r.w = w;
        return r;
    }
    if (a.nzcv) sg_fail("a slice of NZCV", hi, lo);
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

static inline SgD sg_d_or(SgCtx* g, SgD a, SgD b);
static inline SgD sg_d_lsl(SgCtx* g, SgD a, long long n);
static inline SgD sg_d_concat(SgCtx* g, SgD h, SgD l) {
    SgX* x = sgx(g);
    unsigned w = h.w + l.w;
    if (!l.w) return h; /* x : Zeros(0), a shift by 0 */
    if (!h.w) return l;
    if ((h.x || l.x) && w == 128 && h.w == 64) {
        /* two doublewords in an XMM register: PUNPCKLQDQ (the low's high bits are the high's low ones) */
        int lx = sgx_xreg(x, &l), hx = sgx_xreg(x, &h);
        return sgx_xval(128, sgx_xconcat(x, lx, hx));
    }
    sgx_flat(x, &h);
    sgx_flat(x, &l);
    if (w > 64) {
        /* each doubleword of the result: l's there, ORed with h's shifted up past l */
        SgD hd[SGX_MAXDW], ld[SGX_MAXDW], out[SGX_MAXDW];
        unsigned j;
        if (w > 64 * SGX_MAXDW) sg_fail("a concatenation over 512 bits", h.w, l.w);
        sgx_dws(g, h, hd);
        sgx_dws(g, l, ld);
        for (j = 0; j < sgx_ndw(w); ++j)
            out[j] = sg_d_or(g, sgx_dw_at(g, ld, sgx_ndw(l.w), 64ll * j),
                             sgx_dw_at(g, hd, sgx_ndw(h.w), 64ll * j - (long long)l.w));
        return sgx_of_dws(g, w, out);
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
    sgx_flat(sgx(g), &a);

    (void)g;
    if (sgx_is_const(&a) && a.w > 64) {
        a.k = ~a.k;
        a.kh = ~a.kh & sg_ones(a.w - 64);
        return a;
    }
    if (sgx_is_const(&a)) return sgx_const(a.w, ~a.k);
    a.inv = !a.inv;
    return a;
}

/* a value past 64 bits by its doublewords: 0 the low 64 bits, 1 the rest (a NOT of the whole is each half's) */
static inline SgD sgx_half(SgD d, int k) {
    SgD r;
    if (d.x) sg_fail("a doubleword of an XMM value read as general-purpose bits", d.w, k);
    if (d.wide) sg_fail("bits past 64 of a concatenation this back end doesn't make, read", d.w, 0);
    if (k == 0) r = d.v ? sgx_val(64, d.v) : sgx_const(64, d.k);
    else
        r = d.vh ? sgx_val(d.w - 64, d.vh) : sgx_const(d.w - 64, d.kh);
    if (d.inv) {
        if (sgx_is_const(&r)) r = sgx_const(r.w, ~r.k);
        else
            r.inv = 1;
    }
    return r;
}

static inline SgD sgx_logic(SgCtx* g, int op, SgD a, SgD b) {
    sgx_flat(sgx(g), &a);
    sgx_flat(sgx(g), &b);

    SgX* x = sgx(g);
    unsigned w = a.w, pw = sgx_width(w);
    if (a.w > 64) { /* each doubleword's own */
        SgD ad[SGX_MAXDW], bd[SGX_MAXDW];
        unsigned k;
        sgx_dws(g, a, ad);
        sgx_dws(g, b, bd);
        for (k = 0; k < sgx_ndw(a.w); ++k) ad[k] = sgx_logic(g, op, ad[k], bd[k]);
        return sgx_of_dws(g, a.w, ad);
    }
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
        int imm = sgx_is_const(&b) && sgx_imm_ok(x, SGX_IMM_LOGIC + op, b.k, pw);
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
    sgx_flat(sgx(g), &a);
    sgx_flat(sgx(g), &b);

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
        if (sgx_imm_ok(x, SGX_IMM_ADD, k, pw)) {
            r = SGX_OP(x, leai, pw, a, sgx_const(32, k));
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
    {
        /* a sum of two registers the region made already (an address a load and a store both take): that value */
        if (!sub && a.v && b.v && !a.inv && !b.inv && !(sgx_is_const(&a) || sgx_is_const(&b))) {
            int k;
            for (k = 0; k < 8; ++k) {
                int v = x->sum_seen[k], d;
                if (v <= 0 || v >= x->nv) continue;
                d = x->v[v].def;
                if (d >= 0 && d < x->n && x->ins[d].kind == SGX_K_SEL && x->ins[d].def == v &&
                    x->ins[d].sel == (pw == 64 ? &sgx_sel_lea_64[0] : &sgx_sel_lea_32[0]) &&
                    ((x->ins[d].in[0] == a.v && x->ins[d].in[1] == b.v) ||
                     (x->ins[d].in[0] == b.v && x->ins[d].in[1] == a.v))) {
                    r = sgx_val(pw, v);
                    r.w = w;
                    return r;
                }
            }
            r = SGX_OP(x, lea, pw, a, b);
            if (r.v) {
                x->sum_seen[x->sum_next++ & 7] = r.v;
                x->v[r.v].op = SGX_C_SUM;
                x->v[r.v].cw = (unsigned char)pw;
                x->v[r.v].src = a.v;
                x->v[r.v].k = (SnxU64)b.v;
            }
            r.w = w;
            return r;
        }
    }
        r = sub ? SGX_OP(x, sub, pw, a, b) : SGX_OP(x, lea, pw, a, b);
    r.w = w;
    return r;
}
static inline SgD sg_d_add(SgCtx* g, SgD a, SgD b) { return sgx_addsub(g, a, b, 0); }
static inline SgD sg_d_sub(SgCtx* g, SgD a, SgD b) { return sgx_addsub(g, a, b, 1); }
static inline SgD sg_d_add_int(SgCtx* g, SgD a, long long n) { return sgx_addsub(g, a, sgx_const(a.w, (SnxU64)n), 0); }

static inline SgD sgx_shift_i(SgCtx* g, int kind, SgD a, long long n) {
    sgx_flat(sgx(g), &a);

    SgX* x = sgx(g);
    unsigned w = a.w, pw = sgx_width(w);
    SgD r;
    if (!n) return a;
    if (kind == 3 && w != 32 && w != 64 && w) {
        /* a rotation of other bits (SHA's y @ x): the low n of them over the rest */
        n %= w;
        return n ? sg_d_concat(g, sg_d_slice(g, a, n - 1, 0), sg_d_slice(g, a, w - 1, n)) : a;
    }
    if (kind != 3 && n >= (long long)w && w <= 64) {
        /* by the width or more (SRI's mask of LSR(Ones(esize), esize)): zeros, or the sign's copies */
        if (kind != 2) return sgx_const(w, 0);
        n = w - 1;
    }
    if (w > 64 && kind != 3) {
        /* over a doubleword: as slices of it, the bits shifted in zeros (or the sign's, ASR) */
        if (n >= (long long)w) {
            if (kind != 2) return sgx_const(w, 0) ;
            n = w - 1;
        }
        if (kind == 0)
            return sg_d_concat(g, sg_d_slice(g, a, w - 1 - n, 0), sgx_const((unsigned)n, 0));
        {
            SgD top = sg_d_slice(g, a, w - 1, n);
            if (kind == 1) return sg_d_zext(g, top, w);
            return sg_d_sext(g, top, w);
        }
    }
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
    sgx_flat(sgx(g), &a);
    sgx_flat(sgx(g), &amount);

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
    unsigned w, n;
    if (lo == 0 && (unsigned)(hi + 1) == a->w && v.w == a->w) { /* all of it */
        *a = v;
        return;
    }
    if (v.x && lo == 0 && sgx_is_const(a) && !a->k && (a->w <= 64 || !a->kh)) {
        /* the low bits of a zero (a vector made from its first element): the XMM value cleared above them */
        *a = sg_d_zext(g, v, a->w);
        return;
    }
    sgx_flat(sgx(g), a);
    sgx_flat(sgx(g), &v);
    w = a->w;
    n = (unsigned)(hi - lo + 1);
    if (w > 64) {
        /* over a doubleword: its bits above the slice, the slice's n bits of v, and its bits below, concatenated */
        SgD mid = v.w > n ? sg_d_slice(g, v, n - 1, 0) : sg_d_zext(g, v, n);
        SgD low = lo ? sg_d_slice(g, *a, lo - 1, 0) : sgx_const(0, 0);
        SgD high = (unsigned)hi + 1 < w ? sg_d_slice(g, *a, w - 1, hi + 1) : sgx_const(0, 0);
        *a = sg_d_concat(g, high, sg_d_concat(g, mid, low));
        return;
    }
    SnxU64 mask = sg_ones(n) << lo;
    SgD keep = sg_d_and(g, *a, sgx_const(w, ~mask));
    SgD put = sg_d_lsl(g, sg_d_zext(g, v, w), lo);
    *a = sg_d_or(g, keep, put);
}

static inline SgD sg_d_replicate(SgCtx* g, SgD a, long long n) {
    SgX* x = sgx(g);
    unsigned w = a.w * (unsigned)n;
    sgx_flat(x, &a);
    if (sgx_is_const(&a)) {
        /* its bits, n times */
        SnxU64 k = 0;
        long long i;
        if (w > 64) goto concat;
        for (i = 0; i < n; ++i) k |= a.k << (a.w * (unsigned)i);
        return sgx_const(w, k);
    }
    if (a.w == 1 && w <= 64) {
        /* one bit everywhere: 0 - the bit (SBFM's Replicate(src<S>), the sign of ASR) */
        SgD r = sg_d_sub(g, sgx_const(w, 0), sg_d_zext(g, a, w));
        r.w = w;
        return r;
    }
    if (w <= 64 && n > 1) {
        /* the bits times 1 every a.w bits: one multiplication */
        SnxU64 pattern = 0;
        long long i;
        unsigned pw = sgx_width(w);
        SgD r;
        for (i = 0; i < n; ++i) pattern |= 1ull << (a.w * (unsigned)i);
        r = SGX_OP(x, mul, pw, sg_d_zext(g, a, pw), sgx_const(pw, pattern));
        r.w = w;
        return r;
    }
concat : {
    SgD r = a;
    long long i;
    for (i = 1; i < n; ++i) r = sg_d_concat(g, r, a);
    return r;
}
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

/* when c holds, sym(state, args...) on a cold path (out of the hot code: the exact helper, where the inline code's
   guards fail), else the value tied: the value of both, in tied's register (an XMM one for a vector's) */
static inline SgD sgx_cold(SgX* x, SgB c, SgD tied, const char* sym, SgD* args, int nargs) {
    int regs[5], k, from, cc, t, cls;
    SgXIns* i;
    SgXBool* b = &x->b[c];
    if (!c || (b->kind == SGX_B_CONST && !b->v)) return tied;
    if (nargs > 5) sg_fail("a cold call of more than five arguments", nargs, 0);
    for (k = 0; k < nargs; ++k) regs[k] = args[k].x ? args[k].x : sgx_reg(x, &args[k]);
    cls = tied.x != 0;
    t = cls ? tied.x : sgx_reg(x, &tied);
    if (b->kind == SGX_B_CONST) {
        /* always: the flags of a TEST of a register of 1 */
        SgD one = sgx_const(32, 1);
        SgB r = sgx_bool(x, SGX_B_REG, sgx_reg(x, &one), 0);
        c = r;
    }
    cc = sgx_bool_cc(x, c, &from);
    i = sgx_append(x, SGX_K_COLD);
    i->cc = (unsigned char)cc;
    i->flags_from = from;
    i->sym = sym;
    i->w = (unsigned char)nargs;
    i->in[0] = t;
    for (k = 0; k < nargs; ++k) i->in[1 + k] = regs[k];
    i->side = 1; /* its call changes FPSR though nothing reads its value */
    i->def = cls ? sgx_xvreg(x, x->n - 1) : sgx_vreg(x, x->n - 1, 64);
    return cls ? sgx_xval(tied.w, i->def) : sgx_val(tied.w, i->def);
}

/* sgx_cold for a floating-point helper sym(state, args..., FPCR, k): FPCR (the state's 32-bit field at fpcr) and the
   constant k loaded on the cold path alone, at fpcr_at and k_at of the arguments (-1: none); the call reads and writes
   no state but the fields at fpcr and fpsr (what the store pass knows). zacc bit 7: a zero result is the hot path's
   still where the state's FPCR has none of zmask's bits and (bits 0-2 not all clear) one of those arguments is a
   zero, and (bit 6) where FPCR.FZ is set none of the floating-point arguments before fpcr_at (of the width k's low
   byte names) is a denormal FZ flushes, tested on the cold path before the call (the guards' exact zeros, out of the
   hot code). The cold path is taken where c holds (0: never) or one of the tests does, each a jump of its own: their
   conditions are never made 0 or 1 */
static inline SgD sgx_cold_fp(SgX* x, SgB c, SgD tied, const char* sym, SgD* args, int nargs, int fpcr_at, int k_at,
                              long long k, int fpcr, int fpsr, unsigned zacc, SnxU32 zmask, const SgXTest* tests,
                              int ntests) {
    int regs[5], j, from = -1, cc = 0, t, cls, n = 0, w32 = 0;
    SgXIns* i;
    SgXBool* b = &x->b[c];
    int known = !c || b->kind == SGX_B_CONST, always = c && known && b->v;
    if (known && !always && !ntests) return tied;
    if (ntests > SGX_TESTS) sg_fail("a cold path of this many tests", ntests, SGX_TESTS);
    for (j = 0; j < nargs + (fpcr_at >= 0) + (k_at >= 0); ++j) {
        if (j == fpcr_at || j == k_at) {
            regs[j] = 0;
            continue;
        }
        if (n >= nargs) sg_fail("a cold call's arguments", j, 0);
        if (args[n].x) w32 |= (args[n].w <= 32) << j;
        regs[j] = args[n].x ? args[n].x : sgx_reg(x, &args[n]);
        ++n;
    }
    if (j > 5) sg_fail("a cold call of more than five arguments", j, 0);
    cls = tied.x != 0;
    t = cls ? tied.x : sgx_reg(x, &tied);
    if (cls && tied.w <= 32) w32 |= 0x80;
    if (always) {
        SgD one = sgx_const(32, 1);
        c = sgx_bool(x, SGX_B_REG, sgx_reg(x, &one), 0);
    }
    if (!known || always) cc = sgx_bool_cc(x, c, &from);
    if (ntests) sgx_flags_clobbered(x); /* the tests' own compares end them, after the condition's jump */
    i = sgx_append(x, SGX_K_COLD);
    i->cc = (unsigned char)cc;
    i->flags_from = from;
    i->sym = sym;
    i->w = (unsigned char)j;
    i->in[0] = t;
    for (n = 0; n < j; ++n) i->in[1 + n] = regs[n];
    for (n = 0; n < ntests; ++n) {
        const SgXTest* p = &tests[n];
        int l = 0, seen = 0;
        for (j = 0; j < i->ntests; ++j) /* the same value's again: an operand that is both of them */
            seen |= i->test[j].kind == p->kind && i->in[i->test[j].of] == i->in[p->of];
        if (seen) continue;
        if (!x->v[i->in[p->of]].cls) sg_fail("a cold path's test of a value in no vector register", n, p->of);
        if (p->limit)
            for (l = 6; i->in[l] && i->in[l] != p->limit;)
                if (++l == SGX_INS_IN) sg_fail("a cold path's tests of more than two limits", n, ntests);
        if (l) i->in[l] = p->limit;
        i->test[i->ntests].kind = (unsigned char)p->kind;
        i->test[i->ntests].of = (unsigned char)p->of;
        i->test[i->ntests].limit = (unsigned char)l;
        i->test[i->ntests++].w = (unsigned char)p->w;
    }
    i->side = 1;
    i->cstate = 1;
    i->disp = fpcr;
    i->mdisp = fpsr;
    i->cfpcr = (unsigned char)(fpcr_at + 1);
    i->ckat = (unsigned char)(k_at + 1);
    i->imm = k;
    i->cw32 = (unsigned char)w32;
    i->zacc = (unsigned char)zacc;
    i->kv[0] = zmask;
    i->def = cls ? sgx_xvreg(x, x->n - 1) : sgx_vreg(x, x->n - 1, 64);
    return cls ? sgx_xval(tied.w, i->def) : sgx_val(tied.w, i->def);
}

/* a floating-point compare of the XMM values a and b (w bits) whose result is the host's flags, Arm's flag f the AND of
   the condition codes fcc[f][0] and fcc[f][1] (-1: none) on them, kept as bools (SgXIns' fb): in the hot code the
   tests' jumps, the machine's compare and, where it is unordered, a jump to the cold path, which for snan (FCMP: only a
   signalling NaN raises IOC) tests a and b for one first and goes back without the call where neither is. The call
   sym(state, a, b, FPCR, k) gives the flags as the machine's flags register would hold them after its compare
   (x64_asm.h: an RFLAGS image, loaded before going back), so the flags after it are the compare's on every path. Its
   instruction's index, for sgx_nzcv */
static inline int sgx_cold_cmp(SgX* x, int a, int b, unsigned w, int snan, const char* sym, long long k, int fpcr,
                               int fpsr, const signed char fcc[4][2], const SgXTest* tests, int ntests) {
    SgD args[2] = {sgx_xval(w, a), sgx_xval(w, b)};
    int at, f, j;
    SgXIns* i;
    if (!ntests) sg_fail("a compare's cold path with no test", w, 0);
    /* sgx_cold_fp's instruction, with no condition (its tests and the compare's jump take it there) and no value */
    sgx_cold_fp(x, 0, args[0], sym, args, 2, 2, 3, k, fpcr, fpsr, 0, 0, tests, ntests);
    at = x->n - 1;
    i = &x->ins[at];
    if (i->def == x->nv - 1) --x->nv;
    i->in[0] = 0;
    i->def = 0;
    i->vflags = 3;
    i->vout = (signed char)!!snan;
    x->flags_ins = at;
    for (f = 0; f < 4; ++f)
        for (j = 0; j < 2; ++j) x->ins[at].fb[f][j] = fcc[f][j] < 0 ? 0 : sgx_bool(x, SGX_B_CC, at, fcc[f][j]);
    return at;
}

/* the vector form of sgx_cold: when c holds, sym(state, &out, &a, &b, &d, desc) on the cold path, the XMM values a,
   b, d (0: a's) passed by their addresses and the result read from out's 16 bytes; else tied's */
static inline int sgx_vcold(SgX* x, SgB c, int tied, const char* sym, int a, int b, int d, SnxU32 desc) {
    int from, cc;
    SgXIns* i;
    SgXBool* bb = &x->b[c];
    if (!c || (bb->kind == SGX_B_CONST && !bb->v)) return tied;
    if (bb->kind == SGX_B_CONST) {
        SgD one = sgx_const(32, 1);
        c = sgx_bool(x, SGX_B_REG, sgx_reg(x, &one), 0);
    }
    cc = sgx_bool_cc(x, c, &from);
    i = sgx_append(x, SGX_K_COLD);
    i->cc = (unsigned char)cc;
    i->flags_from = from;
    i->sym = sym;
    i->vflags = 2;
    i->imm = desc;
    i->in[0] = tied;
    i->in[1] = a;
    i->in[2] = b ? b : a;
    i->in[3] = d ? d : a;
    i->w = 3;
    i->side = 1;
    i->def = sgx_xvreg(x, x->n - 1);
    return i->def;
}

/* sgx_vcold whose cold path tests first (zacc, the machine's sgx_cold_vwarm: x64_asm.h's says its bits) and goes back
   to the hot code without the call where they all pass; FPCR and FPSR at fpcr and fpsr in the state, mask (0: none) a
   vector of the hot code's whose set lanes are the call's */
static inline int sgx_vcold_warm(SgX* x, SgB c, int tied, const char* sym, int a, int b, int d, SnxU32 desc,
                                 unsigned zacc, int fpcr, int fpsr, int mask) {
    int n = x->n, v = sgx_vcold(x, c, tied, sym, a, b, d, desc);
    if (x->n > n) {
        SgXIns* i = &x->ins[x->n - 1];
        i->zacc = (unsigned char)zacc;
        i->disp = fpcr;
        i->mdisp = fpsr;
        i->in[6] = mask;
    }
    return v;
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
    SgD va, vb;
    /* against a constant: the other or its negation (CBZ's IsZero(x) == TRUE stays a condition code) */
    if (x->b[a].kind == SGX_B_CONST) {
        SgB t = a;
        a = b;
        b = t;
    }
    if (x->b[b].kind == SGX_B_CONST) return (x->b[b].v != 0) != (ne != 0) ? a : sg_b_not(g, a);
    va = sgx_bool_bits(x, a), vb = sgx_bool_bits(x, b);
    SgD r = SGX_OP(x, xor, 32, va, vb);
    r.w = 1;
    if (!ne) r = SGX_OP(x, xor_i, 32, r, sgx_const(32, 1));
    return sgx_bool(x, SGX_B_REG, r.v, 0);
}

/* c ? a : b of bits: CMOVcc on c's condition code */
static inline SgD sg_d_select(SgCtx* g, SgB c, SgD a, SgD b) {
    sgx_flat(sgx(g), &a);
    sgx_flat(sgx(g), &b);

    SgX* x = sgx(g);
    SgXBool cb = x->b[c];
    unsigned w = a.w, pw = sgx_width(w);
    const SgXSel* s;
    SgD in[3];
    int from, cc, r;
    if (cb.kind == SGX_B_CONST) return cb.v ? a : b;
    if (w > 64) { /* each doubleword's choice */
        SgD ad[SGX_MAXDW], bd[SGX_MAXDW];
        unsigned k;
        sgx_dws(g, a, ad);
        sgx_dws(g, b, bd);
        for (k = 0; k < sgx_ndw(w); ++k) ad[k] = sg_d_select(g, c, ad[k], bd[k]);
        return sgx_of_dws(g, w, ad);
    }
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

/* a compare's NZCV (sgx_cold_cmp's) == 0010, greater (FCMGT's and FACGT's whole NZCV): UCOMISx b, a's CF without ZF,
   on its flags where they are still its; -1 for another k, or flags no longer its (NZCV made whole and compared) */
static inline SgB sgx_cmp_greater(SgCtx* g, int ins, SnxU64 k) {
    SgX* x = sgx(g);
    if (k != 2 || x->flags_ins != ins) return -1;
    return sg_b_and(g, sgx_bool(x, SGX_B_CC, ins, 2), sgx_bool(x, SGX_B_CC, ins, 5)); /* B, NE */
}
/* a == b of bits: CMP (or TEST of a value with itself against 0), and the condition code the selection gives Arm's EQ
   on its flags */
static inline SgB sg_d_eq(SgCtx* g, SgD a, SgD b) {
    if (a.nzcv && !sgx(g)->ins[a.nzcv - 1].fsel && sgx_is_const(&b)) {
        SgB r = sgx_cmp_greater(g, a.nzcv - 1, b.k);
        if (r >= 0) return r;
    }
    sgx_flat(sgx(g), &a);
    sgx_flat(sgx(g), &b);

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
    } else if (sgx_is_const(&b) && sgx_imm_ok(x, SGX_IMM_CMP, b.k, pw)) {
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
/* a < b, unsigned: CMP's carry (Arm's CC, LO) */
static inline SgB sgx_d_ult(SgCtx* g, SgD a, SgD b) {
    SgX* x = sgx(g);
    unsigned pw;
    const SgXSel* s;
    SgD in[2];
    sgx_flat(x, &a);
    sgx_flat(x, &b);
    if (sgx_is_const(&a) && sgx_is_const(&b)) return sgx_bool(x, SGX_B_CONST, a.k < b.k, 0);
    pw = sgx_width(a.w);
    if (sgx_is_const(&a)) sgx_reg(x, &a);
    a = sgx_zext(x, a, pw);
    if (!sgx_is_const(&b)) b = sgx_zext(x, b, pw);
    if (a.inv) sgx_reg(x, &a);
    if (b.inv) sgx_reg(x, &b);
    in[0] = a;
    if (sgx_is_const(&b) && sgx_imm_ok(x, SGX_IMM_CMP, b.k, pw)) {
        in[1] = sgx_const(32, b.k);
        s = sgx_pick(x, SGX_TABLE(cmp_i, pw), SGX_TABLE_N(cmp_i, pw), b.k, pw, 0);
    } else {
        in[1] = b;
        if (sgx_is_const(&b)) sgx_reg(x, &in[1]);
        s = sgx_pick(x, SGX_TABLE(cmp, pw), SGX_TABLE_N(cmp, pw), 0, pw, 0);
    }
    sgx_emit(x, s, in, 2, pw, 0, -1);
    return sgx_bool(x, SGX_B_CC, x->flags_ins, s->cc_of[SGX_ARM_CC]);
}

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

/* ---- an integer's exact value ---- */

/* the exact integer numbered id */
static inline SgXInt* sgx_int_at(SgX* x, unsigned id) {
    SgXInt* n = &x->ints[id % SGX_MAXINT];
    if (!id || n->id != id) sg_fail("an integer of more than SGX_MAXINT operations", (long long)id, SGX_MAXINT);
    return n;
}
/* a new one: op of a and b, of so many bits */
static inline SgXInt* sgx_int_new(SgX* x, int op, unsigned a, unsigned b, int bits, int pos) {
    SgXInt* n;
    if (bits > 64 * SGX_IDW) sg_fail("an integer over 256 bits", bits, op);
    n = &x->ints[++x->nints % SGX_MAXINT];
    memset(n, 0, sizeof *n);
    n->id = x->nints;
    n->op = (unsigned char)op;
    n->a = a;
    n->b = b;
    n->bits = (short)bits;
    n->pos = (unsigned char)(pos != 0);
    return n;
}
/* how many doublewords hold it: past them its sign's copies (zeros, where it is never negative) */
static inline unsigned sgx_int_ndw(const SgXInt* n) {
    int bits = n->pos ? n->bits - 1 : n->bits;
    return bits <= 64 ? 1u : ((unsigned)bits + 63) / 64;
}
static inline SgD sgx_i_val(SgCtx* g, SgI* i);
/* i's node: the one it has, or of its 64 bits as a signed integer */
static inline unsigned sgx_i_node(SgCtx* g, SgI* i) {
    SgX* x = sgx(g);
    SgXInt* n;
    SgD v;
    if (i->node) return i->node;
    v = sgx_i_val(g, i);
    n = sgx_int_new(x, SGX_IN_BITS, 0, 0, 64, 0);
    n->dw[0] = v;
    n->made = 1;
    return i->node = n->id;
}
static inline int sgx_i_bits(SgCtx* g, const SgI* i) { return i->node ? sgx_int_at(sgx(g), i->node)->bits : 64; }
static inline int sgx_i_pos(SgCtx* g, const SgI* i) { return i->node ? sgx_int_at(sgx(g), i->node)->pos : 0; }

static inline SnxU64 sgx_mulh(SnxU64 a, SnxU64 b, int is_signed);
static inline SgD sgx_int_dw(SgCtx* g, unsigned id, unsigned k);
/* a bool as a doubleword 0 or 1 */
static inline SgD sgx_b_dw(SgCtx* g, SgB b) { return sg_d_zext(g, sgx_bool_bits(sgx(g), b), 64); }
/* the carry (a difference's borrow) out of the first doubleword of a sum */
static inline SgD sgx_int_carry(SgCtx* g, unsigned id) {
    SgXInt* n = sgx_int_at(sgx(g), id);
    SgD a, b, c;
    if (n->carried) return n->carry;
    a = sgx_int_dw(g, n->a, 0);
    b = sgx_int_dw(g, n->b, 0);
    c = sgx_b_dw(g, n->op == SGX_IN_SUB ? sgx_d_ult(g, a, b) : sgx_d_ult(g, sgx_int_dw(g, id, 0), a));
    n->carry = c;
    n->carried = 1;
    return c;
}
/* the amount's bit that moves a whole doubleword (64) or two (128) */
static inline SgB sgx_int_moves(SgCtx* g, SgD amount, unsigned by) {
    return sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, amount, sgx_const(64, by))));
}
/* SGX_IN_SHLV's doubleword k by the amount's low 6 bits alone */
static inline SgD sgx_int_shifted(SgCtx* g, const SgXInt* n, unsigned k) {
    SgD lo = sg_d_shift_dyn(g, sgx_int_dw(g, n->a, k), n->n, 0), other;
    if (!k) return lo;
    other = sg_d_lsr(g, sgx_int_dw(g, n->a, k - 1), 1);
    return sg_d_or(g, lo, sg_d_shift_dyn(g, other, sg_d_not(g, n->n), 1)); /* by 63 - (n MOD 64) more */
}
/* doubleword k of an integer's value in two's complement */
static inline SgD sgx_int_dw(SgCtx* g, unsigned id, unsigned k) {
    SgXInt* n = sgx_int_at(sgx(g), id);
    unsigned ndw = sgx_int_ndw(n);
    SgD r;
    if (k >= ndw) {
        if (n->pos) return sgx_const(64, 0);
        return sg_d_asr(g, sgx_int_dw(g, id, ndw - 1), 63);
    }
    if (n->made >> k & 1) return n->dw[k];
    switch (n->op) {
    case SGX_IN_ADD:
    case SGX_IN_SUB: {
        SgD a = sgx_int_dw(g, n->a, k), b = sgx_int_dw(g, n->b, k);
        r = n->op == SGX_IN_SUB ? sg_d_sub(g, a, b) : sg_d_add(g, a, b);
        if (k > 1) sg_fail("a sum of integers past 128 bits", n->bits, k);
        if (k) {
            SgD in = sgx_int_carry(g, id);
            r = n->op == SGX_IN_SUB ? sg_d_sub(g, r, in) : sg_d_add(g, r, in);
        }
        break;
    }
    case SGX_IN_MUL: {
        /* of two integers that fit 64 bits with their sign: the low half is made already, the high one is SMULH */
        const SgXInt *a = sgx_int_at(sgx(g), n->a), *b = sgx_int_at(sgx(g), n->b);
        SgD av, bv;
        SgX* x = sgx(g);
        if (k != 1 || a->bits > 64 || b->bits > 64) sg_fail("a product of integers over 64 bits", n->bits, k);
        av = sgx_int_dw(g, n->a, 0);
        bv = sgx_int_dw(g, n->b, 0);
        if (sgx_is_const(&av) && sgx_is_const(&bv)) r = sgx_const(64, sgx_mulh(av.k, bv.k, 1));
        else
            r = SGX_OP(x, smulh, 64, av, bv);
        break;
    }
    case SGX_IN_SHL: {
        long long q = n->k / 64, s = n->k % 64;
        SgD lo = (long long)k >= q ? sgx_int_dw(g, n->a, (unsigned)((long long)k - q)) : sgx_const(64, 0);
        r = sg_d_lsl(g, lo, s);
        if (s && (long long)k > q)
            r = sg_d_or(g, r, sg_d_lsr(g, sgx_int_dw(g, n->a, (unsigned)((long long)k - q - 1)), 64 - s));
        break;
    }
    case SGX_IN_SHR: {
        unsigned q = (unsigned)(n->k / 64);
        long long s = n->k % 64;
        const SgXInt* a = sgx_int_at(sgx(g), n->a);
        SgD lo = sgx_int_dw(g, n->a, k + q);
        if (!s) r = lo;
        else if (k + q + 1 >= sgx_int_ndw(a))
            r = a->pos ? sg_d_lsr(g, lo, s) : sg_d_asr(g, lo, s);
        else
            r = sg_d_or(g, sg_d_lsr(g, lo, s), sg_d_lsl(g, sgx_int_dw(g, n->a, k + q + 1), 64 - s));
        break;
    }
    case SGX_IN_SHLV: {
        /* by 64 where the amount is 64 or more (sgx_i_shift): the doublewords moved up one */
        SgB far = sgx_int_moves(g, n->n, 192);
        SgD t = sgx_int_shifted(g, n, k);
        r = sg_d_select(g, far, k ? sgx_int_dw(g, n->a, k - 1) : sgx_const(64, 0), t);
        break;
    }
    case SGX_IN_SHRV: { /* of one doubleword: the sign from an amount of 64 on */
        const SgXInt* a = sgx_int_at(sgx(g), n->a);
        SgD t;
        if (sgx_int_ndw(a) > 1) sg_fail("a shift right by the guest's amount of an integer over 64 bits", a->bits, k);
        t = sg_d_shift_dyn(g, sgx_int_dw(g, n->a, 0), n->n, a->pos ? 1 : 2);
        r = sg_d_select(g, sgx_int_moves(g, n->n, 192), sgx_int_dw(g, n->a, 1), t);
        break;
    }
    case SGX_IN_SELECT: r = sg_d_select(g, n->c, sgx_int_dw(g, n->a, k), sgx_int_dw(g, n->b, k)); break;
    default: sg_fail("an integer's doubleword nothing made", n->op, k);
    }
    n->dw[k] = r;
    n->made |= (unsigned char)(1u << k);
    return r;
}

/* UInt(x), SInt(x) and a constant */
static inline SgI sgx_i_bits_of(SgCtx* g, SgD bits, int is_signed) {
    SgXInt* n;
    SgI i;
    unsigned w = bits.w;
    if (w > 64) sg_fail("an integer of bits past 64", w, is_signed);
    i = sgx_int(is_signed ? sg_d_sext(g, bits, 64) : sg_d_zext(g, bits, 64), is_signed);
    n = sgx_int_new(sgx(g), SGX_IN_BITS, 0, 0, (int)w + !is_signed, !is_signed);
    n->dw[0] = i.v;
    n->made = 1;
    i.uint = !is_signed;
    i.node = n->id;
    return i;
}
static inline SgI sg_i_uint(SgCtx* g, SgD x) { return sgx_i_bits_of(g, x, 0); }
static inline SgI sg_i_sint(SgCtx* g, SgD x) { return sgx_i_bits_of(g, x, 1); }
static inline SgI sg_i_const(SgCtx* g, long long v) {
    SgI i = sgx_int(sgx_const(64, (SnxU64)v), 1);
    SnxU64 m = v < 0 ? ~(SnxU64)v : (SnxU64)v;
    int bits = 1;
    SgXInt* n;
    for (; m; m >>= 1) ++bits;
    n = sgx_int_new(sgx(g), SGX_IN_BITS, 0, 0, bits, v >= 0);
    n->dw[0] = i.v;
    n->made = 1;
    i.node = n->id;
    return i;
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
    if (i->node) {
        SgXInt* n = sgx_int_at(x, i->node);
        if (!(n->made & 1)) n->dw[0] = i->v, n->made |= 1;
    }
    return i->v;
}
/* the sum (op SGX_IN_ADD) or the difference of two */
static inline SgI sgx_i_sum(SgCtx* g, int op, SgI a, SgI b) {
    SgX* x = sgx(g);
    SgD av = sgx_i_val(g, &a), bv = sgx_i_val(g, &b);
    SgI r = sgx_int(op == SGX_IN_SUB ? sg_d_sub(g, av, bv) : sg_d_add(g, av, bv), a.is_signed);
    unsigned na = sgx_i_node(g, &a), nb = sgx_i_node(g, &b);
    int ba = sgx_i_bits(g, &a), bb = sgx_i_bits(g, &b);
    SgXInt* n = sgx_int_new(x, op, na, nb, (ba > bb ? ba : bb) + 1,
                            op == SGX_IN_ADD && sgx_i_pos(g, &a) && sgx_i_pos(g, &b));
    n->dw[0] = r.v;
    n->made = 1;
    r.node = n->id;
    return r;
}
static inline SgI sg_i_add(SgCtx* g, SgI a, SgI b) { return sgx_i_sum(g, SGX_IN_ADD, a, b); }
static inline SgI sg_i_sub(SgCtx* g, SgI a, SgI b) { return sgx_i_sum(g, SGX_IN_SUB, a, b); }
static inline SgI sg_i_neg(SgCtx* g, SgI a) { return sg_i_sub(g, sg_i_const(g, 0), a); }
static inline SgI sg_i_mul(SgCtx* g, SgI a, SgI b) {
    SgI r;
    unsigned na = sgx_i_node(g, &a), nb = sgx_i_node(g, &b);
    int pos = sgx_i_pos(g, &a) && sgx_i_pos(g, &b);
    memset(&r, 0, sizeof r);
    r.kind = SGX_I_MUL;
    r.a = sgx_i_val(g, &a);
    r.b = sgx_i_val(g, &b);
    r.is_signed = a.is_signed || b.is_signed;
    r.w = 64;
    r.node = sgx_int_new(sgx(g), SGX_IN_MUL, na, nb, sgx_i_bits(g, &a) + sgx_i_bits(g, &b) - pos, pos)->id;
    return r;
}
/* a << k and a >> k (a floor), k known when it recompiles (sg_i_shl, sg_i_shr) or an integer from 0 to 255 known when
   the guest runs (sg_i_shl_dyn, sg_i_shr_dyn). A shift left by one of 64 or more is by 64: the value's low 64 bits are
   0 either way, and unless it is 0 it is past what 64 bits hold, which is all a slice of 64 bits or a saturation to
   them asks of it (an amount below 0, which the Sail doesn't shift by, gives any value) */
static inline SgI sgx_i_shift(SgCtx* g, int op, SgI a, long long k, SgI* amount) {
    unsigned na = sgx_i_node(g, &a), id;
    int ba = sgx_i_bits(g, &a), pos = sgx_i_pos(g, &a), left = op == SGX_IN_SHL || op == SGX_IN_SHLV;
    SgXInt* n;
    SgI r;
    (void)sgx_i_val(g, &a); /* its low doubleword, made (a product's) */
    if (!amount && (k < 0 || k > 255)) sg_fail("an integer shifted by an amount out of its range", k, left);
    if (left) ba += amount ? 64 : (int)k;
    else if (!amount)
        ba = ba - (int)k > pos + 1 ? ba - (int)k : pos + 1;
    n = sgx_int_new(sgx(g), op, na, 0, ba, pos);
    id = n->id;
    n->k = k;
    if (amount) {
        n->n = sgx_i_val(g, amount);
        sgx_flat(sgx(g), &n->n);
    }
    r = sgx_int(sgx_int_dw(g, id, 0), a.is_signed);
    r.node = id;
    return r;
}
static inline SgI sg_i_shl(SgCtx* g, SgI a, long long k) { return sgx_i_shift(g, SGX_IN_SHL, a, k, NULL); }
static inline SgI sg_i_shr(SgCtx* g, SgI a, long long k) { return sgx_i_shift(g, SGX_IN_SHR, a, k, NULL); }
static inline SgI sg_i_shl_dyn(SgCtx* g, SgI a, SgI k) { return sgx_i_shift(g, SGX_IN_SHLV, a, 0, &k); }
static inline SgI sg_i_shr_dyn(SgCtx* g, SgI a, SgI k) { return sgx_i_shift(g, SGX_IN_SHRV, a, 0, &k); }

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
    a.node = 0;
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
    sgx_flat(sgx(g), &a);

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
    sgx_flat(sgx(g), &a);

    SgX* x = sgx(g);
    unsigned w = a.w;
    if (w < 32) {
        /* an element's (CLZ (vector)): of it zero extended to 32 bits, less the 32 - w zeros above it */
        SgI r = sg_i_clz(g, sg_d_zext(g, a, 32));
        return sgx_int(sg_d_sub(g, r.v, sgx_const(64, 32 - w)), 0);
    }
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
    return ((fsel->flag_neg >> f) & 1 ? sgx_flag_ncc[j] : sgx_flag_cc[j]) | SGX_CC_EXTRA(fsel);
}
/* Arm's flag f of a compare whose flags are bools of its own (sgx_cold_cmp's fb), 0 or 1 in a register (32 bits) */
static inline SgD sgx_cmp_flag_bits(SgX* x, int ins, int f) {
    SgD r = sgx_bool_bits(x, x->ins[ins].fb[f][0]);
    if (x->ins[ins].fb[f][1]) {
        SgD o = sgx_bool_bits(x, x->ins[ins].fb[f][1]);
        r = SGX_OP(x, and, 32, sgx_val(32, r.v), sgx_val(32, o.v)); /* the flags it ends: the others' SETcc first */
    }
    r.w = 1;
    return r;
}

/* the machine's part, before an instruction makes flags it keeps for later (AddWithCarry's): what it would emit
   between them and their reader that changes the flags, made first (AArch32's condition's skip) */
static inline void sgx_flags_begin(SgCtx* g);
static inline void sg_add_with_carry(SgCtx* g, SgD a, SgD b, SgD cin, SgD* result, SgD* nzcv) {
    sgx_flags_begin(g);
    sgx_flat(sgx(g), &a);
    sgx_flat(sgx(g), &b);
    sgx_flat(sgx(g), &cin);

    SgX* x = sgx(g);
    unsigned w = a.w;
    const SgXSel *s, *alt;
    SgD in[2];
    int r;
    if (w != 32 && w != 64) sg_fail("AddWithCarry of a width not 32 or 64", w, 0);
    if (!sgx_is_const(&cin)) {
        /* a carry in: ADCS's or SBCS's form (b NOT of an operand: SBB), whose CF must be the carry, or its negation
           (carry_neg: SBB's); CF made from the carry's bit by the flags of ADD #-1 (Arm's C of it is the carry) or of
           CMP #1 (its C is the carry too, and the selection gives it as NOT CF), whichever gives the polarity */
        const SgXSel *s2, *setter = NULL;
        SgD o = b, c32 = sg_d_zext(g, cin, 32), si[2];
        int use_sub = b.inv, from, by_cmp = 0;
        if (use_sub) o.inv = 0;
        s2 = use_sub ? sgx_pick(x, SGX_TABLE(sbcs, w), SGX_TABLE_N(sbcs, w), 0, w, 0)
                     : sgx_pick(x, SGX_TABLE(adcs, w), SGX_TABLE_N(adcs, w), 0, w, 0);
        if (!s2) sg_fail("no ADC or SBB form", w, 0);
        {
            const SgXSel* add1 = sgx_pick(x, sgx_sel_adds_i_32, sgx_sel_adds_i_32_n, 0xFFFFFFFFu, 32, 0);
            const SgXSel* cmp1 = sgx_pick(x, sgx_sel_cmp_i_32, sgx_sel_cmp_i_32_n, 1, 32, 0);
            /* each's map of Arm's C: CF (flag 0), negated or not */
            if (add1 && add1->flag_of[2] == SGX_CARRY && ((add1->flag_neg >> 2) & 1) == s2->carry_neg) setter = add1;
            else if (cmp1 && cmp1->flag_of[2] == SGX_CARRY && ((cmp1->flag_neg >> 2) & 1) == s2->carry_neg)
                setter = cmp1, by_cmp = 1;
        }
        if (!setter) sg_fail("no form sets CF from a carry", w, 0);
        si[0] = c32;
        si[1] = sgx_const(32, by_cmp ? 1 : 0xFFFFFFFFu);
        sgx_emit(x, setter, si, 2, 32, 0, -1);
        from = x->n - 1;
        in[0] = a;
        in[1] = use_sub ? o : b;
        sgx_reg(x, &in[0]);
        sgx_reg(x, &in[1]);
        if (x->flags_ins != from) sg_fail("the carry's flags overwritten before ADC", from, 0);
        r = sgx_emit(x, s2, in, 2, w, 0, from);
        *result = sgx_val(w, r);
        *nzcv = sgx_nzcv(x, x->n - 1);
        return;
    }
    in[0] = a;
    if (sgx_is_const(&b)) {
        /* an immediate: a + k, or a - k where b is NOT k and the carry in 1 (ADD's and SUB's imm8 and imm32 forms with
           their flags; CMP's where nothing reads the result) */
        SnxU64 m = sg_ones(w), k = cin.k ? ~b.k & m : b.k & m;
        if (sgx_imm_ok(x, cin.k ? SGX_IMM_SUBS : SGX_IMM_ADDS, k, w)) {
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
    sgx_flat(sgx(g), &r);

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
/* whether nothing between instructions d and i may change the state's 16 bytes at disp: no store of them, no call,
   no store at all of guest memory (it is not the state, but nothing here proves an address of it is not) */
static inline int sgx_state_kept(const SgX* x, int d, int i, int disp) {
    unsigned q = (unsigned)disp >> 2;
    for (int j = d + 1; j < i; ++j) {
        const SgXIns* s = &x->ins[j];
        if (s->dead) continue;
        if (s->kind == SGX_K_MACHINE || s->kind == SGX_K_COLD || s->kind == SGX_K_UNDEF || s->side) return 0;
        if (s->kind == SGX_K_STORE || s->kind == SGX_K_STORE_FLAG) {
            unsigned w0 = (unsigned)s->disp >> 2, nw = s->kind != SGX_K_STORE_FLAG && s->w == 64 ? 2 : 1;
            if (w0 < q + 4 && q < w0 + nw) return 0;
        }
        if (s->kind == SGX_K_VEC && s->mstate && s->vout == s->mplace) return 0; /* a store of a V register */
    }
    return 1;
}
/* an AVX twin's operand 2 that a load of the state or of the constant pool made, read there alone: the load is its
   memory operand ([base + disp] or [rip + k]), and no register holds it. The VEX forms take an operand at any
   address, the SSE ones only a 16-byte aligned one, which nothing makes the state's V registers */
static inline void sgx_fold_loads(SgX* x) {
    unsigned char* uses = x->vlive; /* each value's readers, 2 for more than one; sgx_optimize clears it after */
    memset(uses, 0, (size_t)x->nv + 1);
    for (int i = 0; i < x->n; ++i) {
        const SgXIns* s = &x->ins[i];
        if (s->dead) continue;
        for (int k = 0; k < SGX_INS_IN; ++k)
            if (s->in[k] > 0 && s->in[k] < x->nv && uses[s->in[k]] < 2) ++uses[s->in[k]];
    }
    for (int i = 0; i < x->n; ++i) {
        SgXIns* s = &x->ins[i];
        int v = s->in[2];
        if (s->dead || s->kind != SGX_K_VEC || !s->vfold || s->mplace >= 0 || v <= 0 || v >= x->nv || uses[v] != 1)
            continue;
        int d = x->v[v].def;
        if (d < 0 || d >= i) continue;
        SgXIns* l = &x->ins[d];
        if (l->dead || l->kind != SGX_K_VEC || l->vf != SGX_VF_LD16 || l->def != v || !(l->mstate || l->mrip)) continue;
        if (l->mstate && !sgx_state_kept(x, d, i, l->mdisp)) continue;
        s->mplace = 2;
        s->mstate = l->mstate;
        s->mrip = l->mrip;
        s->mdisp = l->mdisp;
        s->kv[0] = l->kv[0];
        s->kv[1] = l->kv[1];
        s->in[2] = 0;
        l->dead = 1;
        for (int k = 0; k < SGX_INS_IN; ++k) l->in[k] = 0;
    }
}
static inline void sgx_optimize(SgX* x) {
    unsigned char *vlive = x->vlive, *flive = x->flive; /* the region's part cleared */
    int i, k;
    sgx_fold_loads(x);
    memset(vlive, 0, (size_t)x->nv + 1);
    memset(flive, 0, (size_t)x->n + 1);
    /* a store later ones to the same place overwrite, with no load of it and no call of the runtime (which reads the
       state) between, is dropped: a flag an instruction of the block set and a later one set again, a register written
       twice. One pass backward, each 4-byte word of the state marked while a later store covers it (a load of it, a
       vector access of the state, a call or the machine's own instruction ends every mark) */
    {
        unsigned *stamp = x->stamp, now = x->now;
        SnxU64 pset = 0; /* the pinned host registers a later PSET writes */
        unsigned short dw[200];
        int ndw, e;
        if (++now == 0) {
            memset(stamp, 0, sizeof x->stamp);
            now = 1;
        }
        /* past the end: what nothing reads is as good as overwritten */
        ndw = x->end_dead || x->end_vdead ? sgx_dead_words(x->end_dead, x->end_vdead, dw) : 0;
        for (e = 0; e < ndw; ++e) stamp[dw[e]] = now;
        for (i = x->n - 1; i >= 0; --i) {
            SgXIns* s = &x->ins[i];
            unsigned w0, nw;
            if (s->dead) continue;
            if (s->kind == SGX_K_COLD && s->cstate) {
                /* FPCR and FPSR read, nothing else */
                if ((unsigned)s->disp >> 2 < SGX_STATE_WORDS) stamp[(unsigned)s->disp >> 2] = 0;
                if ((unsigned)s->mdisp >> 2 < SGX_STATE_WORDS) stamp[(unsigned)s->mdisp >> 2] = 0;
                continue;
            }
            if (s->kind == SGX_K_VEC && s->mstate && (unsigned)(s->mdisp >> 2) + 4 <= SGX_STATE_WORDS) {
                /* a V register's 16 bytes: MOVDQU to the state (a store later ones may replace) or from it */
                unsigned q = (unsigned)s->mdisp >> 2, e;
                if (s->vf == SGX_VF_ST16) {
                    if (stamp[q] == now && stamp[q + 1] == now && stamp[q + 2] == now && stamp[q + 3] == now) {
                        s->dead = 1;
                        s->side = 0;
                        for (e = 0; e < SGX_INS_IN; ++e) s->in[e] = 0;
                        continue;
                    }
                    for (e = 0; e < 4; ++e) stamp[q + e] = now;
                } else
                    for (e = 0; e < 4; ++e) stamp[q + e] = 0;
                continue;
            }
            if (s->kind == SGX_K_MACHINE && s->jdead) {
                /* a jump: a word is dead before it where it is on both ways, the jump's (its mask) and on (its mark) */
                unsigned char keep[200];
                ndw = sgx_dead_words(s->kv[1], (SnxU32)s->kv[0], dw);
                for (e = 0; e < ndw; ++e) keep[e] = stamp[dw[e]] == now;
                if (++now == 0) {
                    memset(stamp, 0, sizeof x->stamp);
                    now = 1;
                }
                for (e = 0; e < ndw; ++e)
                    if (keep[e]) stamp[dw[e]] = now;
                pset = 0;
                continue;
            }
            if (s->kind == SGX_K_UNDEF || s->kind == SGX_K_MACHINE || s->kind == SGX_K_COLD ||
                (s->kind == SGX_K_VEC && s->mstate)) {
                if (++now == 0) {
                    memset(stamp, 0, sizeof x->stamp);
                    now = 1;
                }
                if (s->kind != SGX_K_VEC) pset = 0; /* the machine's own flush the pinned registers */
                continue;
            }
            if (s->kind == SGX_K_PGET) {
                pset &= ~(1ull << (s->imm & 63));
                continue;
            }
            if (s->kind == SGX_K_PSET) {
                /* a pinned register's write a later one replaces, nothing reading it between */
                if ((pset >> (s->imm & 63)) & 1) {
                    s->dead = 1;
                    s->side = 0;
                    s->in[0] = 0;
                } else
                    pset |= 1ull << (s->imm & 63);
                continue;
            }
            if (s->kind != SGX_K_LOAD && s->kind != SGX_K_STORE && s->kind != SGX_K_STORE_FLAG) continue;
            w0 = (unsigned)s->disp >> 2;
            nw = s->kind != SGX_K_STORE_FLAG && s->w == 64 ? 2 : 1;
            if (w0 + nw > SGX_STATE_WORDS) continue;
            if (s->kind == SGX_K_LOAD) {
                stamp[w0] = 0;
                if (nw == 2) stamp[w0 + 1] = 0;
                continue;
            }
            if (stamp[w0] == now && (nw == 1 || stamp[w0 + 1] == now)) {
                s->kind = SGX_K_SEL; /* dead: what made its value is dropped too where nothing else reads it */
                s->sel = NULL;
                s->dead = 1;
                s->in[0] = 0;
                s->flags_from = -1;
                continue;
            }
            stamp[w0] = now;
            if (nw == 2) stamp[w0 + 1] = now;
        }
        x->now = now;
    }
    for (i = x->n - 1; i >= 0; --i) {
        SgXIns* s = &x->ins[i];
        int root = s->kind == SGX_K_STORE || s->kind == SGX_K_STORE_FLAG || s->kind == SGX_K_RET || s->kind == SGX_K_UNDEF ||
                   s->kind == SGX_K_MACHINE || s->side;
        int live = !s->dead && (root || (s->def && vlive[s->def]) || flive[i]);
        if (!live) {
            s->dead = 1;
            continue;
        }
        if (s->kind == SGX_K_SEL && s->def && !vlive[s->def] && s->alt) {
            s->sel = s->alt;
            s->def = 0;
        }
        if (s->kind == SGX_K_SEL && s->def && !flive[i] && s->mplace < 0 && (s->w == 32 || s->w == 64)) {
            /* an addition whose flags nothing reads: LEA, which ties no input and writes no flags */
            const SgXSel* t = s->sel;
            int w64 = s->w == 64;
            unsigned na = w64 ? sgx_sel_add_64_n : sgx_sel_add_32_n, nas = w64 ? sgx_sel_adds_64_n : sgx_sel_adds_32_n;
            unsigned nai = w64 ? sgx_sel_add_i_64_n : sgx_sel_add_i_32_n,
                     nasi = w64 ? sgx_sel_adds_i_64_n : sgx_sel_adds_i_32_n;
            const SgXSel *a0 = w64 ? sgx_sel_add_64 : sgx_sel_add_32, *a1 = w64 ? sgx_sel_adds_64 : sgx_sel_adds_32;
            const SgXSel *i0 = w64 ? sgx_sel_add_i_64 : sgx_sel_add_i_32, *i1 = w64 ? sgx_sel_adds_i_64 : sgx_sel_adds_i_32;
            if (((t >= a0 && t < a0 + na) || (t >= a1 && t < a1 + nas)) && !t->imm && s->in[0] && s->in[1]) {
                s->sel = s->fsel = w64 ? &sgx_sel_lea_64[0] : &sgx_sel_lea_32[0];
                s->alt = NULL;
            } else if (((t >= i0 && t < i0 + nai) || (t >= i1 && t < i1 + nasi)) && t->imm == 2 && s->in[0]) {
                s->sel = s->fsel = w64 ? &sgx_sel_leai_64[0] : &sgx_sel_leai_32[0];
                s->alt = NULL;
            }
        }
        for (k = 0; k < SGX_INS_IN; ++k)
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
            if (!sgx_imm_ok(x, SGX_IMM_ADD, (SnxU64)sum, s->w)) continue;
            if (!sum && s->def && (s->w == 64 || x->v[p->in[0]].zab <= s->w)) {
                /* a sum of 0: the value is the first's operand, which every later reader takes instead, where its
                   register is as zero extended as the sum's was (a 32-bit addition's result is, and its readers
                   counted on it) */
                int j, from = s->def, to = p->in[0];
                for (j = i + 1; j < x->n; ++j)
                    for (k = 0; k < SGX_INS_IN; ++k)
                        if (x->ins[j].in[k] == from) x->ins[j].in[k] = to;
                s->dead = 1;
                changed = 1;
                continue;
            }
            /* the sum by an addition's form (a subtraction's immediate was the negation) */
            {
                const SgXSel* t = sgx_pick(x, SGX_TABLE(leai, s->w), SGX_TABLE_N(leai, s->w), (SnxU64)sum, s->w, 0);
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
                           s->kind == SGX_K_UNDEF || s->kind == SGX_K_MACHINE || s->side;
                if (s->dead) continue;
                if (!(root || (s->def && vlive[s->def]) || flive[i])) {
                    s->dead = 1;
                    continue;
                }
                for (k = 0; k < SGX_INS_IN; ++k)
                    if (s->in[k]) vlive[s->in[k]] = 1;
            }
        }
    }
    for (i = 0; i < x->nv; ++i) x->v[i].last = -1;
    for (i = 0; i < x->n; ++i) {
        if (x->ins[i].dead) continue;
        for (k = 0; k < SGX_INS_IN; ++k)
            if (x->ins[i].in[k]) x->v[x->ins[i].in[k]].last = i;
    }
}

/* the assembler of a function: its bytes, its spill slots ([RSP + 8 * k], the frame the prologue makes and the
   epilogues give back, its size patched in once the function's code is done) and the machine's labels and
   relocations (x64_a64.h's) */
typedef struct SgXAsm {
    SgX* x;
    SgXBuf* b;
    int owner[SGX_NREG]; /* the virtual register a host one holds, 0 for none, -1 a copy made for one instruction */
    int slots, slots_max;
    int xowner[SGX_NXREG]; /* the value an XMM register holds: a vector's (cls 1), or a general-purpose one spilled
                             there; 0 for none, -1 a copy made for one instruction (PBLENDVB's XMM0) */
    int* next_call;       /* for each instruction, the next one at or after it that calls out (x->n: none) */
    size_t frame_at[256]; /* where an imm32 of the frame's size is */
    int nframe;
    void* machine;        /* the machine's part's own (its labels, its relocations) */
    /* a helper's call: by a relocation reloc records (an object's), or else at the address resolve gives */
    void (*reloc)(void* machine, size_t at, const char* sym, long long addend);
    SnxU64 (*resolve)(const char* sym);
    void (*pin_flush)(void* machine); /* the guest registers kept in host ones to the state (before UNDEF's call) */
    /* the pins in caller-saved registers (R10, R11, XMM8-15) to the state (store) or back, around a call; the host
       registers pins hold, which no value is given (reserved, xreserved) */
    void (*pin_cs)(void* machine, int store);
    unsigned reserved, xreserved;
    unsigned xsaved; /* the callee's vector registers the function saved itself (x64.h's SGX_XCALLEE) */
    /* the constant pool (16 bytes each) the function's RIP-relative operands read, after its code
       (sgx_pool_emit), and where each such operand's displacement is */
    SnxU64 (*pool)[2];
    int npool, cap_pool;
    struct SgXRip {
        size_t at, end;
        int idx;
        unsigned char cold; /* at and end are the cold buffer's, until sgx_cold_emit puts it after the code */
    } * rips;
    int nrips, cap_rips;
    /* the cold paths (SGX_K_COLD's calls), after the function's code and before its pool (sgx_cold_emit): their
       bytes, the jumps between them and the hot code, their calls' relocations */
    SgXBuf cold;
    struct SgXCJump {
        size_t at, to;          /* the rel32's place and its target: into the cold code from the hot, or back */
        unsigned char in_cold;  /* the rel32 is the cold code's (a jump back) */
    } * cjumps;
    int ncjumps, cap_cjumps;
    struct SgXCCall {
        size_t at;
        const char* sym;
    } * ccalls;
    int nccalls, cap_ccalls;
} SgXAsm;

/* ---- the machine's assembly (x64_asm.h's for x86-64) ----
   a register's copy; a register to or from memory at base + disp (64 bits, or 32: a 32-bit load zero extends); a
   general-purpose register to (to_x) or from a vector register, as bytes (a spill there); a flag's byte at base +
   disp, zero extended (what sgx_store_flag stored); the condition code cc on the flags of the instruction at's
   flags_from stored as the 32-bit 0 or 1 at base + disp; a vector register's copy; a cold path (SGX_K_COLD) */
static inline void sgx_mov_rr(SgXAsm* a, int dst, int src);
static inline void sgx_mov_mem(SgXAsm* a, int load, int w, int reg, int base, int disp);
static inline void sgx_spill_x(SgXAsm* a, int to_x, int xr, int gr);
static inline void sgx_load8(SgXAsm* a, int d, int base, int disp);
static inline void sgx_store_flag(SgXAsm* a, int at, int base, int disp, int cc);
static inline void sgx_xmov(SgXAsm* a, int d, int s);
static inline void sgx_assemble_cold(SgXAsm* a, int at);
/* a vector register's 16 bytes (or its low 8) stored to or loaded from base + disp (the frame's slots) */
static inline void sgx_xmem(SgXAsm* a, int store, int bytes, int xr, int base, int disp);
/* the machine's part: the instructions of its own kind (SGX_K_MACHINE, SGX_K_UNDEF, SGX_K_RET), their inputs in
   registers r[] (in[k]'s), which it may only read; the prologue and the epilogues */
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r);

static inline void sgx_spill_at(SgXAsm* a, int r, int at) {
    int v = a->owner[r], k;
    SgXV* vv = &a->x->v[v];
    if (SGX_HAS_VREGS && vv->slot < 0 && vv->xmm < 0 && a->next_call && at >= 0 && a->next_call[at] > vv->last)
        for (k = 0; k < SGX_NXREG; ++k) {
            if ((a->xreserved >> k & 1) || a->xowner[k] < 0 ||
                (a->xowner[k] > 0 && a->x->v[a->xowner[k]].last >= at))
                continue;
            if (a->xowner[k] > 0) {
                if (a->x->v[a->xowner[k]].cls) a->x->v[a->xowner[k]].reg = -1;
                else
                    a->x->v[a->xowner[k]].xmm = -1;
            }
            a->xowner[k] = v;
            vv->xmm = (signed char)k;
            sgx_spill_x(a, 1, k, r);
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
            sgx_mov_mem(a, 0, 64, r, SGX_FRAME, 8 * vv->slot);
        }
    }
    vv->reg = -1;
    a->owner[r] = 0;
}
static inline void sgx_spill(SgXAsm* a, int r) { sgx_spill_at(a, r, -1); }

/* a host register free at instruction at, none of avoid's (a mask: the registers the instruction reads or writes):
   one whose value nothing reads after, or else the one whose value is read latest, spilled */
static inline int sgx_free_reg(SgXAsm* a, int at, unsigned avoid) {
    const signed char* pool = SGX_POOL(a);
    unsigned k, n = a->x->pool && (unsigned)a->x->pool < SGX_POOL_N(a) ? (unsigned)a->x->pool : SGX_POOL_N(a);
    int best = -1;
    avoid |= a->reserved;
    for (k = 0; k < n; ++k) {
        int r = pool[k];
        if (avoid & (1u << r)) continue;
        if (!a->owner[r] || (a->owner[r] > 0 && a->x->v[a->owner[r]].last < at)) {
            if (a->owner[r] > 0) a->x->v[a->owner[r]].reg = -1;
            a->owner[r] = 0;
            return r;
        }
    }
    for (k = 0; k < n; ++k) {
        int r = pool[k];
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
    if (vv->xmm >= 0) sgx_spill_x(a, 0, vv->xmm, r);
    else
        sgx_mov_mem(a, 1, 64, r, SGX_FRAME, 8 * vv->slot);
    a->owner[r] = v;
    a->x->v[v].reg = (signed char)r;
    return r;
}

/* moves what host register r holds elsewhere, where it is still read after at */
static inline void sgx_evict(SgXAsm* a, int r, int at, unsigned avoid) {
    int v = a->owner[r];
    if (v > 0 && a->x->v[v].last > at) {
        /* a free register of the pool, else its slot (spilled from r, where the instruction may still read it) */
        const signed char* pool = SGX_POOL(a);
        unsigned k, n = a->x->pool && (unsigned)a->x->pool < SGX_POOL_N(a) ? (unsigned)a->x->pool : SGX_POOL_N(a);
        int to = -1;
        avoid |= a->reserved; /* R10 and R11 when they hold pins, which the value would overwrite */
        for (k = 0; k < n && to < 0; ++k) {
            int c = pool[k];
            if (((avoid | (1u << r)) >> c) & 1) continue;
            if (!a->owner[c] || (a->owner[c] > 0 && a->x->v[a->owner[c]].last < at)) {
                if (a->owner[c] > 0) a->x->v[a->owner[c]].reg = -1;
                a->owner[c] = 0;
                to = c;
            }
        }
        if (to < 0) {
            sgx_spill_at(a, r, at);
            a->owner[r] = 0;
            return;
        }
        sgx_mov_rr(a, to, r);
        a->x->v[v].reg = (signed char)to;
        a->owner[to] = v;
    }
    a->owner[r] = 0;
}

/* a caller-saved register none of held's, for a call's argument of no fixed register */
static inline int sgx_arg_free(SgXAsm* a, unsigned held) {
    const signed char* pool = SGX_POOL(a);
    unsigned k;
    held |= a->reserved;
    for (k = 0; k < SGX_POOL_N(a); ++k)
        if (!(held & (1u << pool[k]))) return pool[k];
    sg_fail("no register for a call's argument", (long long)held, 0);
}

/* the inputs of instruction at in registers, none moved by another's reload: the mask of the registers they are in */
static inline unsigned sgx_inputs_in_regs(SgXAsm* a, int at, unsigned avoid) {
    SgXIns* i = &a->x->ins[at];
    unsigned held = 0;
    int k;
    for (k = 0; k < SGX_INS_IN; ++k)
        if (i->in[k]) held |= 1u << sgx_in_reg(a, i->in[k], at, avoid | held);
    return held;
}

/* a pinned value read last at (tv) by a tied form whose result (def) a PSET soon puts back in the same pinned register,
   through a cold check or tied forms (an XMM clear) that take it in turn, nothing else between reading the register:
   the form may write the pinned register itself, and the PSET moves nothing */
static inline int sgx_pin_inplace(SgXAsm* a, int at, int tv, int def) {
    SgX* x = a->x;
    int j, cur = def, key;
    if (!def || !x->v[tv].pinned || x->v[tv].last != at) return 0;
    key = x->v[tv].cls ? SGX_XPIN + x->v[tv].reg : x->v[tv].reg;
    for (j = at + 1; j < x->n && j <= at + 8; ++j) {
        SgXIns* s = &x->ins[j];
        int k, reads = 0;
        if (s->dead) continue;
        if (s->kind == SGX_K_PSET) return s->imm == key && s->in[0] == cur;
        if (s->kind == SGX_K_PGET || s->kind == SGX_K_MACHINE || s->kind == SGX_K_UNDEF) return 0;
        for (k = 0; k < SGX_INS_IN; ++k) reads += s->in[k] == cur;
        if (!reads) continue;
        if (x->v[cur].last != j || reads != 1) return 0;
        if (s->kind == SGX_K_COLD && s->in[0] == cur) cur = s->def;
        else if (s->kind == SGX_K_VEC && s->vtied && s->vout >= 0 && s->in[(int)s->vout] == cur)
            cur = s->def;
        else
            return 0;
    }
    return 0;
}
/* the pinned register a result (def, of class cls) goes to by a PSET soon after, through a cold check or tied forms
   that take it in turn, where nothing reads that register's old value after at: the result may be made there and
   the PSET moves nothing. -1 for none */
static inline int sgx_pin_target(SgXAsm* a, int at, int def, int cls) {
    SgX* x = a->x;
    int j, cur = def;
    if (!def) return -1;
    for (j = at + 1; j < x->n && j <= at + 8; ++j) {
        SgXIns* s = &x->ins[j];
        int k, reads = 0;
        if (s->dead) continue;
        if (s->kind == SGX_K_PSET) {
            int p = (int)(s->imm >= SGX_XPIN ? s->imm - SGX_XPIN : s->imm), o;
            if (s->in[0] != cur || (s->imm >= SGX_XPIN) != (cls != 0)) return -1;
            o = cls ? a->xowner[p] : a->owner[p];
            if (o > 0 && x->v[o].reg == p && x->v[o].last > at) return -1;
            return p;
        }
        if (s->kind == SGX_K_PGET || s->kind == SGX_K_MACHINE || s->kind == SGX_K_UNDEF) return -1;
        for (k = 0; k < SGX_INS_IN; ++k) reads += s->in[k] == cur;
        if (!reads) continue;
        if (x->v[cur].last != j || reads != 1) return -1;
        if (s->kind == SGX_K_COLD && s->in[0] == cur) cur = s->def;
        else if (s->kind == SGX_K_VEC && s->vtied && s->vout >= 0 && s->in[(int)s->vout] == cur)
            cur = s->def;
        else
            return -1;
    }
    return -1;
}
/* the register an input k of instruction at is in, or -1 */
static inline int sgx_any_input_in(SgXAsm* a, int at, int reg, int cls, int except) {
    SgXIns* i = &a->x->ins[at];
    int k;
    for (k = 0; k < SGX_INS_IN; ++k)
        if (k != except && i->in[k] && a->x->v[i->in[k]].reg == reg && a->x->v[i->in[k]].cls == cls) return 1;
    return 0;
}
static inline void sgx_assemble_sel(SgXAsm* a, int at) {
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    const SgXSel* s = i->sel;
    unsigned char r[4] = {0, 0, 0, 0};
    int k, out_reg = -1, tied_in = s->tied ? s->tied - 1 : -1;
    unsigned c, fixed = s->clobber, held;
    int zero_place = -1;
    if (s->zero) zero_place = __builtin_ctz(s->zero);
    if (zero_place >= SGX_FIX) fixed |= 1u << (zero_place - SGX_FIX);
    /* the fixed registers it reads and writes: what is moved out of one goes to none of them */
    for (k = 0; k < 4; ++k)
        if (s->in[k] >= SGX_FIX) fixed |= 1u << (s->in[k] - SGX_FIX);
    if (s->out >= SGX_FIX) fixed |= 1u << (s->out - SGX_FIX);
    held = sgx_inputs_in_regs(a, at, fixed);
    /* the inputs in their places: a fixed register's copied in first (what it held moved out of the way), then the
       others, which that may have moved */
    for (k = 0; k < 5; ++k) {
        /* in[4], the zeroed value, goes into a fixed register where the form's zeroed place is one (DIV's RDX) */
        int v = i->in[k], p = k < 4 ? s->in[k] : zero_place;
        if (!v || p < SGX_FIX) continue;
        if (x->v[v].reg != p - SGX_FIX) {
            int o = a->owner[p - SGX_FIX], j, reads = 0;
            for (j = 0; j < SGX_INS_IN && o > 0; ++j) reads |= i->in[j] == o;
            if (reads) {
                /* another input of it (DIV's divisor in RDX): to a register, which its operand names. Spilled to a
                   slot or an XMM register it would have none, and the operand would read a stray address */
                int to = sgx_free_reg(a, at, fixed | held);
                sgx_mov_rr(a, to, p - SGX_FIX);
                x->v[o].reg = (signed char)to;
                a->owner[to] = o;
                held |= 1u << to;
            } else
                sgx_evict(a, p - SGX_FIX, at - 1, fixed | held);
            sgx_mov_rr(a, p - SGX_FIX, x->v[v].reg);
            a->owner[p - SGX_FIX] = -1;
        }
    }
    for (k = 0; k < 4; ++k) {
        int v = i->in[k], p = s->in[k];
        if (v && p >= 0 && p < SGX_FIX) r[p] = (unsigned char)x->v[v].reg;
    }
    if (i->mplace >= 0) {
        /* the memory operand, which the r/m place takes */
        r[(int)i->mplace] = (unsigned char)SGX_MEMR(x->v[i->in[5]].reg);
        a->b->index = i->in[6] ? x->v[i->in[6]].reg : -1;
        a->b->disp = i->mdisp;
    }
    /* the fixed registers it writes */
    for (unsigned m = s->clobber; m; m &= m - 1)
        if ((c = (unsigned)__builtin_ctz(m)), 1) {
            int keep = 0;
            for (k = 0; k < 5; ++k)
                if (i->in[k] && x->v[i->in[k]].reg == (int)c && x->v[i->in[k]].last == at) keep = 1;
            if (!keep || a->owner[c] > 0) sgx_evict(a, (int)c, at, fixed | held);
        }
    /* the result: in its fixed register, in the register of the input (or the zeroed value) it overwrites, or free */
    if (s->out >= 0 && s->out != i->mplace) {
        int tv = tied_in >= 0 ? i->in[tied_in] : zero_place >= 0 && zero_place < SGX_FIX ? i->in[4] : 0;
        if (s->out >= SGX_FIX) {
            out_reg = s->out - SGX_FIX;
            if (a->owner[out_reg] > 0 && a->owner[out_reg] != i->def) sgx_evict(a, out_reg, at, fixed | held);
        } else if (tv) {
            int from = x->v[tv].reg,
                dies = x->v[tv].last == at && (!x->v[tv].pinned || sgx_pin_inplace(a, at, tv, i->def));
            for (k = 0; k < 4; ++k)
                if (k != tied_in && i->in[k] == tv) dies = 0;
            if (dies) {
                out_reg = from;
                a->owner[from] = 0;
                x->v[tv].reg = -1;
            } else {
                int pt = sgx_pin_target(a, at, i->def, 0);
                out_reg = pt >= 0 && !(fixed >> pt & 1) && !sgx_any_input_in(a, at, pt, 0, tied_in)
                              ? pt
                              : sgx_free_reg(a, at, fixed | held);
                sgx_mov_rr(a, out_reg, from);
            }
            r[(int)s->out] = (unsigned char)out_reg;
        } else {
            int pt = sgx_pin_target(a, at, i->def, 0);
            out_reg = pt >= 0 && !(fixed >> pt & 1) ? pt : sgx_free_reg(a, at, fixed | held);
            r[(int)s->out] = (unsigned char)out_reg;
        }
    }
    if (i->mplace >= 0) {
        /* the operand registers moved out of the way may have moved the base or the index */
        r[(int)i->mplace] = (unsigned char)SGX_MEMR(x->v[i->in[5]].reg);
        a->b->index = i->in[6] ? x->v[i->in[6]].reg : -1;
        a->b->disp = i->mdisp;
    }
    s->enc(a->b, r, i->imm, i->cc);
    a->b->index = -1;
    if (fixed) /* the copies made into fixed registers for it */
        for (c = 0; c < SGX_NREG; ++c)
            if (a->owner[c] == -1) a->owner[c] = 0;
    if (out_reg >= 0 && i->def) {
        if (a->owner[out_reg] > 0 && a->owner[out_reg] != i->def) a->x->v[a->owner[out_reg]].reg = -1;
        a->owner[out_reg] = i->def;
        x->v[i->def].reg = (signed char)out_reg;
    }
}

/* ---- vector registers: vectors' values and spilled general-purpose ones ---- */

/* 16 bytes of the frame (two 8-byte slots; MOVDQU needs no alignment) */
static inline int sgx_slot2(SgXAsm* a) {
    int k = a->slots;
    a->slots += 2;
    if (a->slots > a->slots_max) a->slots_max = a->slots;
    return k;
}
static inline void sgx_xrelease(SgXAsm* a, int k) {
    int o = a->xowner[k];
    if (o > 0) {
        if (a->x->v[o].cls) a->x->v[o].reg = -1;
        else
            a->x->v[o].xmm = -1;
    }
    a->xowner[k] = 0;
}
/* what vector register k holds, kept where it is read after at: a vector in its 16 bytes of the frame, a spilled
   general-purpose value in its 8 (once: an SSA value never changes) */
static inline void sgx_xspill(SgXAsm* a, int k, int at) {
    int o = a->xowner[k];
    if (o > 0 && a->x->v[o].last > at && a->x->v[o].slot < 0) {
        SgXV* vv = &a->x->v[o];
        if (vv->cls) {
            vv->slot = (short)sgx_slot2(a);
            sgx_xmem(a, 1, 16, k, SGX_FRAME, 8 * vv->slot);
        } else {
            vv->slot = (short)a->slots++;
            if (a->slots > a->slots_max) a->slots_max = a->slots;
            sgx_xmem(a, 1, 8, k, SGX_FRAME, 8 * vv->slot);
        }
    }
    sgx_xrelease(a, k);
}
/* a vector register free at at, none of avoid's: one holding nothing read after, or else the one whose value is read
   latest, spilled */
static inline int sgx_xfree(SgXAsm* a, int at, unsigned avoid) {
    int k, best = -1;
    avoid |= a->xreserved;
    for (k = 0; k < SGX_NXREG; ++k) {
        int o = a->xowner[k];
        if ((avoid >> k) & 1) continue;
        if (!o || (o > 0 && a->x->v[o].last < at)) {
            sgx_xrelease(a, k);
            return k;
        }
    }
    for (k = 0; k < SGX_NXREG; ++k)
        if (!((avoid >> k) & 1) && a->xowner[k] > 0 &&
            (best < 0 || a->x->v[a->xowner[k]].last > a->x->v[a->xowner[best]].last))
            best = k;
    if (best < 0) sg_fail("no vector register to spill", at, (long long)avoid);
    sgx_xspill(a, best, at);
    return best;
}
/* vector value v in a vector register (reloaded from its slot where it was spilled), not one of avoid's */
static inline int sgx_xin(SgXAsm* a, int v, int at, unsigned avoid) {
    SgXV* vv = &a->x->v[v];
    int k;
    if (vv->reg >= 0) return vv->reg;
    if (vv->slot < 0) sg_fail("a vector in neither a register nor a slot", v, at);
    k = sgx_xfree(a, at, avoid);
    sgx_xmem(a, 0, 16, k, SGX_FRAME, 8 * a->x->v[v].slot);
    a->xowner[k] = v;
    a->x->v[v].reg = (signed char)k;
    return k;
}
/* the constant pool's entry of these 16 bytes */
static inline int sgx_pool_index(SgXAsm* a, SnxU64 lo, SnxU64 hi) {
    int k;
    for (k = 0; k < a->npool; ++k)
        if (a->pool[k][0] == lo && a->pool[k][1] == hi) return k;
    if (a->npool == a->cap_pool) {
        a->cap_pool = a->cap_pool ? 2 * a->cap_pool : 16;
        a->pool = (SnxU64(*)[2])realloc(a->pool, (size_t)a->cap_pool * sizeof *a->pool);
        if (!a->pool) sg_fail("out of memory", 0, 0);
    }
    a->pool[a->npool][0] = lo;
    a->pool[a->npool][1] = hi;
    return a->npool++;
}
/* a pool operand's displacement at at, its instruction ending at end, in the buffer the assembler writes now */
static inline void sgx_rip_add(SgXAsm* a, size_t at, size_t end, SnxU64 lo, SnxU64 hi) {
    if (a->nrips == a->cap_rips) {
        a->cap_rips = a->cap_rips ? 2 * a->cap_rips : 32;
        a->rips = (struct SgXRip*)realloc(a->rips, (size_t)a->cap_rips * sizeof *a->rips);
        if (!a->rips) sg_fail("out of memory", 0, 0);
    }
    a->rips[a->nrips].at = at;
    a->rips[a->nrips].end = end;
    a->rips[a->nrips].cold = a->b == &a->cold;
    a->rips[a->nrips++].idx = sgx_pool_index(a, lo, hi);
}
/* a vector form (SGX_K_VEC): its operands in registers (a tied result copied from its input unless that dies here),
   a memory operand the state's (SGX_MEMR of the base), the constant pool's (SGX_POOLR, its displacement recorded for
   sgx_pool_emit to patch) or guest memory's (SGX_MEMR of in[5], b's index in[6] and disp) */
static inline void sgx_assemble_vec(SgXAsm* a, int at) {
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    const SgXVForm* f = i->vf;
    SgXBuf* b = a->b;
    unsigned char r[4] = {0, 0, 0, 0};
    unsigned heldg = 0, heldx = 0;
    int k, out = i->vout, outreg = -1, implicit = -1, base = -1, index = -1, isx = 0;
    for (k = 0; k < f->nops; ++k)
        if (SGX_IMPLICIT_X0 && k != f->reg && k != f->rm && k != f->vvvv && ((f->xmask >> k) & 1))
            implicit = k; /* x86's <XMM0>: a vector operand no field encodes */
    if (implicit >= 0 && i->in[implicit]) {
        int from = sgx_xin(a, i->in[implicit], at, 0);
        if (from != 0) {
            int o = a->xowner[0];
            if (o > 0 && x->v[o].last >= at) { /* another operand of it, where it is last read here */
                int to = sgx_xfree(a, at, 1u | (1u << from));
                sgx_xmov(a, to, 0);
                a->xowner[0] = 0;
                if (x->v[o].cls) x->v[o].reg = (signed char)to;
                else
                    x->v[o].xmm = (signed char)to;
                a->xowner[to] = o;
            } else
                sgx_xrelease(a, 0);
            sgx_xmov(a, 0, from);
            a->xowner[0] = -1;
        }
        heldx |= 1;
    }
    for (k = 0; k < f->nops; ++k) {
        int v = i->in[k], rr;
        if (!v || k == implicit || k == i->mplace) continue;
        if ((f->xmask >> k) & 1) {
            rr = sgx_xin(a, v, at, heldx);
            heldx |= 1u << rr;
        } else {
            rr = sgx_in_reg(a, v, at, heldg);
            heldg |= 1u << rr;
        }
        r[k] = (unsigned char)rr;
    }
    if (i->mplace >= 0 && !i->mstate && !i->mrip) {
        base = sgx_in_reg(a, i->in[5], at, heldg);
        heldg |= 1u << base;
        if (i->in[6]) {
            index = sgx_in_reg(a, i->in[6], at, heldg);
            heldg |= 1u << index;
        }
    }
    if (out >= 0 && out != i->mplace) {
        isx = (f->xmask >> out) & 1;
        if (i->vtied) {
            int tv = i->in[out], from = r[out],
                dies = x->v[tv].last == at && (!x->v[tv].pinned || sgx_pin_inplace(a, at, tv, i->def));
            for (k = 0; k < f->nops; ++k)
                if (k != out && i->in[k] == tv) dies = 0;
            if (dies) {
                outreg = from;
                if (isx) {
                    a->xowner[from] = 0;
                    x->v[tv].reg = -1;
                } else {
                    a->owner[from] = 0;
                    x->v[tv].reg = -1;
                }
            } else if (isx) {
                int pt = sgx_pin_target(a, at, i->def, 1);
                outreg = pt >= 0 && !sgx_any_input_in(a, at, pt, 1, out) ? pt : sgx_xfree(a, at, heldx | (1u << from));
                sgx_xmov(a, outreg, from);
            } else {
                int pt = sgx_pin_target(a, at, i->def, 0);
                outreg = pt >= 0 && !sgx_any_input_in(a, at, pt, 0, out) ? pt : sgx_free_reg(a, at, heldg | (1u << from));
                sgx_mov_rr(a, outreg, from);
            }
        } else {
            int pt = sgx_pin_target(a, at, i->def, isx);
            outreg = pt >= 0 && !(i->vzero && sgx_any_input_in(a, at, pt, isx, -1)) ? pt
                     : isx                                                   ? sgx_xfree(a, at, heldx)
                                                                             : sgx_free_reg(a, at, heldg);
        }
        r[out] = (unsigned char)outreg;
        if (i->vzero && f->rm >= 0) r[(int)f->rm] = (unsigned char)outreg; /* x86's PXOR of itself */
    }
    if (i->mplace >= 0) {
        if (i->mstate) r[(int)i->mplace] = (unsigned char)SGX_MEMR(x->base);
        else if (i->mrip)
            r[(int)i->mplace] = SGX_POOLR;
        else
            r[(int)i->mplace] = (unsigned char)SGX_MEMR(base);
        b->index = index;
        b->disp = i->mrip ? 0 : i->mdisp;
    }
    f->enc(b, r, i->imm, 0);
    b->index = -1;
    if (i->mrip) sgx_rip_add(a, b->rip_at, b->n, i->kv[0], i->kv[1]);
    for (k = 0; k < SGX_NREG; ++k)
        if (a->owner[k] == -1) a->owner[k] = 0;
    for (k = 0; k < SGX_NXREG; ++k)
        if (a->xowner[k] == -1) a->xowner[k] = 0;
    if (outreg >= 0 && i->def) {
        if (isx) {
            if (a->xowner[outreg] > 0 && a->xowner[outreg] != i->def) sgx_xrelease(a, outreg);
            a->xowner[outreg] = i->def;
        } else {
            if (a->owner[outreg] > 0 && a->owner[outreg] != i->def) a->x->v[a->owner[outreg]].reg = -1;
            a->owner[outreg] = i->def;
        }
        x->v[i->def].reg = (signed char)outreg;
    }
}

/* one region of code (the instructions in x since the last region): dropped what nothing reads, its values given
   registers, its bytes written after what a's buffer holds. No value lives from one region into the next */
static inline void sgx_assemble_region(SgXAsm* a, SgX* x) {
    int i, k;
    SgXBuf* b = a->b;
    a->x = x;
    if (x->xpool) a->xreserved |= ~0u << x->xpool;
    a->xreserved |= SGX_XCALLEE(a);
    memset(a->owner, 0, sizeof a->owner);
    memset(a->xowner, 0, sizeof a->xowner);
    a->slots = SGX_SLOT0(a);
    for (i = 0; i < x->nv; ++i) {
        x->v[i].reg = -1;
        x->v[i].slot = -1;
        x->v[i].xmm = -1;
        x->v[i].pinned = 0;
    }
    sgx_optimize(x);
    a->next_call = x->next_call;
    a->next_call[x->n] = x->n;
    for (i = x->n - 1; i >= 0; --i)
        a->next_call[i] = !x->ins[i].dead && (x->ins[i].calls || x->ins[i].kind == SGX_K_MACHINE) ? i : a->next_call[i + 1];
    for (i = 0; i < x->n; ++i) {
        SgXIns* s = &x->ins[i];
        unsigned char r[SGX_MACHINE_IN] = {0};
        if (s->dead) continue;
        switch (s->kind) {
        case SGX_K_SEL: sgx_assemble_sel(a, i); break;
        case SGX_K_VEC: sgx_assemble_vec(a, i); break;
        case SGX_K_COLD: sgx_assemble_cold(a, i); break;
        case SGX_K_PGET:
            /* the value is the pinned register's own, no copy (until a PSET of it, which moves it if still read); an
               vector register's for imm SGX_XPIN and up */
            if (s->imm >= SGX_XPIN) a->xowner[s->imm - SGX_XPIN] = s->def;
            else
                a->owner[s->imm] = s->def;
            x->v[s->def].reg = (signed char)(s->imm >= SGX_XPIN ? s->imm - SGX_XPIN : s->imm);
            x->v[s->def].pinned = 1;
            break;
        case SGX_K_PSET:
            if (s->imm >= SGX_XPIN) {
                int p = (int)(s->imm - SGX_XPIN), o = a->xowner[p], src;
                if (o > 0 && o != s->in[0] && x->v[o].pinned && x->v[o].reg == p && x->v[o].last > i) {
                    int d = sgx_xfree(a, i, 1u << p);
                    sgx_xmov(a, d, p);
                    a->xowner[d] = o;
                    x->v[o].reg = (signed char)d;
                    x->v[o].pinned = 0;
                }
                src = sgx_xin(a, s->in[0], i, 0);
                if (src != p) sgx_xmov(a, p, src), a->xowner[p] = 0;
                else /* made in place: the value is the register's, as a PGET's */
                    x->v[s->in[0]].pinned = 1;
                break;
            }
            {
            int p = (int)s->imm, o = a->owner[p], src;
            if (o > 0 && o != s->in[0] && x->v[o].pinned && x->v[o].reg == p && x->v[o].last > i) {
                int d = sgx_free_reg(a, i, 1u << p);
                sgx_mov_rr(a, d, p);
                a->owner[d] = o;
                x->v[o].reg = (signed char)d;
                x->v[o].pinned = 0;
            }
            src = sgx_in_reg(a, s->in[0], i, 0);
            if (src != p) sgx_mov_rr(a, p, src), a->owner[p] = 0;
            else
                x->v[s->in[0]].pinned = 1;
            }
            break;
        case SGX_K_LOAD: {
            int d = sgx_free_reg(a, i, 0);
            if (s->w == 8) sgx_load8(a, d, x->base, s->disp);
 else
                sgx_mov_mem(a, 1, s->w == 64 ? 64 : 32, d, x->base, s->disp);
            a->owner[d] = s->def;
            x->v[s->def].reg = (signed char)d;
            break;
        }
        case SGX_K_STORE:
            sgx_mov_mem(a, 0, s->w == 64 ? 64 : 32, sgx_in_reg(a, s->in[0], i, 0), x->base, s->disp);
            break;
        case SGX_K_STORE_FLAG: {
            sgx_store_flag(a, i, x->base, s->disp, s->cc);
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
            if (s->calls) {
                /* a call: everything in a register spilled to its home (the call changes the caller-saved ones, and
                   its arguments' registers may hold one another's values), then each argument loaded from its home
                   straight into its register: no move has to wait for another */
                for (k = 0; k < SGX_NREG; ++k)
                    if (a->owner[k] > 0) sgx_spill(a, k);
                for (k = 0; k < SGX_NXREG; ++k) /* every vector register is caller-saved: what is read after, in its slot */
                    sgx_xspill(a, k, i);
                for (k = 0; k < SGX_MACHINE_IN; ++k)
                    if (s->in[k]) {
                        SgXV* vv = &x->v[s->in[k]];
                        int stack = s->fixed[k] >= SGX_NREG; /* on the stack: through a register, put there */
                        int fr = s->fixed[k] >= 0 && !stack ? s->fixed[k] : sgx_arg_free(a, held);
                        if (vv->xmm >= 0) sgx_spill_x(a, 0, vv->xmm, fr);
                        else
                            sgx_mov_mem(a, 1, 64, fr, SGX_FRAME, 8 * vv->slot);
                        if (stack) {
                            sgx_mov_mem(a, 0, 64, fr, SGX_FRAME, 8 * (s->fixed[k] - SGX_NREG));
                            continue;
                        }
                        held |= 1u << fr;
                        r[k] = (unsigned char)fr;
                    }
                sgx_machine_ins(a, i, r);
                for (k = 0; k < SGX_NREG; ++k) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
                if (s->def) {
                    a->owner[SGX_RET] = s->def;
                    x->v[s->def].reg = SGX_RET;
                }
                if (s->def2) {
                    a->owner[SGX_RET2] = s->def2;
                    x->v[s->def2].reg = SGX_RET2;
                }
                break;
            }
            for (k = 0; k < SGX_MACHINE_IN; ++k)
                if (s->in[k] && s->fixed[k] >= 0) {
                    int fr = s->fixed[k];
                    int v = s->in[k], from = sgx_in_reg(a, v, i, held | (1u << fr));
                    if (from != fr) {
                        if (a->owner[fr] > 0) sgx_evict(a, fr, i - 1, held | (1u << fr) | (1u << from));
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
                for (k = 0; k < SGX_NREG; ++k)
                    if ((SGX_CALLER_SAVED & (1u << k)) && a->owner[k] > 0 && x->v[a->owner[k]].last > i) sgx_spill(a, k);
            /* the registers it uses besides (and its results' SGX_RET and SGX_RET2): what they hold that is read after,
               moved out of the way */
            {
                unsigned clob = s->mclobber | (s->def ? 1u << SGX_RET : 0) | (s->def2 ? 1u << SGX_RET2 : 0);
                for (k = 0; k < SGX_NREG; ++k)
                    if (((clob >> k) & 1) && a->owner[k] > 0) {
                        /* read after: copied elsewhere (an input there is still there for the instruction to read) */
                        if (x->v[a->owner[k]].last > i) sgx_evict(a, k, i, held | clob);
                        else if (!((held >> k) & 1)) {
                            x->v[a->owner[k]].reg = -1;
                            a->owner[k] = 0;
                        }
                    }
            }
            sgx_machine_ins(a, i, r);
            for (k = 0; k < SGX_NREG; ++k)
                if ((s->mclobber >> k) & 1) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
            for (k = 0; k < SGX_NREG; ++k)
                if (a->owner[k] == -1 || (s->calls && (SGX_CALLER_SAVED & (1u << k)))) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
            if (s->def) {
                /* its result is in SGX_RET, a second in SGX_RET2 */
                a->owner[SGX_RET] = s->def;
                x->v[s->def].reg = SGX_RET;
            }
            if (s->def2) {
                a->owner[SGX_RET2] = s->def2;
                x->v[s->def2].reg = SGX_RET2;
            }
            break;
        }
        }
    }
    a->next_call = NULL;
    x->n = 0;
    x->nv = 1;
    x->nb = 1;
    x->flags_ins = -1;
    x->unmade = 0;
}

/* NZCV of an instruction's flags as 4 bits (N the top), where something reads it as bits (CCMP's choice between it
   and its immediate): each flag by SETcc on them, or 0 where the form gives it as a constant 0 (ANDS's C and V) */
static inline SgD sgx_nzcv_bits(SgX* x, int ins) {
    const SgXSel* fs = x->ins[ins].fsel;
    SgD r = sgx_const(32, 0);
    int f;
    if (!fs) {
        /* a compare's (sgx_cold_cmp's): its flags' bools, each made 0 or 1 once */
        for (f = 0; f < 4; ++f) {
            SgD b = sgx_cmp_flag_bits(x, ins, f);
            b.w = 32;
            if (f < 3) b = SGX_OP(x, lsl_i, 32, b, sgx_const(5, (SnxU64)(3 - f)));
            r = sgx_is_const(&r) ? b : SGX_OP(x, or, 32, r, b);
        }
        r.w = 4;
        return r;
    }
    if (x->flags_ins != ins) sg_fail("NZCV read as bits after the x86 flags that are them were overwritten", ins, 0);
    {
        /* every SETcc first, from the flags as they are, then the shifts and ORs */
        int bits[4];
        for (f = 0; f < 4; ++f) {
            int cc = sgx_arm_flag_cc(fs, f);
            bits[f] = 0;
            if (cc >= 0) {
                int z = sgx_movi(x, 0);
                SgXIns* s = sgx_append(x, SGX_K_SETCC);
                s->cc = (unsigned char)cc;
                s->flags_from = ins;
                s->in[0] = z;
                s->def = sgx_vreg(x, x->n - 1, 1);
                bits[f] = s->def;
            }
        }
        x->flags_ins = -1;
        for (f = 0; f < 4; ++f)
            if (bits[f]) {
                SgD b = sgx_val(32, bits[f]);
                if (f < 3) b = SGX_OP(x, lsl_i, 32, b, sgx_const(5, (SnxU64)(3 - f)));
                r = sgx_is_const(&r) ? b : SGX_OP(x, or, 32, r, b);
            }
    }
    r.w = 4;
    return r;
}

/* CountLeadingSignBits(x): CLZ(x<N-1:1> EOR x<N-2:0>) of N - 1 bits, which is CLZ of ((x EOR (x << 1)) OR 1) of N */
static inline SgI sg_i_cls(SgCtx* g, SgD a) {
    sgx_flat(sgx(g), &a);

    SgD t = sg_d_or(g, sg_d_xor(g, a, sg_d_lsl(g, a, 1)), sgx_const(a.w, 1));
    return sg_i_clz(g, t);
}

/* BitReverse: BSWAP, then the bits of each byte swapped in three steps of masks and shifts (nibbles, pairs, bits) */
static inline SgD sg_d_bitreverse(SgCtx* g, SgD a) {
    sgx_flat(sgx(g), &a);

    SgX* x = sgx(g);
    unsigned w = a.w, k;
    static const SnxU64 masks[3] = {0x0F0F0F0F0F0F0F0Full, 0x3333333333333333ull, 0x5555555555555555ull};
    if (w < 32) {
        /* a byte's or a halfword's (CRC32's operand, reversed in the Sail): the 32-bit reversal's top bits */
        SgD r = sg_d_lsr(g, sg_d_bitreverse(g, sg_d_zext(g, a, 32)), 32 - w);
        r.w = w;
        return r;
    }
    if (w != 32 && w != 64) sg_fail("RBIT of a value not of 32 or 64 bits", w, 0);
    a = SGX_OP(x, rev, w, a);
    for (k = 0; k < 3; ++k) {
        unsigned sh = 4u >> k;
        SgD m = sgx_const(w, masks[k]);
        SgD lo = sg_d_lsl(g, sg_d_and(g, a, m), sh), hi = sg_d_and(g, sg_d_lsr(g, a, sh), m);
        a = sg_d_or(g, lo, hi);
    }
    return a;
}

/* RoundTowardsZero(Real(a) / Real(b)) modulo 2^64: DIV or IDIV of 64 bits (a 32-bit one's operands came extended),
   the divisor 1 where it is 0 (the Sail's select makes its 0) or -1 under the most negative value (whose quotient,
   the dividend's negation, is what 1 gives modulo 2^64): the udiv and sdiv primitives' ASSUME */
static inline SgI sg_i_div(SgCtx* g, SgI a, SgI b) {
    SgX* x = sgx(g);
    int s = a.is_signed;
    SgD av = sgx_i_val(g, &a), bv = sgx_i_val(g, &b), d, q;
    SgB avoid;
    if ((sgx_is_const(&bv) && bv.k && !(s && bv.k == ~0ull)) || SGX_DIV_ARM) d = bv;
    else {
        avoid = sg_d_is_zero(g, bv);
        if (s) avoid = sg_b_or(g, avoid, sg_b_and(g, sg_d_eq(g, av, sgx_const(64, 1ull << 63)), sg_d_eq(g, bv, sgx_const(64, ~0ull))));
        d = sg_d_select(g, avoid, sgx_const(64, 1), bv);
    }
    if (sgx_is_const(&av) && sgx_is_const(&d)) {
        /* Arm's: 0 for a divisor of 0 (SGX_DIV_ARM's d may be), the dividend for the most negative over -1 */
        SnxU64 r = !d.k ? 0
                   : s && d.k == ~0ull && av.k == 1ull << 63 ? av.k
                   : s ? (SnxU64)((long long)av.k / (long long)d.k) : av.k / d.k;
        return sgx_int(sgx_const(64, r), s);
    }
#if SGX_DIV_ARM
    /* the machine's division is Arm's own: 0 for a divisor of 0, the dividend for the most negative over -1 */
    (void)d;
    q = s ? SGX_OP(x, sdiv, 64, av, bv) : SGX_OP(x, udiv, 64, av, bv);
#elif SGX_DIV_2OP
    q = s ? SGX_OP(x, sdiv, 64, av, d) : SGX_OP(x, udiv, 64, av, d);
#else
    q = s ? SGX_OP(x, sdiv, 64, av, sg_d_asr(g, av, 63), d) : SGX_OP(x, udiv, 64, av, d);
#endif
    return sgx_int(q, s);
}

/* c ? a : b of integers */
static inline SgI sg_i_select(SgCtx* g, SgB c, SgI a, SgI b) {
    int s = a.is_signed || b.is_signed;
    SgX* x = sgx(g);
    SgD av = sgx_i_val(g, &a), bv = sgx_i_val(g, &b);
    unsigned na, nb;
    SgXInt* n;
    SgI r;
    if (x->b[c].kind == SGX_B_CONST) return x->b[c].v ? a : b;
    r = sgx_int(sg_d_select(g, c, av, bv), s);
    na = sgx_i_node(g, &a), nb = sgx_i_node(g, &b);
    n = sgx_int_new(x, SGX_IN_SELECT, na, nb, 0, sgx_i_pos(g, &a) && sgx_i_pos(g, &b));
    {
        int ba = sgx_i_bits(g, &a), bb = sgx_i_bits(g, &b);
        n->bits = (short)(ba > bb ? ba : bb);
    }
    n->c = c;
    n->dw[0] = r.v;
    n->made = 1;
    r.node = n->id;
    return r;
}

/* the sum of x's elements of size bits, modulo 2^size (gen.c's halves, ADDV's Reduce, on a machine whose back end has
   no lanes: the elements added one after another) */
static inline SgD sg_d_reduce_add(SgCtx* g, SgD x, unsigned size) {
    SgD sum = sg_d_slice(g, x, size - 1, 0);
    unsigned k;
    if (!size || x.w % size) sg_fail("a sum of elements that don't divide their bits", x.w, size);
    for (k = size; k < x.w; k += size) {
        SgD e = sg_d_slice(g, x, k + size - 1, k);
        unsigned pw = sgx_width(size);
        sum = SGX_OP(sgx(g), add, pw, sg_d_zext(g, sum, pw), sg_d_zext(g, e, pw));
        sum.w = size;
    }
    return sum;
}

/* ---- AArch32's builtins (core.h's, for LLVM) ---- */

/* IsZeroBit: '1' where x is zero */
static inline SgD sg_d_is_zero_bit(SgCtx* g, SgD x) { return sgx_bool_bits(sgx(g), sg_d_is_zero(g, x)); }

/* a op b of 64-bit values, signed or not */
static inline SgB sgx_d_cmp64(SgCtx* g, int op, SgD av, SgD bv, int uns) {
    static const int scond[6] = {SGX_ARM_EQ, SGX_ARM_NE, SGX_ARM_LT, SGX_ARM_LE, SGX_ARM_GT, SGX_ARM_GE};
    static const int ucond[6] = {SGX_ARM_EQ, SGX_ARM_NE, SGX_ARM_CC, SGX_ARM_LS, SGX_ARM_HI, SGX_ARM_CS};
    const int* cond = uns ? ucond : scond;
    static const int mirror[6] = {0, 1, 4, 5, 2, 3}; /* a op b is b mirror[op] a */
    SgX* x = sgx(g);
    sgx_flat(x, &av);
    sgx_flat(x, &bv);
    if (sgx_is_const(&av) && sgx_is_const(&bv)) {
        long long p = (long long)av.k, q = (long long)bv.k;
        SnxU64 up = av.k, uq = bv.k;
        int less = uns ? up < uq : p < q;
        int more = !less && p != q;
        int r = op <= 1 ? (p == q) != op : op == 2 ? less : op == 3 ? !more : op == 4 ? more : !less;
        return sgx_bool(x, SGX_B_CONST, r, 0);
    }
    if (sgx_is_const(&av)) {
        SgD t = av;
        av = bv;
        bv = t;
        op = mirror[op];
    }
    SgD in[2];
    const SgXSel* s;
    in[0] = sgx_zext(x, av, 64);
    if (sgx_is_const(&bv) && sgx_imm_ok(x, SGX_IMM_CMP, bv.k, 64)) {
        in[1] = sgx_const(32, bv.k);
        s = sgx_pick(x, sgx_sel_cmp_i_64, sgx_sel_cmp_i_64_n, bv.k, 64, 0);
    } else {
        in[1] = sgx_zext(x, bv, 64);
        sgx_reg(x, &in[1]);
        s = sgx_pick(x, sgx_sel_cmp_64, sgx_sel_cmp_64_n, 0, 64, 0);
    }
    if (!s) sg_fail("no CMP form", op, 0);
    sgx_emit(x, s, in, 2, 64, 0, -1);
    return sgx_bool(x, SGX_B_CC, x->flags_ins, s->cc_of[cond[op]]);
}
/* a == b, !=, <, <=, > and >= (op 0 to 5) of integers known only at run time: their values modulo 2^64 compared as
   signed, where their bits fit (UInt and SInt of 32 bits at most, and their sums), or as unsigned where both are
   UInt's (or one a constant from 0 to 2^63 - 1: CMHS's elements of 64 bits) */
static inline SgB sg_i_cmp(SgCtx* g, int op, SgI a, SgI b) {
    int uns;
    if (op < 0 || op > 5) sg_fail("a comparison of integers it does not know", op, 5);
    SgD av = sgx_i_val(g, &a), bv = sgx_i_val(g, &b);
    uns = (a.uint || (sgx_is_const(&av) && (long long)av.k >= 0)) &&
          (b.uint || (sgx_is_const(&bv) && (long long)bv.k >= 0)) && (a.uint || b.uint) &&
          sgx_i_bits(g, &a) <= 65 && sgx_i_bits(g, &b) <= 65;
    if (!uns && (sgx_i_bits(g, &a) > 64 || sgx_i_bits(g, &b) > 64))
        sg_fail("a comparison of integers past 64 bits", sgx_i_bits(g, &a), sgx_i_bits(g, &b));
    return sgx_d_cmp64(g, op, av, bv, uns);
}
/* Min(a, b), Max(a, b) and Abs(a) */
static inline SgI sg_i_minmax(SgCtx* g, SgI a, SgI b, int minimum) {
    SgB c = sg_i_cmp(g, minimum ? 3 : 5, a, b);
    return sg_i_select(g, c, a, b);
}
static inline SgI sg_i_abs(SgCtx* g, SgI a) {
    SgI n, r;
    if (sgx_i_pos(g, &a)) return a;
    n = sg_i_neg(g, a);
    r = sg_i_select(g, sg_i_cmp(g, 2, a, sg_i_const(g, 0)), n, a);
    if (r.node) sgx_int_at(sgx(g), r.node)->pos = 1;
    return r;
}
/* SatQ(i, N, is_unsigned): i's low N bits where i is the integer they are, else the limit nearer to it, and whether it
   isn't: its doublewords against those bits' extension, and the limit by its sign */
static inline void sg_SatQ(SgCtx* g, SgI i, long long n, int is_unsigned, SgD* result, SgB* saturated) {
    unsigned id = sgx_i_node(g, &i), k;
    int bits = sgx_i_bits(g, &i), pos = sgx_i_pos(g, &i) != 0;
    SgD low = sg_i_slice(g, i, n - 1, 0), ext, diff, sign, limit;
    is_unsigned = is_unsigned != 0;
    const SgXInt* node = sgx_int_at(sgx(g), id);
    unsigned ndw = sgx_int_ndw(node);
    if (n < 1 || n > 64) sg_fail("a saturation to these bits", n, is_unsigned);
    if (is_unsigned ? pos && bits - 1 <= n : bits <= n) { /* it always is */
        *result = low;
        *saturated = sg_bool(g, 0);
        return;
    }
    ext = is_unsigned ? sg_d_zext(g, low, 64) : sg_d_sext(g, low, 64);
    diff = sg_d_xor(g, sgx_int_dw(g, id, 0), ext);
    sign = is_unsigned ? sgx_const(64, 0) : sg_d_asr(g, ext, 63);
    /* its sign too, past its doublewords, where the limits' kind is another's: an unsigned one's of an integer that may
       be negative, a signed one's of one that never is */
    for (k = 1; k < ndw + (is_unsigned != pos); ++k) diff = sg_d_or(g, diff, sg_d_xor(g, sgx_int_dw(g, id, k), sign));
    /* the limit: 0 or all ones for an unsigned one, the smallest or the largest, by the sign of i's top doubleword */
    if (pos) limit = sgx_const(64, sg_ones((unsigned)(is_unsigned ? n : n - 1)));
    else
        limit = sg_d_xor(g, sg_d_asr(g, sgx_int_dw(g, id, ndw - 1), 63),
                         sgx_const(64, is_unsigned ? ~0ull : sg_ones((unsigned)n - 1)));
    *saturated = sg_b_not(g, sg_d_is_zero(g, diff));
    *result = sg_d_slice(g, sg_d_select(g, *saturated, limit, sg_d_zext(g, low, 64)), n - 1, 0);
}
static inline void sg_SignedSatQ(SgCtx* g, SgI i, long long n, SgD* result, SgB* saturated) {
    sg_SatQ(g, i, n, 0, result, saturated);
}
static inline void sg_UnsignedSatQ(SgCtx* g, SgI i, long long n, SgD* result, SgB* saturated) {
    sg_SatQ(g, i, n, 1, result, saturated);
}
/* BitCount(x): the bits of each two, of each four, of each byte, then all the bytes' */
static inline SgI sg_i_bitcount(SgCtx* g, SgD x) {
    unsigned w = x.w;
    SgD v;
    if (w > 64) sg_fail("BitCount of over 64 bits", w, 0);
    v = sg_d_zext(g, x, 64);
    v = sg_d_sub(g, v, sg_d_and(g, sg_d_lsr(g, v, 1), sgx_const(64, 0x5555555555555555ull)));
    v = sg_d_add(g, sg_d_and(g, v, sgx_const(64, 0x3333333333333333ull)),
                 sg_d_and(g, sg_d_lsr(g, v, 2), sgx_const(64, 0x3333333333333333ull)));
    v = sg_d_and(g, sg_d_add(g, v, sg_d_lsr(g, v, 4)), sgx_const(64, 0x0F0F0F0F0F0F0F0Full));
    if (w > 8) v = sg_d_lsr(g, SGX_OP(sgx(g), mul, 64, v, sgx_const(64, 0x0101010101010101ull)), 56);
    return sg_i_uint(g, sg_d_slice(g, v, 6, 0));
}
/* Elem_read(x, e, size), e known only when the guest runs: the doubleword it is in, picked by each bit of its number,
   shifted down (an e past x's elements, which the Sail doesn't read, gives any bits) */
static inline SgD sg_d_elem_dyn(SgCtx* g, SgD a, SgI e, long long size) {
    SgD dw[SGX_MAXDW], at = sgx_i_val(g, &e);
    unsigned n = sgx_ndw(a.w), k, j, bit = 0, log = 0;
    if (size != 8 && size != 16 && size != 32 && size != 64) sg_fail("an element of this size", size, 0);
    if (a.w > 64 * SGX_MAXDW || a.w % 64) sg_fail("an element of a value of these bits", a.w, size);
    while ((8 << log) < size) ++log;
    at = sg_d_lsl(g, at, 3 + log); /* the element's first bit */
    sgx_dws(g, a, dw);
    for (bit = 0; n > 1; ++bit, n = (n + 1) / 2) {
        SgB odd = sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, at, sgx_const(64, 64ull << bit))));
        for (k = j = 0; k < n; k += 2, ++j) dw[j] = k + 1 < n ? sg_d_select(g, odd, dw[k + 1], dw[k]) : dw[k];
    }
    return sg_d_slice(g, sg_d_shift_dyn(g, dw[0], at, 1), size - 1, 0);
}

/* LSL_C, LSR_C, ASR_C and ROR_C (kind 0 to 3) by an amount known when it recompiles: the shifted value and the last bit
   shifted out (AArch32's shifter carry) */
static inline void sg_d_shift_c(SgCtx* g, SgD x, long long n, int kind, SgD* result, SgD* carry) {
    long long w = (long long)x.w;
    if (n <= 0 || !w || w > 64) sg_fail("a shift with carry by an amount out of its range", n, kind);
    switch (kind) {
    case 0:
        *result = n >= w ? sgx_const((unsigned)w, 0) : sg_d_lsl(g, x, n);
        *carry = n <= w ? sg_d_slice(g, x, w - n, w - n) : sgx_const(1, 0);
        return;
    case 1:
        *result = n >= w ? sgx_const((unsigned)w, 0) : sg_d_lsr(g, x, n);
        *carry = n <= w ? sg_d_slice(g, x, n - 1, n - 1) : sgx_const(1, 0);
        return;
    case 2:
        *result = sg_d_asr(g, x, n >= w ? w - 1 : n);
        *carry = sg_d_slice(g, x, n >= w ? w - 1 : n - 1, n >= w ? w - 1 : n - 1);
        return;
    default:
        *result = sg_d_ror(g, x, n % w);
        *carry = sg_d_slice(g, *result, w - 1, w - 1);
        return;
    }
}
/* the same by an amount known only at run time (AArch32's register shifts, 0 to 255), on 32 bits, by x86 shifts of 64
   bits by CL (the count masked to 6 bits) where the amount is below 64, and the zeros or the sign the Arm ARM gives past
   it: LSL as the 64-bit shift of the value, its bit 32 the carry; LSR of the value shifted left by one, its bit 0 the
   carry; ASR the same of the sign extended value by at most 63; ROR the 32-bit rotation, whose count mask is MOD 32,
   its top bit the carry. An amount of 0, where the Sail's Shift_C takes the value and the carry in, gives what it gives:
   the Sail's select drops it */
static inline void sg_d_shift_c_dyn(SgCtx* g, SgD x, SgI n, int kind, SgD* result, SgD* carry) {
    SgX* xs = sgx(g);
    SgD k = sgx_i_val(g, &n);
    if (x.w != 32) sg_fail("a shift with carry by a register of a value not of 32 bits", x.w, kind);
    sgx_flat(xs, &k);
    if (sgx_is_const(&k)) {
        if (!k.k) {
            *result = x;
            *carry = sgx_const(1, 0);
        } else
            sg_d_shift_c(g, x, k.k > 255 ? 255 : (long long)k.k, kind, result, carry);
        return;
    }
    k = sgx_zext(xs, k, 64);
    /* the compare after the shifts, whose flags would end its condition code before the CMOVs read it */
    switch (kind) {
    case 0:
    case 1: {
        SgD u = kind == 0 ? SGX_OP(xs, lslv, 64, sg_d_zext(g, x, 64), k)
                          : SGX_OP(xs, lsrv, 64, sg_d_lsl(g, sg_d_zext(g, x, 64), 1), k);
        SgD r = kind == 0 ? sg_d_slice(g, u, 31, 0) : sg_d_slice(g, u, 32, 1);
        SgD c = kind == 0 ? sg_d_slice(g, u, 32, 32) : sg_d_slice(g, u, 0, 0);
        SgB ok = sgx_d_ult(g, k, sgx_const(64, 64));
        *result = sg_d_select(g, ok, r, sgx_const(32, 0));
        *carry = sg_d_select(g, ok, c, sgx_const(1, 0));
        return;
    }
    case 2: {
        SgB ok = sgx_d_ult(g, k, sgx_const(64, 64));
        SgD kc = sg_d_select(g, ok, k, sgx_const(64, 63)), wide = sg_d_lsl(g, sg_d_sext(g, x, 64), 1);
        SgD u = SGX_OP(xs, asrv, 64, wide, kc);
        *result = sg_d_slice(g, u, 32, 1);
        *carry = sg_d_slice(g, u, 0, 0);
        return;
    }
    default:
        *result = SGX_OP(xs, rorv, 32, x, sg_d_slice(g, k, 31, 0));
        *carry = sg_d_slice(g, *result, 31, 31);
        return;
    }
}
