/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sail/sm86.sail held to what the instructions compute: the same Sail that sm86_translate.c's emitters are generated
   from (sailgen --gpu), generated again with the evaluating back end (tools/sailgen/eval.h, sail/sm86_eval_state.h),
   and run on random states against sm86_eval (the CPU's run of the integer ALU, sm86_eval_step) or, for the
   instructions sm86_eval does not take, the reference below. Each instruction is a random encoding of its opcode and
   forms, the registers it names mostly a few and RZ, its immediates mostly small, and the states random: every
   register, uniform register and predicate must come out the same, and no operation the emitter makes may be one
   SPIR-V leaves undefined (eval.h's undefined). The truth tables of LOP3 and PLOP3 are also checked whole: each of the
   256 on the three inputs that hold every minterm */
#include "nx/gpu/shader/sm86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm86_sail_eval.inc"

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static u64 rng = 0x9e3779b97f4a7c15ull;
static u64 next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static u32 cb_word(void* user, u32 bank, u32 off) {
    (void)user;
    u64 h = (u64)bank * 0x100000001b3ull ^ (u64)off * 0x9e3779b97f4a7c15ull;
    h ^= h >> 29;
    return (u32)(h * 0xbf58476d1ce4e5b9ull >> 32);
}
static int no_mem(void* user, u64 address, u32* words, u32 count) {
    (void)user, (void)address, (void)words, (void)count;
    return 0;
}
static const Sm86EvalIo io = {NULL, cb_word, no_mem};

static void set(u64* lo, u64* hi, unsigned at, unsigned n, u64 v) {
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = at + i;
        u64* w = bit < 64 ? lo : hi;
        u64 m = 1ull << (bit & 63);
        *w = (v >> i) & 1 ? *w | m : *w & ~m;
    }
}
/* a register field: mostly one of a few registers or the zero register */
static u32 some_reg(int uniform) {
    u32 r = (u32)next();
    if (r & 1) return (r >> 1) & 7;
    if (r & 2) return uniform ? SM86_URZ : SM86_RZ;
    return (r >> 2) & (uniform ? 63 : 255);
}
static void random_state(Sm86EvalState* s) {
    for (u32 k = 0; k < 256; k++) s->r[k] = (u32)next();
    for (u32 k = 0; k < 64; k++) s->ur[k] = (u32)next();
    for (u32 k = 0; k < 8; k++) {
        s->p[k] = next() & 1;
        s->up[k] = next() & 1;
    }
    /* small values and the edges, which the shifts and the comparisons turn on */
    for (u32 k = 0; k < 8; k++) {
        u32 pick = (u32)next() % 6;
        u32 v = pick == 0 ? (u32)next() % 40 : pick == 1 ? 0x80000000u : pick == 2 ? 0xffffffffu : s->r[k];
        s->r[k] = v;
        s->ur[k] = pick == 3 ? (u32)next() % 40 : s->ur[k];
    }
}

/* the instructions sm86_eval does not take, as the hand-written translator computed them */
static u32 ref_msb(u32 x) {
    for (int i = 31; i >= 0; i--)
        if (x >> i & 1) return (u32)i;
    return 0xffffffffu;
}
static int ref_step(Sm86EvalState* s, const Sm86Insn* in) {
    const Sm86Src* b = &in->b;
    u32 x = b->kind == SM86_SRC_REG    ? (b->reg < SM86_RZ ? s->r[b->reg] : 0)
            : b->kind == SM86_SRC_UREG ? (b->reg < SM86_URZ ? s->ur[b->reg] : 0)
            : b->kind == SM86_SRC_IMM  ? b->value
                                       : cb_word(NULL, b->bank, b->value);
    u32 v;
    switch (in->op) {
        case SM86_FLO:
        case SM86_UFLO:
            if (sm86_field(in, 63, 1)) x = ~x;
            v = sm86_field(in, 73, 1) ? ref_msb(x >> 31 ? ~x : x) : ref_msb(x);
            if (sm86_field(in, 74, 1) && v != 0xffffffffu) v = 31 - v;
            break;
        case SM86_POPC:
        case SM86_UPOPC:
            if (sm86_field(in, 63, 1)) x = ~x;
            v = 0;
            for (; x; x &= x - 1) v++;
            break;
        case SM86_BREV:
        case SM86_UBREV:
            v = 0;
            for (int i = 0; i < 32; i++) v |= (x >> i & 1) << (31 - i);
            break;
        case SM86_IABS: v = (int32_t)x < 0 ? 0u - x : x; break;
        default: return 0;
    }
    if (sm86_ops[in->op].kind == SM86_K_UALU) {
        if (in->dst < SM86_URZ) s->ur[in->dst] = v;
    } else if (in->dst < SM86_RZ)
        s->r[in->dst] = v;
    return 1;
}

typedef struct {
    int op;
    u16 code;  /* bits 0-8 */
    u8 forms;  /* bit f: form f (bits 9-11) */
    int ref;   /* sm86_eval's (1) or ref_step's (0) */
} OpCase;
static const OpCase cases[] = {
    {SM86_IADD3, 0x010, 0xfe, 1},  {SM86_UIADD3, 0x090, 0x16, 1}, {SM86_IMAD, 0x024, 0xfe, 1},
    {SM86_UIMAD, 0x0a4, 0x16, 1},  {SM86_IMNMX, 0x017, 0x72, 1},  {SM86_UIMNMX, 0x085, 0x16, 1},
    {SM86_ISETP, 0x00c, 0x72, 1},  {SM86_UISETP, 0x08c, 0x16, 1}, {SM86_LEA, 0x011, 0xfe, 1},
    {SM86_ULEA, 0x091, 0x16, 1},   {SM86_LOP3, 0x012, 0xfe, 1},   {SM86_ULOP3, 0x092, 0x16, 1},
    {SM86_SHF, 0x019, 0xfe, 1},    {SM86_USHF, 0x099, 0x16, 1},   {SM86_SEL, 0x007, 0x72, 1},
    {SM86_USEL, 0x087, 0x16, 1},   {SM86_MOV, 0x002, 0x72, 1},    {SM86_PLOP3, 0x01c, 0x10, 1},
    {SM86_UPLOP3, 0x09c, 0x10, 1}, {SM86_FLO, 0x100, 0x72, 0},    {SM86_UFLO, 0x0bd, 0x16, 0},
    {SM86_POPC, 0x109, 0x72, 0},   {SM86_UPOPC, 0x0bf, 0x16, 0},  {SM86_BREV, 0x101, 0x72, 0},
    {SM86_UBREV, 0x0be, 0x16, 0},  {SM86_IABS, 0x013, 0x72, 0},
};

/* a random encoding of the case's opcode, unpredicated, decoded: 0 when it isn't the opcode (or sm86_eval won't take
   it), which the caller draws again */
static int random_insn(const OpCase* k, Sm86Insn* in) {
    u64 lo = next(), hi = next();
    u32 form;
    do form = (u32)next() & 7;
    while (!(k->forms >> form & 1));
    int uniform = sm86_ops[k->op].kind == SM86_K_UALU;
    set(&lo, &hi, 0, 9, k->code);
    set(&lo, &hi, 9, 3, form);
    set(&lo, &hi, 12, 4, 7);
    set(&lo, &hi, 16, 8, some_reg(uniform));
    set(&lo, &hi, 24, 8, some_reg(uniform));
    if (next() & 1) set(&lo, &hi, 32, 8, some_reg(uniform));
    else if (next() & 1) set(&lo, &hi, 32, 32, next() % 40); /* a small immediate: a shift's, a mask's */
    set(&lo, &hi, 64, 8, some_reg(uniform));
    if (!sm86_decode(lo, hi, in) || in->op != k->op) return 0;
    if (k->ref && !sm86_eval_takes(in)) return 0;
    if ((in->a.kind == SM86_SRC_CBUF && in->a.bindless) || (in->b.kind == SM86_SRC_CBUF && in->b.bindless) ||
        (in->c.kind == SM86_SRC_CBUF && in->c.bindless))
        return 0;
    return 1;
}

static void test_random(void) {
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        const OpCase* k = &cases[c];
        u32 done = 0, tries = 0, bad = 0;
        while (done < 20000 && tries < 2000000) {
            Sm86Insn in;
            tries++;
            if (!random_insn(k, &in)) continue;
            Sm86EvalState want, got;
            random_state(&want);
            got = want;
            int ok = k->ref ? sm86_eval_step(&want, &in, &io) : ref_step(&want, &in);
            CHECK(ok);
            SgG g;
            sg86_begin(&g, &got, &in, &io);
            CHECK(sg86_emit(&g, in.op));
            int same = !g.ev.undefined && !g.ev.thrown && !memcmp(&want, &got, sizeof want);
            if (!same && bad++ < 3) {
                fprintf(stderr, "%s %016llx %016llx: %s\n", sm86_ops[k->op].name, (unsigned long long)in.lo,
                        (unsigned long long)in.hi,
                        g.ev.undefined ? "an undefined SPIR-V result" : g.ev.thrown ? "refused" : "another state");
                for (u32 r = 0; r < 256; r++)
                    if (want.r[r] != got.r[r]) fprintf(stderr, "  R%u %08x, Sail %08x\n", r, want.r[r], got.r[r]);
                for (u32 r = 0; r < 64; r++)
                    if (want.ur[r] != got.ur[r]) fprintf(stderr, "  UR%u %08x, Sail %08x\n", r, want.ur[r], got.ur[r]);
                for (u32 p = 0; p < 8; p++)
                    if (want.p[p] != got.p[p] || want.up[p] != got.up[p])
                        fprintf(stderr, "  P%u %u UP%u %u, Sail %u %u\n", p, want.p[p], p, want.up[p], got.p[p], got.up[p]);
            }
            CHECK(same);
            done++;
        }
        CHECK(done == 20000);
        if (bad) fprintf(stderr, "%s: %u of %u differ\n", sm86_ops[k->op].name, bad, done);
    }
}

