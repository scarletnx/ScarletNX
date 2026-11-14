/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Constant buffer words folded into a translation (gpu/vk/cb_fold.c), with no GPU: which words fold, what a
   translation records of each load at a constant place, when it still holds for a draw's buffers, when one that
   folds more is worth making, and how many of a program's translations fold by value before one folds only what
   they agree on */
#include "nx/gpu/vk/cb_fold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/endian.h"
#include "nx/gpu/shader/shader.h"

#define WORDS 16
#define ONE   0x3F800000u
#define PI    0x40490FDBu
static struct {
    unsigned checks, failures;
    /* the draw's buffers: bank 2 is WORDS words, bank 3 is not bound */
    u8 bank[WORDS * 4];
    u32 held;                  /* the byte offset in bank 2 the GPU holds, which no read may land: ~0 for none */
    unsigned reads, ranges;    /* how many times the word reader and the byte reader were asked */
    u32 range_off, range_size; /* the last range of bytes asked for */
} g_test = {.held = ~0u};
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static void set(u32 word, u32 value) { snx_st_le32(g_test.bank + word * 4, value); }
/* the reader of a BRX's table: it lands what the GPU holds, so it answers every word of the bank */
static int reader(u32 idx, u32 off, u32* out) {
    g_test.reads++;
    if (idx != 2 || off / 4 >= WORDS) return 0;
    *out = snx_ld_le32(g_test.bank + off);
    return 1;
}
/* the folds' reader: the bytes, unless the GPU holds some of them */
static const u8* bytes(u32 idx, u32 off, u32 size) {
    g_test.ranges++;
    g_test.range_off = off;
    g_test.range_size = size;
    if (idx != 2 || size > sizeof g_test.bank || off > sizeof g_test.bank - size) return NULL;
    if (g_test.held >= off && g_test.held < off + size) return NULL;
    return g_test.bank + off;
}
static int note(VkCbReads* reads, const VkCbFold* fold, u32 off, u32* out) {
    int failed = 0, folds = vk_cb_fold_note(reads, fold, 2, off, out, &failed);
    CHECK(!failed, "a record grows");
    return folds;
}

