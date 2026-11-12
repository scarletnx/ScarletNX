/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The linker (nx/loader/relocate.h): three modules in load order, a, b and c, each a page of tables and a quarter page
   of data, with every kind of relocation and every case of the rule a symbol is bound by. They are linked into copies
   of their data, then in place, as a game's own linker relocates them, and the two are compared */
#include "nx/loader/relocate.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* where a module's parts lie */
enum { MOD0 = 0x40, DYNAMIC = 0x80, SYMTAB = 0x200, STRTAB = 0x2c0, RELA = 0x400, JMPREL = 0x500, RELR = 0x600 };
enum { DATA = 0x800, DATA_SIZE = 0x400, SIZE = 0x1000 };
enum { ABS64 = 257, ABS32 = 258, GLOB_DAT = 1025, JUMP_SLOT = 1026, RELATIVE = 1027, TLS = 1028 };
#define BASE_A 0x100000ull
#define BASE_B 0x200000ull
#define BASE_C 0x300000ull

typedef struct {
    unsigned char* image;
    unsigned symbols, names, rela, jmprel, relr;
} Build;

/* an empty module: MOD0 and room for seven symbols (the strings follow them, which tells how many they are) */
static void begin(Build* b, unsigned char* image) {
    memset(image, 0, SIZE);
    memset(b, 0, sizeof(*b));
    b->image = image;
    b->names = 1;
    snx_st_le32(image + 4, MOD0);
    memcpy(image + MOD0, "MOD0", 4);
    snx_st_le32(image + MOD0 + 4, DYNAMIC - MOD0);
}
/* a symbol: info its binding and type, other its visibility, section 0 for one the module does not define. Its
   index */
static unsigned symbol(Build* b, const char* name, unsigned info, unsigned other, unsigned section, SnxU64 value) {
    unsigned char* s = b->image + SYMTAB + 24 * ++b->symbols;
    size_t n = strlen(name) + 1;
    snx_st_le32(s, b->names);
    s[4] = (unsigned char)info;
    s[5] = (unsigned char)other;
    snx_st_le16(s + 6, (uint16_t)section);
    snx_st_le64(s + 8, value);
    memcpy(b->image + STRTAB + b->names, name, n);
    b->names += (unsigned)n;
    return b->symbols;
}
static void rela(Build* b, int plt, SnxU64 target, unsigned type, unsigned sym, SnxU64 addend) {
    unsigned char* r = b->image + (plt ? JMPREL + 24 * b->jmprel++ : RELA + 24 * b->rela++);
    snx_st_le64(r, target);
    snx_st_le64(r + 8, ((SnxU64)sym << 32) | type);
    snx_st_le64(r + 16, addend);
}
static void relr(Build* b, SnxU64 entry) { snx_st_le64(b->image + RELR + 8 * b->relr++, entry); }
/* the dynamic table of what was put: rela_size bytes of RELA when not 0, else its entries' */
static void end(Build* b, SnxU64 rela_size) {
    unsigned char* d = b->image + DYNAMIC;
    SnxU64 tags[9][2] = {
        {6, SYMTAB}, {5, STRTAB},         {10, b->names}, {7, RELA}, {8, 0}, {23, JMPREL}, {2, 24ull * b->jmprel},
        {36, RELR},  {35, 8ull * b->relr}};
    unsigned i;
    tags[4][1] = rela_size ? rela_size : 24ull * b->rela;
    for (i = 0; i < 9; ++i) {
        snx_st_le64(d + 16 * i, tags[i][0]);
        snx_st_le64(d + 16 * i + 8, tags[i][1]);
    }
}

static unsigned char g_image[3][SIZE], g_copy[3][DATA_SIZE];

