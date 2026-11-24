/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's machine back ends, their code buffer and the forms a selection lists (copied after bits.h and before the
   machine's own file, x64.h or arm64.h, mach.h's README): the bytes an encoder appends, a primitive's candidate
   (SgXSel) and a vector form (SgXVForm), which the machine's encoders write and mach.h appends and assembles */

/* ---- the code ---- */

typedef struct {
    unsigned char* p;
    size_t n, cap;
    int disp;  /* a memory operand's displacement and index register (-1 none), for the machine's encoders */
    int index;
    size_t rip_at; /* where an encoder put a pool-relative operand's displacement, which the assembler patches */
} SgXBuf;

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

/* a selected form (x64_isel.inc, arm64.h): its encoder; the feature it needs; its operand that may be memory (x86's
   ModRM.rm); where each of the primitive's inputs goes (the form's operand 0 to 3, or SGX_FIX + a fixed register); the
   inputs it reads whole (clean) and those that are immediates; the immediate's fit; where the result is, whether zero
   extended, and the input it is tied to (+1); the fixed registers it writes besides, the operands it needs zeroed
   before it, the flags it writes (the machine's, x86's CF PF AF ZF SF OF); Arm's flags as its flags (x86's 0 CF ...
   5 OF, negated by flag_neg's bit); the carry flag the carry in's negation; and each Arm condition's condition code
   on its flags */
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

/* a vector form (an SSE form an 'enc' line of x64.ops asks for, x64_isel.inc's sgx_v_NAME): its encoder, the feature it needs,
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
