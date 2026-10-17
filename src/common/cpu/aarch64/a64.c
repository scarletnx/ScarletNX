/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp a64.py and codegen.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#include "common/cpu/aarch64/a64.h"
#include <string.h>

#include "common/cpu/aarch64/a64_templates.inc"

/* The validation expressions are compiled into the host decoder and printed into
   the fallback interpreter. Invalid encodings therefore stop discovery and trap
   identically in both paths. */
#define VALIDATIONS(X)                                                                                                 \
    X(1, ((w >> 31) || !(w & 0x400000UL)) &&                                                                           \
             (((((w >> 22) & 1) << 6) | (~(w >> 10) & 63)) & (((((w >> 22) & 1) << 6) | (~(w >> 10) & 63)) - 1)))      \
    X(2, (w >> 31) || !((w >> 21) & 2))                                                                                \
    X(3, ((w >> 22) & 1) == (w >> 31) && ((w >> 31) || !(w & 0x208000UL)))                                             \
    X(4, ((w >> 22) & 1) == (w >> 31) && ((w >> 31) || !(w & 0x8000UL)))                                               \
    X(5, (w >> 13) & 2)                                                                                                \
    X(6, (w >> 31) || !(w & 0x8000UL))                                                                                 \
    X(7, ((w >> 22) & 3) != 3 && ((w >> 31) || !(w & 0x8000UL)))                                                       \
    X(8, ((w >> 10) & 7) <= 4)                                                                                         \
    X(9, (w >> 31) || ((w >> 10) & 63) >= 32)                                                                          \
    X(10, ((w >> 22) & 3) != 3) /* General-register half FMOV requires later FP16, not A57. */

static int valid_word(unsigned validation, SnxU32 w) {
    switch (validation) {
#define CHECK(id, expr)                                                                                                \
    case id: return !!(expr);
        VALIDATIONS(CHECK)
#undef CHECK
        default: return 1;
    }
}

static void emit_validity(FILE* out, unsigned validation) {
    switch (validation) {
#define PRINT(id, expr)                                                                                                \
    case id: fputs(" if(!(" #expr ")) goto U_;\n", out); break;
        VALIDATIONS(PRINT)
#undef PRINT
        default: break;
    }
}

static unsigned char buckets[2048][sizeof(patterns) / sizeof(patterns[0])];
static unsigned char bucket_sizes[2048];
/* 0 until a thread builds the buckets, 1 while it does, 2 once they are built; through __atomic_*. The runtime JIT's
   compile threads decode at once (jit_service.c): two of them filling bucket_sizes together counted patterns twice */
static int buckets_ready;
static const SnxA64Case undefined_case = {SNX_FLOW_UNDEF, "", "", ""};

static void prepare_buckets(void) {
    unsigned top;
    size_t i;
    SnxU32 mask, value;
    int unbuilt = 0;
    if (__atomic_load_n(&buckets_ready, __ATOMIC_ACQUIRE) == 2) return;
    if (!__atomic_compare_exchange_n(&buckets_ready, &unbuilt, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        /* another thread builds them, which takes about a millisecond, once per process */
        while (__atomic_load_n(&buckets_ready, __ATOMIC_ACQUIRE) != 2) continue;
        return;
    }
    for (top = 0; top < 2048; ++top) {
        value = (SnxU32)top << 21;
        for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
            mask = patterns[i].mask & 0xFFE00000UL;
            if ((value & mask) == (patterns[i].value & mask)) buckets[top][bucket_sizes[top]++] = (unsigned char)i;
        }
    }
    __atomic_store_n(&buckets_ready, 2, __ATOMIC_RELEASE);
}

const SnxA64Pattern* snx_a64_patterns(size_t* count) {
    *count = sizeof(patterns) / sizeof(patterns[0]);
    return patterns;
}

const SnxA64Case* snx_a64_decode(SnxU32 word) {
    unsigned b, i, n;
    size_t index;
    const SnxA64Pattern* p;
    prepare_buckets();
    b = word >> 21;
    for (i = 0; i < bucket_sizes[b]; ++i) {
        p = &patterns[buckets[b][i]];
        if ((word & p->mask) != p->value) continue;
        if (!valid_word(p->validation, word)) return &undefined_case;
        index = 0;
        for (n = 0; n < p->field_count; ++n)
            index = (index << p->fields[n].width) | ((word >> p->fields[n].lo) & ((1UL << p->fields[n].width) - 1));
        return &p->cases[index];
    }
    return &undefined_case;
}

const SnxA64Pattern* snx_a64_case_pattern(const SnxA64Case* instruction, size_t* index) {
    size_t i;
    for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i)
        if (instruction >= patterns[i].cases && instruction < patterns[i].cases + patterns[i].case_count) {
            *index = (size_t)(instruction - patterns[i].cases);
            return &patterns[i];
        }
    *index = 0;
    return NULL;
}