/* the three modules as loaded. rela_size: module b's DT_RELASZ, 0 for what its table holds */
static void build(SnxU64 rela_size) {
    Build a, b, c;
    unsigned dup, prot, loc, ext, weak, gone;
    /* a: dup, a local loc, and a PLT slot of c's ext */
    begin(&a, g_image[0]);
    symbol(&a, "dup", 0x12, 0, 1, 0x10);
    symbol(&a, "loc", 0x02, 0, 1, 0x20);
    ext = symbol(&a, "ext", 0x12, 0, 0, 0);
    rela(&a, 1, DATA, JUMP_SLOT, ext, 0);
    snx_st_le64(g_image[0] + DATA, 0x700);
    end(&a, 0);
    /* b: dup again, prot, loc, and every kind of relocation */
    begin(&b, g_image[1]);
    dup = symbol(&b, "dup", 0x12, 0, 1, 0x30);
    prot = symbol(&b, "prot", 0x12, 0, 1, 0x38);
    loc = symbol(&b, "loc", 0x12, 0, 1, 0x40);
    ext = symbol(&b, "ext", 0x12, 0, 0, 0);
    weak = symbol(&b, "gone_weak", 0x22, 0, 0, 0);
    gone = symbol(&b, "gone", 0x12, 0, 0, 0);
    rela(&b, 0, DATA, ABS64, dup, 1);            /* a's dup, the first in load order, not its own */
    rela(&b, 0, DATA + 8, ABS64, prot, 0);       /* its own: the first that defines it */
    rela(&b, 0, DATA + 0x10, GLOB_DAT, weak, 4); /* a weak symbol nothing defines: 0 and the addend */
    rela(&b, 0, DATA + 0x18, ABS64, gone, 0);    /* nothing defines it: as it was */
    rela(&b, 0, DATA + 0x40, RELATIVE, 0, 0x123);
    rela(&b, 0, DATA - 8, RELATIVE, 0, 0);   /* a target before its data */
    rela(&b, 0, DATA + 0x50, TLS, 0, 0);     /* a type with no case */
    rela(&b, 0, DATA + 0x48, ABS32, loc, 0); /* a's loc is local: its own */
    rela(&b, 1, DATA + 0x80, JUMP_SLOT, ext, 0);
    rela(&b, 1, DATA + 0x88, JUMP_SLOT, gone, 0); /* nothing defines it: lazy */
    relr(&b, DATA + 0x100);                       /* that word, then of the 63 after it the first and the third */
    relr(&b, 0xb);
    snx_st_le64(g_image[1] + DATA + 0x18, 0x1111);
    snx_st_le64(g_image[1] + DATA + 0x50, 0x2222);
    snx_st_le64(g_image[1] + DATA + 0x80, 0x700);
    snx_st_le64(g_image[1] + DATA + 0x88, 0x700);
    snx_st_le64(g_image[1] + DATA + 0x100, 0x55);
    snx_st_le64(g_image[1] + DATA + 0x108, 0x66);
    snx_st_le64(g_image[1] + DATA + 0x110, 0x77);
    snx_st_le64(g_image[1] + DATA + 0x118, 0x88);
    end(&b, rela_size);
    /* c: ext, a protected prot, and a use of each and of loc */
    begin(&c, g_image[2]);
    symbol(&c, "ext", 0x12, 0, 1, 0x50);
    prot = symbol(&c, "prot", 0x12, 3, 1, 0x58);
    loc = symbol(&c, "loc", 0x12, 0, 0, 0);
    rela(&c, 0, DATA, GLOB_DAT, prot, 0); /* protected: its own, though b defines it before */
    rela(&c, 0, DATA + 8, ABS64, loc, 0); /* b's: a's is local */
    end(&c, 0);
}
/* the modules, their data linked into the copies, or in place */
static void modules(SnxRelocModule* m, int in_place) {
    static const char* const names[3] = {"a", "b", "c"};
    static const SnxU64 bases[3] = {BASE_A, BASE_B, BASE_C};
    int i;
    for (i = 0; i < 3; ++i) {
        m[i].name = names[i];
        m[i].image = g_image[i];
        m[i].base = bases[i];
        m[i].size = SIZE;
        m[i].data_off = DATA;
        m[i].data_size = DATA_SIZE;
        m[i].lazy = 0;
        if (in_place) {
            m[i].data = g_image[i] + DATA;
        } else {
            memcpy(g_copy[i], g_image[i] + DATA, DATA_SIZE);
            m[i].data = g_copy[i];
        }
    }
}
static SnxU64 word(int module, unsigned offset) { return snx_ld_le64(g_copy[module] + offset); }

