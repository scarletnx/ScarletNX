/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic Maxwell memory handles: the conservative translation and the CPU evaluator that runs before a draw. */
#include "nx/gpu/shader/translator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                            \
            return 0;                                                                                                  \
        }                                                                                                              \
    } while (0)

static u64 mov_r(u32 d, u32 a) {
    return 0x5c98078000070000ull | (u64)a << 20 | d;
}

static u64 mov_c(u32 d, u32 bank, u32 offset) {
    return 0x4c98078000070000ull | d | (u64)bank << 34 | (u64)(offset / 4) << 20;
}

static u64 ldg(u32 d, u32 a, u32 size) {
    return 0xeed4200000070000ull | d | (u64)a << 8 | (u64)size << 48;
}

static u64 lop_or(u32 d, u32 a, u32 b) {
    return 0x5c47020000070000ull | d | (u64)a << 8 | (u64)b << 20;
}

static u64 iadd(u32 d, u32 a, u32 b) {
    return 0x5c58000000070000ull | d | (u64)a << 8 | (u64)b << 20;
}

static u64 tld(u32 handle) {
    return (0xdd3a0007a0070200ull & ~(0xffull << 20)) | (u64)handle << 20;
}

static u64 tex(u32 handle) {
    return (0xdeb800000000320aull & ~(0xffull << 20)) | (u64)handle << 20;
}

static u64 txq(u32 handle) {
    return (0xdf50000180470000ull & ~(0xffull << 8)) | (u64)handle << 8;
}

/* Two address pairs in cbuf 0, two 32-bit loads and their OR. The result is R6. */
static u32 handle_chain(u64* w) {
    w[0] = mov_c(0, 0, 0x110);
    w[1] = mov_c(1, 0, 0x114);
    w[2] = ldg(4, 0, 4);
    w[3] = mov_c(2, 0, 0x120);
    w[4] = mov_c(3, 0, 0x124);
    w[5] = ldg(5, 2, 4);
    w[6] = lop_or(6, 4, 5);
    return 7;
}

typedef struct {
    u64 address[2];
    u32 value[2];
    u32 loads;
    int known;
} EvalState;

static u32 eval_cbuf(void* user, u32 bank, u32 offset) {
    EvalState* s = user;
    (void)s;
    if (bank != 0) return 0;
    switch (offset) {
        case 0x110: return 0x1000;
        case 0x114: return 1;
        case 0x120: return 0x2000;
        case 0x124: return 2;
        default: return 0;
    }
}

static int eval_mem(void* user, u64 address, u32* words, u32 count) {
    EvalState* s = user;
    if (!s->known || count != 1) return 0;
    for (u32 k = 0; k < 2; k++) {
        if (address == s->address[k]) {
            *words = s->value[k];
            s->loads++;
            return 1;
        }
    }
    return 0;
}

static int test_evaluator(void) {
    u64 words[8] = {0};
    u32 count = handle_chain(words);
    u32 probes[] = {SH_PROBE(2, 0, 4), SH_PROBE(5, 0, 5), SH_PROBE(6, 0, 6)};
    u32 out[3] = {0};
    EvalState state = {
        .address = {0x100001000ull, 0x200002000ull},
        .value = {0x120, 0x34000},
        .known = 1,
    };
    Sm86EvalIo io = {&state, eval_cbuf, eval_mem};
    CHECK(direct_eval(words, count, &io, probes, 3, out));
    CHECK(state.loads == 2);
    CHECK(out[0] == 0x120 && out[1] == 0x34000 && out[2] == 0x34120);
    u64 memh[8][2] = {{0}};
    for (u32 k = 0; k < count; k++) memh[k][0] = words[k];
    memset(out, 0, sizeof out);
    CHECK(direct_eval_memh(memh, count, &io, probes, 3, out));
    CHECK(out[0] == 0x120 && out[1] == 0x34000 && out[2] == 0x34120);

    state.known = 0;
    memset(out, 0xa5, sizeof out);
    CHECK(!direct_eval(words, count, &io, probes, 3, out));
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

    state.known = 1;
    words[2] = ldg(4, 0, 5);
    memset(out, 0xa5, sizeof out);
    CHECK(!direct_eval(words, count, &io, probes, 3, out));
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

    words[2] = ldg(4, 0, 4) & ~(1ull << 45); /* Non-.E LDG must not consume r[a+1]. */
    memset(out, 0xa5, sizeof out);
    CHECK(!direct_eval(words, count, &io, probes, 3, out));
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    return 1;
}

