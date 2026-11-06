/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/translator.h"
#include <stdio.h>
#include <string.h>

#define SCHED 0x001F8000FC0007E0ull
#define EXIT  0xE30000000007000Full
#define NOP   0x50B0000000070F00ull

static int failures, reads, invalid_second;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* the program of the hoisted-load cases: IMNMX and SHL bound the index, SSY 19 and the LDC, @P0 SYNC and SYNC
   reconverge at the BRX at 19, whose table entries (24) lead to the EXIT at 23 */
static void hoisted_program(u64* code) {
    memset(code, 0, 64 * sizeof *code);
    for (u32 i = 10; i < 30; i += 4) code[i] = SCHED;
    code[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xe290000002800000ull; /* SSY 19 */
    code[15] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    code[16] = 0xf0f800000000000full; /* @P0 SYNC */
    code[17] = 0xf0f800000007000full; /* SYNC */
    code[19] = 0xe250000000070d0full; /* BRX R13 */
    code[20] = code[21] = code[23] = EXIT;
}

/* shader_hash reads a program as guest memory holds it, little-endian: the host words laid out so first */
static u64 hash_words(const u64* words, u32 bytes, u32* len) {
    u8 b[64 * 8];
    for (u32 i = 0; i < bytes / 8; i++) snx_st_le64(b + 8 * i, words[i]);
    return shader_hash(b, bytes, len);
}

static int table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    reads++;
    *value = offset == 0 || (offset == 4 && !invalid_second) ? 24 : 1024;
    return 1;
}

