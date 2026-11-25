/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the host's native back end makes of a whole program: a fixed one of each kind the drivers recompile, recompiled
   by its driver as a player's game is, where every function (A64) or block (AArch32) must be the back end's. The
   program is made here, from the decoders' own tables, and its code is never run:

     switch     an extracted Switch game: an NSO of every A64 word class of the Ounce (below), each word a function of
                its own with a RET, and the NPDM of a Switch program, for which the words past its CPU are undefined
     ounce      the same NSO with the NPDM of an Ounce program, whose CPU has every one of them
     nro        the same code as an NRO
     switch32   an extracted 32-bit Switch game: an NSO of A32, T32 and T16 code, VFP, Advanced SIMD and the IT block
                among them, each word a block of its own with a BX LR
     3ds        a 3DS game, a bare NCCH, of the ARM11's A32 and Thumb code

   A64's words are those of the generated decoder (llvm_qemu_words.c: every row, every value of its small fields) and
   each system register and cache word EL0 has: no op of the Ounce may lack one. AArch32's are those of the rows of its
   decoders' .decode files (tools/decodetree --rows), drawn the same way, and of each form of vector instruction its
   decoder tells apart, found by going over the bits an encoding's class is in.

   The line of the recompile that says how many the back end made ("x86-64 back end: N of M functions") is printed, and
   the test fails where N is below M, saying each function it left, its address and its word, and where M is below the
   words the program has. On a host with no native back end it says so and is skipped (77).
     recompile_census PROGRAM SCARLETNX SCARLETNX_3DS [--verbose]
   --verbose: the recompile's output too */
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "decode_rows.h"
#include "llvm_qemu_words.h"
#include "nx/loader/loader.h"
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rows-a32.inc"
#include "rows-a32-uncond.inc"
#include "rows-t16.inc"
#include "rows-t32.inc"

#define PAGE      0x1000u
#define CODE      0x80u /* where the words start, past an NRO's header */
#define CTR_BASE  0x100000u
#define TITLE     0x0123456789ABC000ull /* what the game's folder is named by; an NRO's by its file */
#define SAID_MOST 20u                   /* how many of the functions left are said */
#define A64_RET   0xD65F03C0u
#define A32_BX_LR 0xE12FFF1Eu
#define T16_BX_LR 0x4770u
#define T16_IT_EQ 0xBF08u

typedef enum { SWITCH, OUNCE, NRO, SWITCH32, CTR, KINDS } Kind;
static const char* const kind_names[KINDS] = {"switch", "ounce", "nro", "switch32", "3ds"};
/* what the recompile counts, as its line says it */
static const char* const kind_counts[KINDS] = {"functions", "functions", "functions", "AArch32 blocks", "blocks"};

typedef enum { A64, A32, T16, T32 } State;
static const char* const state_names[] = {"A64", "A32", "T16", "T32"};

/* a word of the program, at's the offset of the function or block it begins */
typedef struct {
    uint32_t word; /* a T32's: its first halfword in the high half */
    uint32_t at;
    State state;
    int it; /* after an IT of EQ */
} Case;

typedef struct {
    unsigned char* image;
    size_t size, capacity;
    Case* cases;
    size_t count, case_capacity;
} Program;

static struct {
    uint64_t seed;
    int failures;
} g_recompile_census = {0x9E3779B97F4A7C15ull, 0};

