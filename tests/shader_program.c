/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Program structure (gpu/shader/shader.c, corpus.c): VertexA + VertexB merged, the CRS stack resolved statically,
   and the shader corpus written, found complete or rewritten, and read back. */
#include "nx/gpu/shader/translator.h"

#define SCHED  0x001F8000FC0007E0ull
#define EXIT   0xE30000000007000Full
#define MOV32I 0x010000000007F001ull /* MOV32I R1, imm */
#define SYNC   0xF0F800000007000Full
#define BRK    0xE34000000007000Full
#define NOP    0x50B0000000070F00ull

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* a relative flow instruction at word at to word to (opcode in the top 12 bits) */
static u64 flow(u64 top12, u32 at, u32 to) {
    return top12 << 52 | (u64)(((to - (at + 1)) * 8) & 0xFFFFFF) << 20 | 0x7000Full;
}

/* the program words as guest memory holds them, little-endian, for the functions that read guest bytes */
static void to_guest(u8* dst, const u64* words, u32 n) {
    for (u32 i = 0; i < n; i++) snx_st_le64(dst + 8 * i, words[i]);
}
static int merge_words(const u64* a, const u64* b, u64* out, u32 max_bytes) {
    u8 ga[64 * 8], gb[64 * 8], go[64 * 8] = {0};
    int r;
    to_guest(ga, a, 64);
    to_guest(gb, b, 64);
    r = shader_merge_dual(ga, gb, go, max_bytes);
    for (u32 i = 0; i < 64; i++) out[i] = i < max_bytes / 8 ? snx_ld_le64(go + 8 * i) : 0;
    return r;
}

static void merge(void) {
    u64 a[64] = {0}, b[64] = {0}, out[64];
    int i;
    u32 len = 0;
    for (i = 0; i < 10; i++) {
        a[i] = 0xAAAAAAAA00000000ull | (u64)i;
        b[i] = 0xBBBBBBBB00000000ull | (u64)i;
    }
    a[10] = b[10] = SCHED;
    a[11] = MOV32I;
    a[12] = (EXIT & ~0xF001Full) | 0x30007ull; /* EXIT under P3, flow condition 7 */
    a[13] = EXIT;
    b[11] = MOV32I | 2;
    b[12] = EXIT;
    check(merge_words(a, b, out, sizeof out) == 1, "VertexA and VertexB merge");
    for (i = 0; i < 10; i++) check(out[i] == b[i], "the merged program has VertexB's header");
    check(out[10] == SCHED && out[11] == MOV32I, "VertexA's code comes first");
    check(decode(out[12])->id == O_BRA && bra_target(out[12], 12) == 15 && B(out[12], 16, 4) == 3 &&
              B(out[12], 0, 5) == 7,
          "a conditional EXIT of VertexA branches to VertexB under its predicate and condition");
    check(decode(out[13])->id == O_BRA && bra_target(out[13], 13) == 15 && flow_always(out[13]),
          "VertexA's final EXIT branches to VertexB");
    check(out[14] == SCHED && out[15] == b[11] && out[16] == b[12] && !out[17], "VertexB follows at the next bundle");
    check(merge_words(a, b, out, 16 * 8) == 0, "a merge that doesn't fit fails");
    /* VertexA ending before its last bundle does: the bundle's word after the EXIT is VertexA's, never zero, which no
       instruction is (a game's dual vertex programs end in a BRA to itself with a word left in the bundle) */
    a[12] = EXIT;
    a[13] = NOP;
    check(merge_words(a, b, out, sizeof out) == 1 && out[13] == NOP &&
              out[14] == SCHED && out[15] == b[11],
          "VertexA's last bundle is copied whole");
    /* a zero after VertexA's end would end the merged program before VertexB (shader_hash): it becomes a NOP */
    a[13] = 0;
    check(merge_words(a, b, out, sizeof out) == 1 && out[13] == NOP &&
              out[15] == b[11],
          "a zero word after VertexA becomes a NOP");
    {
        u8 g[sizeof out];
        to_guest(g, out, 64);
        shader_hash(g, sizeof out, &len);
    }
    check(len == 17 * 8, "the merged program's length takes in VertexB");
}

