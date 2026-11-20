/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 cache of ASTC levels (gpu/vk/bc7_cache.c), without a GPU. A level's key changes with every input that
   changes its blocks (each field of its shape, the encoder's build, any bit of its guest bytes, a word of its layout)
   and is the same for the same inputs. Blocks come back as stored, of any size, while the cache holds them, and a new
   cache holds none. Past its bound it keeps the levels used most: an encode finds room only in levels used less that
   the last two presents left alone, counts halve with time, and the system's low memory word frees what no recent
   present used. A hit copied on the recording thread (vk_bc7_cache_read_later) is copied from blocks that are still
   there when the packet runs, however the level left the cache meanwhile. The bound follows the machine's memory */
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
/* record.c, devmem.c and volk, for the reads on the recording thread: here vk_rec_call runs the read at once, as
   record.c does with no such thread, and the copy is counted */
static int g_threaded;
int vk_rec_threaded(void) { return g_threaded; }
/* with g_defer set, the packets wait for run_deferred, in order, as ones the recording thread gets to later */
static int g_defer;
static unsigned g_ndeferred;
static void (*g_deferred_fn[2048])(const void* arg);
static _Alignas(16) u8 g_deferred[2048][256];
void vk_rec_call(void (*fn)(const void* arg), const void* arg, u64 size) {
    if (!g_defer) {
        fn(arg);
        return;
    }
    if (size > sizeof g_deferred[0] || g_ndeferred == sizeof g_deferred / sizeof g_deferred[0]) abort();
    memcpy(g_deferred[g_ndeferred], arg, (size_t)size);
    g_deferred_fn[g_ndeferred++] = fn;
}
VkResult vk_devmem_flush(DevMem m, u64 off, u64 size) {
    (void)m;
    (void)off;
    (void)size;
    return VK_SUCCESS;
}
static unsigned g_copies;
static u64 g_copy_at;
static VkImageLayout g_copy_layout; /* the layout the copy was given (VK_LAYOUT's, from the GPU thread) */
static void VKAPI_PTR copy_stub(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t n,
                                const VkBufferImageCopy* r) {
    (void)cb;
    (void)src;
    (void)dst;
    g_copy_layout = layout;
    g_copies += n;
    g_copy_at = r->bufferOffset;
}
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = copy_stub;

static char g_said[1024]; /* the last line logged */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_said, sizeof g_said, fmt, ap);
    va_end(ap);
    fputs(g_said, stdout);
}
u32 g_vk_present;

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
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
/* the level's blocks read back the same */
static int reads_as(const u64 key[2], const u8* p, u64 n) {
    u8* back = malloc((size_t)n);
    int ok = vk_bc7_cache_find(key) == n && vk_bc7_cache_read(key, back, n) && !memcmp(back, p, (size_t)n);
    free(back);
    return ok;
}
static void run_deferred(void) {
    for (unsigned i = 0; i < g_ndeferred; i++) g_deferred_fn[i](g_deferred[i]);
    g_ndeferred = 0;
}
#define KEY(n) ((const u64[2]){0x1000 + (u64)(n), 0x77})

