/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 cache of ASTC levels (gpu/vk/bc7_cache.c), without a GPU. A level's key changes with every input that
   changes its blocks (each field of its shape, the encoder's build, any bit of its guest bytes, a word of its layout)
   and is the same for the same inputs. Records come back as written, with any size (one piece of the reading or
   several), from the file in a later run too; a file of another version starts over; a record whose blocks changed on
   disk is dropped when read; one cut short by a crash is cut off, the records before it kept; the bound starts the
   file over; and the file stays locked while it is open */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/bc7_cache.c"
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86) /* as xxh3.c builds it */
#include "xxh_x86dispatch.h"
#define XXH3_64bits  XXH3_64bits_dispatch
#define XXH3_128bits XXH3_128bits_dispatch
#else
#include "xxhash.h"
#endif

/* cmd.c's hashes, which the cache takes */
u64 vk_hash(const void* p, u64 size) { return (u64)XXH3_64bits(p, (size_t)size); }
void vk_hash128(const void* p, u64 size, u64 hash[2]) {
    XXH128_hash_t h = XXH3_128bits(p, (size_t)size);
    hash[0] = h.low64;
    hash[1] = h.high64;
}
u64 vk_hash_copy(void* dst, const void* src, u64 size) {
    memcpy(dst, src, (size_t)size);
    return vk_hash(src, size);
}
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#define PATH "bc7_cache_test.vkbc7"
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            vk_bc7_cache_close();                                                                                      \
            remove(PATH);                                                                                              \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static int same(const u64 a[2], const u64 b[2]) { return a[0] == b[0] && a[1] == b[1]; }

/* a level's key from its guest bytes and layout words as texture.c makes them */
static void key_of(const u8* bytes, u64 n, const u32 layout[8], const VkBc7Level* lv, u64 key[2]) {
    u64 content[2] = {0, 0};
    vk_bc7_content(content, layout, 8 * sizeof *layout);
    vk_bc7_content(content, bytes, n);
    vk_bc7_key(content, lv, key);
}

static void keys(void) {
    u8 bytes[256];
    for (u32 i = 0; i < sizeof bytes; i++) bytes[i] = (u8)(i * 37 + 11);
    const u32 layout[8] = {1, 0, 16 * 32, 64, 4, 0, 8, 1};
    const VkBc7Level base = {128, 256, 1, 1, 8, 8, 0, 0};
    u64 k0[2], k[2];
    key_of(bytes, sizeof bytes, layout, &base, k0);
    key_of(bytes, sizeof bytes, layout, &base, k);
    CHECK(same(k0, k)); /* the same inputs hit */
    /* each field of the shape, by one step and by a high bit */
    for (u32 f = 0; f < sizeof base / sizeof(u32); f++)
        for (u32 d = 0; d < 2; d++) {
            VkBc7Level lv = base;
            u32 w[8];
            memcpy(w, &lv, sizeof w);
            w[f] ^= d ? 0x80u : 1u;
            memcpy(&lv, w, sizeof w);
            key_of(bytes, sizeof bytes, layout, &lv, k);
            CHECK(!same(k0, k));
        }
    /* every bit of the bytes, and every word of the layout */
    for (u32 b = 0; b < sizeof bytes * 8; b++) {
        bytes[b / 8] ^= (u8)(1u << b % 8);
        key_of(bytes, sizeof bytes, layout, &base, k);
        bytes[b / 8] ^= (u8)(1u << b % 8);
        CHECK(!same(k0, k));
    }
    for (u32 i = 0; i < 8; i++) {
        u32 l[8];
        memcpy(l, layout, sizeof l);
        l[i] ^= 1;
        key_of(bytes, sizeof bytes, l, &base, k);
        CHECK(!same(k0, k));
    }
    /* bytes in another order of the same runs, and one byte fewer */
    u64 c[2] = {0, 0};
    vk_bc7_content(c, bytes, 128);
    vk_bc7_content(c, layout, sizeof layout);
    vk_bc7_content(c, bytes + 128, 128);
    vk_bc7_key(c, &base, k);
    CHECK(!same(k0, k));
    key_of(bytes, sizeof bytes - 1, layout, &base, k);
    CHECK(!same(k0, k));
    puts("PASS a level's key changes with each input that changes its blocks, and only with them");
}

static u8* blocks(u64 n, u32 seed) {
    u8* p = malloc((size_t)n);
    for (u64 i = 0; i < n; i++) p[i] = (u8)((i * 2654435761u + seed) >> 13);
    return p;
}
static u64 file_size(void) {
    FILE* f = fopen(PATH, "rb");
    if (!f || fseek(f, 0, SEEK_END)) return 0;
    long n = ftell(f);
    fclose(f);
    return n < 0 ? 0 : (u64)n;
}
/* the record's blocks read back the same */
static int reads_as(const u64 key[2], const u8* p, u64 n) {
    u8* back = malloc((size_t)n);
    int ok = vk_bc7_cache_find(key) == n && vk_bc7_cache_read(key, back, n) && !memcmp(back, p, (size_t)n);
    free(back);
    return ok;
}