static void crs(void) {
    u64 code[32] = {0};
    u32 pops[32];
    int i;
    for (i = 10; i < 32; i += 4) code[i] = SCHED;
    code[11] = flow(0xE29, 11, 19); /* SSY 19 */
    code[12] = flow(0xE24, 12, 16) & ~0xF0000ull; /* @P0 BRA 16 */
    code[13] = SYNC;
    code[15] = MOV32I;
    code[16] = SYNC;
    code[17] = MOV32I;
    code[19] = EXIT;
    check(shader_resolve_crs(code, 20, pops, NULL, 0) == 1 && pops[13] == 19 && pops[16] == 19,
          "both SYNCs of an SSY region pop to its target");
    code[11] = MOV32I;
    check(shader_resolve_crs(code, 20, pops, NULL, 0) == 0, "a SYNC with no SSY token fails");
    /* PBK 23, SSY 19, BRK: the BRK pops through the SSY token to the PBK's */
    code[11] = flow(0xE2A, 11, 23);
    code[12] = flow(0xE29, 12, 19);
    code[13] = BRK;
    code[15] = code[16] = code[17] = MOV32I;
    code[19] = SYNC;
    code[23] = EXIT;
    check(shader_resolve_crs(code, 24, pops, NULL, 0) == 1 && pops[13] == 23,
          "a BRK drops the SSY token above its own");
    code[17] = flow(0xE26, 17, 21); /* CAL */
    check(shader_resolve_crs(code, 24, pops, NULL, 0) == 0, "a program with calls keeps its stack at run time");
}

/* PRET pushes a return point as PBK pushes a break's, and RET pops it as BRK does: a PRET has no predicate (its bits
   16 to 19 are zero, as UAM writes it), a RET has one. A path that runs on into a return point keeps its token, which
   only matters to a later pop that reaches it */
#define RET_P0 0xE32000000000000Full /* @P0 RET */
#define RET_P1 0xE32000000001000Full /* @P1 RET */
#define RET    0xE32000000007000Full
static u64 pret(u32 at, u32 to) { return 0xE27ull << 52 | (u64)(((to - (at + 1)) * 8) & 0xFFFFFF) << 20; }