static void values(void) {
    const struct {
        u32 word;
        int folds;
    } cases[] = {{0, 1},           {1, 1},           {255, 1},         {256, 0},         {ONE, 1},
                 {ONE - 1, 0},     {ONE + 1, 0},     {0xBF800000u, 0}, {0x40000000u, 0}, {0x3F000000u, 0},
                 {0x80000000u, 0}, {0xFFFFFFFFu, 0}, {0x00010000u, 0}, {0x3F800100u, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        if (shader_cb_fold_value(cases[i].word) != cases[i].folds) printf("word %08x\n", cases[i].word);
        CHECK(shader_cb_fold_value(cases[i].word) == cases[i].folds,
              "zero, 1.0f and a whole number up to 255 fold, and nothing else");
    }
}

static void notes(void) {
    const VkCbFold by_value = {.mode = VK_CB_FOLD_VALUES, .bytes = bytes};
    VkCbReads reads = {0};
    u32 out = 77;
    int failed = 0;
    memset(g_test.bank, 0, sizeof g_test.bank);
    set(0, ONE);
    set(1, PI);
    set(2, 7);
    g_test.ranges = 0;
    CHECK(note(&reads, &by_value, 0, &out) == 1 && out == ONE, "a word of 1.0f is the translation's constant");
    CHECK(note(&reads, &by_value, 4, &out) == 0, "a word of any other value stays a load");
    CHECK(note(&reads, &by_value, 8, &out) == 1 && out == 7, "a small whole number is the translation's constant");
    CHECK(vk_cb_fold_note(&reads, &by_value, 3, 0, &out, &failed) == 0 && !failed,
          "a word of a buffer that is not bound stays a load");
    CHECK(reads.n == 4 && g_test.ranges == 4, "each word is read once and recorded once");
    CHECK(reads.r[0].ok == VK_CB_FOLD && reads.r[0].v == ONE && reads.r[0].idx == 2 && reads.r[0].off == 0 &&
              reads.r[1].ok == VK_CB_LOAD && reads.r[2].ok == VK_CB_FOLD && reads.r[2].v == 7 &&
              reads.r[3].ok == VK_CB_LOAD && reads.r[3].idx == 3,
          "the record says which words fold, to what, and which stay loads");
    /* the same word again, in another block of the program: the first answer, whatever the buffer holds by then */
    set(0, PI);
    set(1, 0);
    out = 77;
    CHECK(note(&reads, &by_value, 0, &out) == 1 && out == ONE && note(&reads, &by_value, 4, &out) == 0 &&
              reads.n == 4 && g_test.ranges == 4,
          "a word met again gets its first answer without another read");
    CHECK(vk_cb_fold_count(&reads) == 2, "the folded words are counted");
    free(reads.r);
    /* a word the GPU holds: reading it would land it, so it is left a load */
    reads = (VkCbReads){0};
    memset(g_test.bank, 0, sizeof g_test.bank);
    g_test.held = 5;
    CHECK(note(&reads, &by_value, 4, &out) == 0 && note(&reads, &by_value, 8, &out) == 1 && reads.r[0].ok == VK_CB_LOAD,
          "a word only a landing read gets stays a load");
    g_test.held = ~0u;
    free(reads.r);
    /* more words than one growth of the record holds */
    reads = (VkCbReads){0};
    for (u32 n = 0; n < 40; n++) {
        u32 bank = n / WORDS;
        CHECK(vk_cb_fold_note(&reads, &by_value, 2 + bank, n % WORDS * 4, &out, &failed) == (bank == 0) && !failed,
              "a long record grows");
    }
    CHECK(reads.n == 40 && vk_cb_fold_count(&reads) == WORDS, "forty loads are forty records");
    free(reads.r);
}

static void holds(void) {
    /* as a translation leaves them: a load, a fold, a BRX's read, a fold of another bank's order, a read of no
       buffer, a load, a fold */
    VkCbRead r[7] = {{2, 4, VK_CB_LOAD, 0},   {2, 40, VK_CB_FOLD, ONE}, {2, 0, VK_CB_READ, 11},
                     {2, 8, VK_CB_FOLD, ONE}, {3, 0, VK_CB_UNBOUND, 0}, {2, 12, VK_CB_LOAD, 0},
                     {2, 16, VK_CB_FOLD, 0}};
    VkCbReads reads = {r, 7};
    vk_cb_reads_sort(&reads);
    CHECK(r[0].ok == VK_CB_READ && r[1].ok == VK_CB_UNBOUND && r[2].ok == VK_CB_FOLD && r[2].off == 8 &&
              r[3].ok == VK_CB_FOLD && r[3].off == 16 && r[4].ok == VK_CB_FOLD && r[4].off == 40 &&
              r[5].ok == VK_CB_LOAD && r[5].off == 4 && r[6].ok == VK_CB_LOAD && r[6].off == 12,
          "the reads come first as the translation met them, then the folds by offset, then the loads");
    memset(g_test.bank, 0, sizeof g_test.bank);
    set(0, 11);
    set(1, 0x12345678u);
    set(2, ONE);
    set(3, 0x9ABCDEF0u);
    set(10, ONE);
    g_test.reads = g_test.ranges = 0;
    CHECK(vk_cb_folds_hold(&reads, reader, bytes) && g_test.reads == 2 && g_test.ranges == 1 && g_test.range_off == 8 &&
              g_test.range_size == 36,
          "a translation holds while its reads and folds do: a bank's folds from one range, and no load read");
    set(1, 5);
    set(3, 6);
    CHECK(vk_cb_folds_hold(&reads, reader, bytes), "a load holds whatever its word");
    set(2, 0);
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "a folded word that changed no longer holds");
    set(2, ONE);
    set(4, 1);
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "nor does one folded to zero that is zero no more");
    set(4, 0);
    set(10, PI);
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "nor the last of a bank's folds");
    set(10, ONE);
    set(0, 12);
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "nor a read that changed");
    set(0, 11);
    r[1].idx = 2; /* the read of no buffer, now of one that is bound */
    r[1].off = 20;
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "a read of no buffer holds only while there is none");
    r[1].idx = 3;
    CHECK(vk_cb_folds_hold(&reads, reader, bytes), "and all of them together hold again");
    g_test.held = 24; /* between two folds: the range that holds them is the GPU's */
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "folds whose bytes the GPU holds do not hold, and nothing lands");
    g_test.held = ~0u;
    r[4].idx = 3; /* a fold whose buffer is gone: a run of its own */
    g_test.ranges = 0;
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes) && g_test.ranges == 2 && g_test.range_size == 4,
          "a folded word whose buffer is not bound does not hold");
    r[4].idx = 2;
    /* a compute program's reads, which fold nothing, through the word reader alone */
    VkCbRead brx[2] = {{2, 0, VK_CB_READ, 11}, {3, 0, VK_CB_UNBOUND, 0}};
    const VkCbReads table = {brx, 2};
    CHECK(vk_cb_reads_hold(&table, reader), "reads alone hold by the word reader");
    set(0, 12);
    CHECK(!vk_cb_reads_hold(&table, reader), "and stop holding with their word");
}

/* a bank's reads next to each other, checked from one range of the bytes they span: no word through the reader while
   the range is there, and each word through it where the GPU holds bytes of the span or a read lies past the bank */
