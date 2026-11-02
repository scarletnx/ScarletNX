/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The sites follow Eden's patcher (src/core/arm/nce/patcher.cpp and instructions.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* NCE's patcher (nce_patch.h): which words of the game's code change, and the stubs that take their place */
#include "nx/runtime/nce/nce_patch.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* the system registers as MRS and MSR name them in bits 5 to 19 (o0, op1, CRn, CRm, op2) */
#define SYS_TPIDR   0x5E82u
#define SYS_TPIDRRO 0x5E83u
#define SYS_CNTFRQ  0x5F00u
#define SYS_CNTPCT  0x5F01u
#define SYS_CNTVCT  0x5F02u

NceKind nce_kind(SnxU32 w, unsigned* reg, unsigned* imm) {
    unsigned rt = w & 31u, sys = (w >> 5) & 0x7FFFu;
    *reg = rt;
    *imm = 0;
    if ((w & 0xFFE0001Fu) == 0xD4000001u) { /* SVC #imm16 */
        *imm = (w >> 5) & 0xFFFFu;
        return NCE_SVC;
    }
    if ((w & 0xFFF00000u) == 0xD5300000u && rt != 31) { /* MRS Xt, reg: one to XZR changes nothing */
        if (sys == SYS_TPIDRRO) return NCE_TPIDRRO;
        if (sys == SYS_TPIDR) return NCE_TPIDR;
        if (sys == SYS_CNTPCT || sys == SYS_CNTVCT) return NCE_TICKS;
        if (sys == SYS_CNTFRQ) return NCE_FREQ;
    }
    if ((w & 0xFFF00000u) == 0xD5100000u && sys == SYS_TPIDR) return NCE_TPIDR_SET; /* MSR TPIDR_EL0, Xt */
    /* the exclusives without ordering: size 001000 o2=0 L o1 Rs o0 Rt2 Rn Rt, with o1 set (a pair) or Rt2 31. The
       Cortex-A57 orders each exclusive as its acquire or release form does, and later cores do not, so the game's
       code may lean on it (Eden's Exclusive::AsOrdered) */
    if ((w & 0x3F800000u) == 0x08000000u && ((w >> 21) & 1u || ((w >> 10) & 31u) == 31u) && !((w >> 15) & 1u))
        return NCE_EXCLUSIVE;
    return NCE_NONE;
}

/* ---- the scan ---- */

/* a word's state: not known, code that the flow reaches, a literal that a load reads (which stops the flow: a pool
   after a function's last branch), a word that an ADR takes the address of and is not code, and a word after either
   of those two in a run with no code (the rest of a pool or a table) */
enum { W_UNKNOWN, W_CODE, W_LITERAL, W_REF, W_DATA };

typedef struct {
    SnxU64 lo, hi;       /* the text's guest addresses */
    unsigned char* state; /* one W_ for each word */
    const unsigned char* image;
    SnxU64 base; /* the guest address of image */
    size_t words;
    unsigned char* taken; /* 1 for each word that an ADR of the first flow takes the address of */
} Text;

static SnxU32 word_at(const Text* t, SnxU64 a) { return snx_ld_le32(t->image + (size_t)(a - t->base)); }

typedef struct {
    SnxU64* v;
    size_t n, cap;
} Stack;

static void push(Stack* s, SnxU64 a) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->v = (SnxU64*)snx_realloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = a;
}

static Text* text_of(Text* texts, size_t n, SnxU64 a) {
    for (size_t i = 0; i < n; i++)
        if (a >= texts[i].lo && a < texts[i].hi && !(a & 3)) return &texts[i];
    return NULL;
}