static void file(void) {
    const u64 v1[2] = {1, 2}, v2[2] = {1, 3};
    const u64 ka[2] = {10, 20}, kb[2] = {30, 40}, kc[2] = {50, 60};
    const u64 na = 48, nb = (5u << 20) / 2 + 16, nc = 4096; /* one piece, three, one */
    u8 *a = blocks(na, 1), *b = blocks(nb, 2), *c = blocks(nc, 3);
    remove(PATH);
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    CHECK(!vk_bc7_cache_find(ka));
    vk_bc7_cache_put(ka, a, na);
    vk_bc7_cache_put(kb, b, nb);
    CHECK(reads_as(ka, a, na) && reads_as(kb, b, nb));
    u8 small[16];
    CHECK(!vk_bc7_cache_read(ka, small, 16)); /* another size is not the record */
    intptr_t lock;
    CHECK(snx_lock_file(PATH, &lock) == 1); /* held while open */
    vk_bc7_cache_close();
    CHECK(!snx_lock_file(PATH, &lock));
    snx_unlock_file(lock);
    /* a later run */
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    CHECK(reads_as(ka, a, na) && reads_as(kb, b, nb) && !vk_bc7_cache_find(kc));
    vk_bc7_cache_close();
    /* another version: the encoder, its SPIR-V, the GPU or the driver changed */
    CHECK(vk_bc7_cache_open(PATH, v2, 64u << 20));
    CHECK(!vk_bc7_cache_find(ka) && !vk_bc7_cache_find(kb) && file_size() == BC7_HEAD);
    vk_bc7_cache_put(ka, a, na);
    vk_bc7_cache_put(kb, b, nb);
    vk_bc7_cache_close();
    /* a byte of b's blocks changed on disk: found by the index, dropped when read */
    FILE* f = fopen(PATH, "r+b");
    CHECK(f && !fseek(f, (long)(BC7_HEAD + BC7_REC + na + BC7_REC + nb - 7), SEEK_SET));
    int byte = fgetc(f);
    CHECK(byte != EOF && !fseek(f, -1, SEEK_CUR) && fputc(byte ^ 4, f) != EOF && !fclose(f));
    CHECK(vk_bc7_cache_open(PATH, v2, 64u << 20));
    u8* back = malloc((size_t)nb);
    CHECK(vk_bc7_cache_find(kb) == nb && !vk_bc7_cache_read(kb, back, nb) && !vk_bc7_cache_find(kb));
    free(back);
    CHECK(reads_as(ka, a, na));
    vk_bc7_cache_put(kb, b, nb); /* encoded again, and kept again */
    CHECK(reads_as(kb, b, nb));
    vk_bc7_cache_put(kc, c, nc);
    vk_bc7_cache_close();
    /* c's record cut short by a crash: cut off, the others kept */
    const u64 whole = file_size();
    f = fopen(PATH, "r+b");
    CHECK(f && !snx_cut(f, whole - 5) && !fclose(f));
    CHECK(vk_bc7_cache_open(PATH, v2, 64u << 20));
    CHECK(!vk_bc7_cache_find(kc) && reads_as(ka, a, na) && reads_as(kb, b, nb));
    CHECK(file_size() == whole - BC7_REC - nc);
    vk_bc7_cache_close();
    /* the bound: a record that would pass it starts the file over */
    const u64 max = BC7_HEAD + 2 * (BC7_REC + nc);
    u64 hits, stored, restarts, bytes;
    CHECK(vk_bc7_cache_open(PATH, v1, max));
    vk_bc7_cache_put(ka, c, nc);
    vk_bc7_cache_put(kb, c, nc);
    CHECK(vk_bc7_cache_find(ka) && vk_bc7_cache_find(kb));
    vk_bc7_cache_put(kc, c, nc);
    vk_bc7_cache_stats(&hits, &stored, &restarts, &bytes);
    CHECK(restarts == 1 && stored == 3 && bytes == BC7_HEAD + BC7_REC + nc && file_size() <= max);
    CHECK(!vk_bc7_cache_find(ka) && !vk_bc7_cache_find(kb) && reads_as(kc, c, nc));
    vk_bc7_cache_put(kc, a, 16); /* a key held already is not written again */
    CHECK(reads_as(kc, c, nc));
    vk_bc7_cache_close();
    /* many records: the index grows, and finds each again from the file */
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    for (u64 i = 0; i < 3000; i++) {
        const u64 k[2] = {i * 0x9E3779B97F4A7C15ull, i};
        vk_bc7_cache_put(k, a + i % 32, 16);
    }
    vk_bc7_cache_close();
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    for (u64 i = 0; i < 3000; i++) {
        const u64 k[2] = {i * 0x9E3779B97F4A7C15ull, i};
        CHECK(reads_as(k, a + i % 32, 16));
    }
    vk_bc7_cache_close();
    remove(PATH);
    free(a);
    free(b);
    free(c);
    puts("PASS the cache's records come back as written, across runs, and its file holds up to damage and its bound");
}

int main(void) {
    keys();
    file();
    printf("All %u BC7 cache checks passed\n", checks);
    return 0;
}