[[gnu::format(printf, 1, 2)]] static void fail(const char* format, ...) {
    va_list args;
    fputs("FAIL: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    ++g_recompile_census.failures;
}

static uint64_t next_random(void) {
    uint64_t* s = &g_recompile_census.seed;
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* ---- the program's code ---- */

static void put16(Program* p, unsigned half) {
    if (p->size + 2 > p->capacity) {
        p->capacity = p->capacity ? 2 * p->capacity : (size_t)1 << 20;
        p->image = (unsigned char*)snx_realloc(p->image, p->capacity);
    }
    snx_st_le16(p->image + p->size, (uint16_t)half);
    p->size += 2;
}
static void put32(Program* p, uint32_t word) {
    put16(p, word & 0xFFFF);
    put16(p, word >> 16);
}

/* the word's function or block, and the return that ends it */
static void add_case(Program* p, State state, uint32_t word, int it) {
    if (p->count == p->case_capacity) {
        p->case_capacity = p->case_capacity ? 2 * p->case_capacity : 4096;
        p->cases = (Case*)snx_realloc(p->cases, p->case_capacity * sizeof *p->cases);
    }
    p->cases[p->count++] = (Case){word, (uint32_t)p->size, state, it};
    if (state == A64 || state == A32) {
        put32(p, word);
        put32(p, state == A64 ? A64_RET : A32_BX_LR);
        return;
    }
    if (it) put16(p, T16_IT_EQ);
    if (state == T32) put16(p, word >> 16);
    put16(p, word & 0xFFFF);
    put16(p, T16_BX_LR);
    if (p->size & 2) put16(p, 0xBF00); /* NOP, so that an A32 word is aligned */
}

/* ---- A64's words ---- */

static int by_word(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

/* a word of every op the Ounce's CPU has (the Switch's too, a subset), and what EL0 does with the system registers,
   the caches and the hints */
static void a64_code(Program* p) {
    uint32_t* words;
    size_t rows = lqd_wide_row_words(6, 4, next_random, &words), count = rows;
    words = (uint32_t*)snx_realloc(words, (rows + 4 * 0x800u) * sizeof *words);
    /* MRS, MSR, SYSL and SYS of EL0 (op1 3): each CRn, CRm and op2 */
    for (unsigned top = 0; top < 4; ++top)
        for (unsigned at = 0; at < 0x800; ++at)
            words[count++] =
                (top & 2 ? 0xD5200000u : 0xD5000000u) | (top & 1 ? 0x180000u : 0x080000u) | 3u << 16 | at << 5 | 1u;
    qsort(words, count, sizeof *words, by_word);
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        SnxA64Insn insn;
        if (kept && words[kept - 1] == words[i]) continue;
        if (snx_a64_decode_word(words[i], SNX_A64_TARGET_OUNCE, &insn) != SNX_A64_OP_UNDEF) words[kept++] = words[i];
    }
    size_t ops = 0;
    const char* missing = lqd_op_without_word(words, kept, SNX_A64_TARGET_OUNCE, &ops);
    if (missing) fail("the program has no word of %s, an op of the Ounce's CPU", missing);
    printf("program: %zu A64 words of %zu ops\n", kept, ops);
    for (size_t i = 0; i < kept; ++i) add_case(p, A64, words[i], 0);
    free(words);
}

/* ---- AArch32's words ---- */

/* the keys seen, each how often (0 an empty slot): a word is kept while its key is one of the first each of them */
typedef struct {
    struct {
        uint64_t key;
        unsigned count;
    }* slots;
    size_t used, capacity;
} Seen;

static size_t seen_slot(const Seen* s, uint64_t key) {
    size_t at = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (s->capacity - 1);
    while (s->slots[at].count && s->slots[at].key != key) at = (at + 1) & (s->capacity - 1);
    return at;
}
static int seen_wanted(Seen* s, uint64_t key, unsigned each) {
    if (2 * (s->used + 1) > s->capacity) {
        Seen old = *s;
        s->capacity = old.capacity ? 2 * old.capacity : 1 << 12;
        s->slots = snx_alloc(s->capacity * sizeof *s->slots);
        memset(s->slots, 0, s->capacity * sizeof *s->slots);
        for (size_t i = 0; i < old.capacity; ++i)
            if (old.slots[i].count) s->slots[seen_slot(s, old.slots[i].key)] = old.slots[i];
        free(old.slots);
    }
    size_t at = seen_slot(s, key);
    if (s->slots[at].count >= each) return 0;
    s->used += !s->slots[at].count;
    s->slots[at].key = key;
    ++s->slots[at].count;
    return 1;
}

static uint64_t mix(uint64_t key, uint64_t value) { return (key ^ value) * 0x100000001B3ull; }

/* what the vector decoder tells the back ends of a VFP or Advanced SIMD word, but for its registers and immediates: 0
   where it decodes to nothing */
static uint64_t vector_form(uint32_t w) {
    SnxArmVector v = {0};
    if (!snx_arm_vector_decode(w, &v)) return 0;
    uint64_t key = mix(0xCBF29CE484222325ull, (uint64_t)v.op << 32 | v.flags);
    key = mix(key, (uint64_t)v.width << 32 | v.bytes << 16 | (v.aux & 0xFF));
    key = mix(key, (uint64_t)v.count << 32 | v.groups << 16 | v.stride << 8 | (v.alignment != 0));
    return mix(key, w >> 28 == 15 ? 2 : w >> 28 != 14);
}

/* value's low bits spread over the bits of mask, lowest first */
static uint32_t deposit(uint32_t value, uint32_t mask) {
    uint32_t out = 0;
    for (; mask; mask &= mask - 1, value >>= 1)
        if (value & 1) out |= mask & (0u - mask);
    return out;
}
static unsigned bits_of(uint32_t mask) {
    unsigned n = 0;
    for (; mask; mask &= mask - 1) ++n;
    return n;
}

/* the words of a row of a .decode file: its free bits clear, set and six times random; then every value of each field
   of 3 bits or fewer in those three samples, and of each of 4, the registers among them, in the third. *sample is which
   of the samples the word is, 8 past them */
typedef struct {
    const SnxDecodeRow* row;
    uint32_t base[3];
    unsigned sample, field, value;
} RowWords;
static int row_next(RowWords* r, uint32_t free_bits, uint32_t* word, unsigned* sample) {
    if (r->sample < 8) {
        uint32_t bits = !r->sample ? 0 : r->sample == 1 ? ~0u : (uint32_t)next_random();
        *word = r->row->value | (bits & free_bits);
        if (r->sample < 3) r->base[r->sample] = *word;
        *sample = r->sample++;
        return 1;
    }
    for (; r->field < 24; ++r->field, r->value = 0) {
        uint32_t mask = r->row->fields[r->field / 3];
        unsigned bits = bits_of(mask), base = r->field % 3;
        if (!bits || bits > 4 || (bits == 4 && base != 2) || r->value == 1u << bits) continue;
        *word = (r->base[base] & ~mask) | deposit(r->value++, mask);
        *sample = 8;
        return 1;
    }
    return 0;
}

/* where a vector instruction's class is: the top byte (under A32's AL, EQ and no condition, and Thumb's) */
static const unsigned char vector_a32[] = {0xEC, 0xED, 0xEE, 0x0C, 0x0D, 0x0E, 0xFE, 0xF2, 0xF3, 0xF4};
static const unsigned char vector_t32[] = {0xEC, 0xED, 0xEE, 0xFE, 0xEF, 0xFF, 0xF9};
/* the i-th word of a top byte: every value of [23:20], [19:16] and [11:4], twice, its registers drawn */
#define VECTOR_WORDS 0x20000u
static uint32_t vector_word(unsigned top, uint32_t i) {
    uint64_t r = next_random();
    return (uint32_t)top << 24 | (i >> 1 & 0xFF00) << 8 | (uint32_t)(r & 15) << 12 | (i >> 1 & 0xFF) << 4 |
           (uint32_t)(r >> 8 & 15);
}

/* the words of the A32 decoder's rows under an AL, the first three of each row under an EQ, and the vector
   instructions', which no row has: two of each form. A word the profile lacks is UNDEFINED, code the back end makes
   too */
static void a32_code(Program* p, SnxArmProfile profile) {
    Seen words = {0}, forms = {0};
    size_t before = p->count;
    uint32_t w;
    unsigned sample;
    for (size_t i = 0; i < sizeof rows_a32 / sizeof *rows_a32; ++i)
        for (RowWords r = {.row = &rows_a32[i]}; row_next(&r, ~rows_a32[i].mask, &w, &sample);)
            for (unsigned eq = 0; eq < (sample < 3 ? 2u : 1u); ++eq) {
                w = (w & 0x0FFFFFFF) | (eq ? 0 : 0xE0000000u);
                if (seen_wanted(&words, w, 1)) add_case(p, A32, w, 0);
            }
    for (size_t i = 0; i < sizeof rows_a32_uncond / sizeof *rows_a32_uncond; ++i)
        for (RowWords r = {.row = &rows_a32_uncond[i]}; row_next(&r, ~rows_a32_uncond[i].mask, &w, &sample);)
            if (seen_wanted(&words, w, 1)) add_case(p, A32, w, 0);
    for (size_t t = 0; t < sizeof vector_a32; ++t)
        for (uint32_t i = 0; i < VECTOR_WORDS; ++i) {
            w = vector_word(vector_a32[t], i);
            uint64_t key = snx_a32_decode_profile(profile, w) == SNX_A32_VECTOR ? vector_form(w) : 0;
            if (key && seen_wanted(&forms, key, 2) && seen_wanted(&words, w, 1)) add_case(p, A32, w, 0);
        }
    printf("program: %zu A32 words, %zu vector forms\n", p->count - before, forms.used);
    free(words.slots), free(forms.slots);
}

/* a Thumb instruction, as it is and under an IT block's condition where the profile has IT */
static void thumb_word(Program* p, Seen* words, SnxArmProfile profile, uint32_t w, int wide) {
    if (!seen_wanted(words, w | (uint64_t)wide << 32, 1)) return;
    add_case(p, wide ? T32 : T16, w, 0);
    if (profile == SNX_ARM_PROFILE_A57 && (wide || w >> 8 != 0xBF))
        add_case(p, wide ? T32 : T16, w, 1); /* not IT's own */
}

/* what a 16-bit instruction is to its decoder but for registers below R15 and immediates */
static uint64_t thumb_form(SnxArmProfile profile, unsigned half) {
    SnxThumb t = {0};
    if (snx_thumb_decode_profile(profile, (uint16_t)half, 0, &t) != 1) return 0;
    uint64_t key = mix(0x84222325CBF29CE4ull, (uint64_t)t.shape << 32 | t.op << 16 | t.flags);
    return mix(key, (uint64_t)(t.rd == 15) << 3 | (t.rn == 15) << 2 | (t.rm == 15) << 1 | (t.imm == 0));
}

/* the words of the Thumb decoders' rows, two of each form of a 16-bit instruction, and with Thumb-2 (the A57's) the
   vector instructions' */
static void thumb_code(Program* p, SnxArmProfile profile) {
    Seen words = {0}, forms = {0};
    const int wide = profile == SNX_ARM_PROFILE_A57;
    size_t before = p->count;
    uint32_t w;
    unsigned sample;
    for (size_t i = 0; i < sizeof rows_t16 / sizeof *rows_t16; ++i)
        for (RowWords r = {.row = &rows_t16[i]}; row_next(&r, ~rows_t16[i].mask & 0xFFFF, &w, &sample);)
            thumb_word(p, &words, profile, w & 0xFFFF, 0);
    for (unsigned half = 0; half < 0x10000; ++half) {
        uint64_t key = thumb_form(profile, half);
        if (key && seen_wanted(&forms, key, 2)) thumb_word(p, &words, profile, half, 0);
    }
    for (size_t i = 0; wide && i < sizeof rows_t32 / sizeof *rows_t32; ++i)
        for (RowWords r = {.row = &rows_t32[i]}; row_next(&r, ~rows_t32[i].mask, &w, &sample);)
            if (w >> 27 >= 0x1D) thumb_word(p, &words, profile, w, 1);
    for (size_t t = 0; wide && t < sizeof vector_t32; ++t)
        for (uint32_t i = 0; i < VECTOR_WORDS; ++i) {
            SnxThumb d = {0};
            w = vector_word(vector_t32[t], i);
            if (snx_thumb_decode_profile(profile, (uint16_t)(w >> 16), (uint16_t)w, &d) != 2 || d.shape != TH_A32 ||
                d.op != SNX_A32_VECTOR)
                continue;
            uint64_t key = vector_form(d.imm);
            if (key && seen_wanted(&forms, key, 2)) thumb_word(p, &words, profile, w, 1);
        }
    printf("program: %zu Thumb words\n", p->count - before);
    free(words.slots), free(forms.slots);
}

/* ---- the files ---- */

static void pad_to_page(Program* p) {
    while (p->size & (PAGE - 1)) put16(p, 0);
}

static void write_file(const char* path, const void* data, size_t size) {
    if (snx_write_file(path, data, size)) fail("cannot write %s", path);
}

/* the code and a page each of read-only data and data: an NSO's image, or with nro an NRO's, its header in the code's
   first bytes */
static void nx_image(Program* p, int nro) {
    pad_to_page(p);
    uint32_t text = (uint32_t)p->size;
    while (p->size < text + 2 * PAGE) put16(p, 0);
    if (!nro) return;
    memcpy(p->image + 0x10, "NRO0", 4);
    snx_st_le32(p->image + 0x18, text + 2 * PAGE);
    snx_st_le32(p->image + 0x24, text);
    snx_st_le32(p->image + 0x28, text);
    snx_st_le32(p->image + 0x2c, PAGE);
    snx_st_le32(p->image + 0x30, text + PAGE);
    snx_st_le32(p->image + 0x34, PAGE);
    memset(p->image + 0x40, 0xCE, 32);
}

static void write_nso(const char* path, const Program* p) {
    uint32_t text = (uint32_t)p->size - 2 * PAGE;
    unsigned char* file = (unsigned char*)snx_alloc(0x100 + p->size);
    memset(file, 0, 0x100);
    memcpy(file, "NSO0", 4);
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t at = i ? text + (i - 1) * PAGE : 0, size = i ? PAGE : text;
        snx_st_le32(file + 0x10 + i * 0x10, 0x100 + at);
        snx_st_le32(file + 0x14 + i * 0x10, at);
        snx_st_le32(file + 0x18 + i * 0x10, size);
        snx_st_le32(file + 0x60 + i * 4, size);
    }
    memset(file + 0x40, 0xCE, 32);
    memcpy(file + 0x100, p->image, p->size);
    write_file(path, file, 0x100 + p->size);
    free(file);
}

/* an NPDM: the ACID's magic, past its signature and key, is at 0x200 in a Switch's and at 0x300 in an Ounce's */
static void write_npdm(const char* path, int is64, int ounce) {
    unsigned char npdm[0x500] = {0};
    memcpy(npdm, "META", 4);
    npdm[0x0c] = is64 ? 7 : 0; /* 64-bit in 39 bits of addresses, or 32-bit */
    npdm[0x0e] = 44;
    snx_st_le32(npdm + 0x1c, 0x100000);
    memcpy(npdm + 0x20, "census", 6);
    snx_st_le32(npdm + 0x70, 0x80);
    snx_st_le32(npdm + 0x74, 0x40);
    snx_st_le32(npdm + 0x78, 0x100);
    snx_st_le32(npdm + 0x7c, 0x400);
    memcpy(npdm + 0x80, "ACI0", 4);
    snx_st_le64(npdm + 0x90, TITLE);
    memcpy(npdm + 0x100 + (ounce ? 0x300 : 0x200), "ACID", 4);
    write_file(path, npdm, sizeof npdm);
}

static void sha256(const void* data, size_t size, unsigned char* out) {
    if (!EVP_Digest(data, size, out, NULL, EVP_sha256(), NULL)) fail("no SHA-256 of %zu bytes", size);
}

/* a bare NCCH: its ExHeader, and an ExeFS of .code alone, the program's text and a page each of read-only data and
   data. No RomFS, nothing encrypted or compressed */
static void write_ncch(const char* path, Program* p) {
    uint32_t text = (uint32_t)p->size;
    pad_to_page(p);
    uint32_t pages = (uint32_t)(p->size / PAGE);
    while (p->size < (pages + 2) * PAGE) put16(p, 0);
    size_t exefs = 0x200 + p->size, size = 0xA00 + exefs;
    unsigned char *ncch = (unsigned char*)snx_alloc(size), *ex = ncch + 0x200, *fs = ncch + 0xA00;
    memset(ncch, 0, 0xA00 + 0x200);
    const uint32_t segments[3][3] = {
        {CTR_BASE, pages, text}, {CTR_BASE + pages * PAGE, 1, 16}, {CTR_BASE + (pages + 1) * PAGE, 1, 4}};
    memcpy(ex, "CENSUS", 6);
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned k = 0; k < 3; ++k) snx_st_le32(ex + 0x10 + i * 0x10 + k * 4, segments[i][k]);
    snx_st_le32(ex + 0x1C, 0x4000);
    snx_st_le32(ex + 0x3C, PAGE);
    snx_st_le64(ex + 0x200, 0x0004000001234500ull);
    ex[0x20F] = 0x30;
    memcpy(fs, ".code", 5);
    snx_st_le32(fs + 12, (uint32_t)p->size);
    memcpy(fs + 0x200, p->image, p->size);
    sha256(fs + 0x200, p->size, fs + 0x1E0);
    memcpy(ncch + 0x100, "NCCH", 4);
    snx_st_le32(ncch + 0x104, (uint32_t)(size / 512));
    snx_st_le16(ncch + 0x112, 2);
    snx_st_le64(ncch + 0x118, 0x0004000001234500ull);
    memcpy(ncch + 0x150, "CTR-P-CENS", 10);
    sha256(ex, 0x400, ncch + 0x160);
    snx_st_le32(ncch + 0x180, 0x400);
    ncch[0x18C] = 1, ncch[0x18D] = 2, ncch[0x18F] = 6; /* an application, with no RomFS and no crypto */
    snx_st_le32(ncch + 0x1A0, 0xA00 / 512);
    snx_st_le32(ncch + 0x1A4, (uint32_t)(exefs / 512));
    snx_st_le32(ncch + 0x1A8, 1);
    sha256(fs, 0x200, ncch + 0x1C0);
    write_file(path, ncch, size);
    free(ncch);
}