static void held(void) {
    static const u64 sizes[] = {16, 4096, 1u << 20, (1u << 20) + 16, 5u << 20};
    VkBc7CacheStats s;
    CHECK(!vk_bc7_cache_max() && !vk_bc7_cache_find(KEY(0))); /* no cache yet */
    vk_bc7_cache_put(KEY(0), (const u8*)"0123456789abcdef", 16);
    CHECK(!vk_bc7_cache_find(KEY(0)));
    CHECK(vk_bc7_cache_open(64u << 20) && vk_bc7_cache_max() == 64u << 20);
    for (u32 i = 0; i < 5; i++) {
        u8* p = blocks(sizes[i], i);
        CHECK(!vk_bc7_cache_find(KEY(i)));
        vk_bc7_cache_put(KEY(i), p, sizes[i]);
        CHECK(reads_as(KEY(i), p, sizes[i]));
        u8 small[16];
        CHECK(!vk_bc7_cache_read(KEY(i), small, sizes[i] + 16)); /* another size is not this level's */
        free(p);
    }
    u8* odd = blocks(40, 9);
    vk_bc7_cache_put(KEY(9), odd, 40); /* not whole blocks */
    CHECK(!vk_bc7_cache_find(KEY(9)));
    free(odd);
    /* 3000 more keys: the table grows, and the first ones are still read */
    for (u32 i = 0; i < 3000; i++) {
        const u32 word[4] = {i, ~i, i * 7, 1};
        vk_bc7_cache_put(KEY(100 + i), (const u8*)word, 16);
    }
    u8* p = blocks(sizes[4], 4);
    CHECK(reads_as(KEY(4), p, sizes[4]));
    free(p);
    for (u32 i = 0; i < 3000; i += 13) {
        const u32 word[4] = {i, ~i, i * 7, 1};
        CHECK(reads_as(KEY(100 + i), (const u8*)word, 16));
    }
    vk_bc7_cache_stats(&s);
    CHECK(s.stored == 3005 && s.levels == 3005 && !s.evicted && !s.refused);
    CHECK(s.bytes == 16 + 4096 + (1u << 20) + (1u << 20) + 16 + (5u << 20) + 3000 * 16);
    /* a new cache holds nothing of the one before */
    CHECK(vk_bc7_cache_open(1u << 20) && !vk_bc7_cache_find(KEY(0)) && !vk_bc7_cache_find(KEY(100)));
    vk_bc7_cache_stats(&s);
    CHECK(!s.stored && !s.levels && !s.bytes && !s.hits);
    vk_bc7_cache_close();
    CHECK(!vk_bc7_cache_max() && !vk_bc7_cache_find(KEY(0)));
    puts("PASS blocks come back as stored while the cache holds them, and a new cache holds none");
}

/* a cache of four levels of 1024 bytes, past which a level needs one used less to leave */
static void most_used(void) {
    VkBc7CacheStats s;
    u8* b[8];
    for (u32 i = 0; i < 8; i++) b[i] = blocks(1024, 50 + i);
    g_vk_present = 100;
    CHECK(vk_bc7_cache_open(4096));
    for (u32 i = 0; i < 4; i++) vk_bc7_cache_put(KEY(i), b[i], 1024);
    g_vk_present = 110;
    vk_bc7_cache_put(KEY(4), b[4], 1024); /* used once, as the four held: none yields */
    vk_bc7_cache_stats(&s);
    CHECK(!vk_bc7_cache_find(KEY(4)) && s.refused == 1 && !s.evicted && s.bytes == 4096 && s.levels == 5);
    for (u32 i = 0; i < 4; i++) CHECK(reads_as(KEY(i), b[i], 1024)); /* each now used twice, in this present */
    vk_bc7_cache_put(KEY(4), b[4], 1024);                            /* twice too: still none used less */
    CHECK(!vk_bc7_cache_find(KEY(4)));
    vk_bc7_cache_put(KEY(4), b[4], 1024); /* three times, but the game is on the four (this present) */
    CHECK(!vk_bc7_cache_find(KEY(4)));
    g_vk_present = 111;
    vk_bc7_cache_put(KEY(4), b[4], 1024); /* the present after: still theirs */
    CHECK(!vk_bc7_cache_find(KEY(4)));
    g_vk_present = 112;
    vk_bc7_cache_put(KEY(4), b[4], 1024); /* two presents on: one level used less goes, and only one */
    vk_bc7_cache_stats(&s);
    CHECK(reads_as(KEY(4), b[4], 1024) && s.evicted == 1 && s.refused == 4 && s.bytes == 4096);
    u32 left = 0;
    for (u32 i = 0; i < 4; i++) left += vk_bc7_cache_find(KEY(i)) == 1024;
    CHECK(left == 3);
    /* a level larger than what the ones that yield hold takes none of them */
    g_vk_present = 120;
    u8* big = blocks(4096, 99);
    for (u32 n = 0; n < 4; n++)
        vk_bc7_cache_put(KEY(5), big, 4096); /* used 4 times: the three used twice yield, KEY(4) not */
    vk_bc7_cache_stats(&s);
    CHECK(!vk_bc7_cache_find(KEY(5)) && s.evicted == 1 && s.bytes == 4096 && vk_bc7_cache_find(KEY(4)) == 1024);
    for (u32 n = 0; n < 4; n++) vk_bc7_cache_put(KEY(5), big, 4096); /* 8 times: all four yield */
    vk_bc7_cache_stats(&s);
    CHECK(reads_as(KEY(5), big, 4096) && s.evicted == 5 && s.bytes == 4096);
    vk_bc7_cache_put(KEY(6), big, 8192); /* past the bound alone: never kept, nothing goes for it */
    CHECK(!vk_bc7_cache_find(KEY(6)) && vk_bc7_cache_find(KEY(5)) == 4096);
    free(big);
    /* the counts halve every BC7_AGE_PRESENTS presents: the level used 9 times yields to one used 5 times after one
       halving, not before */
    g_vk_present += 2;
    for (u32 n = 0; n < 4; n++) vk_bc7_cache_put(KEY(7), b[7], 1024);
    CHECK(!vk_bc7_cache_find(KEY(7)));
    g_vk_present += BC7_AGE_PRESENTS; /* KEY(5): 9 to 4; KEY(7): 4 to 2 */
    vk_bc7_cache_put(KEY(7), b[7], 1024);
    vk_bc7_cache_put(KEY(7), b[7], 1024);
    CHECK(!vk_bc7_cache_find(KEY(7)) && vk_bc7_cache_find(KEY(5)) == 4096); /* 4 uses against 4 */
    vk_bc7_cache_put(KEY(7), b[7], 1024);
    CHECK(reads_as(KEY(7), b[7], 1024) && !vk_bc7_cache_find(KEY(5)));
    vk_bc7_cache_close();
    for (u32 i = 0; i < 8; i++) free(b[i]);
    puts("PASS past its bound the cache keeps the levels used most, lets none go that the last two presents used, "
         "and halves the counts with time");
}