static void batched_reads(void) {
    VkCbRead r[3] = {{2, 8, VK_CB_READ, 7}, {2, 4, VK_CB_READ, 11}, {2, 20, VK_CB_READ, 13}};
    VkCbReads reads = {r, 3};
    memset(g_test.bank, 0, sizeof g_test.bank);
    set(1, 11);
    set(2, 7);
    set(5, 13);
    g_test.reads = g_test.ranges = 0;
    CHECK(vk_cb_folds_hold(&reads, reader, bytes) && g_test.reads == 0 && g_test.ranges == 1 && g_test.range_off == 4 &&
              g_test.range_size == 20,
          "a bank's reads hold from one range of the bytes they span, none through the word reader");
    set(5, 14);
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "and stop holding with the last word of the range");
    set(5, 13);
    set(1, 12);
    CHECK(!vk_cb_folds_hold(&reads, reader, bytes), "or with its first");
    set(1, 11);
    g_test.held = 12; /* between two reads: the span is the GPU's */
    g_test.reads = 0;
    CHECK(vk_cb_folds_hold(&reads, reader, bytes) && g_test.reads == 3,
          "bytes the GPU holds in the span: each word through the reader, which lands them");
    g_test.held = ~0u;
    r[2] = (VkCbRead){2, WORDS * 4, VK_CB_UNBOUND, 0}; /* past the bank: the range has no such bytes */
    g_test.reads = 0;
    CHECK(vk_cb_folds_hold(&reads, reader, bytes) && g_test.reads == 3,
          "a read past the bank: each word through the reader, and the one past it holds unbound");
}

static void more(void) {
    VkCbRead r[3] = {{2, 0, VK_CB_FOLD, 0}, {2, 4, VK_CB_LOAD, 0}, {2, 8, VK_CB_LOAD, 0}};
    VkCbRead agrees[2] = {{2, 4, VK_CB_FOLD, ONE}, {2, 8, VK_CB_LOAD, 0}};
    VkCbRead differs[1] = {{2, 4, VK_CB_FOLD, 0}};
    const VkCbReads reads = {r, 3}, a = {agrees, 2}, d = {differs, 1};
    const VkCbReads* one[1] = {&a};
    const VkCbReads* both[2] = {&a, &d};
    const VkCbFold by_value = {.mode = VK_CB_FOLD_VALUES, .bytes = bytes};
    const VkCbFold agree = {.mode = VK_CB_FOLD_AGREE, .bytes = bytes, .others = one, .nothers = 1};
    const VkCbFold agree_both = {.mode = VK_CB_FOLD_AGREE, .bytes = bytes, .others = both, .nothers = 2};
    const VkCbFold values_both = {.mode = VK_CB_FOLD_VALUES, .bytes = bytes, .others = both, .nothers = 2};
    VkCbReads made = {0};
    u32 out;
    memset(g_test.bank, 0, sizeof g_test.bank);
    set(0, ONE); /* the fold no longer holds: not what "more" asks */
    set(1, PI);
    set(2, PI);
    CHECK(!vk_cb_fold_more(&reads, &by_value), "loads of words that do not fold: nothing more to fold");
    set(1, ONE);
    CHECK(vk_cb_fold_more(&reads, &by_value), "a load whose word now folds: one more translation folds it");
    CHECK(vk_cb_fold_more(&reads, &agree), "and so where the program's other translation folds it the same");
    CHECK(!vk_cb_fold_more(&reads, &agree_both), "but not where another folds it to something else");
    CHECK(vk_cb_fold_more(&reads, &values_both), "which a translation by value does not look at");
    g_test.held = 6;
    CHECK(!vk_cb_fold_more(&reads, &by_value), "nor is a word the GPU holds a reason for one more");
    g_test.held = ~0u;
    set(1, PI);
    set(2, 0);
    CHECK(vk_cb_fold_more(&reads, &by_value) && !vk_cb_fold_more(&reads, &agree),
          "nor a word another translation left a load");
    /* the translation that folds what the others agree on */
    set(0, 0);
    set(1, ONE);
    set(2, 0);
    set(3, 1);
    CHECK(note(&made, &agree, 0, &out) == 1, "a word no other translation loads folds by its value");
    CHECK(note(&made, &agree, 4, &out) == 1 && out == ONE, "a word the other folds to the same folds");
    CHECK(note(&made, &agree, 8, &out) == 0, "a word the other left a load stays one");
    CHECK(note(&made, &agree, 12, &out) == 1, "a word of a value that folds, which no other met, folds");
    free(made.r);
    made = (VkCbReads){0};
    CHECK(note(&made, &agree_both, 4, &out) == 0, "a word two translations fold to different words stays a load");
    free(made.r);
}

static void modes(void) {
    CHECK(SH_CB_FOLD_VARIANTS == 2, "two translations by value, as docs/layers/gpu/constant-buffer-folds.md measures");
    for (u32 made = 0; made < SH_CB_FOLD_VARIANTS; made++)
        CHECK(vk_cb_fold_mode(made) == VK_CB_FOLD_VALUES, "a program's first translations fold by value");
    CHECK(vk_cb_fold_mode(SH_CB_FOLD_VARIANTS) == VK_CB_FOLD_AGREE, "then one folds what they agree on");
    CHECK(vk_cb_fold_mode(SH_CB_FOLD_VARIANTS + 1) == VK_CB_FOLD_NONE &&
              vk_cb_fold_mode(SH_CB_FOLD_VARIANTS + 2) == VK_CB_FOLD_NONE && vk_cb_fold_mode(1000) == VK_CB_FOLD_NONE,
          "and none after it folds");
}

int main(void) {
    values();
    notes();
    holds();
    batched_reads();
    more();
    modes();
    printf("vk_cb_fold: %u checks, %u failed\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
