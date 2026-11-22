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
    int disp;  /* a memory operand's displacement and index register (-1 none), for sgx_modrm */
    int index;
    size_t rip_at; /* where sgx_modrm put a RIP-relative operand's displacement (r/m SGX_RIP) */
} SgXBuf;
/* the r/m operand that is the constant pool's (RIP-relative: mod 00, r/m 101), which the assembler patches */
#define SGX_RIP 32

static inline void sgx_byte(SgXBuf* b, unsigned char x) {
    if (b->n < b->cap) b->p[b->n] = x;
    ++b->n;
}
static inline void sgx_bytes(SgXBuf* b, const char* hex) {
    /* "F0 49 0F B1 14 32": the bytes as written */
    while (*hex) {
        unsigned v = 0, k;
        while (*hex == ' ') ++hex;
        if (!*hex) break;
        for (k = 0; k < 2; ++k, ++hex) v = v * 16 + (unsigned)(*hex <= '9' ? *hex - '0' : *hex - 'A' + 10);
        sgx_byte(b, (unsigned char)v);
    }
}
static inline void sgx_imm(SgXBuf* b, long long v, int bytes) {
    int i;
    for (i = 0; i < bytes; ++i) sgx_byte(b, (unsigned char)((unsigned long long)v >> (8 * i)));
}
/* REX.X: the index register of a memory r/m operand is R8 to R15 */
static inline unsigned sgx_rex_x(const SgXBuf* b, int rm) { return rm >= 16 && b->index >= 8 ? 2u : 0u; }

/* ModRM: a register (0 to 15, mod 11), or the memory at a register (16 + it) plus b->index (when not -1) plus
   b->disp: mod 00 without a displacement (but for RBP and R13, which mean RIP or a displacement there), 01 of 8 bits,
   10 of 32; a SIB for an index, or for RSP and R12 as the base (Vol. 2A, 2.1.5, tables 2-2 and 2-3) */
static inline void sgx_modrm(SgXBuf* b, int reg, int rm) {
    int base, mod, sib;
    if (rm == SGX_RIP) {
        sgx_byte(b, (unsigned char)((reg & 7) << 3 | 5));
        b->rip_at = b->n;
        sgx_imm(b, 0, 4);
        return;
    }
    if (rm < 16) {
        sgx_byte(b, (unsigned char)(0xC0 | (reg & 7) << 3 | (rm & 7)));
        return;
    }
    base = rm - 16;
    mod = b->disp == 0 && (base & 7) != 5 ? 0 : b->disp >= -128 && b->disp < 128 ? 1 : 2;
    sib = b->index >= 0 || (base & 7) == 4;
    sgx_byte(b, (unsigned char)(mod << 6 | (reg & 7) << 3 | (sib ? 4 : (base & 7))));
    if (sib) sgx_byte(b, (unsigned char)((b->index >= 0 ? (b->index & 7) : 4) << 3 | (base & 7)));
    if (mod == 1) sgx_byte(b, (unsigned char)b->disp);
    else if (mod == 2)
        sgx_imm(b, b->disp, 4);
}

/* the host's features a candidate may need (CPUID's), SgX.features */
enum {
    SGX_F_BMI1 = 1, SGX_F_BMI2 = 2, SGX_F_LZCNT = 4, SGX_F_SSE3 = 8, SGX_F_SSSE3 = 16, SGX_F_SSE41 = 32,
    SGX_F_SSE42 = 64, SGX_F_PCLMUL = 128, SGX_F_F16C = 256, SGX_F_POPCNT = 512, SGX_F_AVX = 1024, SGX_F_FMA = 2048
};

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

/* an SSE form an 'enc' line of x64.ops asks for (x64_isel.inc's sgx_v_NAME): its encoder, the CPUID flag it needs,
   its operands (nops), which of them ModRM.reg, ModRM.rm and VEX.vvvv encode (-1 none), which are XMM registers
   (xmask's bit k: operand k; the others general-purpose), and its immediate's bytes. Operand k's register goes in
   the encoder's r[k], 16 + a base register for the memory an r/m operand may be */
typedef struct SgXVForm {
    void (*enc)(SgXBuf* b, const unsigned char* r, long long imm, int cc);
    unsigned feature;
    signed char nops, reg, rm, vvvv;
    unsigned char xmask, imm_bytes;
    const struct SgXVForm* vex; /* a two-operand SSE form's AVX twin (VPADDD for PADDD: untied, the first source in
                                   VEX.vvvv), which sailgen --isel found in the manual's page; NULL for none */
} SgXVForm;

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
    SGX_K_MACHINE,    /* the machine's own (x64_a64.h's: labels, jumps, calls), op its kind */
    SGX_K_VEC,        /* an SSE form (vf): its operand k is in[k] (none for the result's unless vtied, or the memory's) */
    SGX_K_COLD,       /* def = cc on flags_from's flags ? sym(state, in[1..w]) : in[0], the call out of the hot code */
    SGX_K_PGET,       /* def = the host register imm (a guest register the function keeps there: x64_codegen.c) */
    SGX_K_PSET        /* the host register imm = in[0] */
};

typedef struct {
    unsigned char kind, cc, w, dead;
    unsigned char op; /* SGX_K_MACHINE's kind of instruction */
    const SgXSel* sel;
    int def;
    int def2;  /* a call's second result, in RDX */
    int in[7]; /* the primitive's inputs (0-3), the zeroed value (4), a memory operand's base and index (5, 6) */
    long long imm;
    int disp;
    signed char mplace; /* the form's operand that is the memory [in[5] + in[6] + mdisp], -1 for none */
    int mdisp;
    const char* sym;    /* SGX_K_MACHINE's call of a helper: its name */
    int flags_from; /* the instruction whose flags a SETCC, STORE_FLAG or cc form reads */
    const SgXSel* fsel; /* whose map of Arm's flags its flags follow (sel's, or the ANDS candidate of its AND) */
    const SgXSel* alt;  /* the same flags without the result (CMP for SUBS), for a result nothing reads */
    signed char fixed[5]; /* the machine's instructions: the host register in[k] must be in, -1 any */
    unsigned char side;   /* a selected form that changes memory (a guest store): kept though nothing reads it */
    unsigned char calls;  /* the machine's: it calls out (the caller-saved registers are spilled around it) */
    unsigned char cadd; /* an addition of the constant cadd_k to in[0] (its flags or not): what sgx_optimize folds
                           into the next such addition where nothing reads the flags of either */
    long long cadd_k;
    /* SGX_K_VEC: the form; its result's operand (-1 none: a store, a compare), which reads in[vout] first when vtied;
       vzero: the result is made 0 first (PXOR of itself), no input; the memory operand (mplace) is the state's
       [base + mdisp] (mstate), the constant pool's kv (mrip), or guest memory [in[5] + in[6] + mdisp]; vflags: it
       writes the flags (UCOMISS, PTEST) */
    unsigned short mclobber; /* SGX_K_MACHINE's: the host registers it changes besides its result's (RAX, RDX) */
    const SgXVForm* vf;
    signed char vout;
    unsigned char vtied, vzero, mstate, mrip, vflags;
    SnxU64 kv[2];
    /* SGX_K_COLD of sgx_cold_fp: its call reads and writes the state's 32-bit fields at disp (FPCR) and mdisp (FPSR)
       alone (cstate); argument cfpcr - 1 is the field at disp, ckat - 1 the constant imm, both loaded on the cold path
       alone; cw32: the arguments (bits 0-4) and the result (7) of 32 bits in XMM registers; zacc: sgx_cold_fp's */
    unsigned char cstate, cfpcr, ckat, cw32, zacc;
    unsigned char jdead; /* SGX_K_MACHINE: a jump, kv[1] and kv[0] what is dead where it goes (sgx_dead_words') */
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

#define SGX_MAXI 4096
#define SGX_STATE_WORDS 1024 /* the state's 4-byte words dead-store elimination tracks (CPU's first 4 KiB) */
/* the guest's (x64_a64.h's): the state's words masks of dead registers and flags (dead) and V registers (vdead) name,
   at most 200 of them, into words[]; their count */
static inline int sgx_dead_words(SnxU64 dead, SnxU32 vdead, unsigned short* words);
#define SGX_MAXV 8192
#define SGX_MAXB 4096

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
    SnxU64 end_dead; /* the guest's state nothing reads past the region's end (sgx_dead_words' mask), for the store
                        pass; 0 for none known */
    SnxU32 end_vdead; /* and its V registers */
} SgX;