/* every truth table of LOP3 R0, R1, R2, R3 (P0 its result's test) on the inputs 0xf0, 0xcc and 0xaa in each byte: the
   result is the table; and PLOP3's two of P0-P2 into P3 and P4, on each of the 8 values of the three */
static void test_tables(void) {
    for (u32 lut = 0; lut < 256; lut++) {
        u64 lo = 0, hi = 0;
        set(&lo, &hi, 0, 9, 0x012);
        set(&lo, &hi, 9, 3, 1);
        set(&lo, &hi, 12, 4, 7);
        set(&lo, &hi, 16, 8, 0);
        set(&lo, &hi, 24, 8, 1);
        set(&lo, &hi, 32, 8, 2);
        set(&lo, &hi, 64, 8, 3);
        set(&lo, &hi, 72, 8, lut);
        set(&lo, &hi, 81, 3, 0);
        set(&lo, &hi, 87, 4, 0xf);
        Sm86Insn in;
        CHECK(sm86_decode(lo, hi, &in) && in.op == SM86_LOP3);
        Sm86EvalState s;
        memset(&s, 0, sizeof s);
        s.r[1] = 0xf0f0f0f0u;
        s.r[2] = 0xccccccccu;
        s.r[3] = 0xaaaaaaaau;
        SgG g;
        sg86_begin(&g, &s, &in, &io);
        CHECK(sg86_emit(&g, in.op) && !g.ev.undefined && !g.ev.thrown);
        CHECK(s.r[0] == lut * 0x01010101u);
        CHECK(s.p[0] == (lut != 0));
        if (s.r[0] != lut * 0x01010101u) fprintf(stderr, "LOP3 table 0x%02x gives %08x\n", lut, s.r[0]);
        for (u32 bits = 0; bits < 8; bits++) {
            lo = hi = 0;
            set(&lo, &hi, 0, 9, 0x01c);
            set(&lo, &hi, 9, 3, 4);
            set(&lo, &hi, 12, 4, 7);
            set(&lo, &hi, 64, 3, lut & 7); /* the first table, P3's: 64-66 and 72-76 */
            set(&lo, &hi, 72, 5, lut >> 3);
            set(&lo, &hi, 16, 8, lut ^ 0x5a); /* the second, P4's */
            set(&lo, &hi, 87, 4, 0);          /* x: P0 */
            set(&lo, &hi, 77, 4, 1);          /* y: P1 */
            set(&lo, &hi, 68, 4, 2);          /* z: P2 */
            set(&lo, &hi, 67, 1, 0);
            set(&lo, &hi, 81, 3, 3);
            set(&lo, &hi, 84, 3, 4);
            CHECK(sm86_decode(lo, hi, &in) && in.op == SM86_PLOP3);
            memset(&s, 0, sizeof s);
            s.p[0] = bits >> 2 & 1;
            s.p[1] = bits >> 1 & 1;
            s.p[2] = bits & 1;
            sg86_begin(&g, &s, &in, &io);
            CHECK(sg86_emit(&g, in.op) && !g.ev.undefined && !g.ev.thrown);
            CHECK(s.p[3] == (lut >> bits & 1));
            CHECK(s.p[4] == ((lut ^ 0x5a) >> bits & 1));
        }
    }
}