SnxU64 snx_a64_target(SnxU32 word, SnxU64 pc) {
    unsigned width, shift;
    SnxU64 offset;
    if ((word & 0x7C000000UL) == 0x14000000UL) {
        width = 26;
        shift = 0;
    } else if ((word & 0x7E000000UL) == 0x36000000UL) {
        width = 14;
        shift = 5;
    } else {
        width = 19;
        shift = 5;
    }
    offset = (word >> shift) & ((1UL << width) - 1);
    if (offset & (1UL << (width - 1))) offset -= (SnxU64)1 << width;
    return pc + offset * 4;
}

void snx_a64_hex(FILE* out, SnxU64 value) {
    if (value >> 32)
        fprintf(out, "%lX%08lX", (unsigned long)(value >> 32), (unsigned long)(value & 0xFFFFFFFFUL));
    else
        fprintf(out, "%lX", (unsigned long)value);
}

void snx_a64_literal(FILE* out, SnxU64 value) {
    if (value < 0x80000000UL)
        fprintf(out, "%lu", (unsigned long)value);
    else {
        fputs("0x", out);
        snx_a64_hex(out, value);
        fputs("ULL", out);
    }
}

void snx_a64_scope(FILE* out, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction) {
    fprintf(out, " { const u32 w=0x%08lXu; const u64 pc=", (unsigned long)word);
    snx_a64_literal(out, pc);
    fprintf(out, "; %s (void)w; (void)pc; ", instruction->decl);
}

void snx_a64_body(FILE* out, const char* body, const char* undefined) {
    const char *part, *found;
    part = body;
    while ((found = strstr(part, "UNDEF()")) != NULL) {
        fwrite(part, 1, (size_t)(found - part), out);
        fputs(undefined, out);
        part = found + 7;
    }
    fputs(part, out);
}

static void interp_case(FILE* out, const SnxA64Case* c, int step) {
    switch (c->kind) {
        case SNX_FLOW_UNDEF: fputs("goto U_;", out); break;
        case SNX_FLOW_PLAIN:
            snx_a64_body(out, c->first, "goto U_");
            fputs(" goto N_;", out);
            break;
        case SNX_FLOW_B:
        case SNX_FLOW_BR: fprintf(out, step ? "return %s;" : "pc=%s; goto J_;", c->first); break;
        case SNX_FLOW_BC:
            fprintf(out, step ? "if(%s) return %s; goto N_;" : "if(%s){ pc=%s; goto J_; } goto N_;", c->first,
                    c->second);
            break;
        case SNX_FLOW_BL:
        case SNX_FLOW_BLR:
            fprintf(out, "{ u64 t_=%s; c->x[30]=pc+4; ", c->first);
            fputs(step ? "return t_; }" : "CALLDYN(t_); if(c->pc!=pc+4) return; goto N_; }", out);
            break;
        case SNX_FLOW_RET: fprintf(out, step ? "return %s;" : "c->pc=%s; return;", c->first); break;
        case SNX_FLOW_SVC: fprintf(out, "c->pc=pc+4; c->rt->svc(c,%s); goto N_;", c->first); break;
        case SNX_FLOW_BRK:
            fprintf(out, step ? "c->rt->brk(c,pc,%s); return pc;" : "c->pc=pc; c->rt->brk(c,pc,%s); return;", c->first);
            break;
    }
}