static void crs_returns(void) {
    u64 code[32];
    u32 pops[32];
    char why[96];
    int i;
#define RESET()                                                                                                        \
    do {                                                                                                               \
        memset(code, 0, sizeof code);                                                                                  \
        for (i = 10; i < 32; i += 4) code[i] = SCHED;                                                                  \
    } while (0)
    /* the early return UAM writes for a program's return: PRET to the end, @P0 RET, a loop with its PBK and BRK, and
       the loop's exit running on into the PRET's target */
    RESET();
    code[11] = pret(11, 23);
    code[12] = RET_P0;
    code[13] = flow(0xE2A, 13, 21); /* PBK 21 */
    code[15] = MOV32I;
    code[16] = BRK & ~0xF0000ull;   /* @P0 BRK */
    code[17] = flow(0xE24, 17, 15); /* BRA 15 */
    code[21] = MOV32I;
    code[23] = EXIT;
    check(shader_resolve_crs(code, 24, pops, why, sizeof why) == 1 && pops[12] == 23 && pops[16] == 21 && !why[0],
          "a RET returns to its PRET's target, which the path that runs on to it reaches with the token still pushed");
    /* past that target, an SSY and its SYNC above what the two paths disagree on */
    code[23] = flow(0xE29, 23, 27); /* SSY 27 */
    code[24] = SYNC;
    code[27] = EXIT;
    check(shader_resolve_crs(code, 28, pops, why, sizeof why) == 1 && pops[24] == 27,
          "a token pushed above what the paths disagree on pops");
    /* a SYNC there with no SSY of its own: the path from the RET has no token for it */
    code[23] = SYNC;
    check(shader_resolve_crs(code, 28, pops, why, sizeof why) == 0 && !strncmp(why, "SYNC at 0xb8: ", 14),
          "a pop with no token on one of its paths keeps the stack at run time, and says where");
    /* nested: PRET 25 (outer), PRET 21 (inner), @P0 RET and RET to 21, @P1 RET to 25 */
    RESET();
    code[11] = pret(11, 25);
    code[12] = pret(12, 21);
    code[13] = RET_P0;
    code[15] = MOV32I;
    code[16] = RET;
    code[21] = MOV32I;
    code[23] = RET_P1;
    code[24] = MOV32I;
    code[25] = EXIT;
    check(shader_resolve_crs(code, 26, pops, why, sizeof why) == 1 && pops[13] == 21 && pops[16] == 21 &&
              pops[23] == 25,
          "nested returns each go to their own PRET's target");
    /* the inner region running on into its return point: there a RET returns to it again on that path, and to the
       outer one on the path that returned */
    code[16] = MOV32I;
    check(shader_resolve_crs(code, 26, pops, why, sizeof why) == 0 &&
              !strcmp(why, "RET at 0xb8: its paths disagree on where it returns"),
          "a RET whose paths hold different return points keeps the stack at run time, and says why");
    /* two paths that push different return points meet before a RET */
    RESET();
    code[11] = flow(0xE24, 11, 15) & ~0xF0000ull; /* @P0 BRA 15 */
    code[12] = pret(12, 19);
    code[13] = flow(0xE24, 13, 16); /* BRA 16 */
    code[15] = pret(15, 21);
    code[16] = RET;
    code[17] = EXIT;
    code[19] = EXIT;
    code[21] = EXIT;
    check(shader_resolve_crs(code, 22, pops, why, sizeof why) == 0 &&
              !strcmp(why, "RET at 0x80: its paths disagree on where it returns"),
          "a return point that depends on the path keeps the stack at run time");
    /* with no PRET, a RET ends the program, as it did before PRET resolved */
    code[11] = code[12] = code[13] = code[15] = MOV32I;
    check(shader_resolve_crs(code, 22, pops, why, sizeof why) == 1 && pops[16] == 0,
          "a RET with no PRET's token ends the program");
    code[13] = flow(0xE26, 13, 19); /* CAL */
    check(shader_resolve_crs(code, 22, pops, why, sizeof why) == 0 && !strcmp(why, "CAL at 0x68: a call"),
          "a call keeps the stack at run time, and says so");
#undef RESET
}

static long file_size(const char* path) {
    long size;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    return size;
}

static void corpus(const char* dir) {
    u64 code[16] = {0};
    char path[1024];
    ShaderInfo si, back;
    u32 max_bytes = 0;
    int stage = -1;
    u8* loaded;
    long full;
    FILE* f;
    memset(&si, 0, sizeof si);
    si.stage = ST_FS;
    si.rt_int_mask = 0x1234;
    code[10] = SCHED;
    code[11] = EXIT;
    snprintf(path, sizeof path, "%s/00000000c0ffee00_4.snxs", dir);
    remove(path);
    check(shader_corpus_save(dir, (const u8*)code, 96, 0x10000, ST_FS, &si, 0xc0ffee00ull) == 1, "a corpus entry saves");
    full = file_size(path);
    check(full == (long)(24 + sizeof si + 96), "the entry holds the header, the ShaderInfo and the code");
    loaded = shader_corpus_load(path, &max_bytes, &stage, &back);
    check(loaded && max_bytes == 96 && stage == ST_FS && back.rt_int_mask == 0x1234 && !memcmp(loaded, code, 96),
          "the entry loads back with its recorded length as the window");
    free(loaded);
    f = fopen(path, "wb"); /* a write cut short */
    if (f) {
        fwrite("SNXS", 1, 4, f);
        fclose(f);
    }
    check(!shader_corpus_load(path, &max_bytes, &stage, &back), "a cut-off entry doesn't load");
    check(shader_corpus_save(dir, (const u8*)code, 96, 0x10000, ST_FS, &si, 0xc0ffee00ull) == 1 &&
              file_size(path) == full,
          "a cut-off entry is written again");
    remove(path);
}

int main(int argc, char** argv) {
    merge();
    crs();
    crs_returns();
    if (argc > 1) corpus(argv[1]);
    return failures ? 1 : 0;
}
