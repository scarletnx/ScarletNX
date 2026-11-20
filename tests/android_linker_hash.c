/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Android bridge's linker (src/android/bridge/linker.c) over libraries whose symbol hash tables hold nothing: a
   DT_GNU_HASH of no bucket, one of no filter word, and a DT_HASH of no bucket. A lookup's hash is taken modulo those
   counts, which an app's library gives: such a library has no symbol to find, and a lookup in it answers none. The
   libraries are made here, of a header, two segments and the dynamic section's tables, and nothing of them runs */
#include "android/bridge/bridge.h"
#include "android/bridge/linker.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { DYNAMIC = 0x200, STRINGS = 0x300, SYMBOLS = 0x310, HASH = 0x340, SIZE = 0x400 };

static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, uint32_t v) { put16(p, v & 0xffff), put16(p + 2, v >> 16); }
static void put64(unsigned char* p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }

/* a shared library for AArch64 at path: one loaded segment of SIZE bytes, its dynamic section naming an empty string
   table, a symbol table of the null symbol and a hash table (DT_GNU_HASH when gnu, else DT_HASH) of these four words */
static void library(const char* path, int gnu, const uint32_t words[4]) {
    unsigned char b[SIZE] = {0};
    memcpy(b, "\177ELF\2\1\1", 7);
    put16(b + 16, 3), put16(b + 18, 183), put32(b + 20, 1); /* ET_DYN, EM_AARCH64 */
    put64(b + 32, 64), put16(b + 52, 64), put16(b + 54, 56), put16(b + 56, 2);
    unsigned char* ph = b + 64;
    put32(ph, 1), put32(ph + 4, 6), put64(ph + 32, SIZE), put64(ph + 40, SIZE),
        put64(ph + 48, 0x10000); /* PT_LOAD, RW */
    ph += 56;
    put32(ph, 2), put32(ph + 4, 6), put64(ph + 8, DYNAMIC), put64(ph + 16, DYNAMIC), put64(ph + 32, 64),
        put64(ph + 40, 64);
    const uint64_t dynamic[8] = {5, STRINGS, 6, SYMBOLS, gnu ? 0x6ffffef5u : 4, HASH, 0, 0}; /* DT_STRTAB, DT_SYMTAB */
    for (unsigned i = 0; i < 8; ++i) put64(b + DYNAMIC + 8 * i, dynamic[i]);
    for (unsigned i = 0; i < 4; ++i) put32(b + HASH + 4 * i, words[i]);
    memset(b + HASH + 16, 0xff, 8); /* a filter word that lets every hash through */
    FILE* f = fopen(path, "wb");
    assert(f && fwrite(b, 1, sizeof b, f) == sizeof b && !fclose(f));
}

int main(void) {
    char error[256] = "", dir[64] = "android-linker-hash-XXXXXX", path[128];
    static const struct {
        const char* name;
        int gnu;
        uint32_t words[4]; /* DT_GNU_HASH: buckets, the first symbol hashed, filter words, shift; DT_HASH: buckets */
    } cases[] = {
        {"libnobuckets.so", 1, {0, 1, 1, 6}}, {"libnofilter.so", 1, {1, 1, 0, 6}}, {"libnosysv.so", 0, {0, 1, 0, 0}}};
    if (!bridge_init(NULL, error, sizeof error)) {
        fprintf(stderr, "android_linker_hash: the bridge does not start: %s\n", error);
        return 1;
    }
    assert(mkdtemp(dir));
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, cases[i].name);
        library(path, cases[i].gnu, cases[i].words);
        GuestLibrary* l = linker_load(path, error, sizeof error);
        if (!l) fprintf(stderr, "android_linker_hash: %s does not load: %s\n", cases[i].name, error);
        assert(l && !linker_symbol(l, "anything") && !linker_symbol(NULL, "anything_else"));
        assert(!remove(path));
    }
    assert(!rmdir(dir));
    puts("android_linker_hash: ok");
    return 0;
}