/* an IADD3 of form f (b a register, an immediate or a constant) */
static Sm86Insn iadd3(u32 form, u32 d, u32 a, u32 b, int neg_b, u32 c, int x, u32 p, u32 q, u32 cin1, u32 cin2) {
    u64 lo = 0, hi = 0;
    Sm86Insn in;
    set(&lo, &hi, 0, 9, 0x010);
    set(&lo, &hi, 9, 3, form);
    set(&lo, &hi, 12, 4, 7);
    set(&lo, &hi, 16, 8, d);
    set(&lo, &hi, 24, 8, a);
    if (form == 4)
        set(&lo, &hi, 32, 32, b);
    else if (form == 5) {
        set(&lo, &hi, 40, 14, b); /* the constant's word offset and bank */
        set(&lo, &hi, 54, 5, 1);
    } else
        set(&lo, &hi, 32, 8, b);
    set(&lo, &hi, 63, 1, (u64)neg_b); /* b's negation */
    set(&lo, &hi, 64, 8, c);
    set(&lo, &hi, 74, 1, (u64)x);
    set(&lo, &hi, 77, 4, cin2);
    set(&lo, &hi, 81, 3, p);
    set(&lo, &hi, 84, 3, q);
    set(&lo, &hi, 87, 4, cin1);
    sm86_decode(lo, hi, &in);
    return in;
}