static u32 guest_program(const u64* w, u32 n, u64* dst) {
    u32 at = 10;
    for (u32 i = 0; i <= n; i++) {
        if ((at - 10) % 4 == 0) dst[at++] = 0x001f8000fc0007e0ull;
        dst[at++] = i < n ? w[i] : 0xe30000000007000full;
    }
    return at;
}

static u32* translate(const u64* words, u32 count, int stage, ShaderInfo* si) {
    u64 program[160] = {0};
    u32 n = guest_program(words, count, program);
    size_t word_count = 0;
    memset(si, 0, sizeof *si);
    si->shared_size = 16;
    si->cs_local[0] = si->cs_local[1] = si->cs_local[2] = 1;
    return shader_to_spirv((u8*)program, n * 8, stage, si, &word_count, SH_FTZ_EMULATE, 0, 0, 0);
}

static int test_tld_txq_and_dedup(void) {
    u64 words[16] = {0};
    u32 count = handle_chain(words);
    words[count++] = tld(6);
    words[count++] = mov_r(8, 6);
    words[count++] = txq(8);
    words[count++] = mov_c(20, 2, 0x40);
    words[count++] = tld(20);
    ShaderInfo si;
    u32* code = translate(words, count, ST_CS, &si);
    if (!code) fprintf(stderr, "translation: %s\n", si.first_unsupported);
    CHECK(code != NULL);
    free(code);
    CHECK(si.nmemh & SH_MEMH_DIRECT);
    CHECK(SH_MEMH_NUM(si.nmemh) == 7);
    CHECK(si.ntex == 2);
    u32 memory = 0, ordinary = 0;
    for (int k = 0; k < si.ntex; k++) {
        if (si.tex[k].flags & SNX_TEX_MEMORY) {
            memory++;
            CHECK(si.tex[k].bindless == 1);
            CHECK(si.tex[k].cbuf_off == SH_PROBE(6, 0, 6));
        } else {
            ordinary++;
            CHECK(si.tex[k].bl_cbuf == 2);
        }
    }
    CHECK(memory == 1 && ordinary == 1);
    CHECK(si.chosen_tex == 0);
    return 1;
}

static int test_iadd_composer(void) {
    u64 words[16] = {0};
    u32 count = handle_chain(words);
    words[6] = iadd(6, 4, 5);
    u32 probes[] = {SH_PROBE(6, 0, 6)};
    u32 out[1] = {0};
    EvalState state = {
        .address = {0x100001000ull, 0x200002000ull},
        .value = {0x120, 0x34000},
        .known = 1,
    };
    Sm86EvalIo io = {&state, eval_cbuf, eval_mem};
    CHECK(direct_eval(words, count, &io, probes, 1, out));
    CHECK(out[0] == 0x34120);
    words[count++] = tld(6);
    ShaderInfo si;
    u32* code = translate(words, count, ST_CS, &si);
    if (!code) fprintf(stderr, "iadd translation: %s\n", si.first_unsupported);
    CHECK(code != NULL);
    free(code);
    CHECK(si.nmemh & SH_MEMH_DIRECT);
    CHECK(si.ntex == 1 && (si.tex[0].flags & SNX_TEX_MEMORY));
    return 1;
}

static int test_tex_no_candidate_fallback(void) {
    u64 words[8] = {0};
    u32 count = handle_chain(words);
    words[count++] = tex(6);
    ShaderInfo si;
    u32* code = translate(words, count, ST_CS, &si);
    if (!code) fprintf(stderr, "translation: %s\n", si.first_unsupported);
    CHECK(code != NULL);
    free(code);
    CHECK(si.nmemh & SH_MEMH_DIRECT);
    CHECK(SH_MEMH_NUM(si.nmemh) == 7);
    CHECK(si.ntex == 1 && (si.tex[0].flags & SNX_TEX_MEMORY));
    CHECK(si.tex[0].cbuf_off == SH_PROBE(6, 0, 6));
    CHECK(si.chosen_tex == 0);
    return 1;
}