/* what discovery starts from: the cases' addresses, bit 0 a Thumb one's */
static void write_hints(const char* path, const Program* p, uint32_t base) {
    char* text = (char*)snx_alloc(p->count * 10 + 1);
    size_t n = 0;
    for (size_t i = 0; i < p->count; ++i)
        n += (size_t)sprintf(text + n, "%08x\n", (unsigned)(base + p->cases[i].at + (p->cases[i].state >= T16)));
    write_file(path, text, n);
    free(text);
}

/* ---- the recompile's word ---- */

static const Case* case_at(const Program* p, uint32_t at) {
    size_t lo = 0, hi = p->count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (p->cases[middle].at <= at)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo ? &p->cases[lo - 1] : NULL;
}

/* the recompile's lines that say what the back end left, and the program's word at each address they name */
static void say_left(const Program* p, const char* text, uint32_t base) {
    static const char* const says[] = {"native back end: the function at ", ", the first the ",
                                       " have an instruction it has no code for"};
    unsigned said = 0, more = 0;
    for (const char *line = text, *end; *line; line = *end ? end + 1 : end) {
        end = line + strcspn(line, "\r\n");
        int wanted = 0;
        for (unsigned i = 0; i < sizeof says / sizeof *says; ++i) {
            const char* at = strstr(line, says[i]);
            wanted |= at && at < end;
        }
        if (!wanted) continue;
        if (said == SAID_MOST) {
            ++more;
            continue;
        }
        ++said;
        fprintf(stderr, "  %.*s", (int)(end - line), line);
        const char* address = strstr(line, " at 0x");
        unsigned value;
        if (address && address < end && sscanf(address + 6, "%x", &value) == 1 && value >= base) {
            const Case* c = case_at(p, value - base);
            if (c)
                fprintf(stderr, " [the program's word there: %s %0*x%s at 0x%x]", state_names[c->state],
                        c->state == T16 ? 4 : 8, (unsigned)c->word, c->it ? " under IT EQ" : "",
                        (unsigned)(base + c->at));
        }
        fputc('\n', stderr);
    }
    if (more) fprintf(stderr, "  and %u more\n", more);
}