/* the words that code at a reads as data: a literal load's (size bytes), or an ADR's first */
static SnxU64 data_ref(SnxU32 w, SnxU64 a, SnxU64* size) {
    if ((w & 0x3B000000u) == 0x18000000u) { /* LDR (literal), its SIMD forms, LDRSW, PRFM */
        static const unsigned char gpr[4] = {4, 8, 4, 0}, simd[4] = {4, 8, 16, 0};
        unsigned opc = w >> 30;
        SnxU32 imm = (w >> 5) & 0x7FFFFu;
        SnxU64 off = (SnxU64)imm << 2;
        if (imm & 0x40000u) off |= ~(SnxU64)0 << 21; /* imm19, signed, in words */
        *size = (w >> 26) & 1u ? simd[opc] : gpr[opc];
        return *size ? a + off : 0;
    }
    if ((w & 0x9F000000u) == 0x10000000u) { /* ADR: a table's base, or a function's address that code takes */
        SnxU64 off = (SnxU64)(((w >> 5) & 0x7FFFFu) << 2 | ((w >> 29) & 3u));
        if (off & 0x100000u) off |= ~(SnxU64)0 << 21;
        *size = 4;
        return a + off;
    }
    return 0;
}

/* the flow from a, as codegen.c's explore follows it: the words it reaches are code */
static void walk(Text* texts, size_t n, Stack* todo, SnxU64 features) {
    while (todo->n) {
        SnxU64 a = todo->v[--todo->n];
        for (;;) {
            Text* t = text_of(texts, n, a);
            SnxA64Insn insn;
            SnxU32 w;
            SnxA64Flow flow;
            SnxU64 target;
            unsigned char* s;
            if (!t) break;
            s = &t->state[(a - t->lo) >> 2];
            if (*s == W_CODE || *s == W_LITERAL) break;
            w = word_at(t, a);
            snx_a64_decode_word(w, features, &insn);
            flow = insn.flow;
            if (flow == SNX_A64_FLOW_UNDEF) break;
            *s = W_CODE;
            if (flow == SNX_A64_FLOW_B || flow == SNX_A64_FLOW_BC || flow == SNX_A64_FLOW_BL) {
                target = snx_a64_branch_target(&insn, a);
                if (text_of(texts, n, target)) push(todo, target);
                if (flow == SNX_A64_FLOW_B) break;
            } else if (flow == SNX_A64_FLOW_BR || flow == SNX_A64_FLOW_RET || flow == SNX_A64_FLOW_BRK)
                break;
            a += 4;
        }
    }
}

/* the end of f's code: an FDE's range, else past the last word that the analysis explored */
static SnxU64 function_end(const SnxFunction* f) {
    return f->addresses ? (f->count ? snx_function_address(f, f->count - 1) + 4 : f->start) : f->end;
}

/* whether the word before a ends the flow (B, BR, RET, BRK, a word that does not decode) */
static int after_terminal(Text* texts, size_t n, SnxU64 a, SnxU64 features) {
    Text* t = text_of(texts, n, a - 4);
    SnxA64Flow flow;
    if (!t) return 0;
    flow = snx_a64_flow(word_at(t, a - 4), features);
    return flow == SNX_A64_FLOW_B || flow == SNX_A64_FLOW_BR || flow == SNX_A64_FLOW_RET || flow == SNX_A64_FLOW_BRK ||
           flow == SNX_A64_FLOW_UNDEF;
}

/* where the flow starts: each function's start, and its leaders (jump tables' cases, calls' returns). With texts, a
   leader whose address an ADR takes is left out: the analysis makes one of each ADR (a callback, codegen.c's
   taken_address), and one inside a function that its flow never reaches is a table, not code. So is a leader just
   after a word that ends the flow, which the analysis makes of each such word in an FDE's range: the flow proves
   nothing there (the words may still be patched as unproven) */
static void seed(const SnxCodegen* cg, Stack* todo, Text* texts, size_t n) {
    for (size_t i = 0; i < cg->function_count; i++) {
        const SnxFunction* f = &cg->functions[i];
        push(todo, f->start);
        for (size_t k = 0; f->leaders && k < f->count; k++) {
            SnxU64 a = snx_function_address(f, k);
            Text* t = texts ? text_of(texts, n, a) : NULL;
            if (f->leaders[k] && !(t && (t->taken[(a - t->lo) >> 2] || after_terminal(texts, n, a, cg->features))))
                push(todo, a);
        }
    }
}

/* the words that the code reads. first: after the first flow, the literals, which win over it (a pool after a
   function's last branch, inside its FDE), and the words that ADRs take (taken). Else: the literals again, and each
   word an ADR takes that is not code */