static int test_cached_ldg_subtree(void) {
    u64 words[24] = {0};
    u32 count = handle_chain(words);
    words[count++] = mov_c(8, 0, 0x130);
    words[count++] = mov_c(9, 0, 0x134);
    words[count++] = ldg(10, 8, 4);
    words[count++] = lop_or(11, 6, 10); /* R6 is an LDG-containing internal subtree. */
    words[count++] = tld(11);
    words[count++] = tld(6); /* Reusing that subtree must retain its memory provenance. */
    ShaderInfo si;
    u32* code = translate(words, count, ST_CS, &si);
    if (!code) fprintf(stderr, "translation: %s\n", si.first_unsupported);
    CHECK(code != NULL);
    free(code);
    CHECK(SH_MEMH_NUM(si.nmemh) == 11);
    CHECK(si.ntex == 2);
    for (int k = 0; k < si.ntex; k++) CHECK(si.tex[k].flags & SNX_TEX_MEMORY);
    return 1;
}

static int test_rejections(void) {
    u64 words[16] = {0};
    u32 count = handle_chain(words);
    words[2] = ldg(4, 0, 5);
    words[count++] = tld(6);
    ShaderInfo si;
    u32* code = translate(words, count, ST_CS, &si);
    CHECK(code == NULL);
    CHECK(strstr(si.first_unsupported, "untracked texture handle") != NULL);
    CHECK(SH_MEMH_NUM(si.nmemh) == 0);

    memset(words, 0, sizeof words);
    count = handle_chain(words);
    words[count++] = 0x0100000000071006ull; /* @P0 MOV32I R6, 0: a second reaching definition. */
    words[count++] = tld(6);
    code = translate(words, count, ST_CS, &si);
    CHECK(code == NULL);
    CHECK(strstr(si.first_unsupported, "untracked texture handle") != NULL);
    CHECK(SH_MEMH_NUM(si.nmemh) == 0);

    memset(words, 0, sizeof words);
    count = handle_chain(words);
    words[2] = ldg(4, 0, 4) & ~(1ull << 45); /* Only LDG.E has a two-register address. */
    words[count++] = tld(6);
    code = translate(words, count, ST_CS, &si);
    CHECK(code == NULL);
    CHECK(strstr(si.first_unsupported, "untracked texture handle") != NULL);
    CHECK(SH_MEMH_NUM(si.nmemh) == 0);

    memset(words, 0, sizeof words);
    count = handle_chain(words);
    words[count++] = mov_r(8, 6) & ~(0xfull << 39); /* A non-writing MOV_R form is not an alias. */
    words[count++] = txq(8);
    code = translate(words, count, ST_CS, &si);
    CHECK(code == NULL);
    CHECK(strstr(si.first_unsupported, "untracked texture handle") != NULL);
    CHECK(SH_MEMH_NUM(si.nmemh) == 0);

    memset(words, 0, sizeof words);
    words[0] = mov_c(0, 0, 0x110);
    words[1] = mov_c(1, 0, 0x120);
    words[2] = mov_c(2, 0, 0x130);
    words[3] = mov_c(3, 0, 0x140);
    words[4] = lop_or(4, 0, 1);
    words[5] = lop_or(5, 2, 3);
    words[6] = lop_or(6, 4, 5);
    words[7] = tld(6);
    code = translate(words, 8, ST_CS, &si);
    CHECK(code == NULL); /* A nested pure cbuf OR tree is not memory provenance. */
    CHECK(SH_MEMH_NUM(si.nmemh) == 0);
    return 1;
}

int main(void) {
    CHECK(test_evaluator());
    CHECK(test_tld_txq_and_dedup());
    CHECK(test_tex_no_candidate_fallback());
    CHECK(test_iadd_composer());
    CHECK(test_cached_ldg_subtree());
    CHECK(test_rejections());
    puts("shader_direct_memh: PASS");
    return 0;
}