int main(void) {
    SnxRelocModule copy[3], place[3];
    SnxRelocCounts counts;
    SnxRelocDiff diff;
    char error[512] = "";
    build(0);
    modules(copy, 0);
    check(!snx_relocate(copy, 3, &counts, error, sizeof error), "three modules link");
    check(snx_ld_le64(g_image[1] + DATA) == 0 && snx_ld_le64(g_image[1] + DATA + 0x100) == 0x55,
          "a module linked into a copy stays as it was loaded");
    check(word(1, 0x100) == BASE_B + 0x55 && word(1, 0x108) == BASE_B + 0x66 && word(1, 0x118) == BASE_B + 0x88,
          "a DT_RELR address and the words its bitmap names take the base");
    check(word(1, 0x110) == 0x77, "a word the bitmap leaves out stays");
    check(word(1, 0x40) == BASE_B + 0x123, "a relative relocation is the base and its addend");
    check(word(1, 0) == BASE_A + 0x11, "a symbol two modules define is the first's in load order, with the addend");
    check(word(1, 8) == BASE_B + 0x38, "a module's own definition when it is the first");
    check(word(1, 0x48) == BASE_B + 0x40 && word(2, 8) == BASE_B + 0x40, "a local symbol is no definition");
    check(word(2, 0) == BASE_C + 0x58, "a protected symbol is its own module's");
    check(word(1, 0x10) == 4, "a weak symbol nothing defines is 0 and the addend");
    check(word(1, 0x18) == 0x1111, "another symbol nothing defines leaves its word");
    check(word(0, 0) == BASE_C + 0x50 && word(1, 0x80) == BASE_C + 0x50, "a PLT slot is bound to its function");
    check(word(1, 0x88) == BASE_B + 0x700, "a PLT slot nothing defines keeps its lazy value");
    check(copy[0].lazy == BASE_A + 0x700 && copy[1].lazy == BASE_B + 0x700 && !copy[2].lazy,
          "a module's lazy value is its slots' word as loaded and the base");
    check(word(1, 0x50) == 0x2222, "a relocation of a type with no case is left alone");
    check(counts.relative == 4 && counts.symbolic == 5 && counts.slots == 2, "the relocations applied are counted");
    check(counts.absent == 1 && counts.unresolved == 2 && counts.outside == 1 && counts.other == 1,
          "and the ones that are not");
    check(counts.first_unresolved && !strcmp(counts.first_unresolved, "gone"), "the first symbol nothing defines");
    /* in place, as the game's own linker leaves the modules: the same words */
    modules(place, 1);
    check(!snx_relocate(place, 3, &counts, error, sizeof error), "the modules link in place");
    check(!memcmp(g_image[0] + DATA, g_copy[0], DATA_SIZE) && !memcmp(g_image[1] + DATA, g_copy[1], DATA_SIZE) &&
              !memcmp(g_image[2] + DATA, g_copy[2], DATA_SIZE),
          "linked in place, a module's data is the copy's");
    check(!snx_relocate_compare(copy, 3, &diff, error, sizeof error) && diff.words == 14 && !diff.lazy && !diff.differ,
          "the two agree on every relocated word");
    /* a slot the game's linker has not bound yet is no difference; any other word is */
    snx_st_le64(g_image[1] + DATA + 0x80, BASE_B + 0x700);
    check(!snx_relocate_compare(copy, 3, &diff, error, sizeof error) && diff.lazy == 1 && !diff.differ,
          "a slot that holds its module's lazy value is not bound yet");
    snx_st_le64(g_image[2] + DATA + 8, BASE_A + 0x20);
    snx_st_le64(g_image[1] + DATA, BASE_B + 0x31);
    check(!snx_relocate_compare(copy, 3, &diff, error, sizeof error) && diff.differ == 2 && diff.words == 14,
          "words that differ are counted");
    check(diff.module == 1 && diff.at == DATA && diff.has == BASE_B + 0x31 && diff.wants == BASE_A + 0x11,
          "the first of them is told: its module, its offset and both values");
    snx_st_le64(g_image[1] + DATA + 0x50, 0x3333); /* not a word the linker relocates */
    check(!snx_relocate_compare(copy, 3, &diff, error, sizeof error) && diff.differ == 2,
          "other words are not compared");
    /* a table that is no whole number of entries, and one outside the module: refused, the module and sizes said */
    build(25);
    modules(copy, 0);
    check(snx_relocate(copy, 3, &counts, error, sizeof error) == -1 && strstr(error, "b: ") &&
              strstr(error, "RELA 25 bytes"),
          "a relocation table of 25 bytes is refused, with its size");
    build(0);
    snx_st_le64(g_image[2] + DYNAMIC + 8, 0x2000); /* c's DT_SYMTAB */
    modules(copy, 0);
    check(snx_relocate(copy, 3, &counts, error, sizeof error) == -1 && strstr(error, "c: ") &&
              strstr(error, "0x2000") && strstr(error, "4096 bytes"),
          "symbols outside the module are refused, with where they lie");
    printf("linker: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