/* IADD3_WIDE (sm86.roots' fusion): every pair its guard takes, run as one, against the two run one after the other by
   sm86_eval; and pairs a little off it, which it must not take */
static void test_fusion(void) {
    u32 taken = 0, refused = 0;
    for (u32 t = 0; t < 200000; t++) {
        u32 form = (u32[]){1, 4, 5}[next() % 3];
        int neg = next() & 1;
        u32 p = (u32)next() % 7;
        Sm86Insn in[2];
        in[0] = iadd3(form, some_reg(0), some_reg(0), form == 4 ? (u32)next() : form == 5 ? (u32)next() % 64 : some_reg(0),
                      neg, SM86_RZ, 0, p, 7, 0xf, 0xf);
        in[1] = iadd3(form, some_reg(0), some_reg(0), form == 4 ? (u32)next() : form == 5 ? (u32)next() % 64 : some_reg(0),
                      neg, SM86_RZ, 1, 7, 7, p, 0xf);
        /* a near miss now and then: another carry predicate, a carry written, a second operand negated alone */
        u32 miss = (u32)next() % 8;
        if (miss == 0) in[1] = iadd3(form, in[1].dst, in[1].a.reg, in[1].b.reg, neg, SM86_RZ, 1, 7, 7, (p + 1) % 7, 0xf);
        if (miss == 1) in[1] = iadd3(form, in[1].dst, in[1].a.reg, in[1].b.reg, !neg, SM86_RZ, 1, 7, 7, p, 0xf);
        if (miss == 2) in[0] = iadd3(form, in[0].dst, in[0].a.reg, in[0].b.reg, neg, SM86_RZ, 0, p, (p + 1) % 7, 0xf, 0xf);
        if (in[0].op != SM86_IADD3 || in[1].op != SM86_IADD3) continue;
        Sm86EvalState want, got;
        random_state(&want);
        got = want;
        CHECK(sm86_eval_step(&want, &in[0], &io) && sm86_eval_step(&want, &in[1], &io));
        SgG g;
        sg86_begin(&g, &got, &in[0], &io);
        g.at[1] = &in[1];
        int ops[2] = {in[0].op, in[1].op};
        u32 len = sg86_fuse(&g, ops, 2);
        if (!len) { /* not taken: each runs alone */
            refused++;
            CHECK(sg86_emit(&g, in[0].op));
            sg86_begin(&g, &got, &in[1], &io);
            CHECK(sg86_emit(&g, in[1].op));
        } else
            taken++;
        int same = !g.ev.undefined && !g.ev.thrown && !memcmp(&want, &got, sizeof want);
        if (!same && taken + refused < 50)
            fprintf(stderr, "IADD3_WIDE %s: %016llx %016llx, %016llx %016llx\n", len ? "fused" : "alone",
                    (unsigned long long)in[0].lo, (unsigned long long)in[0].hi, (unsigned long long)in[1].lo,
                    (unsigned long long)in[1].hi);
        CHECK(same);
    }
    /* the guard took most, and left some */
    CHECK(taken > 50000 && refused > 10000);
    printf("shader_sail_sm86: IADD3_WIDE took %u pairs, left %u\n", taken, refused);
}

int main(void) {
    test_tables();
    test_random();
    test_fusion();
    if (failures) {
        fprintf(stderr, "shader_sail_sm86: %d checks failed\n", failures);
        return 1;
    }
    printf("shader_sail_sm86: all checks pass\n");
    return 0;
}