int main(void) {
    u64 code[64] = {0};
    code[10] = SCHED;
    code[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    code[14] = SCHED;
    code[15] = 0xe250000000070d0full; /* BRX R13 */
    code[16] = EXIT;
    code[17] = NOP; /* live code fills its bundles: a zero word is padding after the program */
    code[18] = SCHED;
    code[19] = EXIT;
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = table;
    u32 len = 0;
    hash_words(code, sizeof code, &len);
    check(len == 20 * 8 && reads == 2, "clamped BRX reads only its two live table entries");
    u32 targets[64];
    reads = 0;
    check(shader_brx_targets(code, 15, 64, targets, 64) == 2 && reads == 2 && targets[0] == 19 && targets[1] == 19,
          "a clamped BRX table gives exactly its two targets");

    reads = 0;
    invalid_second = 1;
    code[20] = code[21] = NOP;
    code[23] = EXIT;
    hash_words(code, sizeof code, &len);
    check(len == 24 * 8 && reads == 2, "invalid clamped target leaves BRX unresolved");
    invalid_second = 0;
    code[20] = code[21] = code[23] = 0;

    reads = 0;
    code[13] |= 1ull << 49; /* LDC.B64: 8-byte entries whose low words the BRX would take, not a word table */
    hash_words(code, sizeof code, &len);
    check(len == 20 * 8 && reads == 0, "a non-B32 load leaves the BRX unresolved");

    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = 0xe250000000070d0full; /* unresolved BRX */
    code[12] = EXIT;
    g_shader_cb_read = NULL;
    hash_words(code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX stops after the final exit before zero padding");
    code[13] = NOP;
    code[15] = EXIT;
    hash_words(code, sizeof code, &len);
    check(len == 16 * 8, "unresolved BRX includes live code beyond an earlier exit");
    /* NVN's pool puts the next program after the padding: an unknown table mustn't take it for this one's code */
    code[13] = 0;
    hash_words(code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX ends at the zero padding before the next program");

    u64 distant[64] = {0};
    for (u32 i = 10; i <= 49; i++) distant[i] = (i - 10) % 4 ? NOP : SCHED;
    distant[11] = 0x3820000000170b0bull; /* clamp R11 to two entries */
    distant[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    distant[13] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    distant[45] = 0xe250000000070d0full; /* BRX R13, 32 instructions after LDC */
    distant[49] = EXIT;
    g_shader_cb_read = table;
    reads = 0;
    hash_words(distant, sizeof distant, &len);
    check(len == 50 * 8 && reads == 2, "straight-line distant LDC resolves clamped BRX");
    distant[31] = 0x5c9807800ff7000dull; /* MOV R13, RZ invalidates the table value */
    reads = 0;
    hash_words(distant, sizeof distant, &len);
    check(reads == 0, "a later write to the BRX register rejects distant LDC");
    distant[31] = NOP;
    distant[13] |= 1ull << 49;
    reads = 0;
    hash_words(distant, sizeof distant, &len);
    check(reads == 0, "distant BRX needs a verified 32-bit table load");

    /* The compiler loads the entry inside an if-else (SSY ... SYNC) and branches after it reconverges */
    u64 hoisted[64] = {0};
    for (u32 i = 10; i < 26; i += 4) hoisted[i] = SCHED;
    hoisted[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    hoisted[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    hoisted[13] = 0xe290000002800000ull; /* SSY 19 */
    hoisted[15] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    hoisted[16] = 0xf0f800000000000full; /* @P0 SYNC */
    hoisted[17] = 0xf0f800000007000full; /* SYNC */
    hoisted[19] = 0xe250000000070d0full; /* BRX R13 */
    hoisted[20] = hoisted[21] = hoisted[23] = EXIT;
    reads = 0;
    check(shader_brx_targets(hoisted, 19, 64, targets, 64) == 2 && reads == 2 && targets[0] == 23 &&
              targets[1] == 23,
          "a table loaded before an if-else reaches the BRX after it");
    hash_words(hoisted, sizeof hoisted, &len);
    check(len == 24 * 8, "a resolved table loaded before an if-else bounds the program");
    hoisted[15] = 0xf0f800000000000full; /* @P0 SYNC: one side skips the load */
    hoisted[16] = 0xef94000000070d0dull;
    reads = 0;
    check(shader_brx_targets(hoisted, 19, 64, targets, 64) == -1 && reads == 0,
          "a path around the table load leaves the BRX unresolved");

    u32 pops[64];
    hoisted_program(hoisted);
    check(shader_resolve_crs(hoisted, 24, pops) && pops[16] == 19 && pops[17] == 19,
          "the CRS stack resolves through a resolved hoisted table");

    hoisted_program(hoisted);
    hoisted[21] = 0xe2400ffffd87000full; /* BRA 17, after the BRX: a way into the region that skips the load */
    reads = 0;
    check(shader_brx_targets(hoisted, 19, 64, targets, 64) == -1 && reads == 0,
          "a branch into the region from after the BRX leaves it unresolved");

    hoisted_program(hoisted);
    hoisted[19] = 0xe2500ffffd070d0full; /* BRX R13 - 48: its cases would land at 17, back in the region */
    check(shader_brx_targets(hoisted, 19, 64, targets, 64) == -1, "a table leading back before its BRX is refused");

    hoisted_program(hoisted);
    hoisted[13] = 0xe230000006800000ull; /* PEXIT 27, an exit token whose target lies past the BRX */
    hoisted[16] = 0xe30000000000000full; /* @P0 EXIT: goes to it */
    hoisted[17] = NOP;
    hoisted[27] = EXIT;
    reads = 0;
    check(shader_brx_targets(hoisted, 19, 64, targets, 64) == 2 && reads == 2 && targets[0] == 23,
          "an exit token pushed before the load leaves the table resolved");

    /* nested tokens: PBK 23, the load, SSY 21 with @P0 SYNC and SYNC, then the BRK at 21 */
    hoisted_program(hoisted);
    hoisted[13] = 0xe2a0000004800000ull; /* PBK 23 */
    hoisted[16] = 0xe290000002000000ull; /* SSY 21 */
    hoisted[17] = 0xf0f800000000000full; /* @P0 SYNC */
    hoisted[19] = 0xf0f800000007000full; /* SYNC */
    hoisted[20] = NOP;
    hoisted[21] = 0xe34000000007000full; /* BRK */
    hoisted[23] = 0xe250000000070d0full; /* BRX R13: entries lead to 27 */
    hoisted[24] = hoisted[25] = hoisted[27] = EXIT;
    reads = 0;
    check(shader_brx_targets(hoisted, 23, 64, targets, 64) == 2 && reads == 2 && targets[0] == 27 &&
              targets[1] == 27,
          "a table loaded inside nested SSY and PBK tokens resolves");

    /* a loop: the load at 19, then @P0 BRA 16 back to a write of R13 that the BRA at 17 takes to the BRX */
    hoisted_program(hoisted);
    hoisted[13] = 0xe290000005800000ull; /* SSY 25 */
    hoisted[15] = 0xe24000000187000full; /* BRA 19 */
    hoisted[16] = 0x5c9807800ff7000dull; /* MOV R13, RZ */
    hoisted[17] = 0xe24000000287000full; /* BRA 23 */
    hoisted[19] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    hoisted[20] = 0xe2400ffffd80000full; /* @P0 BRA 16 */
    hoisted[21] = 0xf0f800000007000full; /* SYNC */
    hoisted[23] = 0xf0f800000007000full; /* SYNC */
    hoisted[24] = NOP;
    hoisted[25] = 0xe250000000070d0full; /* BRX R13 */
    hoisted[27] = EXIT;
    reads = 0;
    check(shader_brx_targets(hoisted, 25, 64, targets, 64) == -1 && reads == 0,
          "a loop back to a write before the load leaves the BRX unresolved");
    hoisted[20] = NOP; /* without the loop the write can't be reached */
    check(shader_brx_targets(hoisted, 25, 64, targets, 64) == 2, "the same program without the loop resolves");

    /* an unresolved BRX, then the compiler's end (EXIT and a BRA to itself) and the jump table right after it, as one
       game's program keeps it: the program ends at that BRA, not at the zero after the table, whose offsets aren't
       instructions */
    u64 tail[64] = {0};
    g_shader_cb_read = NULL;
    for (u32 i = 10; i < 30; i += 4) tail[i] = SCHED;
    tail[11] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13]: no table to read */
    tail[12] = 0xe250000000070d0full; /* BRX R13 */
    tail[13] = EXIT;
    tail[15] = EXIT;
    tail[16] = 0xe2400fffff87000full; /* BRA to itself */
    tail[17] = NOP;
    tail[19] = 0x00000318000002e0ull; /* the table's offsets */
    tail[20] = 0x0000039000000350ull;
    tail[21] = 0x00000400000003c0ull;
    hash_words(tail, sizeof tail, &len);
    check(len == 17 * 8, "the compiler's end ends a program whose BRX is unresolved");
    tail[15] = NOP; /* a BRA to itself not after an EXIT ends nothing: the zero after the table does */
    hash_words(tail, sizeof tail, &len);
    check(len == 22 * 8, "a BRA to itself alone doesn't end an unresolved program");
    g_shader_cb_read = old;
    return failures ? 1 : 0;
}
