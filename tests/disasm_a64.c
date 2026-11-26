/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --mcp's A64 listing through the A64 decoder (src/nx/runtime/driver/disasm.c), in a program that links no
   LLVM: hand-encoded words (the Arm ARM's fields, DDI 0487 C6 and C7, as tests/a64_decode_test.c cites them), each listed as a64.decode's name and its
   arguments in their argument set's order; an instruction beyond the Switch's features and one of EL1 by name; a
   word no row matches; the bytes past the last whole word; the targets of the PC-relative branches and ADR; and
   snx_a64_word_fields cut short by a small buffer. */
#include "nx/runtime/driver/disasm.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    SnxU32 word;
    const char* text;
    SnxU64 target; /* from pc, 0 for none */
} Want;

/* at 0x8000100, so a branch back stays above 0 */
#define PC 0x8000100ull

static const Want g_want[] = {
    /* ADD x1, x2, #0x123: sf=1 op=0 S=0 100010 sh=0 imm12=0x123 Rn=2 Rd=1 (C6.2.5); &rri_sf is rd rn imm sf */
    {0x91048c41u, "ADD_i rd=1 rn=2 imm=291 sf=1", 0},
    /* B and BL: imm26 times 4, forward and back (C6.2.26, C6.2.35) */
    {0x14000020u, "B imm=128", PC + 4 + 0x80},
    {0x97ffffffu, "BL imm=-4", PC + 8 - 4},
    /* B.NE +8: imm19=2, cond=0001 (C6.2.27); its arguments in the line's order */
    {0x54000041u, "B_cond c=0 cond=1 imm=8", PC + 12 + 8},
    /* CBZ w3, +16: sf=0 op=0 imm19=4 Rt=3; &cbz is rt imm sf nz */
    {0x34000083u, "CBZ rt=3 imm=16 sf=0 nz=0", PC + 16 + 16},
    /* TBNZ x5, #33, -8: b5=1 op=1 b40=00001 imm14=-2 Rt=5 (C6.2.372); &tbz is rt imm nz bitpos */
    {0xb70fffc5u, "TBZ rt=5 imm=-8 nz=1 bitpos=33", PC + 20 - 8},
    /* ADR x0, -4: immhi all ones, immlo 00 (C6.2.10) */
    {0x10ffffe0u, "ADR rd=0 imm=-4", PC + 24 - 4},
    /* SVC #0x10 and RET: one argument each */
    {0xd4000201u, "SVC imm=16", 0},
    {0xd65f03c0u, "RET rn=30", 0},
    /* MOVZ x0, #0xbeef, LSL #16: sf=1 hw=01 (C6.2.248); &movw is rd sf imm hw */
    {0xd2b7dde0u, "MOVZ rd=0 sf=1 imm=48879 hw=1", 0},
    /* STP x29, x30, [sp, #-16]!: opc=10 imm7=-2 Rt2=30 Rn=31 Rt=29, pre-index: the writeback w and no post-index p */
    {0xa9bf7bfdu, "STP rt2=30 rt=29 rn=31 imm=-2 sz=3 sign=0 w=1 p=0", 0},
    /* FADD v0.4s, v1.4s, v2.4s: Q=1 sz=0, single precision esz=2 */
    {0x4e22d420u, "FADD_v q=1 rd=0 rn=1 rm=2 esz=2", 0},
    /* LDADD w1, w2, [x3]: FEAT_LSE, which the Switch's Cortex-A57 lacks, still by name (C6.2.136) */
    {0xb8210062u, "LDADD rs=1 rn=3 rt=2 a=0 r=0 sz=2", 0},
    /* ERET: UNDEF at EL0, by name all the same (C6.2.117) */
    {0xd69f03e0u, "ERET", 0},
    /* all zero: UDF #0, permanently undefined, no row's */
    {0x00000000u, "<unknown>", 0},
};
#define COUNT (sizeof g_want / sizeof g_want[0])

static struct {
    int failures;
    size_t lines;
} g_test;

static void line(void* ctx, SnxU64 address, SnxU32 word, const char* text) {
    (void)ctx;
    size_t i = g_test.lines++;
    if (i >= COUNT) {
        fprintf(stderr, "disasm_a64: a line past the %zu words: 0x%llx %08x %s\n", COUNT, (unsigned long long)address,
                (unsigned)word, text);
        g_test.failures++;
        return;
    }
    const Want* w = &g_want[i];
    SnxU64 target = snx_disasm_a64_target(word, address);
    SnxU64 want_target = w->target;
    if (address != PC + 4 * i || word != w->word || strcmp(text, w->text) || target != want_target) {
        fprintf(stderr,
                "disasm_a64: line %zu is 0x%llx %08x \"%s\" target 0x%llx, expected 0x%llx %08x \"%s\" target 0x%llx\n",
                i, (unsigned long long)address, (unsigned)word, text, (unsigned long long)target,
                (unsigned long long)(PC + 4 * i), (unsigned)w->word, w->text, (unsigned long long)want_target);
        g_test.failures++;
    }
}

static void cut(size_t size, const char* want) {
    char out[64];
    memset(out, 'x', sizeof out);
    int found = snx_a64_word_fields(0x91048c41u, out, size);
    if (!found || (size && strcmp(out, want)) || out[size] != 'x') {
        fprintf(stderr,
                "disasm_a64: ADD_i in %zu bytes reads \"%.*s\" (found %d), expected \"%s\" and nothing past it\n", size,
                (int)size, out, found, want);
        g_test.failures++;
    }
}

int main(void) {
    /* little-endian words, then two bytes of a word that isn't there */
    unsigned char code[COUNT * 4 + 2];
    for (size_t i = 0; i < COUNT; i++)
        for (size_t b = 0; b < 4; b++) code[i * 4 + b] = (unsigned char)(g_want[i].word >> (8 * b));
    code[COUNT * 4] = 0x1f;
    code[COUNT * 4 + 1] = 0x20;
    snx_disasm_a64(code, sizeof code, PC, line, NULL);
    if (g_test.lines != COUNT) {
        fprintf(stderr, "disasm_a64: %zu lines, expected %zu\n", g_test.lines, COUNT);
        g_test.failures++;
    }
    /* a word no row matches leaves its buffer empty */
    char none[8] = "x";
    if (snx_a64_word_fields(0x00000000u, none, sizeof none) || none[0]) {
        fprintf(stderr, "disasm_a64: UDF #0 reads \"%s\", expected no instruction and an empty text\n", none);
        g_test.failures++;
    }
    /* a buffer too small holds the text's start, ended; one of no bytes is left alone */
    cut(10, "ADD_i rd=");
    cut(5, "ADD_");
    cut(0, "");
    cut(sizeof "ADD_i rd=1 rn=2 imm=291 sf=1", "ADD_i rd=1 rn=2 imm=291 sf=1");
    if (g_test.failures) {
        fprintf(stderr, "disasm_a64: %d failures\n", g_test.failures);
        return 1;
    }
    printf("disasm_a64: ok, %zu words\n", COUNT);
    return 0;
}
