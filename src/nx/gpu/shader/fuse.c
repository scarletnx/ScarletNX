/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fusions of two neighbouring SM86 instructions into one, made before the translation lowers them (fuse.h).
   One pass finds the spans and makes them, in place, in one walk of the array: a pair's first instruction becomes one
   whose fields are the second half's, with the first half's packed into the bits that the second half does not use,
   and its second a NOP, so that no instruction moves. The pass runs after the CALLs are inlined and before the passes
   that move instructions (the globals tracked, the blocks placed), which would invalidate a span found before them. */
#include "nx/gpu/shader/fuse.h"
#include <stdlib.h>

/* the HMMA's A fragment's first register, which a fused pair's LDSM must fill */
#define HMMA_RA 24u
/* an LDSM's fields, which the pass checks: the register its address is in, its immediate offset, the matrices it
   loads and whether it transposes them */
#define LDSM_A   24u
#define LDSM_IMM 40u
#define LDSM_CNT 72u
#define LDSM_TR  78u
static u32 ldsm_imm(const Sm86Insn* in) {
    u32 v = sm86_field(in, LDSM_IMM, 24);
    return v & 0x800000u ? v | 0xff000000u : v;
}

/* the LDSM whose destination is the HMMA's A fragment, from ra on: a register the HMMA kills as D, a fragment the
   LDSM does not fill, a guard the two do not share, or a variant the LDSM's own lowering refuses is not a pair */
static int pair_ok(const Sm86Insn* ldsm, const Sm86Insn* hmma) {
    u32 count = sm86_field(ldsm, LDSM_CNT, 2), ra = sm86_field(hmma, HMMA_RA, 8);
    if (count > 2 || ldsm->dst >= SM86_RZ || ra >= SM86_RZ || ldsm->dst != ra) return 0;
    if (ldsm->pred != hmma->pred || ldsm->pred_not != hmma->pred_not) return 0;
    /* the fields ldsm() rejects: its fused block uses ldsm_words(), which would otherwise accept them */
    if (sm86_field(ldsm, 74, 4) || sm86_field(ldsm, 79, 12) || sm86_field(ldsm, 91, 1)) return 0;
    /* the HMMA's bits that the LDSM's fields are packed into (sm86.h), which must be zero for the OR to keep them */
    if (sm86_field(hmma, 40, 24) || sm86_field(hmma, SM86_FUSE_LDSM_A, SM86_FUSE_LDSM_TR + 1 - SM86_FUSE_LDSM_A))
        return 0;
    u32 fill = 1u << count;
    u32 need = (sm86_field(hmma, 72, 19) >> 3 & 1) ? 4u : 2u; /* A's words, 4 for k16 and 2 for k8 */
    if (fill < need) return 0; /* the unfilled words hold the registers' old values, which the fused block would lose */
    return hmma->dst >= SM86_RZ || hmma->dst + 3 < ldsm->dst || hmma->dst >= ldsm->dst + fill;
}

FuseSpan* sm86_fuse_find(const Sm86Insn* in, u32 n, u32* count) {
    *count = 0;
    if (n < 2) return NULL;
    u32 have = 0, room = 0;
    FuseSpan* spans = NULL;
    for (u32 i = 0; i + 1 < n; i++) {
        const Sm86Insn* first = &in[i];
        if (first->op != SM86_LDSM || in[i + 1].op != SM86_HMMA) continue;
        if (!pair_ok(first, &in[i + 1])) continue;
        if (have == room) { /* the spans' flat array, grown in blocks of eight */
            FuseSpan* more = realloc(spans, (room + 8) * sizeof *spans);
            if (!more) break;
            spans = more, room += 8;
        }
        spans[have].pos = i, spans[have].len = 2, spans[have].tag = SM86_FUSE_LDSM_HMMA;
        have++;
        i++; /* the pair's two instructions, so the next pair starts after them */
    }
    *count = have;
    return have ? spans : (free(spans), (FuseSpan*)NULL);
}

void sm86_fuse_make(Sm86Insn* in, u32 n, FuseSpan* spans, u32 count) {
    if (!spans) return;
    Sm86Insn nop;
    sm86_decode(0x918u | (u64)SM86_PT << 12 | (u64)SM86_RZ << 16, 0, &nop); /* NOP, its destination field RZ */
    for (u32 s = 0; s < count; s++) {
        u32 pos = spans[s].pos, len = spans[s].len;
        if (pos + len > n) break;
        Sm86Insn fused = in[pos + 1]; /* the second half's fields, which the passes and the emitter follow */
        const Sm86Insn* first = &in[pos];
        /* the first half's fields into the bits the second half leaves zero: its immediate where an LDSM puts it, at
           40, and its address register, matrix count and transpose at 91, which is in hi. Its destination is the
           HMMA's A already */
        fused.hi |= (u64)sm86_field(first, LDSM_A, 8) << (SM86_FUSE_LDSM_A - 64) |
                    (u64)sm86_field(first, LDSM_CNT, 2) << (SM86_FUSE_LDSM_CNT - 64) |
                    (u64)sm86_field(first, LDSM_TR, 1) << (SM86_FUSE_LDSM_TR - 64);
        fused.lo |= (u64)ldsm_imm(first) << 40;
        fused.op = (u16)SM86_LDSM_HMMA;
        in[pos] = fused;
        in[pos + 1] = nop;
    }
    free(spans);
}