static void low_memory(void) {
    VkBc7CacheStats s;
    u8* b[4];
    for (u32 i = 0; i < 4; i++) b[i] = blocks(2048, 70 + i);
    g_vk_present = 0xFFFFFFF0u; /* the count of presents wraps */
    CHECK(vk_bc7_cache_open(1u << 20));
    vk_bc7_cache_put(KEY(0), b[0], 2048);
    vk_bc7_cache_put(KEY(1), b[1], 2048);
    g_vk_present += 0x20;
    vk_bc7_cache_put(KEY(2), b[2], 2048);
    CHECK(reads_as(KEY(1), b[1], 2048));
    g_vk_present += 1;
    vk_bc7_cache_put(KEY(3), b[3], 2048);
    CHECK(vk_bc7_cache_release(g_vk_present - 1) == 2048); /* KEY(0) alone was last used before the present before */
    vk_bc7_cache_stats(&s);
    CHECK(!vk_bc7_cache_find(KEY(0)) && s.released == 2048 && s.bytes == 3 * 2048 && s.levels == 4);
    CHECK(reads_as(KEY(1), b[1], 2048) && reads_as(KEY(2), b[2], 2048) && reads_as(KEY(3), b[3], 2048));
    CHECK(vk_bc7_cache_release(g_vk_present + 1) == 3 * 2048 && !vk_bc7_cache_release(g_vk_present + 1));
    vk_bc7_cache_put(KEY(0), b[0], 2048); /* encoded again: kept again */
    CHECK(reads_as(KEY(0), b[0], 2048));
    vk_bc7_cache_close();
    for (u32 i = 0; i < 4; i++) free(b[i]);
    puts("PASS the system's low memory word frees the blocks no recent present used, and they are kept again later");
}

/* hits copied on the recording thread: queued, then the level leaves the cache (for one used more, at a low memory
   word, at the cache's close), then the packets run: each copy still finds its blocks, freed only after it */