typedef int SgB;

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
static _Thread_local int sgx_vcse_seen[16];
static _Thread_local unsigned sgx_vcse_next;
static inline int sgx_vcse_find(SgX* x, const SgXVForm* f, int out, int tied, const int* in, long long imm) {
    int k, j;
    for (k = 0; k < 16; ++k) {
        int v = sgx_vcse_seen[k], d;
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
        /* the AVX twin: a op= b is c = a op b, no copy of a where a is read after */
        int vin[4] = {0, in[0], in[1], 0};
        return sgx_vins(x, f->vex, 0, 0, vin, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    if (mode == SGX_VM_NONE && out >= 0 && tied >= 0 && in) {
        int v = sgx_vcse_find(x, f, out, tied, in, imm);
        if (v) return v;
    }
    if (f->feature & ~x->features & ~(unsigned)(SGX_F_BMI1 | SGX_F_BMI2 | SGX_F_LZCNT))
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
    if (mode == SGX_VM_NONE && tied >= 0 && in) sgx_vcse_seen[sgx_vcse_next++ & 15] = i->def;
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
    static _Thread_local int seen[8];
    static _Thread_local unsigned next;
    int k, v;
    if (!lo && !hi) return sgx_vins(x, &sgx_v_pxor, 0, -1, NULL, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    for (k = 0; k < 8; ++k) {
        int d;
        v = seen[k];
        if (v <= 0 || v >= x->nv) continue;
        d = x->v[v].def;
        if (d >= 0 && d < x->n && x->ins[d].kind == SGX_K_VEC && x->ins[d].mrip && x->ins[d].def == v &&
            x->ins[d].kv[0] == lo && x->ins[d].kv[1] == hi)
            return v;
    }
    v = sgx_vins(x, &sgx_v_movdqu_ld, 0, 0, NULL, 0, 1, SGX_VM_POOL, 0, 0, 0, lo, hi);
    seen[next++ & 7] = v;
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

/* LEA for the additions nothing reads the flags of (add, add_i): three operands, no flags written, so an address's
   arithmetic leaves the flags a branch reads where they are. The SDM's LEA computes EffectiveAddress(SRC), which its
   Operation section doesn't spell out, so these forms are written here rather than selected (the recompiler's tests
   check them as they check the derived ones): operand 0 the result, 1 the base, 2 the index; a 32-bit one is the
   64-bit sum's low half, zero extended (no address-size prefix) */
static inline void sgx_enc_lea(SgXBuf* b, const unsigned char* r, long long imm, int w64, int with_index) {
    int d = r[0], base = r[1], index = with_index ? r[2] : -1;
    if (index == 4) { /* RSP can't be an index */
        index = base;
        base = 4;
    }
    sgx_byte(b, (unsigned char)(0x40 | (w64 ? 8 : 0) | (d >> 3) << 2 | (index > 0 ? (index >> 3) << 1 : 0) | (base >> 3)));
    sgx_byte(b, 0x8D);
    if (index >= 0) {
        int mod = imm ? 2 : (base & 7) == 5 ? 1 : 0;
        sgx_byte(b, (unsigned char)(mod << 6 | (d & 7) << 3 | 4));
        sgx_byte(b, (unsigned char)((index & 7) << 3 | (base & 7)));
        if (mod == 1) sgx_byte(b, 0);
        else if (mod == 2)
            sgx_imm(b, imm, 4);
    } else {
        sgx_byte(b, (unsigned char)(0x80 | (d & 7) << 3 | (base & 7)));
        if ((base & 7) == 4) sgx_byte(b, 0x24);
        sgx_imm(b, imm, 4);
    }
}
static void sgx_enc_lea_rr64(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)imm, (void)cc, sgx_enc_lea(b, r, 0, 1, 1); }
static void sgx_enc_lea_rr32(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)imm, (void)cc, sgx_enc_lea(b, r, 0, 0, 1); }
static void sgx_enc_lea_ri64(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)cc, sgx_enc_lea(b, r, imm, 1, 0); }
static void sgx_enc_lea_ri32(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)cc, sgx_enc_lea(b, r, imm, 0, 0); }
#define SGX_LEA_SEL(e, im) {.enc = e, .rm = -1, .in = {1, (im) ? -1 : 2, -1, -1}, .imm = (im) ? 2 : 0, .fit_bits = 0, \
                            .fit_kind = '-', .out = 0, .clean_out = 1, .flag_of = {-1, -1, -1, -1},                   \
                            .cc_of = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}}