/* the count of the recompile's line, "<name> back end: N of M <what>" */
static int count_of(const char* text, const char* what, char* line, size_t line_size, unsigned long* made,
                    unsigned long* of) {
    static const char back_end[] = " back end: ";
    for (const char* at = strstr(text, back_end); at; at = strstr(at + 1, back_end)) {
        char kind[32];
        if (sscanf(at + sizeof back_end - 1, "%lu of %lu %31[A-Za-z0-9 ]", made, of, kind) != 3 || strcmp(kind, what))
            continue;
        const char *from = at, *to = at + strcspn(at, "\r\n");
        while (from > text && from[-1] != '\n') --from;
        snprintf(line, line_size, "%.*s", (int)(to - from), from);
        return 1;
    }
    return 0;
}

/* why no native back end is here, where none is */
static const char* why_none(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "this x86-64 build has none (SNX_SAIL_X64=OFF), or its CPU lacks SSE4.2, PCLMULQDQ or POPCNT";
#else
    return "none is made for this architecture";
#endif
}

/* how many instructions the analysis reads of the program on a CPU of these features: each word, and the RET past it
   where the word is one there that goes on */
static long a64_instructions(const Program* p, SnxU64 features) {
    long n = 2; /* the entry, and what it goes to */
    for (size_t i = 0; i < p->count; ++i) {
        switch (snx_a64_flow(p->cases[i].word, features)) {
            case SNX_A64_FLOW_UNDEF:
            case SNX_A64_FLOW_B:
            case SNX_A64_FLOW_BR:
            case SNX_A64_FLOW_RET:
            case SNX_A64_FLOW_BRK: ++n; break;
            default: n += 2; break;
        }
    }
    return n;
}