static void later(void) {
    VkBc7CacheStats s;
    u8* b[3];
    u8 dst[3][1024];
    const VkBufferImageCopy copy = {.bufferOffset = 4096};
    for (u32 i = 0; i < 3; i++) b[i] = blocks(1024, 90 + i);
    g_vk_present = 500;
    CHECK(vk_bc7_cache_open(2048));
    vk_bc7_cache_put(KEY(0), b[0], 1024);
    vk_bc7_cache_put(KEY(1), b[1], 1024);
    g_threaded = 0; /* no such thread: the caller reads them itself */
    CHECK(vk_bc7_cache_read_later(NULL, KEY(0), 1024, dst[0], 0, 0, NULL, NULL, 7, &copy) == -1);
    g_threaded = g_defer = 1;
    g_copies = 0;
    CHECK(vk_bc7_cache_read_later(NULL, KEY(2), 1024, dst[2], 0, 0, NULL, NULL, 7, &copy) == 0); /* not held */
    CHECK(vk_bc7_cache_read_later(NULL, KEY(0), 512, dst[0], 0, 0, NULL, NULL, 7, &copy) == 0);  /* another size */
    CHECK(vk_bc7_cache_read_later(NULL, KEY(0), 1024, dst[0], 0, 0, NULL, (VkImage)(uintptr_t)1, 7, &copy) == 1);
    CHECK(vk_bc7_cache_read_later(NULL, KEY(1), 1024, dst[1], 0, 0, NULL, VK_NULL_HANDLE, 7, &copy) == 1);
    CHECK(g_ndeferred == 2 && !g_copies);
    g_vk_present += 2;
    for (u32 n = 0; n < 3; n++) vk_bc7_cache_put(KEY(2), b[2], 1024); /* used 3 times: KEY(0), used twice, goes */
    CHECK(vk_bc7_cache_find(KEY(2)) == 1024 && vk_bc7_cache_find(KEY(0)) + vk_bc7_cache_find(KEY(1)) == 1024);
    CHECK(vk_bc7_cache_release(g_vk_present + 1) == 2048); /* and the rest at a low memory word */
    CHECK(g_ndeferred == 5);
    vk_bc7_cache_stats(&s);
    CHECK(!s.hits);
    run_deferred(); /* the two copies, then the three frees: a build with a memory checker reads freed blocks here */
    CHECK(!memcmp(dst[0], b[0], 1024) && !memcmp(dst[1], b[1], 1024));
    CHECK(g_copies == 1 && g_copy_at == 4096 && g_copy_layout == 7); /* no copy without an image */
    vk_bc7_cache_stats(&s);
    CHECK(s.hits == 2);
    /* a close with a copy queued */
    vk_bc7_cache_put(KEY(0), b[0], 1024);
    memset(dst[0], 0, sizeof dst[0]);
    CHECK(vk_bc7_cache_read_later(NULL, KEY(0), 1024, dst[0], 0, 0, NULL, NULL, 7, &copy) == 1);
    vk_bc7_cache_close();
    run_deferred();
    CHECK(!memcmp(dst[0], b[0], 1024));
    g_threaded = g_defer = 0;
    for (u32 i = 0; i < 3; i++) free(b[i]);
    puts("PASS a hit queued for the recording thread is copied from its blocks however the level left the cache");
}

static void bound(void) {
    CHECK(bc7_share(0, 0) == 256ull << 20 && bc7_share(0, 64ull << 30) == 256ull
                                                                              << 20); /* a system that doesn't tell */
    CHECK(bc7_share(1, 256ull << 20) == 64ull << 20);                                 /* the least */
    CHECK(bc7_share(1, 4000ull << 20) == 500ull << 20);                               /* a phone of 4 GB */
    CHECK(bc7_share(1, 8ull << 30) == 1ull << 30);
    CHECK(bc7_share(1, 16ull << 30) == 2ull << 30 && bc7_share(1, 64ull << 30) == 2ull << 30); /* the most */
    CHECK(vk_bc7_cache_open(0));
    SnxU64 total = 0;
    CHECK(vk_bc7_cache_max() == bc7_share(snx_memory_total(&total), total));
    vk_bc7_cache_close();
    puts("PASS the bound is an eighth of the machine's memory, from 64 MiB to 2 GiB, and 256 MiB where it is not told");
}

/* a file an earlier version kept the cache in: said with its path and size, and left as it is; none, nothing said */
static void old_file(void) {
    static const char path[] = "bc7_cache_test_old.vkbc7";
    u8* p = blocks(3u << 19, 5); /* 1.5 MiB */
    FILE* f = fopen(path, "wb");
    CHECK(f && fwrite(p, 1, 3u << 19, f) == 3u << 19 && !fclose(f));
    g_said[0] = 0;
    CHECK(vk_bc7_cache_old_file(path));
    CHECK(!strcmp(g_said,
                  "vulkan: bc7_cache_test_old.vkbc7 (1.5 MB) is a BC7 cache file of an earlier version, which this "
                  "one does not read or write: the BC7 cache is held in memory now. The game's Delete Old "
                  "Caches in the launcher deletes it\n"));
    f = fopen(path, "rb"); /* still there, whole */
    CHECK(f && !fseek(f, 0, SEEK_END) && ftell(f) == 3 << 19 && !fclose(f));
    remove(path);
    g_said[0] = 0;
    CHECK(!vk_bc7_cache_old_file(path) && !g_said[0]);
    free(p);
    puts("PASS a BC7 cache file of an earlier version is said with its path and size, and left as it is");
}

int main(void) {
    keys();
    old_file();
    held();
    most_used();
    low_memory();
    later();
    bound();
    printf("bc7_cache: %u checks passed\n", checks);
    return 0;
}