static void emit_interp(FILE* out, int step_mode, int checked) {
    unsigned top, j;
    size_t i, k;
    const SnxA64Pattern* p;
    prepare_buckets();
    fputs(step_mode ? "u64 snx_step(CPU* RESTRICT c,u32 w,u64 pc){\n"
                    : "static void snx_interp(CPU* RESTRICT c,u64 e){\n",
          out);
    fputs(" u8* const M=c->mem;\n", out);
    if (!step_mode) {
        fputs(" u64 pc=e; u32 w; if(c->rt->note_hint) c->rt->note_hint(e);\n for(;;){ ", out);
        fputs(checked ? "snx_fetch(c,pc,&w);\n" : "w=RD32(pc);\n", out);
    }
    fputs(" switch(w>>21){\n", out);
    for (top = 0; top < 2048; ++top) {
        if (!bucket_sizes[top]) continue;
        fprintf(out, " case %u:\n", top);
        for (j = 0; j < bucket_sizes[top]; ++j) {
            i = buckets[top][j];
            fprintf(out, " if((w&0x%08lXu)==0x%08lXu) goto E_%lu;\n", (unsigned long)patterns[i].mask,
                    (unsigned long)patterns[i].value, (unsigned long)i);
        }
        fputs(" goto U_;\n", out);
    }
    fputs(" default: goto U_; }\n", out);
    for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        p = &patterns[i];
        fprintf(out, " E_%lu: /* %s */\n", (unsigned long)i, p->name);
        emit_validity(out, p->validation);
        if (p->field_count) fprintf(out, " switch(%s){\n", p->index_expr);
        for (k = 0; k < p->case_count; ++k) {
            if (p->field_count) fprintf(out, " case %lu:", (unsigned long)k);
            fprintf(out, " { %s", p->cases[k].decl);
            interp_case(out, &p->cases[k], step_mode);
            fputs(" }\n", out);
        }
        if (p->field_count) fputs(" default: goto U_; }\n", out);
    }
    if (step_mode)
        fputs(" U_: c->rt->undef(c,pc,w); return pc;\n N_: return pc+4;\n}\n", out);
    else
        fputs(" U_: c->pc=pc; c->rt->undef(c,pc,w); return;\n N_: pc+=4; continue;\n"
              /* every taken branch: a loop the recompiler never found spins here, and gives its core up on poll */
              " J_: { if(UNLIKELY(__atomic_load_n(&c->poll,__ATOMIC_RELAXED))){ c->pc=pc; c->rt->poll(c); }\n"
              /* compiled code takes over where there is some: a miss stays here */
              " GuestFn f_=snx_lookup(c,pc); if(f_!=c->rt->miss){ f_(c,pc); return; } continue; }\n }\n}\n",
              out);
}

void snx_a64_interp(FILE* out, int step_mode) { emit_interp(out, step_mode, 0); }
void snx_a64_interp_checked(FILE* out) { emit_interp(out, 0, 1); }

void snx_a64_static_step(FILE* out, SnxU32 word, SnxU64 pc, const char* name) {
    const SnxA64Case* c;
    c = snx_a64_decode(word);
    fprintf(out, "u64 %s(CPU* RESTRICT c){ u8* const M=c->mem; (void)M;\n", name);
    snx_a64_scope(out, word, pc, c);
    if (c->kind == SNX_FLOW_PLAIN) {
        if (!snx_a64_fast_simd(out, word)) snx_a64_body(out, c->first, "c->rt->undef(c,pc,w)");
        fputs(" return pc+4;", out);
    } else if (c->kind == SNX_FLOW_UNDEF)
        fputs("c->rt->undef(c,pc,w); return pc;", out);
    else
        interp_case(out, c, 1);
    fputs(" }\n N_: return ", out);
    snx_a64_literal(out, pc + 4);
    fputs(";\n}\n", out);
}

/* Preserve the original AOT shortcuts. Unhandled SIMD stays in snx_simd(), the
   same helper used by the fallback interpreter. */