/* the count the recompile said, text its output: all of the program's, and all the back end's */
static void check_count(Kind kind, const Program* p, const char* text, uint32_t base) {
    const char *name = kind_names[kind], *what = kind_counts[kind];
    char line[256];
    unsigned long made, of;
    if (!count_of(text, what, line, sizeof line, &made, &of)) {
        fail("%s: the recompile says no count of %s the native back end made, where this host has one:\n%s", name, what,
             text);
        return;
    }
    printf("census: %s: %s\n", name, line);
    if (made != of) {
        fail("%s: %lu of %lu %s left to LLVM, where this host has the %s back end:", name, of - made, of, what,
             snx_native_name());
        say_left(p, text, base);
    }
    if (of < p->count) fail("%s: the recompile found %lu %s of the program's %zu", name, of, what, p->count);
    if (kind == SWITCH32 || kind == CTR) return;
    /* the CPU the recompile took, the NPDM's (an NRO's is the Switch's): the words past the Switch's end their function
       on it, so fewer instructions are read */
    const char* read = strstr(text, " functions, ");
    long got = read ? strtol(read + 12, NULL, 10) : 0;
    long with = a64_instructions(p, SNX_A64_TARGET_SWITCH), past = a64_instructions(p, SNX_A64_TARGET_OUNCE);
    long mine = kind == OUNCE ? past : with, other = kind == OUNCE ? with : past;
    if (labs(got - mine) >= labs(got - other))
        fail("%s: the recompile read %ld instructions, where the program has %ld on its CPU and %ld on the other's",
             name, got, mine, other);
}