static const SgXSel sgx_sel_lea_64[1] = {SGX_LEA_SEL(sgx_enc_lea_rr64, 0)};
static const SgXSel sgx_sel_lea_32[1] = {SGX_LEA_SEL(sgx_enc_lea_rr32, 0)};
static const SgXSel sgx_sel_leai_64[1] = {SGX_LEA_SEL(sgx_enc_lea_ri64, 1)};
static const SgXSel sgx_sel_leai_32[1] = {SGX_LEA_SEL(sgx_enc_lea_ri32, 1)};
#define sgx_sel_lea_64_n 1u
#define sgx_sel_lea_32_n 1u
#define sgx_sel_leai_64_n 1u
#define sgx_sel_leai_32_n 1u

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
        x->v[v].g0 = sgx_vins(x, w <= 32 ? &sgx_v_movd_rx : &sgx_v_movq_rx, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        x->v[x->v[v].g0].zab = (unsigned char)(w <= 32 ? 32 : 64);
    } else if (w > 32 && x->v[x->v[v].g0].zab == 32) {
        int in[4] = {0, v, 0, 0};
        x->v[v].g0 = sgx_vins(x, &sgx_v_movq_rx, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    memset(d, 0, sizeof *d);
    d->w = w;
    d->v = x->v[v].g0;
    if (w > 64) {
        if (!x->v[v].g1) {
            int in[4] = {0, v, 0, 0};
            x->v[v].g1 = sgx_vins(x, &sgx_v_pextrq, 0, 0, in, 1, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
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
                r = sgx_vins(x, &sgx_v_movq_xr, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
                if (!sgx_is_const(&hi) || hi.k) {
                    in[0] = r;
                    in[1] = sgx_reg(x, &hi);
                    r = sgx_vins(x, &sgx_v_pinsrq, 0, 1, in, 1, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
                }
            }
        }
    } else if (sgx_is_const(d))
        r = sgx_xconst(x, d->k, 0);
    else {
        int in[4] = {0, sgx_reg(x, d), 0, 0};
        r = sgx_vins(x, d->w <= 32 ? &sgx_v_movd_xr : &sgx_v_movq_xr, 0, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    *d = sgx_xval(d->w, r);
    return r;
}
/* an XMM value's bits w and up, to 128, made 0: MOVQ for a doubleword, INSERTPS for a word, byte shifts below */
static inline int sgx_xclear(SgX* x, int v, unsigned w) {
    if (w >= 128) return v;
    if (w == 64) return sgx_v1(x, &sgx_v_movq_ld, v, 0);
    if (w == 32 && (x->features & SGX_F_SSE41)) {
        int in[4] = {v, v, 0, 0};
        return sgx_vins(x, &sgx_v_insertps, 0, 1, in, 0x0E, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    if (w % 8 || w > 64) sg_fail("an XMM value cleared above a width not of bytes", w, 0);
    {
        int in[4] = {v, 0, 0, 0}, t;
        t = sgx_vins(x, &sgx_v_pslldq, 0, 1, in, 16 - w / 8, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        in[0] = t;
        return sgx_vins(x, &sgx_v_psrldq, 0, 1, in, 16 - w / 8, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
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
    if (a.x && a.w <= 64 && w > 64) return sgx_xval((unsigned)w, sgx_xclear(sgx(g), a.x, a.w));
    if (a.x && a.w > 64) {
        a.w = (unsigned)w;
        return a;
    }
    sgx_flat(sgx(g), &a);
    if (a.w == (unsigned)w) return a;
    if (a.w > 64) {
        /* past 64 bits already: its high part's bits above it are 0, as a register's or a constant's are */
        if (a.wide || a.inv) sg_fail("an extension of a value past 64 bits this back end doesn't make", a.w, w);
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

static inline SgD sg_d_sext(SgCtx* g, SgD a, long long w) {
    sgx_flat(sgx(g), &a);

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
    if (a.x) {
        /* an XMM value's low bits are themselves (of a float's or a vector's width: others go to general-purpose
           registers); an aligned element of 32 or 64 bits PSHUFD's to the bottom */
        if (lo == 0 && (w == 16 || w == 32 || w == 64 || w == 128)) {
            a.w = w;
            return a;
        }
        if ((lo == 64 && hi <= 127) || (lo % 32 == 0 && hi < lo + 32 && hi <= 127)) {
            unsigned e = (unsigned)(lo / 32), imm = lo == 64 ? 0xEE : e * 0x55;
            return sgx_xval(w, sgx_v1(x, &sgx_v_pshufd, a.x, imm));
        }
        sgx_flat(x, &a);
    }
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
    if (!l.w) return h; /* x : Zeros(0), a shift by 0 */
    if (!h.w) return l;
    if ((h.x || l.x) && w == 128 && h.w == 64) {
        /* two doublewords in an XMM register: PUNPCKLQDQ (the low's high bits are the high's low ones) */
        int lx = sgx_xreg(x, &l), hx = sgx_xreg(x, &h);
        return sgx_xval(128, sgx_v2(x, &sgx_v_punpcklqdq, lx, hx, 0));
    }
    sgx_flat(x, &h);
    sgx_flat(x, &l);
    if (w > 64) {
        SgD r;
        if (l.w != 64) {
            memset(&r, 0, sizeof r);
            r.w = w;
            r.wide = 1;
            return r;
        }
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
    if (a.w > 64) /* each doubleword's own */
        return sg_d_concat(g, sgx_logic(g, op, sgx_half(a, 1), sgx_half(b, 1)),
                           sgx_logic(g, op, sgx_half(a, 0), sgx_half(b, 0)));
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
        if (sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, k, pw)) {
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
            static _Thread_local int seen[8];
            static _Thread_local unsigned next;
            int k;
            for (k = 0; k < 8; ++k) {
                int v = seen[k], d;
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
                seen[next++ & 7] = r.v;
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
   hot code) */
static inline SgD sgx_cold_fp(SgX* x, SgB c, SgD tied, const char* sym, SgD* args, int nargs, int fpcr_at, int k_at,
                              long long k, int fpcr, int fpsr, unsigned zacc, SnxU32 zmask) {
    int regs[5], j, from, cc, t, cls, n = 0, w32 = 0;
    SgXIns* i;
    SgXBool* b = &x->b[c];
    if (!c || (b->kind == SGX_B_CONST && !b->v)) return tied;
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
    if (b->kind == SGX_B_CONST) {
        SgD one = sgx_const(32, 1);
        c = sgx_bool(x, SGX_B_REG, sgx_reg(x, &one), 0);
    }
    cc = sgx_bool_cc(x, c, &from);
    i = sgx_append(x, SGX_K_COLD);
    i->cc = (unsigned char)cc;
    i->flags_from = from;
    i->sym = sym;
    i->w = (unsigned char)j;
    i->in[0] = t;
    for (n = 0; n < j; ++n) i->in[1 + n] = regs[n];
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
    if (sgx_is_const(&b) && sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, b.k, pw)) {
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
            if (add1 && add1->flag_of[2] == 0 && ((add1->flag_neg >> 2) & 1) == s2->carry_neg) setter = add1;
            else if (cmp1 && cmp1->flag_of[2] == 0 && ((cmp1->flag_neg >> 2) & 1) == s2->carry_neg)
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
static inline void sgx_optimize(SgX* x) {
    /* the thread's scratch, the region's part cleared (no allocation a region) */
    static _Thread_local unsigned char vlive[SGX_MAXV + 1], flive[SGX_MAXI + 1];
    int i, k;
    memset(vlive, 0, (size_t)x->nv + 1);
    memset(flive, 0, (size_t)x->n + 1);
    /* a store later ones to the same place overwrite, with no load of it and no call of the runtime (which reads the
       state) between, is dropped: a flag an instruction of the block set and a later one set again, a register written
       twice. One pass backward, each 4-byte word of the state marked while a later store covers it (a load of it, a
       vector access of the state, a call or the machine's own instruction ends every mark) */
    {
        static _Thread_local unsigned stamp[SGX_STATE_WORDS];
        static _Thread_local unsigned now = 1;
        unsigned pset = 0; /* the pinned host registers a later PSET writes */
        unsigned short dw[200];
        int ndw, e;
        if (++now == 0) {
            memset(stamp, 0, sizeof stamp);
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
                if (s->vf == &sgx_v_movdqu_st) {
                    if (stamp[q] == now && stamp[q + 1] == now && stamp[q + 2] == now && stamp[q + 3] == now) {
                        s->dead = 1;
                        s->side = 0;
                        for (e = 0; e < 7; ++e) s->in[e] = 0;
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
                    memset(stamp, 0, sizeof stamp);
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
                    memset(stamp, 0, sizeof stamp);
                    now = 1;
                }
                if (s->kind != SGX_K_VEC) pset = 0; /* the machine's own flush the pinned registers */
                continue;
            }
            if (s->kind == SGX_K_PGET) {
                pset &= ~(1u << (s->imm & 31));
                continue;
            }
            if (s->kind == SGX_K_PSET) {
                /* a pinned register's write a later one replaces, nothing reading it between */
                if ((pset >> (s->imm & 31)) & 1) {
                    s->dead = 1;
                    s->side = 0;
                    s->in[0] = 0;
                } else
                    pset |= 1u << (s->imm & 31);
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
        for (k = 0; k < 7; ++k)
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
                    for (k = 0; k < 7; ++k)
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
                for (k = 0; k < 7; ++k)
                    if (s->in[k]) vlive[s->in[k]] = 1;
            }
        }
    }
    for (i = 0; i < x->nv; ++i) x->v[i].last = -1;
    for (i = 0; i < x->n; ++i) {
        if (x->ins[i].dead) continue;
        for (k = 0; k < 7; ++k)
            if (x->ins[i].in[k]) x->v[x->ins[i].in[k]].last = i;
    }
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
    int xowner[16];       /* the value an XMM register holds: a vector's (cls 1), or a general-purpose one spilled
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
    /* the constant pool (16 bytes each) the function's RIP-relative operands read, after its code
       (sgx_pool_emit), and where each such operand's displacement is */
    SnxU64 (*pool)[2];
    int npool, cap_pool;
    struct SgXRip {
        size_t at, end;
        int idx;
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
    a->b->index = -1;
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
    avoid |= a->reserved;
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
        /* a free register of the pool, else its slot (spilled from r, where the instruction may still read it) */
        unsigned k, n = a->x->pool && (unsigned)a->x->pool < sizeof sgx_pool ? (unsigned)a->x->pool : sizeof sgx_pool;
        int to = -1;
        avoid |= a->reserved; /* R10 and R11 when they hold pins, which the value would overwrite */
        for (k = 0; k < n && to < 0; ++k) {
            int c = sgx_pool[k];
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
    unsigned k;
    held |= a->reserved;
    for (k = 0; k < sizeof sgx_pool; ++k)
        if (!(held & (1u << sgx_pool[k]))) return sgx_pool[k];
    sg_fail("no register for a call's argument", (long long)held, 0);
}

/* the inputs of instruction at in registers, none moved by another's reload: the mask of the registers they are in */
static inline unsigned sgx_inputs_in_regs(SgXAsm* a, int at, unsigned avoid) {
    SgXIns* i = &a->x->ins[at];
    unsigned held = 0;
    int k;
    for (k = 0; k < 7; ++k)
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
    key = x->v[tv].cls ? 16 + x->v[tv].reg : x->v[tv].reg;
    for (j = at + 1; j < x->n && j <= at + 8; ++j) {
        SgXIns* s = &x->ins[j];
        int k, reads = 0;
        if (s->dead) continue;
        if (s->kind == SGX_K_PSET) return s->imm == key && s->in[0] == cur;
        if (s->kind == SGX_K_PGET || s->kind == SGX_K_MACHINE || s->kind == SGX_K_UNDEF) return 0;
        for (k = 0; k < 7; ++k) reads += s->in[k] == cur;
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
            int p = (int)(s->imm & 15), o;
            if (s->in[0] != cur || (s->imm >= 16) != (cls != 0)) return -1;
            o = cls ? a->xowner[p] : a->owner[p];
            if (o > 0 && x->v[o].reg == p && x->v[o].last > at) return -1;
            return p;
        }
        if (s->kind == SGX_K_PGET || s->kind == SGX_K_MACHINE || s->kind == SGX_K_UNDEF) return -1;
        for (k = 0; k < 7; ++k) reads += s->in[k] == cur;
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
    for (k = 0; k < 7; ++k)
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
    if (zero_place >= 16) fixed |= 1u << (zero_place - 16);
    /* the fixed registers it reads and writes: what is moved out of one goes to none of them */
    for (k = 0; k < 4; ++k)
        if (s->in[k] >= 16) fixed |= 1u << (s->in[k] - 16);
    if (s->out >= 16) fixed |= 1u << (s->out - 16);
    held = sgx_inputs_in_regs(a, at, fixed);
    /* the inputs in their places: a fixed register's copied in first (what it held moved out of the way), then the
       others, which that may have moved */
    for (k = 0; k < 5; ++k) {
        /* in[4], the zeroed value, goes into a fixed register where the form's zeroed place is one (DIV's RDX) */
        int v = i->in[k], p = k < 4 ? s->in[k] : zero_place;
        if (!v || p < 16) continue;
        if (x->v[v].reg != p - 16) {
            int o = a->owner[p - 16], j, reads = 0;
            for (j = 0; j < 7 && o > 0; ++j) reads |= i->in[j] == o;
            if (reads) {
                /* another input of it (DIV's divisor in RDX): to a register, which its operand names. Spilled to a
                   slot or an XMM register it would have none, and the operand would read a stray address */
                int to = sgx_free_reg(a, at, fixed | held);
                sgx_mov_rr(a, to, p - 16);
                x->v[o].reg = (signed char)to;
                a->owner[to] = o;
                held |= 1u << to;
            } else
                sgx_evict(a, p - 16, at - 1, fixed | held);
            sgx_mov_rr(a, p - 16, x->v[v].reg);
            a->owner[p - 16] = -1;
        }
    }
    for (k = 0; k < 4; ++k) {
        int v = i->in[k], p = s->in[k];
        if (v && p >= 0 && p < 16) r[p] = (unsigned char)x->v[v].reg;
    }
    if (i->mplace >= 0) {
        /* the memory operand, which the r/m place takes */
        r[(int)i->mplace] = (unsigned char)(16 + x->v[i->in[5]].reg);
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
        int tv = tied_in >= 0 ? i->in[tied_in] : zero_place >= 0 && zero_place < 16 ? i->in[4] : 0;
        if (s->out >= 16) {
            out_reg = s->out - 16;
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
        r[(int)i->mplace] = (unsigned char)(16 + x->v[i->in[5]].reg);
        a->b->index = i->in[6] ? x->v[i->in[6]].reg : -1;
        a->b->disp = i->mdisp;
    }
    s->enc(a->b, r, i->imm, i->cc);
    a->b->index = -1;
    if (fixed) /* the copies made into fixed registers for it */
        for (c = 0; c < 16; ++c)
            if (a->owner[c] == -1) a->owner[c] = 0;
    if (out_reg >= 0 && i->def) {
        if (a->owner[out_reg] > 0 && a->owner[out_reg] != i->def) a->x->v[a->owner[out_reg]].reg = -1;
        a->owner[out_reg] = i->def;
        x->v[i->def].reg = (signed char)out_reg;
    }
}

/* ---- XMM registers: vectors' values and spilled general-purpose ones ---- */

/* 16 bytes of the frame (two 8-byte slots; MOVDQU needs no alignment) */
static inline int sgx_slot2(SgXAsm* a) {
    int k = a->slots;
    a->slots += 2;
    if (a->slots > a->slots_max) a->slots_max = a->slots;
    return k;
}
/* f between XMM register xr and the memory at base + disp (MOVDQU, MOVQ to or from it) */
static inline void sgx_vmem(SgXAsm* a, const SgXVForm* f, int xr, int base, int disp) {
    unsigned char r[4] = {0, 0, 0, 0};
    r[(int)f->reg] = (unsigned char)xr;
    r[(int)f->rm] = (unsigned char)(16 + base);
    a->b->disp = disp;
    a->b->index = -1;
    f->enc(a->b, r, 0, 0);
}
static inline void sgx_vrr(SgXAsm* a, const SgXVForm* f, int dst, int src) {
    unsigned char r[4] = {0, 0, 0, 0};
    r[(int)f->reg] = (unsigned char)dst;
    r[(int)f->rm] = (unsigned char)src;
    f->enc(a->b, r, 0, 0);
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
/* what XMM register k holds, kept where it is read after at: a vector in its 16 bytes of the frame, a spilled
   general-purpose value in its 8 (once: an SSA value never changes) */
static inline void sgx_xspill(SgXAsm* a, int k, int at) {
    int o = a->xowner[k];
    if (o > 0 && a->x->v[o].last > at && a->x->v[o].slot < 0) {
        SgXV* vv = &a->x->v[o];
        if (vv->cls) {
            vv->slot = (short)sgx_slot2(a);
            sgx_vmem(a, &sgx_v_movdqu_st, k, SGX_RSP, 8 * vv->slot);
        } else {
            vv->slot = (short)a->slots++;
            if (a->slots > a->slots_max) a->slots_max = a->slots;
            sgx_vmem(a, &sgx_v_movq_st, k, SGX_RSP, 8 * vv->slot);
        }
    }
    sgx_xrelease(a, k);
}
/* an XMM register free at at, none of avoid's: one holding nothing read after, or else the one whose value is read
   latest, spilled */
static inline int sgx_xfree(SgXAsm* a, int at, unsigned avoid) {
    int k, best = -1;
    avoid |= a->xreserved;
    for (k = 0; k < 16; ++k) {
        int o = a->xowner[k];
        if ((avoid >> k) & 1) continue;
        if (!o || (o > 0 && a->x->v[o].last < at)) {
            sgx_xrelease(a, k);
            return k;
        }
    }
    for (k = 0; k < 16; ++k)
        if (!((avoid >> k) & 1) && a->xowner[k] > 0 &&
            (best < 0 || a->x->v[a->xowner[k]].last > a->x->v[a->xowner[best]].last))
            best = k;
    if (best < 0) sg_fail("no XMM register to spill", at, (long long)avoid);
    sgx_xspill(a, best, at);
    return best;
}
/* vector value v in an XMM register (reloaded from its slot where it was spilled), not one of avoid's */
static inline int sgx_xin(SgXAsm* a, int v, int at, unsigned avoid) {
    SgXV* vv = &a->x->v[v];
    int k;
    if (vv->reg >= 0) return vv->reg;
    if (vv->slot < 0) sg_fail("a vector in neither a register nor a slot", v, at);
    k = sgx_xfree(a, at, avoid);
    sgx_vmem(a, &sgx_v_movdqu_ld, k, SGX_RSP, 8 * a->x->v[v].slot);
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
/* the pool after the code, 16-aligned, and the operands' displacements to it; emptied for the next function */
static inline void sgx_pool_emit(SgXAsm* a) {
    SgXBuf* b = a->b;
    size_t at;
    int k;
    if (!a->npool) {
        a->nrips = 0;
        return;
    }
    while (b->n % 16) sgx_byte(b, 0xCC);
    at = b->n;
    for (k = 0; k < a->npool; ++k) {
        sgx_imm(b, (long long)a->pool[k][0], 8);
        sgx_imm(b, (long long)a->pool[k][1], 8);
    }
    for (k = 0; k < a->nrips; ++k) {
        long long d = (long long)(at + 16 * (size_t)a->rips[k].idx) - (long long)a->rips[k].end;
        size_t p = a->rips[k].at;
        if (p + 4 <= b->cap) {
            b->p[p] = (unsigned char)d;
            b->p[p + 1] = (unsigned char)(d >> 8);
            b->p[p + 2] = (unsigned char)(d >> 16);
            b->p[p + 3] = (unsigned char)(d >> 24);
        }
    }
    a->npool = a->nrips = 0;
}

static inline void sgx_assemble_vec(SgXAsm* a, int at) {
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    const SgXVForm* f = i->vf;
    SgXBuf* b = a->b;
    unsigned char r[4] = {0, 0, 0, 0};
    unsigned heldg = 0, heldx = 0;
    int k, out = i->vout, outreg = -1, implicit = -1, base = -1, index = -1, isx = 0;
    for (k = 0; k < f->nops; ++k)
        if (k != f->reg && k != f->rm && k != f->vvvv && ((f->xmask >> k) & 1)) implicit = k; /* <XMM0> */
    if (implicit >= 0 && i->in[implicit]) {
        int from = sgx_xin(a, i->in[implicit], at, 0);
        if (from != 0) {
            int o = a->xowner[0];
            if (o > 0 && x->v[o].last > at) {
                int to = sgx_xfree(a, at, 1u | (1u << from));
                sgx_vrr(a, &sgx_v_movaps, to, 0);
                a->xowner[0] = 0;
                if (x->v[o].cls) x->v[o].reg = (signed char)to;
                else
                    x->v[o].xmm = (signed char)to;
                a->xowner[to] = o;
            } else
                sgx_xrelease(a, 0);
            sgx_vrr(a, &sgx_v_movaps, 0, from);
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
                sgx_vrr(a, &sgx_v_movaps, outreg, from);
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
        if (i->vzero) r[(int)f->rm] = (unsigned char)outreg; /* PXOR of itself */
    }
    if (i->mplace >= 0) {
        if (i->mstate) r[(int)i->mplace] = (unsigned char)(16 + x->base);
        else if (i->mrip)
            r[(int)i->mplace] = SGX_RIP;
        else
            r[(int)i->mplace] = (unsigned char)(16 + base);
        b->index = index;
        b->disp = i->mrip ? 0 : i->mdisp;
    }
    f->enc(b, r, i->imm, 0);
    b->index = -1;
    if (i->mrip) {
        if (a->nrips == a->cap_rips) {
            a->cap_rips = a->cap_rips ? 2 * a->cap_rips : 32;
            a->rips = (struct SgXRip*)realloc(a->rips, (size_t)a->cap_rips * sizeof *a->rips);
            if (!a->rips) sg_fail("out of memory", 0, 0);
        }
        a->rips[a->nrips].at = b->rip_at;
        a->rips[a->nrips].end = b->n;
        a->rips[a->nrips++].idx = sgx_pool_index(a, i->kv[0], i->kv[1]);
    }
    for (k = 0; k < 16; ++k) {
        if (a->owner[k] == -1) a->owner[k] = 0;
        if (a->xowner[k] == -1) a->xowner[k] = 0;
    }
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

/* ---- cold paths ---- */

static inline void sgx_cjump(SgXAsm* a, size_t at, size_t to, int in_cold) {
    if (a->ncjumps == a->cap_cjumps) {
        a->cap_cjumps = a->cap_cjumps ? 2 * a->cap_cjumps : 32;
        a->cjumps = (struct SgXCJump*)realloc(a->cjumps, (size_t)a->cap_cjumps * sizeof *a->cjumps);
        if (!a->cjumps) sg_fail("out of memory", 0, 0);
    }
    a->cjumps[a->ncjumps].at = at;
    a->cjumps[a->ncjumps].to = to;
    a->cjumps[a->ncjumps++].in_cold = (unsigned char)in_cold;
}
static inline void sgx_cold_room(SgXAsm* a, size_t need) {
    if (a->cold.cap - a->cold.n >= need) return;
    a->cold.cap = a->cold.cap * 2 + need;
    a->cold.p = (unsigned char*)realloc(a->cold.p, a->cold.cap);
    if (!a->cold.p) sg_fail("out of memory", 0, 0);
}

/* SGX_K_COLD at at: the hot code jumps out where its condition holds; the cold code keeps the registers the call
   changes that are read after (and the arguments) in scratch slots of the frame above the region's, calls with the
   arguments from where they are, puts RAX in the result's register, takes the others back and jumps in again */
/* RAX = a value of the cold path's (v; -1: the result, in reg out, an XMM one where cls), its doubled magnitude's
   flags then (ADD of itself, 32 bits where w32): RAX was pushed, so a value in RAX is [rsp], a slot 8 further */
static inline void sgx_cold_zf(SgXAsm* a, int v, int out, int cls, int w32) {
    SgXBuf* c = a->b;
    int reg = v < 0 ? out : a->x->v[v].reg, xcls = v < 0 ? cls : a->x->v[v].cls;
    if (xcls && reg >= 0) {
        const SgXVForm* f = w32 ? &sgx_v_movd_rx : &sgx_v_movq_rx;
        unsigned char r[4] = {0, 0, 0, 0};
        r[(int)f->rm] = 0;
        r[(int)f->reg] = (unsigned char)reg;
        f->enc(c, r, 0, 0);
    } else if (reg == 0)
        sgx_mov_mem(a, 1, 64, 0, SGX_RSP, 0);
    else if (reg > 0)
        sgx_mov_rr(a, 0, reg);
    else if (!xcls && a->x->v[v].xmm >= 0)
        sgx_movq(c, 0, a->x->v[v].xmm, 0);
    else
        sgx_mov_mem(a, 1, 64, 0, SGX_RSP, 8 + 8 * a->x->v[v].slot);
    if (!w32) sgx_byte(c, 0x48);
    sgx_byte(c, 0x01); /* add eax, eax */
    sgx_byte(c, 0xC0);
}
static inline void sgx_assemble_cold(SgXAsm* a, int at) {
    static const signed char args_reg[5] = {6, 2, 1, 8, 9}; /* RSI RDX RCX R8 R9, RDI the state */
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    SgXBuf* b = a->b;
    SgXBuf* c;
    int tv = i->in[0], cls = x->v[tv].cls, from, outreg, k, n = 0, nargs = i->w;
    int gslot[16], xslot[16], argslot[5];
    unsigned char saved_g[16] = {0}, saved_x[16] = {0};
    /* the result's register: the tied value's, which dies here, or a copy of it */
    from = cls ? sgx_xin(a, tv, at, 0) : sgx_in_reg(a, tv, at, 0);
    if (x->v[tv].last == at && !x->v[tv].pinned) {
        outreg = from;
        if (cls) a->xowner[from] = 0;
        else
            a->owner[from] = 0;
        x->v[tv].reg = -1;
    } else if (cls) {
        outreg = sgx_xfree(a, at, 1u << from);
        sgx_vrr(a, &sgx_v_movaps, outreg, from);
    } else {
        outreg = sgx_free_reg(a, at, 1u << from);
        sgx_mov_rr(a, outreg, from);
    }
    /* the hot code: Jcc rel32 into the cold */
    sgx_byte(b, 0x0F);
    sgx_byte(b, (unsigned char)(0x80 | (i->cc & 15)));
    sgx_cjump(a, b->n, a->cold.n, 0);
    sgx_imm(b, 0, 4);
    sgx_cold_room(a, 1024);
    {
        SgXBuf* hot = a->b;
        size_t back = hot->n;
        a->b = &a->cold;
        c = a->b;
        if ((i->zacc || i->cfpcr) && x->base != 3) sg_fail("a cold call's state field off RBX", x->base, 3);
        if (i->zacc & 0x80) {
            /* a zero result that is exact: back to the hot code without the call */
            size_t to_call[8];
            int ncall = 0, nok = 0, j;
            size_t to_ok[5];
            sgx_byte(c, 0x50); /* push rax */
            sgx_cold_zf(a, -1, outreg, cls, i->cw32 >> 7 & 1);
            sgx_byte(c, 0x75), sgx_byte(c, 0), to_call[ncall++] = c->n; /* jnz call */
            sgx_byte(c, 0xF7), sgx_byte(c, 0x83), sgx_imm(c, i->disp, 4), sgx_imm(c, (long long)i->kv[0], 4); /* test */
            sgx_byte(c, 0x75), sgx_byte(c, 0), to_call[ncall++] = c->n;
            if (i->zacc & 0x40) {
                /* under FZ a denormal argument is flushed, raising IDC: the call's. Its doubled magnitude is not zero
                   and its exponent is (SHR of it) */
                const int w32 = (i->imm & 0xFF) == 32;
                size_t no_fz;
                sgx_byte(c, 0xF7), sgx_byte(c, 0x83), sgx_imm(c, i->disp, 4), sgx_imm(c, 1ll << 24, 4); /* test FZ */
                sgx_byte(c, 0x74), sgx_byte(c, 0), no_fz = c->n;                                        /* jz */
                for (j = 0; j + 1 < i->cfpcr; ++j) {
                    size_t zero;
                    sgx_cold_zf(a, i->in[1 + j], 0, 0, w32);
                    sgx_byte(c, 0x74), sgx_byte(c, 0), zero = c->n; /* jz: a zero */
                    if (!w32) sgx_byte(c, 0x48);
                    sgx_byte(c, 0xC1), sgx_byte(c, 0xE8), sgx_byte(c, w32 ? 24 : 53); /* shr eax or rax: the exponent */
                    sgx_byte(c, 0x74), sgx_byte(c, 0), to_call[ncall++] = c->n;       /* jz call: a denormal */
                    c->p[zero - 1] = (unsigned char)(c->n - zero);
                }
                c->p[no_fz - 1] = (unsigned char)(c->n - no_fz);
            }
            if (i->zacc & 7) {
                for (j = 0; j < 3; ++j)
                    if (i->zacc >> j & 1) {
                        sgx_cold_zf(a, i->in[1 + j], 0, 0, i->cw32 >> j & 1);
                        sgx_byte(c, 0x74), sgx_byte(c, 0), to_ok[nok++] = c->n; /* jz ok */
                    }
                sgx_byte(c, 0xEB), sgx_byte(c, 0), to_call[ncall++] = c->n; /* jmp call */
            }
            for (j = 0; j < nok; ++j) c->p[to_ok[j] - 1] = (unsigned char)(c->n - to_ok[j]);
            sgx_byte(c, 0x58); /* ok: pop rax, back */
            sgx_byte(c, 0xE9);
            sgx_cjump(a, c->n, back, 1);
            sgx_imm(c, 0, 4);
            for (j = 0; j < ncall; ++j) c->p[to_call[j] - 1] = (unsigned char)(c->n - to_call[j]);
            sgx_byte(c, 0x58); /* call: pop rax */
        }
        /* the caller-saved general-purpose registers read after at, or an argument's */
        for (k = 0; k < 16; ++k) {
            int o = a->owner[k], keep = 0, j;
            if (o <= 0 || k == (cls ? -1 : outreg) || !((SGX_CALLER_SAVED >> k) & 1)) continue;
            keep = x->v[o].last > at;
            for (j = 0; j < nargs; ++j) keep |= i->in[1 + j] == o;
            if (!keep) continue;
            gslot[k] = a->slots + n++;
            saved_g[k] = 1;
            sgx_mov_mem(a, 0, 64, k, SGX_RSP, 8 * gslot[k]);
        }
        for (k = 0; k < 16; ++k) {
            int o = a->xowner[k];
            if (o <= 0 || (cls && k == outreg) || x->v[o].last <= at) continue;
            if (n % 2) ++n;
            xslot[k] = a->slots + n;
            n += 2;
            saved_x[k] = 1;
            sgx_vmem(a, &sgx_v_movdqu_st, k, SGX_RSP, 8 * xslot[k]);
        }
        if (a->slots + n > a->slots_max) a->slots_max = a->slots + n;
        if (i->vflags == 2) {
            /* the vector form: each XMM argument's 16 bytes in memory (its slot where it was spilled), the result's
               16 bytes, their addresses in RSI (the result), RDX, RCX, R8, desc in R9D */
            int outslot, aslot[3];
            if (n % 2) ++n;
            outslot = a->slots + n;
            n += 2;
            for (k = 0; k < 3; ++k) {
                SgXV* vv = &x->v[i->in[1 + k]];
                if (vv->reg >= 0) {
                    aslot[k] = a->slots + n;
                    n += 2;
                    sgx_vmem(a, &sgx_v_movdqu_st, vv->reg, SGX_RSP, 8 * aslot[k]);
                } else
                    aslot[k] = vv->slot;
            }
            if (a->slots + n > a->slots_max) a->slots_max = a->slots + n;
            {
                static const signed char lea_to[4] = {6, 2, 1, 8};
                for (k = 0; k < 4; ++k) {
                    int dst = lea_to[k], at8 = 8 * (k ? aslot[k - 1] : outslot);
                    sgx_byte(c, (unsigned char)(0x48 | (dst >> 3) << 2)); /* lea dst, [rsp + at8] */
                    sgx_byte(c, 0x8D);
                    sgx_byte(c, (unsigned char)(0x84 | (dst & 7) << 3));
                    sgx_byte(c, 0x24);
                    sgx_imm(c, at8, 4);
                }
                sgx_byte(c, 0x41); /* mov r9d, desc */
                sgx_byte(c, 0xB9);
                sgx_imm(c, i->imm, 4);
            }
            nargs = 0;
            argslot[0] = outslot;
        }
        /* the arguments: from a saved register's slot, an XMM register, or the value's own slot */
        for (k = 0; k < nargs; ++k) {
            int v = i->in[1 + k], dst = args_reg[k];
            SgXV* vv = &x->v[v];
            argslot[k] = -1;
            if (k + 1 == i->cfpcr) {
                /* mov dst32, [rbx + disp] */
                if (dst >= 8) sgx_byte(c, 0x44);
                sgx_byte(c, 0x8B);
                sgx_byte(c, (unsigned char)(0x80 | (dst & 7) << 3 | 3));
                sgx_imm(c, i->disp, 4);
            } else if (k + 1 == i->ckat) {
                if (dst >= 8) sgx_byte(c, 0x41); /* mov dst32, imm32 */
                sgx_byte(c, (unsigned char)(0xB8 | (dst & 7)));
                sgx_imm(c, i->imm, 4);
            } else if (vv->cls) {
                if (vv->reg >= 0) {
                    const SgXVForm* f = i->cw32 >> k & 1 ? &sgx_v_movd_rx : &sgx_v_movq_rx;
                    unsigned char r[4] = {0, 0, 0, 0};
                    r[(int)f->rm] = (unsigned char)dst;
                    r[(int)f->reg] = (unsigned char)vv->reg;
                    f->enc(c, r, 0, 0);
                } else
                    sgx_mov_mem(a, 1, 64, dst, SGX_RSP, 8 * vv->slot);
            } else if (vv->reg >= 0 && saved_g[vv->reg])
                sgx_mov_mem(a, 1, 64, dst, SGX_RSP, 8 * gslot[(int)vv->reg]);
            else if (vv->reg >= 0)
                sgx_mov_rr(a, dst, vv->reg); /* a callee-saved one */
            else if (vv->xmm >= 0)
                sgx_movq(c, 0, vv->xmm, dst);
            else
                sgx_mov_mem(a, 1, 64, dst, SGX_RSP, 8 * vv->slot);
        }
        if (a->pin_cs) a->pin_cs(a->machine, 1); /* the caller-saved pins, which the call changes, to the state */
        sgx_byte(c, 0x48); /* mov rdi, rbx */
        sgx_byte(c, 0x89);
        sgx_byte(c, 0xDF);
        if (a->reloc) {
            sgx_byte(c, 0xE8);
            if (a->nccalls == a->cap_ccalls) {
                a->cap_ccalls = a->cap_ccalls ? 2 * a->cap_ccalls : 32;
                a->ccalls = (struct SgXCCall*)realloc(a->ccalls, (size_t)a->cap_ccalls * sizeof *a->ccalls);
                if (!a->ccalls) sg_fail("out of memory", 0, 0);
            }
            a->ccalls[a->nccalls].at = c->n;
            a->ccalls[a->nccalls++].sym = i->sym;
            sgx_imm(c, 0, 4);
        } else {
            SnxU64 to = a->resolve ? a->resolve(i->sym) : 0;
            if (!to) sg_fail("a helper nothing resolves", 0, 0);
            sgx_byte(c, 0x48); /* mov rax, imm64; call rax */
            sgx_byte(c, 0xB8);
            sgx_imm(c, (long long)to, 8);
            sgx_byte(c, 0xFF);
            sgx_byte(c, 0xD0);
        }
        if (i->vflags == 2)
            sgx_vmem(a, &sgx_v_movdqu_ld, outreg, SGX_RSP, 8 * argslot[0]);
        else if (cls) {
            unsigned char r[4] = {0, 0, 0, 0};
            r[(int)sgx_v_movq_xr.reg] = (unsigned char)outreg;
            r[(int)sgx_v_movq_xr.rm] = 0; /* RAX */
            sgx_v_movq_xr.enc(c, r, 0, 0);
        } else
            sgx_mov_rr(a, outreg, 0);
        for (k = 0; k < 16; ++k) {
            if (saved_g[k] && x->v[a->owner[k]].last > at) sgx_mov_mem(a, 1, 64, k, SGX_RSP, 8 * gslot[k]);
            if (saved_x[k]) sgx_vmem(a, &sgx_v_movdqu_ld, k, SGX_RSP, 8 * xslot[k]);
        }
        if (a->pin_cs) a->pin_cs(a->machine, 0);
        sgx_byte(c, 0xE9); /* jmp back */
        sgx_cjump(a, c->n, back, 1);
        sgx_imm(c, 0, 4);
        a->b = hot;
    }
    if (cls) a->xowner[outreg] = i->def;
    else
        a->owner[outreg] = i->def;
    x->v[i->def].reg = (signed char)outreg;
}

/* the cold paths after the code (and the jumps between, and their calls' relocations); emptied for the next */
static inline void sgx_cold_emit(SgXAsm* a) {
    SgXBuf* b = a->b;
    size_t base = b->n, k;
    for (k = 0; k < a->cold.n; ++k) sgx_byte(b, a->cold.p[k]);
    for (k = 0; k < (size_t)a->ncjumps; ++k) {
        size_t at = a->cjumps[k].in_cold ? base + a->cjumps[k].at : a->cjumps[k].at;
        size_t to = a->cjumps[k].in_cold ? a->cjumps[k].to : base + a->cjumps[k].to;
        long long d = (long long)to - (long long)(at + 4);
        if (at + 4 <= b->cap) {
            b->p[at] = (unsigned char)d;
            b->p[at + 1] = (unsigned char)(d >> 8);
            b->p[at + 2] = (unsigned char)(d >> 16);
            b->p[at + 3] = (unsigned char)(d >> 24);
        }
    }
    for (k = 0; k < (size_t)a->nccalls; ++k) a->reloc(a->machine, base + a->ccalls[k].at, a->ccalls[k].sym, -4);
    a->cold.n = 0;
    a->ncjumps = a->nccalls = 0;
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
    memset(a->xowner, 0, sizeof a->xowner);
    a->slots = 0;
    for (i = 0; i < x->nv; ++i) {
        x->v[i].reg = -1;
        x->v[i].slot = -1;
        x->v[i].xmm = -1;
        x->v[i].pinned = 0;
    }
    sgx_optimize(x);
    {
        static _Thread_local int next_call[SGX_MAXI + 1];
        a->next_call = next_call;
    }
    a->next_call[x->n] = x->n;
    for (i = x->n - 1; i >= 0; --i)
        a->next_call[i] = !x->ins[i].dead && (x->ins[i].calls || x->ins[i].kind == SGX_K_MACHINE) ? i : a->next_call[i + 1];
    for (i = 0; i < x->n; ++i) {
        SgXIns* s = &x->ins[i];
        unsigned char r[5] = {0, 0, 0, 0, 0};
        if (s->dead) continue;
        switch (s->kind) {
        case SGX_K_SEL: sgx_assemble_sel(a, i); break;
        case SGX_K_VEC: sgx_assemble_vec(a, i); break;
        case SGX_K_COLD: sgx_assemble_cold(a, i); break;
        case SGX_K_PGET:
            /* the value is the pinned register's own, no copy (until a PSET of it, which moves it if still read); an
               XMM register's for imm 16 and up */
            if (s->imm >= 16) a->xowner[s->imm & 15] = s->def;
            else
                a->owner[s->imm & 15] = s->def;
            x->v[s->def].reg = (signed char)(s->imm & 15);
            x->v[s->def].pinned = 1;
            break;
        case SGX_K_PSET:
            if (s->imm >= 16) {
                int p = (int)(s->imm & 15), o = a->xowner[p], src;
                if (o > 0 && o != s->in[0] && x->v[o].pinned && x->v[o].reg == p && x->v[o].last > i) {
                    int d = sgx_xfree(a, i, 1u << p);
                    sgx_vrr(a, &sgx_v_movaps, d, p);
                    a->xowner[d] = o;
                    x->v[o].reg = (signed char)d;
                    x->v[o].pinned = 0;
                }
                src = sgx_xin(a, s->in[0], i, 0);
                if (src != p) sgx_vrr(a, &sgx_v_movaps, p, src), a->xowner[p] = 0;
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
            if (s->w == 8) {
                /* MOVZX r32, byte [base + disp32]: a flag a SETcc stored (a wider load wouldn't forward from it) */
                if (d >= 8 || x->base >= 8) sgx_byte(b, (unsigned char)(0x40 | (d >> 3) << 2 | (x->base >> 3)));
                sgx_byte(b, 0x0F);
                sgx_byte(b, 0xB6);
                if ((x->base & 7) == 4) {
                    sgx_byte(b, (unsigned char)(0x84 | (d & 7) << 3));
                    sgx_byte(b, 0x24);
                } else
                    sgx_byte(b, (unsigned char)(0x80 | (d & 7) << 3 | (x->base & 7)));
                sgx_imm(b, s->disp, 4);
            } else
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
            b->index = -1;
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
            if (s->calls) {
                /* a call: everything in a register spilled to its home (the call changes the caller-saved ones, and
                   its arguments' registers may hold one another's values), then each argument loaded from its home
                   straight into its register: no move has to wait for another */
                for (k = 0; k < 16; ++k)
                    if (a->owner[k] > 0) sgx_spill(a, k);
                for (k = 0; k < 16; ++k) /* every XMM register is caller-saved: what is read after, in its slot */
                    sgx_xspill(a, k, i);
                for (k = 0; k < 5; ++k)
                    if (s->in[k]) {
                        SgXV* vv = &x->v[s->in[k]];
                        int fr = s->fixed[k] >= 0 ? s->fixed[k] : sgx_arg_free(a, held);
                        if (vv->xmm >= 0) sgx_movq(b, 0, vv->xmm, fr);
                        else
                            sgx_mov_mem(a, 1, 64, fr, SGX_RSP, 8 * vv->slot);
                        held |= 1u << fr;
                        r[k] = (unsigned char)fr;
                    }
                sgx_machine_ins(a, i, r);
                for (k = 0; k < 16; ++k) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
                if (s->def) {
                    a->owner[0] = s->def;
                    x->v[s->def].reg = 0;
                }
                if (s->def2) {
                    a->owner[2] = s->def2;
                    x->v[s->def2].reg = 2;
                }
                break;
            }
            for (k = 0; k < 5; ++k)
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
                for (k = 0; k < 16; ++k)
                    if ((SGX_CALLER_SAVED & (1u << k)) && a->owner[k] > 0 && x->v[a->owner[k]].last > i) sgx_spill(a, k);
            /* the registers it uses besides (and its results' RAX and RDX): what they hold that is read after, moved
               out of the way */
            {
                unsigned clob = s->mclobber | (s->def ? 1u : 0) | (s->def2 ? 4u : 0);
                for (k = 0; k < 16; ++k)
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
            for (k = 0; k < 16; ++k)
                if ((s->mclobber >> k) & 1) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
            for (k = 0; k < 16; ++k)
                if (a->owner[k] == -1 || (s->calls && (SGX_CALLER_SAVED & (1u << k)))) {
                    if (a->owner[k] > 0) x->v[a->owner[k]].reg = -1;
                    a->owner[k] = 0;
                }
            if (s->def) {
                /* its result is in RAX, a second in RDX */
                a->owner[0] = s->def;
                x->v[s->def].reg = 0;
            }
            if (s->def2) {
                a->owner[2] = s->def2;
                x->v[s->def2].reg = 2;
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

/* NZCV of an instruction's flags as 4 bits (N the top), where something reads it as bits (CCMP's choice between it
   and its immediate): each flag by SETcc on them, or 0 where the form gives it as a constant 0 (ANDS's C and V) */
static inline SgD sgx_nzcv_bits(SgX* x, int ins) {
    const SgXSel* fs = x->ins[ins].fsel;
    SgD r = sgx_const(32, 0);
    int f;
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
    if (sgx_is_const(&bv) && bv.k && !(s && bv.k == ~0ull)) d = bv;
    else {
        avoid = sg_d_is_zero(g, bv);
        if (s) avoid = sg_b_or(g, avoid, sg_b_and(g, sg_d_eq(g, av, sgx_const(64, 1ull << 63)), sg_d_eq(g, bv, sgx_const(64, ~0ull))));
        d = sg_d_select(g, avoid, sgx_const(64, 1), bv);
    }
    if (sgx_is_const(&av) && sgx_is_const(&d)) {
        SnxU64 r = s ? (SnxU64)((long long)av.k / (long long)d.k) : av.k / d.k;
        return sgx_int(sgx_const(64, r), s);
    }
    q = s ? SGX_OP(x, sdiv, 64, av, sg_d_asr(g, av, 63), d) : SGX_OP(x, udiv, 64, av, d);
    return sgx_int(q, s);
}

/* c ? a : b of integers, modulo 2^64 (UDIV's and SDIV's divisor-0 case) */
static inline SgI sg_i_select(SgCtx* g, SgB c, SgI a, SgI b) {
    int s = a.is_signed || b.is_signed;
    SgD av = sgx_i_val(g, &a), bv = sgx_i_val(g, &b);
    return sgx_int(sg_d_select(g, c, av, bv), s);
}

/* ---- AArch32's builtins (core.h's, for LLVM) ---- */

/* IsZeroBit: '1' where x is zero */
static inline SgD sg_d_is_zero_bit(SgCtx* g, SgD x) { return sgx_bool_bits(sgx(g), sg_d_is_zero(g, x)); }

/* a == b, !=, <, <=, > and >= (op 0 to 5) of integers known only at run time: their values modulo 2^64 compared as
   signed, which is exact for what AArch32's decode compares (UInt and SInt of 32 bits at most, and their sums); a
   product, which may not be, is left to another back end */
static inline SgB sg_i_cmp(SgCtx* g, int op, SgI a, SgI b) {
    static const int cond[6] = {SGX_ARM_EQ, SGX_ARM_NE, SGX_ARM_LT, SGX_ARM_LE, SGX_ARM_GT, SGX_ARM_GE};
    static const int mirror[6] = {0, 1, 4, 5, 2, 3}; /* a op b is b mirror[op] a */
    SgX* x = sgx(g);
    if (op < 0 || op > 5) sg_fail("a comparison of integers it does not know", op, 5);
    if (a.kind == SGX_I_MUL || b.kind == SGX_I_MUL) sg_fail("a comparison of a product", op, 0);
    SgD av = sgx_i_val(g, &a), bv = sgx_i_val(g, &b);
    sgx_flat(x, &av);
    sgx_flat(x, &bv);
    if (sgx_is_const(&av) && sgx_is_const(&bv)) {
        long long p = (long long)av.k, q = (long long)bv.k;
        int r = op == 0 ? p == q : op == 1 ? p != q : op == 2 ? p < q : op == 3 ? p <= q : op == 4 ? p > q : p >= q;
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
    if (sgx_is_const(&bv) && sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, bv.k, 64)) {
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