static void mark_refs(Text* texts, size_t n, int first) {
    for (size_t i = 0; i < n; i++)
        for (SnxU64 a = texts[i].lo; a < texts[i].hi; a += 4) {
            SnxU64 size, ref;
            SnxU32 w;
            if (texts[i].state[(a - texts[i].lo) >> 2] != W_CODE) continue;
            w = word_at(&texts[i], a);
            if (!(ref = data_ref(w, a, &size))) continue;
            int literal = (w & 0x3B000000u) == 0x18000000u;
            for (SnxU64 at = ref & ~(SnxU64)3; at < ref + size; at += 4) {
                Text* t = text_of(texts, n, at);
                unsigned char* st = t ? &t->state[(at - t->lo) >> 2] : NULL;
                if (!st) continue;
                if (literal)
                    *st = W_LITERAL;
                else if (first)
                    t->taken[(at - t->lo) >> 2] = 1;
                else if (*st == W_UNKNOWN)
                    *st = W_REF;
            }
        }
}

/* the function of the analysis that holds a, or NULL */
static const SnxFunction* function_at(const SnxCodegen* cg, SnxU64 a) {
    size_t lo = 0, hi = cg->function_count;
    while (lo < hi) { /* the last function that starts at or before a */
        size_t mid = lo + (hi - lo) / 2;
        if (cg->functions[mid].start <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo && a < function_end(&cg->functions[lo - 1]) ? &cg->functions[lo - 1] : NULL;
}

static void add_site(NceSites* out, SnxU64 a, SnxU32 w, NceKind kind, unsigned reg, unsigned imm, int proven) {
    NceSite* s;
    if (out->count == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 256;
        out->site = (NceSite*)snx_realloc(out->site, out->cap * sizeof *out->site);
    }
    s = &out->site[out->count++];
    s->addr = a;
    s->word = w;
    s->kind = (unsigned char)kind;
    s->reg = (unsigned char)reg;
    s->imm = imm;
    s->proven = (unsigned char)proven;
    if (proven)
        out->proven++;
    else
        out->unproven++;
}

void nce_find_sites(const SnxCodegen* cg, NceSites* out) {
    size_t n = cg->module_count;
    Text* texts = (Text*)snx_alloc((n ? n : 1) * sizeof *texts);
    Stack todo = {0};
    *out = (NceSites){0};
    for (size_t i = 0; i < n; i++) {
        const SnxCodeImage* m = &cg->modules[i];
        SnxU64 size = m->text_size;
        if (m->text_off > m->image_size) size = 0;
        else if (size > m->image_size - m->text_off) size = m->image_size - m->text_off;
        texts[i].lo = m->base + m->text_off;
        texts[i].hi = texts[i].lo + (size & ~(SnxU64)3);
        texts[i].image = m->image;
        texts[i].base = m->base;
        texts[i].words = (size_t)(size / 4);
        texts[i].state = (unsigned char*)snx_alloc(texts[i].words + 1);
        memset(texts[i].state, W_UNKNOWN, texts[i].words + 1);
        texts[i].taken = (unsigned char*)snx_alloc(texts[i].words + 1);
        memset(texts[i].taken, 0, texts[i].words + 1);
    }
    /* the code: the flow from the analysis's functions. Then the literals that it loads and the words that its ADRs
       take, and the flow again, with the literals as stops and without the leaders that only an ADR made */
    for (int pass = 0; pass < 2; pass++) {
        seed(cg, &todo, pass ? texts : NULL, n);
        walk(texts, n, &todo, cg->features);
        mark_refs(texts, n, pass == 0);
        if (pass == 0)
            for (size_t i = 0; i < n; i++)
                for (size_t k = 0; k < texts[i].words; k++)
                    if (texts[i].state[k] == W_CODE) texts[i].state[k] = W_UNKNOWN;
    }
    /* a pool or a table goes on to the next code or the end of its function */
    for (size_t i = 0; i < n; i++) {
        unsigned char* st = texts[i].state;
        for (size_t k = 1; k < texts[i].words; k++) {
            SnxU64 a = texts[i].lo + (SnxU64)k * 4;
            const SnxFunction* f = function_at(cg, a);
            if (st[k - 1] >= W_LITERAL && st[k] == W_UNKNOWN && f && f == function_at(cg, a - 4)) st[k] = W_DATA;
        }
    }
    for (size_t i = 0; i < n; i++)
        for (SnxU64 a = texts[i].lo; a < texts[i].hi; a += 4) {
            unsigned reg, imm, state = texts[i].state[(a - texts[i].lo) >> 2];
            SnxU32 w = word_at(&texts[i], a);
            NceKind kind = nce_kind(w, &reg, &imm);
            if (kind == NCE_NONE) continue;
            if (state == W_CODE)
                add_site(out, a, w, kind, reg, imm, 1);
            else if (state != W_UNKNOWN)
                out->data++;
            else if (function_at(cg, a) && snx_a64_flow(w, cg->features) != SNX_A64_FLOW_UNDEF)
                add_site(out, a, w, kind, reg, imm, 0);
            else
                out->outside++;
        }
    for (size_t i = 0; i < n; i++) {
        free(texts[i].state);
        free(texts[i].taken);
    }
    free(texts);
    free(todo.v);
}

void nce_sites_free(NceSites* sites) {
    free(sites->site);
    *sites = (NceSites){0};
}

/* ---- the encodings ---- */

#define BRANCH_REACH (128ull << 20)

/* whether a B at from reaches to */
static int reaches(SnxU64 from, SnxU64 to) {
    SnxU64 d = to - from;
    return !(d & 3) && (d < BRANCH_REACH || d >= (SnxU64)0 - BRANCH_REACH);
}
static SnxU32 b_to(SnxU64 from, SnxU64 to) { return 0x14000000u | (SnxU32)(((to - from) >> 2) & 0x3FFFFFFu); }
static SnxU32 bl_to(SnxU64 from, SnxU64 to) { return 0x94000000u | (SnxU32)(((to - from) >> 2) & 0x3FFFFFFu); }
static SnxU32 movz(unsigned d, unsigned imm, unsigned hw) { return 0xD2800000u | hw << 21 | (imm & 0xFFFFu) << 5 | d; }
static SnxU32 movk(unsigned d, unsigned imm, unsigned hw) { return 0xF2800000u | hw << 21 | (imm & 0xFFFFu) << 5 | d; }
static SnxU32 ldr_x(unsigned t, unsigned n, unsigned off) { return 0xF9400000u | (off / 8) << 10 | n << 5 | t; }
static SnxU32 str_x(unsigned t, unsigned n, unsigned off) { return 0xF9000000u | (off / 8) << 10 | n << 5 | t; }
static SnxU32 add_hi(unsigned d, unsigned n, unsigned imm) { return 0x91400000u | (imm & 0xFFFu) << 10 | n << 5 | d; }
static SnxU32 mrs_tpidr(unsigned t) { return 0xD53BD040u | t; } /* the host's TPIDR_EL0: its C library's TLS */
#define SUB_SP_FRAME 0xD10083FFu /* SUB SP, SP, #32 */
#define ADD_SP_FRAME 0x910083FFu /* ADD SP, SP, #32 */
#define STP_X16_X17  0xA90047F0u /* STP X16, X17, [SP] */
#define LDP_X16_X17  0xA94047F0u /* LDP X16, X17, [SP] */
#define BLR_X17      0xD63F0220u
#define BR_X16       0xD61F0200u

typedef struct {
    unsigned char* p;
    SnxU64 at; /* the guest address of the next word */
} Out;

static void put(Out* o, SnxU32 w) {
    snx_st_le32(o->p, w);
    o->p += 4;
    o->at += 4;
}

/* the thread's CPU pointer into xt from the host's thread pointer: at tls, or through the pointer there (indirect).
   Three words either way */
static void load_cpu(Out* o, const NceEnv* env, unsigned t) {
    put(o, mrs_tpidr(t));
    if (env->indirect) {
        put(o, ldr_x(t, t, (unsigned)env->tls));
        put(o, ldr_x(t, t, 0));
        return;
    }
    put(o, add_hi(t, t, (unsigned)(env->tls >> 12)));
    put(o, ldr_x(t, t, (unsigned)(env->tls & 0xFFFu)));
}

/* whether load_cpu reaches tls: a multiple of 8, below 16 MiB (ADD #imm12, LSL #12 and LDR), or 32 KiB (LDR alone) */
static int tls_reached(const NceEnv* env) {
    return !(env->tls & 7) && env->tls < (env->indirect ? 1u << 15 : 1u << 24);
}

/* the call to the runtime: x16, x17 and x30 below the guest's sp, the outcall's code in x16, then a BLR to the
   runtime, which comes back to the next word with sp as it left it and the three registers in their slots */
static void outcall(Out* o, const NceEnv* env, SnxU64 code, int dynamic_return, SnxU64 back) {
    put(o, SUB_SP_FRAME);
    put(o, STP_X16_X17);
    put(o, str_x(30, 31, 16));
    put(o, movz(16, (unsigned)code, 0));
    put(o, movk(16, (unsigned)(code >> 16), 1));
    put(o, 0x58000000u | (7u << 5) | 17u); /* LDR X17, the literal 7 words on (after the padding word) */
    put(o, BLR_X17);
    put(o, LDP_X16_X17);
    put(o, ldr_x(30, 31, 16));
    put(o, ADD_SP_FRAME);
    put(o, dynamic_return ? BR_X16 : b_to(o->at, back)); /* a hook's return: the pc that the runtime put in x16 */
    put(o, 0xD503201Fu); /* NOP: the literal 8-aligned */
    snx_st_le64(o->p, env->outcall);
    o->p += 8;
    o->at += 8;
}

#define OUTCALL_BYTES 56

SnxU64 nce_stub_size(NceKind kind) {
    switch (kind) {
    case NCE_SVC:
    case NCE_TICKS: return OUTCALL_BYTES;
    case NCE_TPIDRRO:
    case NCE_TPIDR: return 24; /* five words, rounded to 8 */
    case NCE_TPIDR_SET: return 32;
    case NCE_FREQ: return 16;
    case NCE_HOOK: return OUTCALL_BYTES + 32;
    default: return 0;
    }
}

/* a pool with size bytes free whose start is reached from each of the addresses, and from which they are */
static NcePool* pool_for(NcePool* pools, size_t count, SnxU64 size, SnxU64 site) {
    for (size_t i = 0; i < count; i++) {
        NcePool* p = &pools[i];
        SnxU64 at = p->addr + p->used;
        if (p->size - p->used < size) continue;
        if (reaches(site, at) && reaches(at + size - 4, site + 4) && reaches(at, site) && reaches(site, at + size))
            return p;
    }
    return NULL;
}

int nce_emit(NcePool* pools, size_t count, const NceEnv* env, const NceSite* site, SnxU32* patch) {
    SnxU64 size = nce_stub_size((NceKind)site->kind), back = site->addr + 4;
    unsigned t = site->reg;
    NcePool* p;
    Out o;
    if (site->kind == NCE_EXCLUSIVE) {
        *patch = site->word | 1u << 15; /* o0: LDXR to LDAXR, STXR to STLXR, the pairs alike */
        return 0;
    }
    if (!size || !tls_reached(env) || !(p = pool_for(pools, count, size, site->addr))) return -1;
    o.p = p->buf + p->used;
    o.at = p->addr + p->used;
    *patch = b_to(site->addr, o.at);
    switch ((NceKind)site->kind) {
    case NCE_SVC: outcall(&o, env, NCE_OUTCALL(NCE_SVC, site->imm), 0, back); break;
    case NCE_TICKS: outcall(&o, env, NCE_OUTCALL(NCE_TICKS, t), 0, back); break;
    case NCE_TPIDRRO:
    case NCE_TPIDR:
        load_cpu(&o, env, t);
        put(&o, ldr_x(t, t, site->kind == NCE_TPIDRRO ? offsetof(CPU, tpidrro) : offsetof(CPU, tpidr)));
        put(&o, b_to(o.at, back));
        break;
    case NCE_TPIDR_SET: {
        unsigned s = t == 16 ? 17 : 16; /* a scratch register, kept below the guest's sp */
        put(&o, 0xF81F0FE0u | s);       /* STR Xs, [SP, #-16]! */
        load_cpu(&o, env, s);
        put(&o, str_x(t, s, offsetof(CPU, tpidr))); /* Rt 31 stores XZR */
        put(&o, 0xF84107E0u | s);                   /* LDR Xs, [SP], #16 */
        put(&o, b_to(o.at, back));
        break;
    }
    case NCE_FREQ:
        put(&o, movz(t, NCE_CNTFRQ & 0xFFFFu, 0));
        put(&o, movk(t, NCE_CNTFRQ >> 16, 1));
        put(&o, b_to(o.at, back));
        break;
    default: return -1;
    }
    p->used += size;
    return 0;
}

int nce_divert(NcePool* pools, size_t count, const NceEnv* env, SnxU64 entry, SnxU32 first, unsigned index,
               SnxU32* patch, SnxU64* original) {
    SnxU64 size = nce_stub_size(NCE_HOOK), target = 0;
    NcePool* p;
    Out o;
    int moved = 0; /* 1: an ADR or ADRP, made absolute; 2: a B, which never comes back to entry + 4; 3: a BL */
    if ((first & 0xFF000010u) == 0x54000000u || (first & 0x7E000000u) == 0x34000000u ||
        (first & 0x7E000000u) == 0x36000000u || (first & 0x3B000000u) == 0x18000000u)
        return -1; /* B.cond, CBZ, CBNZ, TBZ, TBNZ, a literal load: none starts a function that a hook names */
    if ((first & 0x1F000000u) == 0x10000000u) { /* ADR, ADRP */
        SnxU64 imm = (SnxU64)(((first >> 5) & 0x7FFFFu) << 2 | ((first >> 29) & 3u));
        if (imm & 0x100000u) imm |= ~(SnxU64)0 << 21;
        target = first >> 31 ? (entry & ~(SnxU64)0xFFF) + (imm << 12) : entry + imm;
        moved = 1;
    } else if ((first & 0x7C000000u) == 0x14000000u) { /* B, BL */
        SnxU64 imm = first & 0x3FFFFFFu;
        if (imm & 0x2000000u) imm |= ~(SnxU64)0 << 26;
        target = entry + (imm << 2);
        moved = first >> 31 ? 3 : 2;
    }
    if (!tls_reached(env) || !(p = pool_for(pools, count, size, entry))) return -1;
    o.p = p->buf + p->used;
    o.at = p->addr + p->used;
    *patch = b_to(entry, o.at);
    outcall(&o, env, NCE_OUTCALL(NCE_HOOK, index), 1, 0);
    *original = o.at;
    if (moved == 1) {
        unsigned d = first & 31u;
        put(&o, movz(d, (unsigned)target, 0));
        put(&o, movk(d, (unsigned)(target >> 16), 1));
        put(&o, movk(d, (unsigned)(target >> 32), 2));
        put(&o, movk(d, (unsigned)(target >> 48), 3));
    } else if (moved >= 2) {
        if (!reaches(o.at, target)) return -1;
        put(&o, moved == 2 ? b_to(o.at, target) : bl_to(o.at, target));
    } else
        put(&o, first);
    if (moved != 2) put(&o, b_to(o.at, entry + 4));
    p->used += size;
    return 0;
}

int nce_trap(NcePool* pools, size_t count, const NceEnv* env, unsigned index, SnxU64* addr) {
    Out o;
    for (size_t i = 0; i < count; i++) {
        NcePool* p = &pools[i];
        if (p->size - p->used < OUTCALL_BYTES) continue;
        o.p = p->buf + p->used;
        o.at = p->addr + p->used;
        *addr = o.at;
        outcall(&o, env, NCE_OUTCALL(NCE_HOOK, index), 1, 0);
        p->used += OUTCALL_BYTES;
        return 0;
    }
    return -1;
}

/* ---- the pools and the file ---- */

SnxU64 nce_pool_size(const NceSites* s, SnxU64 lo, SnxU64 hi) {
    SnxU64 size = NCE_HOOK_ROOM;
    for (size_t i = 0; i < s->count; i++)
        if (s->site[i].addr >= lo && s->site[i].addr < hi) size += nce_stub_size((NceKind)s->site[i].kind);
    return (size + 0xFFFu) & ~(SnxU64)0xFFFu;
}

void nce_sites_move(NceSites* s, SnxU64 lo, SnxU64 hi, SnxU64 delta) {
    for (size_t i = 0; i < s->count; i++)
        if (s->site[i].addr >= lo && s->site[i].addr < hi) s->site[i].addr += delta;
}

/* "SNXNCE", the version, then the counts of pools and sites, the pools (address, size) and the sites (address,
   word, imm, kind, reg, proven, 5 bytes of zero), all little-endian, each address from the first module's base */
static const unsigned char nce_magic[8] = {'S', 'N', 'X', 'N', 'C', 'E', 0, 2};
#define NCE_HEADER 24u
#define NCE_POOL   16u
#define NCE_SITE   24u

int nce_file_write(const char* path, SnxU64 base, const NceRange* pools, size_t pool_count, const NceSites* s) {
    size_t size = NCE_HEADER + pool_count * NCE_POOL + s->count * NCE_SITE;
    unsigned char* out = (unsigned char*)snx_alloc(size);
    unsigned char* p = out + NCE_HEADER;
    int r;
    memset(out, 0, size);
    memcpy(out, nce_magic, sizeof nce_magic);
    snx_st_le64(out + 8, pool_count);
    snx_st_le64(out + 16, s->count);
    for (size_t i = 0; i < pool_count; i++, p += NCE_POOL) {
        snx_st_le64(p, pools[i].addr - base);
        snx_st_le64(p + 8, pools[i].size);
    }
    for (size_t i = 0; i < s->count; i++, p += NCE_SITE) {
        const NceSite* t = &s->site[i];
        snx_st_le64(p, t->addr - base);
        snx_st_le32(p + 8, t->word);
        snx_st_le32(p + 12, t->imm);
        p[16] = t->kind;
        p[17] = t->reg;
        p[18] = t->proven;
    }
    r = snx_write_file(path, out, size);
    free(out);
    return r ? -1 : 0;
}

int nce_file_read(const char* path, SnxU64 base, NceRange** pools, size_t* pool_count, NceSites* s) {
    size_t size = 0;
    unsigned char* in = snx_read_file(path, &size);
    const unsigned char* p;
    SnxU64 np, ns;
    *pools = NULL;
    *pool_count = 0;
    *s = (NceSites){0};
    if (!in) return -1;
    np = size >= NCE_HEADER ? snx_ld_le64(in + 8) : 0;
    ns = size >= NCE_HEADER ? snx_ld_le64(in + 16) : 0;
    if (size < NCE_HEADER || memcmp(in, nce_magic, sizeof nce_magic) || np > (size - NCE_HEADER) / NCE_POOL ||
        ns > (size - NCE_HEADER - np * NCE_POOL) / NCE_SITE || size != NCE_HEADER + np * NCE_POOL + ns * NCE_SITE) {
        free(in);
        return -1;
    }
    p = in + NCE_HEADER;
    *pools = (NceRange*)snx_alloc((size_t)(np ? np : 1) * sizeof **pools);
    for (size_t i = 0; i < np; i++, p += NCE_POOL) {
        (*pools)[i].addr = base + snx_ld_le64(p);
        (*pools)[i].size = snx_ld_le64(p + 8);
    }
    *pool_count = (size_t)np;
    s->site = (NceSite*)snx_alloc((size_t)(ns ? ns : 1) * sizeof *s->site);
    s->count = s->cap = (size_t)ns;
    for (size_t i = 0; i < ns; i++, p += NCE_SITE) {
        NceSite* t = &s->site[i];
        t->addr = base + snx_ld_le64(p);
        t->word = snx_ld_le32(p + 8);
        t->imm = snx_ld_le32(p + 12);
        t->kind = p[16];
        t->reg = p[17];
        t->proven = p[18];
        if (t->proven)
            s->proven++;
        else
            s->unproven++;
    }
    free(in);
    return 0;
}