/* the game: its input, and the addresses discovery starts from, hints, where the recompile reads them */
static void write_game(Kind kind, Program* p, const char* input, const char* hints, uint32_t base) {
    char *exefs, *file, *folder = snx_dirname(hints);
    if (snx_mkdirs(folder)) fail("cannot make %s", folder);
    free(folder);
    write_hints(hints, p, base);
    if (kind == CTR) {
        write_ncch(input, p);
        return;
    }
    nx_image(p, kind == NRO);
    if (kind == NRO) {
        write_file(input, p->image, p->size);
        return;
    }
    exefs = snx_path_join(input, "exefs");
    if (snx_mkdirs(exefs)) fail("cannot make %s", exefs);
    file = snx_path_join(exefs, "main");
    write_nso(file, p);
    free(file);
    file = snx_path_join(exefs, "main.npdm");
    write_npdm(file, kind != SWITCH32, kind == OUNCE);
    free(file), free(exefs);
}

/* the driver's recompile of input, its output in *text: its status */
static int recompile(Kind kind, const char* const* drivers, const char* root, const char* out, const char* input,
                     const char* hints, char** text) {
    char *data = snx_path_join(root, "data"), *folder = snx_path_join(out, "census");
    /* --nce=0: code made ahead of time, which an NCE recompile has none of */
    const char* nx[] = {drivers[0], "--data", data, "-r", input, "--outdir", out, "--nce=0", NULL};
    const char* ctr[] = {drivers[1], "--data", data, "-r", "--jit-cache", hints, "--output", folder, input, NULL};
    const char* const* args = kind == CTR ? ctr : nx;
    /* root stands in for the player's home, whose settings, caches and online account are not read */
    int rc = snx_run_all(args, root, root, text);
    if (!*text) *text = snx_strdup("");
    free(data), free(folder);
    return rc;
}

