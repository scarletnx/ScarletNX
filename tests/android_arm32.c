/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit native bridge (src/android/bridge/arm32*.c): its loader on ELF32 ARM libraries the test writes, and
   bionic's functions as 32-bit guest code calls them, through each import's export and a call into the guest
   (arm32_call), against what bionic gives on Android 16 */
#include "android/bridge/arm32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
    int failures;
    char dir[64]; /* the libraries the test writes */
} g_t;

static void expect_int(const char* what, int64_t got, int64_t want) {
    if (got != want) {
        fprintf(stderr, "android_arm32: %s: got %lld (0x%llx), want %lld (0x%llx)\n", what, (long long)got,
                (unsigned long long)got, (long long)want, (unsigned long long)want);
        g_t.failures++;
    }
}

/* ---- the loader ---- */

static void le16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void le32(unsigned char* p, uint32_t v) {
    le16(p, (uint16_t)v);
    le16(p + 2, (uint16_t)(v >> 16));
}
static uint32_t gnu_hash_of(const char* name) {
    uint32_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) h = h * 33 + *p;
    return h;
}

enum { SYSV = 1, GNU = 2 };
/* A library of two A32 functions, answer() (42) and twice(x) (x + x), with the hash tables of hashes (SYSV, GNU or
   both), as one PT_LOAD of 320 bytes: the ELF header, two program headers, the code at 128, the symbols at 144, their
   names at 192, the SysV table at 208, the GNU one at 232 and the dynamic section at 264 */
static void write_library(const char* path, int hashes) {
    unsigned char f[320] = {0};
    memcpy(f, "\177ELF\1\1\1", 7);
    le16(f + 16, 3);  /* ET_DYN */
    le16(f + 18, 40); /* EM_ARM */
    le32(f + 20, 1);
    le32(f + 28, 52);         /* e_phoff */
    le32(f + 36, 0x5000000u); /* EABI 5 */
    le16(f + 40, 52);
    le16(f + 42, 32);
    le16(f + 44, 2);
    le16(f + 46, 40);
    unsigned char* ph = f + 52; /* PT_LOAD of the whole file, read, write and execute */
    le32(ph, 1);
    le32(ph + 16, sizeof f);
    le32(ph + 20, sizeof f);
    le32(ph + 24, 7);
    le32(ph + 28, 0x1000);
    ph += 32; /* PT_DYNAMIC */
    le32(ph, 2);
    le32(ph + 4, 264);
    le32(ph + 8, 264);
    le32(ph + 16, 56);
    le32(ph + 20, 56);
    le32(ph + 24, 6);
    le32(f + 128, 0xe3a0002au); /* answer: mov r0, #42 */
    le32(f + 132, 0xe12fff1eu); /* bx lr */
    le32(f + 136, 0xe0800000u); /* twice: add r0, r0, r0 */
    le32(f + 140, 0xe12fff1eu); /* bx lr */
    static const char names[] = "\0answer\0twice";
    memcpy(f + 192, names, sizeof names);
    const uint32_t name_at[3] = {0, 1, 8}, value[3] = {0, 128, 136};
    for (unsigned i = 1; i < 3; ++i) {
        unsigned char* sym = f + 144 + 16 * i;
        le32(sym, name_at[i]);
        le32(sym + 4, value[i]);
        le32(sym + 8, 8);
        sym[12] = 0x12; /* STB_GLOBAL, STT_FUNC */
        le16(sym + 14, 1);
    }
    /* SysV: one bucket, its chain 1 then 2 */
    le32(f + 208, 1);
    le32(f + 212, 3);
    le32(f + 216, 1);
    le32(f + 220 + 4 * 1, 2);
    /* GNU: one bucket from symbol 1, a bloom word of 32 bits with both names' two bits, their hashes, the last odd */
    uint32_t h1 = gnu_hash_of("answer"), h2 = gnu_hash_of("twice"), shift = 5;
    uint32_t bloom = 1u << (h1 % 32) | 1u << ((h1 >> shift) % 32) | 1u << (h2 % 32) | 1u << ((h2 >> shift) % 32);
    le32(f + 232, 1);
    le32(f + 236, 1);
    le32(f + 240, 1);
    le32(f + 244, shift);
    le32(f + 248, bloom);
    le32(f + 252, 1);
    le32(f + 256, h1 & ~1u);
    le32(f + 260, h2 | 1u);
    unsigned char* d = f + 264;
    const uint32_t tags[][2] = {{5, 192}, {6, 144}, {10, sizeof names}, {11, 16}};
    for (unsigned i = 0; i < 4; ++i, d += 8) {
        le32(d, tags[i][0]);
        le32(d + 4, tags[i][1]);
    }
    if (hashes & SYSV) {
        le32(d, 4); /* DT_HASH */
        le32(d + 4, 208);
        d += 8;
    }
    if (hashes & GNU) {
        le32(d, 0x6ffffef5u); /* DT_GNU_HASH */
        le32(d + 4, 232);
    }
    FILE* out = fopen(path, "wb");
    if (!out || fwrite(f, 1, sizeof f, out) != sizeof f || fclose(out)) {
        fprintf(stderr, "android_arm32: cannot write %s\n", path);
        exit(1);
    }
}

/* each library's functions found by name and called, whatever hash tables it has */
static void test_loader(void) {
    static const struct {
        const char* name;
        int hashes;
    } libraries[] = {{"libsysv.so", SYSV}, {"libgnu.so", GNU}, {"libboth.so", SYSV | GNU}};
    for (size_t i = 0; i < sizeof libraries / sizeof *libraries; ++i) {
        char path[128], what[160], error[256];
        snprintf(path, sizeof path, "%s/%s", g_t.dir, libraries[i].name);
        write_library(path, libraries[i].hashes);
        const Arm32Library* l = arm32_load(path, error, sizeof error);
        if (!l) {
            fprintf(stderr, "android_arm32: %s does not load: %s\n", libraries[i].name, error);
            g_t.failures++;
            continue;
        }
        uint32_t answer = arm32_symbol(l, "answer"), twice = arm32_symbol(l, "twice");
        snprintf(what, sizeof what, "%s: answer is found", libraries[i].name);
        expect_int(what, answer != 0, 1);
        snprintf(what, sizeof what, "%s: twice is found", libraries[i].name);
        expect_int(what, twice != 0, 1);
        snprintf(what, sizeof what, "%s: a name it lacks is not", libraries[i].name);
        expect_int(what, arm32_symbol(l, "absent"), 0);
        if (answer) {
            snprintf(what, sizeof what, "%s: answer()", libraries[i].name);
            expect_int(what, arm32_call(answer, NULL, 0, NULL), 42);
        }
        if (twice) {
            const uint32_t x = 21;
            snprintf(what, sizeof what, "%s: twice(21)", libraries[i].name);
            expect_int(what, arm32_call(twice, &x, 1, NULL), 42);
        }
        remove(path);
    }
}

int main(void) {
    char error[256];
    if (!arm32_init(error, sizeof error)) {
        fprintf(stderr, "android_arm32: the 32-bit bridge does not start: %s\n", error);
        return 1;
    }
    snprintf(g_t.dir, sizeof g_t.dir, "android-arm32-XXXXXX");
    if (!mkdtemp(g_t.dir)) {
        fprintf(stderr, "android_arm32: no folder for the test's libraries in the working folder\n");
        return 1;
    }
    test_loader();
    rmdir(g_t.dir);
    if (g_t.failures) fprintf(stderr, "android_arm32: %d check(s) failed\n", g_t.failures);
    return g_t.failures ? 1 : 0;
}