int snx_a64_fast_simd(FILE* out, SnxU32 w) {
    unsigned q, u, size, rd, rn, rm, op, hi, sz, lanes, i, scalar, l, h, mb, index, m;
    unsigned imm5, imm4;
    const char *type, *operation, *acc;
    char lane;
    q = (w >> 30) & 1;
    u = (w >> 29) & 1;
    size = (w >> 22) & 3;
    rd = w & 31;
    rn = (w >> 5) & 31;
    rm = (w >> 16) & 31;
    if ((w & 0x9F200400UL) == 0x0E200400UL) {
        op = (w >> 11) & 31;
        if (op == 3) {
            if (!u && size == 2 && rn == rm) {
                fprintf(out, "c->v[%u].d[0]=c->v[%u].d[0]; c->v[%u].d[1]=", rd, rn, rd);
                if (q)
                    fprintf(out, "c->v[%u].d[1];", rn);
                else
                    fputs("0;", out);
                return 1;
            }
            if (u && size) return 0;
            operation = u ? "^" : (size == 0 ? "&" : size == 1 ? "&~" : size == 2 ? "|" : "|~");
            fprintf(out,
                    "{ u64 a0=c->v[%u].d[0],a1=c->v[%u].d[1],b0=c->v[%u].d[0],b1=c->v[%u].d[1]; "
                    "c->v[%u].d[0]=a0%sb0; c->v[%u].d[1]=",
                    rn, rn, rm, rm, rd, operation, rd);
            if (q)
                fprintf(out, "a1%sb1; }", operation);
            else
                fputs("0; }", out);
            return 1;
        }
        if (op >= 0x18) {
            sz = size & 1;
            hi = size >> 1;
            if (sz && !q) return 0;
            lanes = sz ? 2 : (q ? 4 : 2);
            type = sz ? "d" : "s";
            operation = NULL;
            acc = NULL;
            if (!u && !hi && op == 0x1A)
                operation = "SNX_FP_ADD";
            else if (!u && hi && op == 0x1A)
                operation = "SNX_FP_SUB";
            else if (u && !hi && op == 0x1B)
                operation = "SNX_FP_MUL";
            else if (u && !hi && op == 0x1F)
                operation = "SNX_FP_DIV";
            else if (!u && op == 0x19)
                acc = hi ? (sz ? "^0x8000000000000000ull" : "^0x80000000u") : "";
            if (!operation && !acc) return 0;
            fprintf(out, "{ V128 a=c->v[%u],b=c->v[%u],r", rn, rm);
            if (acc) fprintf(out, "=c->v[%u]", rd);
            fputs("; ", out);
            for (i = 0; i < lanes; ++i) {
                if (acc)
                    fprintf(out, "r.%s[%u]=snx_fp_fma(c,r.%s[%u],a.%s[%u]%s,b.%s[%u],%u); ", type, i, type, i, type, i,
                            acc, type, i, sz ? 64 : 32);
                else
                    fprintf(out, "r.%s[%u]=snx_fp_binary(c,a.%s[%u],b.%s[%u],%u,%s); ", type, i, type, i, type, i,
                            sz ? 64 : 32, operation);
            }
            if (!q) fputs("r.d[1]=0; ", out);
            fprintf(out, "c->v[%u]=r; }", rd);
            return 1;
        }
        if (op == 0x10 && (q || size < 3)) {
            lanes = (q ? 16 : 8) >> size;
            lane = "bhsd"[size];
            fprintf(out, "{ V128 a=c->v[%u],b=c->v[%u],r; ", rn, rm);
            for (i = 0; i < lanes; ++i)
                fprintf(out, "r.%c[%u]=a.%c[%u]%cb.%c[%u]; ", lane, i, lane, i, u ? '-' : '+', lane, i);
            if (!q) fputs("r.d[1]=0; ", out);
            fprintf(out, "c->v[%u]=r; }", rd);
            return 1;
        }
        return 0;
    }
    if ((w & 0x9F000400UL) == 0x0F000000UL || (w & 0xDF000400UL) == 0x5F000000UL) {
        scalar = (w >> 28) & 1;
        op = (w >> 12) & 15;
        if (u || (op != 1 && op != 5 && op != 9) || size < 2) return 0;
        sz = size & 1;
        l = (w >> 21) & 1;
        h = (w >> 11) & 1;
        mb = (w >> 20) & 1;
        if (sz && l) return 0;
        index = sz ? h : ((h << 1) | l);
        m = (mb << 4) | ((w >> 16) & 15);
        if (!scalar && sz && !q) return 0;
        lanes = scalar ? 1 : (sz ? 2 : (q ? 4 : 2));
        type = sz ? "d" : "s";
        fprintf(out, "{ V128 a=c->v[%u],r", rn);
        if (!scalar && op != 9) fprintf(out, "=c->v[%u]", rd);
        fputs("; ", out);
        if (scalar && op != 9) fprintf(out, "%s d0=c->v[%u].%s[0]; ", sz ? "u64" : "u32", rd, type);
        if (scalar || op == 9) fputs("r.d[0]=0; r.d[1]=0; ", out);
        fprintf(out, "%s e=c->v[%u].%s[%u]; ", sz ? "u64" : "u32", m, type, index);
        for (i = 0; i < lanes; ++i) {
            if (op == 9)
                fprintf(out, "r.%s[%u]=snx_fp_binary(c,a.%s[%u],e,%u,SNX_FP_MUL); ", type, i, type, i, sz ? 64 : 32);
            else {
                fprintf(out, "r.%s[%u]=snx_fp_fma(c,", type, i);
                if (scalar)
                    fputs("d0", out);
                else
                    fprintf(out, "r.%s[%u]", type, i);
                fprintf(out, ",a.%s[%u]%s,e,%u); ", type, i,
                        op == 5 ? (sz ? "^0x8000000000000000ull" : "^0x80000000u") : "", sz ? 64 : 32);
            }
        }
        if (!q && !scalar) fputs("r.d[1]=0; ", out);
        fprintf(out, "c->v[%u]=r; }", rd);
        return 1;
    }
    if ((w & 0x9FE08400UL) == 0x0E000400UL) {
        imm5 = (w >> 16) & 31;
        imm4 = (w >> 11) & 15;
        if (!imm5) return 0;
        sz = 0;
        while (!(imm5 & (1U << sz))) ++sz;
        if (sz > 3) return 0;
        index = imm5 >> (sz + 1);
        lane = "bhsd"[sz];
        lanes = (q ? 16 : 8) >> sz;
        if (!u && imm4 == 7 && ((sz == 3) == !!q)) {
            if (rd != 31) fprintf(out, "c->x[%u]=(u64)c->v[%u].%c[%u];", rd, rn, lane, index);
            return 1;
        }
        if (!u && imm4 == 3 && q) {
            fprintf(out, "c->v[%u].%c[%u]=", rd, lane, index);
            if (rn == 31)
                fputs("0;", out);
            else
                fprintf(out, "c->x[%u];", rn);
            return 1;
        }
        if (u && q) {
            fprintf(out, "c->v[%u].%c[%u]=c->v[%u].%c[%u];", rd, lane, index, rn, lane, imm4 >> sz);
            return 1;
        }
        if (!u && (imm4 == 0 || imm4 == 1) && !(sz == 3 && !q)) {
            fprintf(out, "{ V128 r; u%u e=", 8U << sz);
            if (!imm4)
                fprintf(out, "c->v[%u].%c[%u]; ", rn, lane, index);
            else if (rn == 31)
                fputs("0; ", out);
            else
                fprintf(out, "(u%u)c->x[%u]; ", 8U << sz, rn);
            for (i = 0; i < lanes; ++i) fprintf(out, "r.%c[%u]=e; ", lane, i);
            if (!q) fputs("r.d[1]=0; ", out);
            fprintf(out, "c->v[%u]=r; }", rd);
            return 1;
        }
        return 0;
    }
    if ((w & 0xFFFFFFE0UL) == 0x2F00E400UL || (w & 0xFFFFFFE0UL) == 0x6F00E400UL) {
        fprintf(out, "c->v[%u].d[0]=0; c->v[%u].d[1]=0;", rd, rd);
        return 1;
    }
    return 0;
}