int main(int argc, char** argv) {
    Kind kind = SWITCH;
    const int verbose = argc == 5 && !strcmp(argv[4], "--verbose");
    if (argc != 4 + verbose) {
        fprintf(stderr, "recompile_census PROGRAM SCARLETNX SCARLETNX_3DS [--verbose]\n");
        return 2;
    }
    while (kind < KINDS && strcmp(argv[1], kind_names[kind])) ++kind;
    if (kind == KINDS) {
        fprintf(stderr, "recompile_census: %s is not switch, ounce, nro, switch32 or 3ds\n", argv[1]);
        return 2;
    }
    if (kind == SWITCH32 || kind == CTR ? !snx_native_arm_available() : !snx_native_available()) {
        printf("SKIP: no native back end on this host: %s\n", why_none());
        return 77;
    }
    Program p = {0};
    /* past an NRO's header: the entry, which goes there */
    put32(&p, kind == CTR ? A32_BX_LR : kind == SWITCH32 ? 0xEA000000u | (CODE / 4 - 2) : 0x14000000u | CODE / 4);
    while (p.size < CODE) put16(&p, 0);
    if (kind == SWITCH32 || kind == CTR) {
        a32_code(&p, kind == CTR ? SNX_ARM_PROFILE_ARM11 : SNX_ARM_PROFILE_A57);
        thumb_code(&p, kind == CTR ? SNX_ARM_PROFILE_ARM11 : SNX_ARM_PROFILE_A57);
    } else
        a64_code(&p);

    /* the game's folder is named by its title ID, or its file where it has none */
    const uint32_t base = kind == CTR ? CTR_BASE : kind == SWITCH32 ? SNX_CODE_BASE32 : SNX_CODE_BASE;
    const char* files[KINDS] = {"game", "game", "census.nro", "game", "census.cxi"};
    char name[32] = "census", *text = NULL;
    if (kind == SWITCH || kind == OUNCE || kind == SWITCH32) snprintf(name, sizeof name, "%016llX", TITLE);
    char* root = snx_temp_dir(NULL);
    if (!root) return 1;
    char *out = snx_path_join(root, "out"), *input = snx_path_join(root, files[kind]);
    char* hints = snx_format("%s/%s/%s.hints", out, name, name);
    write_game(kind, &p, input, hints, base);
    if (!g_recompile_census.failures) {
        int rc = recompile(kind, (const char* const*)&argv[2], root, out, input, hints, &text);
        if (verbose) puts(text);
        if (rc) {
            fail("%s: the driver's recompile exited with %d, saying:\n%s", kind_names[kind], rc, text);
            fputs("what it left of the back end's:\n", stderr);
            say_left(&p, text, base);
        } else
            check_count(kind, &p, text, base);
    }
    if (snx_remove_tree(root)) fail("cannot remove %s", root);
    free(text), free(hints), free(input), free(out), free(root), free(p.image), free(p.cases);
    return g_recompile_census.failures != 0;
}
