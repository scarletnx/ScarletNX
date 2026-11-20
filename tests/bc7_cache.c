/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 cache of ASTC levels (gpu/vk/bc7_cache.c), without a GPU. A level's key changes with every input that
   changes its blocks (each field of its shape, the encoder's build, any bit of its guest bytes, a word of its layout)
   and is the same for the same inputs. Records come back as written, with any size (one piece of the reading or
   several), from the file in a later run too; a file of another version starts over; a record whose blocks changed on
   disk is dropped when read; one cut short by a crash is cut off, the records before it kept; the bound starts the
   file over; and the file stays locked while it is open. A hit read for the recording thread (vk_bc7_cache_read_later)
   is copied only when it checks out, and one that doesn't is dropped; one the disk holds is left late, its record
   kept, and the log says what it cost */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <unistd.h>
#endif
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
int vk_rec_sync_until(u64 until) {
    (void)until;
    return 1;
}
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
/* the lines logged, the cache's I/O thread's too, kept for the checks */
static PMutex g_log_m = PMUTEX_INIT;
static char g_logged[1 << 14];
static size_t g_logged_n;
void logf_(int lvl, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
#if defined(__linux__)
    /* the I/O thread's lines written slowly, as on the slow disk they are about: a wait for that thread to be idle
       (io_idle_by) finds them written all the same */
    if (strstr(line, "the BC7 cache took ") || strstr(line, "reads caught up")) usleep(100000);
#endif
    pm_lock(&g_log_m);
    const size_t n = strlen(line);
    if (n < sizeof g_logged - g_logged_n) {
        memcpy(g_logged + g_logged_n, line, n + 1);
        g_logged_n += n;
    }
    pm_unlock(&g_log_m);
}

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
/* A disk that answers no read while g_hold is set, as one past full held the recording thread 184 s (#2487): every
   pread of the cache (file.c's plat_pread_fd) comes through here, and waits */
static _Atomic int g_hold, g_hold_writes;
static _Atomic u32 g_slow_us;                                /* and a disk that answers each read this late */
ssize_t pread64(int fd, void* buf, size_t n, __off64_t off); /* unistd.h declares it only for _LARGEFILE64_SOURCE */
ssize_t pread64(int fd, void* buf, size_t n, __off64_t off) {
    while (atomic_load_explicit(&g_hold, memory_order_acquire)) usleep(1000);
    if (atomic_load_explicit(&g_slow_us, memory_order_acquire))
        usleep(atomic_load_explicit(&g_slow_us, memory_order_acquire));
    return (ssize_t)syscall(SYS_pread64, fd, buf, n, off);
}
/* the same for writes (file.c's plat_pwrite_fd): a disk that takes none while g_hold_writes is set */
ssize_t pwrite64(int fd, const void* buf, size_t n, __off64_t off);
ssize_t pwrite64(int fd, const void* buf, size_t n, __off64_t off) {
    while (atomic_load_explicit(&g_hold_writes, memory_order_acquire)) usleep(1000);
    return (ssize_t)syscall(SYS_pwrite64, fd, buf, n, off);
}
#endif

#define PATH "bc7_cache_test.vkbc7"
#define IDX  PATH ".idx"
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            vk_bc7_cache_close();                                                                                      \
            remove(PATH);                                                                                              \
            remove(IDX);                                                                                               \
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
static u64 size_of(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END)) return 0;
    long n = ftell(f);
    fclose(f);
    return n < 0 ? 0 : (u64)n;
}
static u64 file_size(void) { return size_of(PATH); }
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
    remove(IDX);
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    CHECK(!vk_bc7_cache_find(ka));
    CHECK(vk_bc7_cache_cold()); /* the run's notice: every level is encoded as it first loads */
    vk_bc7_cache_put(ka, a, na);
    vk_bc7_cache_put(kb, b, nb);
    CHECK(reads_as(ka, a, na) && reads_as(kb, b, nb));
    CHECK(vk_bc7_cache_cold()); /* as it opened, the levels it takes since aside */
    u8 small[16];
    CHECK(!vk_bc7_cache_read(ka, small, 16)); /* another size is not the record */
    intptr_t lock;
    CHECK(snx_lock_file(PATH, &lock) == 1); /* held while open */
    vk_bc7_cache_close();
    CHECK(!vk_bc7_cache_cold()); /* none open */
    CHECK(!snx_lock_file(PATH, &lock));
    snx_unlock_file(lock);
    /* a later run */
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    CHECK(reads_as(ka, a, na) && reads_as(kb, b, nb) && !vk_bc7_cache_find(kc));
    CHECK(!vk_bc7_cache_cold());
    vk_bc7_cache_close();
    /* another version: the encoder, its SPIR-V, the GPU or the driver changed */
    CHECK(vk_bc7_cache_open(PATH, v2, 64u << 20));
    CHECK(!vk_bc7_cache_find(ka) && !vk_bc7_cache_find(kb) && file_size() == BC7_HEAD);
    CHECK(vk_bc7_cache_cold());
    vk_bc7_cache_close();
    CHECK(vk_bc7_cache_open(PATH, v2, 64u << 20)); /* its header alone, of this version: no level yet */
    CHECK(vk_bc7_cache_cold() && file_size() == BC7_HEAD);
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
    /* the index file: an entry a record, in the file's order. Gone, or of another version, the records' headers are
       read and it is written anew; each record is found either way */
    const u64 idx_size = size_of(IDX);
    CHECK(idx_size == BC7_HEAD + 3001ull * 32);
    for (int damage = 0; damage < 2; damage++) {
        if (!damage)
            remove(IDX);
        else {
            f = fopen(IDX, "r+b");
            CHECK(f && !fseek(f, 20, SEEK_SET) && fputc(0x5a, f) != EOF && !fclose(f));
        }
        CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
        CHECK(size_of(IDX) == idx_size && reads_as(kc, c, nc));
        for (u64 i = 0; i < 3000; i += 7) {
            const u64 k[2] = {i * 0x9E3779B97F4A7C15ull, i};
            CHECK(reads_as(k, a + i % 32, 16));
        }
        vk_bc7_cache_close();
    }
    remove(PATH);
    remove(IDX);
    free(a);
    free(b);
    free(c);
    puts("PASS the cache's records come back as written, across runs, and its file holds up to damage and its bound");
}

/* hits read on the recording thread: copied into the image when they check out, not copied when they don't, and then
   dropped at the index's next look; with no such thread they are read at once (-1) */
/* the reads that failed on the recording thread, as vk_bc7_cache_take_failed gives them */
static unsigned g_failed;
static void* g_failed_owner;
static u64 g_failed_gva, g_failed_key[2];
static void took_failed(void* owner, u64 gva, const u64 key[2]) {
    g_failed++;
    g_failed_owner = owner;
    g_failed_gva = gva;
    g_failed_key[0] = key[0];
    g_failed_key[1] = key[1];
}

static void later(void) {
    const u64 v1[2] = {1, 2};
    const u64 ka[2] = {10, 20}, kb[2] = {30, 40}, kc[2] = {50, 60};
    const u64 na = 48, nb = (5u << 20) / 2 + 16; /* one piece, three */
    u8 *a = blocks(na, 4), *b = blocks(nb, 5), *dst = malloc((size_t)nb);
    int owner;
    remove(PATH);
    remove(IDX);
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    vk_bc7_cache_put(ka, a, na);
    vk_bc7_cache_put(kb, b, nb);
    vk_bc7_cache_close();
    FILE* f = fopen(PATH, "r+b"); /* a byte of b's blocks changed on disk */
    CHECK(f && !fseek(f, (long)(BC7_HEAD + BC7_REC + na + BC7_REC + 9), SEEK_SET));
    int byte = fgetc(f);
    CHECK(byte != EOF && !fseek(f, -1, SEEK_CUR) && fputc(byte ^ 1, f) != EOF && !fclose(f));
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    const VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)8;
    const VkBuffer buf = (VkBuffer)(uintptr_t)16;
    const VkImage img = (VkImage)(uintptr_t)24;
    const VkBufferImageCopy copy = {.bufferOffset = 4096};
    const VkImageLayout L = VK_IMAGE_LAYOUT_GENERAL; /* as with unified image layouts */
    /* before the recording thread runs: read at once, and the next read once it runs goes there */
    CHECK(vk_bc7_cache_read_later(cb, ka, na, dst, 1, 4096, buf, img, L, &copy, NULL, 0) == -1);
    CHECK(!vk_bc7_cache_failed_pending());
    g_threaded = 1;
    CHECK(vk_bc7_cache_read_later(cb, ka, na, dst, 1, 4096, buf, img, L, &copy, NULL, 0) == 1);
    CHECK(g_copies == 1 && g_copy_at == 4096 && g_copy_layout == L && !memcmp(dst, a, (size_t)na));
    CHECK(vk_bc7_cache_read_later(cb, ka, na, dst, 1, 4096, buf, VK_NULL_HANDLE, L, &copy, NULL, 0) == 1 &&
          g_copies == 1);
    CHECK(vk_bc7_cache_read_later(cb, ka, 16, dst, 1, 4096, buf, img, L, &copy, NULL, 0) == 0); /* another size */
    CHECK(vk_bc7_cache_read_later(cb, kc, na, dst, 1, 4096, buf, img, L, &copy, NULL, 0) == 0); /* not there */
    CHECK(vk_bc7_cache_find(kb) == nb);
    CHECK(vk_bc7_cache_read_later(cb, kb, nb, dst, 1, 4096, buf, img, L, &copy, &owner, 0x1234000) == 1 &&
          g_copies == 1);
    CHECK(!vk_bc7_cache_find(kb) && vk_bc7_cache_find(ka) == na);
    CHECK(vk_bc7_cache_failed_pending());  /* until its owner has it */
    vk_bc7_cache_take_failed(took_failed); /* given to its owner once */
    CHECK(g_failed == 1 && g_failed_owner == &owner && g_failed_gva == 0x1234000 && same(g_failed_key, kb));
    CHECK(!vk_bc7_cache_failed_pending());
    vk_bc7_cache_take_failed(took_failed);
    CHECK(g_failed == 1);
    u64 hits, stored, restarts, bytes;
    vk_bc7_cache_stats(&hits, &stored, &restarts, &bytes);
    CHECK(hits == 2);
    vk_bc7_cache_put(kb, b, nb); /* encoded again, and kept again at another place */
    CHECK(vk_bc7_cache_read_later(cb, kb, nb, dst, 1, 4096, buf, img, L, &copy, &owner, 0x1234000) == 1 &&
          g_copies == 2 && !memcmp(dst, b, (size_t)nb));
    vk_bc7_cache_take_failed(took_failed);
    CHECK(g_failed == 1);
    /* the rate of the reads so far: none for nothing, and a whole budget's worth before any read was timed */
    CHECK(!vk_bc7_cache_read_ns(0) && vk_bc7_cache_read_ns(1u << 20) < 1000000000ull);
    vk_bc7_cache_close();
    g_threaded = 0;
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    CHECK(vk_bc7_cache_read_later(cb, ka, na, dst, 1, 4096, buf, img, L, &copy, NULL, 0) == -1 && reads_as(ka, a, na));
    vk_bc7_cache_close();
    remove(PATH);
    remove(IDX);
    free(a);
    free(b);
    free(dst);
    puts("PASS hits read on the recording thread are copied when they check out, and given back when they don't");
}

/* Hits on a disk that holds every read, on one that answers each in 150 ms, and on one that holds a read and then
   answers slowly. Held: a read queued before the recording thread gets to it, and none more is asked once that read
   runs past BC7_IO_LATE_NS (0: the level is encoded); the recording thread leaves it, with no copy, and goes on; once
   the disk lets go, the log says how long the read took and what came late or was not tried. Slow but answering:
   four reads queued at once, 600 ms of reads, are all waited for and copied, none left. Held, then slow: a read left
   late, and while the I/O thread goes on with the read after it, no read is asked (0) until it has caught up, which
   the log says with the longest read. Each late read goes to its owner with its record kept, and a disk that answers
   again has the reads copied. A watchdog lets the disk go after 3 s, so a recording thread that waits for a held read
   fails the check of its time rather than hang */
#if defined(__linux__)
/* the next packet deferred */
static void run_deferred(void) {
    static unsigned ran;
    g_deferred_fn[ran](g_deferred[ran]);
    if (++ran == g_ndeferred) ran = g_ndeferred = 0;
}
static void let_go_by(void* arg) {
    const u64 until = *(const u64*)arg;
    while (atomic_load_explicit(&g_hold, memory_order_acquire) && plat_ns() < until) usleep(1000);
    atomic_store_explicit(&g_hold, 0, memory_order_release);
}
/* the double after `at` in the lines logged, -1 when no line has it */
static double logged_after(const char* at) {
    pm_lock(&g_log_m);
    const char* said = strstr(g_logged, at);
    const double v = said ? atof(said + strlen(at)) : -1;
    pm_unlock(&g_log_m);
    return v;
}
static int logged(const char* text) {
    pm_lock(&g_log_m);
    const int found = strstr(g_logged, text) != NULL;
    pm_unlock(&g_log_m);
    return found;
}
static void slow_disk(void) {
    const u64 v1[2] = {1, 2}, ka[2] = {70, 80};
    const u64 na = 4096;
    u8 *a = blocks(na, 6), *dst = malloc((size_t)na);
    int owner;
    remove(PATH);
    remove(IDX);
    CHECK(vk_bc7_cache_open(PATH, v1, 64u << 20));
    vk_bc7_cache_put(ka, a, na);
    g_threaded = 1;
    const VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)8;
    const VkBuffer buf = (VkBuffer)(uintptr_t)16;
    const VkImage img = (VkImage)(uintptr_t)24;
    const VkBufferImageCopy copy = {.bufferOffset = 4096};
    const VkImageLayout L = VK_IMAGE_LAYOUT_GENERAL;
    const unsigned copies = g_copies, failed = g_failed;
#define READ_A() vk_bc7_cache_read_later(cb, ka, na, dst, 1, 4096, buf, img, L, &copy, &owner, 0x5000)
    /* a disk that answers: the read is copied, the recording thread having waited for the I/O thread */
    memset(dst, 0, (size_t)na);
    CHECK(READ_A() == 1 && g_copies == copies + 1 && !memcmp(dst, a, (size_t)na));

    /* held */
    static u64 until;
    until = plat_ns() + 3000000000ull;
    atomic_store_explicit(&g_hold, 1, memory_order_release);
    CHECK(plat_thread(let_go_by, &until, 0) == 0);
    g_defer = 1;
    CHECK(READ_A() == 1); /* queued, its packet still to come */
    g_defer = 0;
    usleep(300000);
    CHECK(READ_A() == 0); /* the I/O thread held past BC7_IO_LATE_NS: not tried */
    u64 t0 = plat_ns();
    run_deferred();
    u64 took = plat_ns() - t0;
    printf("The recording thread left the held read after %.0f ms\n", (double)took / 1e6);
    CHECK(took < 2500000000ull);   /* left, not held to the watchdog's 3 s */
    CHECK(g_copies == copies + 1); /* its copy left out */
    CHECK(vk_bc7_cache_failed_pending());
    usleep(450000); /* the read held 0.75 s at least */
    atomic_store_explicit(&g_hold, 0, memory_order_release);
    CHECK(io_idle_by(plat_ns() + 5000000000ull));
    const double held = logged_after("vulkan: a read of 4 KiB from the BC7 cache took ");
    CHECK(held >= 0.7 && held < 3.0);
    CHECK(logged(" s: 1 reads came too late meanwhile and 1 were not tried, their levels encoded instead\n"));

    /* slow but answering: 75 ms each of a hit's two reads (its record's header, then its blocks), 150 ms a read */
    atomic_store_explicit(&g_slow_us, 75000, memory_order_release);
    g_defer = 1;
    for (int i = 0; i < 4; i++) CHECK(READ_A() == 1);
    g_defer = 0;
    t0 = plat_ns();
    for (int i = 0; i < 4; i++) {
        memset(dst, 0, (size_t)na);
        run_deferred();
        CHECK(g_copies == copies + 2 + (unsigned)i && !memcmp(dst, a, (size_t)na));
    }
    took = plat_ns() - t0;
    printf("The recording thread took four reads of 150 ms in %.0f ms\n", (double)took / 1e6);
    CHECK(took >= 500000000ull); /* waited for, one after another */
    u64 late, skipped, unkept;
    vk_bc7_cache_late(&late, &skipped, &unkept);
    CHECK(late == 1 && skipped == 1); /* no more than the held read's */

    /* held, then slow: the read after the one left late runs while the I/O thread has not caught up */
    atomic_store_explicit(&g_hold, 1, memory_order_release);
    until = plat_ns() + 3000000000ull;
    CHECK(plat_thread(let_go_by, &until, 0) == 0);
    g_defer = 1;
    CHECK(READ_A() == 1 && READ_A() == 1); /* the second queued while the first has just begun */
    g_defer = 0;
    usleep(300000);
    run_deferred(); /* the first, held past BC7_IO_LATE_NS: left */
    CHECK(g_copies == copies + 5);
    const u64 held_since = atomic_load_explicit(&g_bc7_cache.io.since, memory_order_acquire);
    atomic_store_explicit(&g_hold, 0, memory_order_release);
    while (atomic_load_explicit(&g_bc7_cache.io.since, memory_order_acquire) == held_since) usleep(1000);
    CHECK(READ_A() == 0); /* the second read runs, under BC7_IO_LATE_NS, behind one left late: not tried */
    run_deferred();       /* the second, while the I/O thread has not caught up: left at once, or taken if done */
    CHECK(io_idle_by(plat_ns() + 5000000000ull));
    atomic_store_explicit(&g_slow_us, 0, memory_order_release);
    CHECK(logged("vulkan: the BC7 cache's reads caught up: "));
    const double longest = logged_after("were not tried, their levels encoded instead (the longest read took ");
    CHECK(longest >= 140 && longest < 250); /* the second read's 150 ms */

    vk_bc7_cache_late(&late, &skipped, &unkept);
    CHECK(late >= 2 && late <= 3 && skipped == 2);
    vk_bc7_cache_take_failed(took_failed); /* to their owner, loaded again */
    CHECK(g_failed == failed + late && g_failed_owner == &owner && g_failed_gva == 0x5000 && same(g_failed_key, ka));
    CHECK(vk_bc7_cache_find(ka) == na); /* the record kept */
    /* the disk answers again: the reads go to the I/O thread and are copied */
    const unsigned now_copies = g_copies;
    memset(dst, 0, (size_t)na);
    CHECK(READ_A() == 1 && g_copies == now_copies + 1 && !memcmp(dst, a, (size_t)na));
    /* a load's burst: 2000 hits asked before the recording thread takes any, as many as its ring holds of a frame's,
       are all read and copied, none refused */
    vk_bc7_cache_late(&late, &skipped, &unkept);
    const u64 skipped_before = skipped;
    g_defer = 1;
    for (int i = 0; i < 2000; i++) CHECK(READ_A() == 1);
    g_defer = 0;
    for (int i = 0; i < 2000; i++) run_deferred();
    vk_bc7_cache_late(&late, &skipped, &unkept);
    CHECK(skipped == skipped_before && g_copies == now_copies + 2001);
#undef READ_A
    vk_bc7_cache_close();
    g_threaded = 0;
    remove(PATH);
    remove(IDX);
    free(a);
    free(dst);
    puts("PASS a held read: the recording thread leaves it late and goes on, no more are asked until the I/O thread "
         "caught up, and the log says what it cost; a slow disk that answers has its reads waited for");
}

/* Records put while the disk takes no write: the GPU thread puts each at once, in the index, and goes on; a read of
   one, with the write held past BC7_IO_LATE_NS, is not tried (0), its record kept; past 64 MiB waiting a record is
   not kept; once the disk takes them they are read back, and in a later run, and the log says how long the write took
   and what was not kept. A watchdog lets the disk go after 3 s, so a put that waits for its write fails the check of
   its time */
static void let_writes_go_by(void* arg) {
    const u64 until = *(const u64*)arg;
    while (atomic_load_explicit(&g_hold_writes, memory_order_acquire) && plat_ns() < until) usleep(1000);
    atomic_store_explicit(&g_hold_writes, 0, memory_order_release);
}
static void slow_writes(void) {
    const u64 v1[2] = {1, 2}, k1[2] = {91, 1}, k2[2] = {91, 2}, k3[2] = {91, 3};
    const u64 n = 30u << 20; /* two wait, the third would pass 64 MiB */
    u8 *a = blocks(n, 7), *b = blocks(n, 8), *c = blocks(n, 9);
    remove(PATH);
    remove(IDX);
    CHECK(vk_bc7_cache_open(PATH, v1, 1ull << 30));
    u64 late0, skipped0, unkept0;
    vk_bc7_cache_late(&late0, &skipped0, &unkept0);
    static u64 until;
    until = plat_ns() + 3000000000ull;
    atomic_store_explicit(&g_hold_writes, 1, memory_order_release);
    CHECK(plat_thread(let_writes_go_by, &until, 0) == 0);
    const u64 t0 = plat_ns();
    vk_bc7_cache_put(k1, a, n);
    const u64 took = plat_ns() - t0;
    printf("The GPU thread put a record of 30 MiB in %.1f ms with the disk taking no write\n", (double)took / 1e6);
    CHECK(took < 1500000000ull);       /* not held to the watchdog's 3 s */
    CHECK(vk_bc7_cache_find(k1) == n); /* in the index at once */
    u8* back = malloc((size_t)n);
    usleep(300000); /* the write held past BC7_IO_LATE_NS: the reads are not tried */
    CHECK(!vk_bc7_cache_read(k1, back, n) && vk_bc7_cache_find(k1) == n); /* left, the record kept */
    vk_bc7_cache_put(k2, b, n);
    vk_bc7_cache_put(k3, c, n);
    CHECK(vk_bc7_cache_find(k2) == n && !vk_bc7_cache_find(k3)); /* the third not kept */
    usleep(500000);                                              /* the write held 0.8 s at least */
    atomic_store_explicit(&g_hold_writes, 0, memory_order_release);
    CHECK(io_idle_by(plat_ns() + 10000000000ull));
    const double held = logged_after("vulkan: a write of 30720 KiB to the BC7 cache took ");
    CHECK(held >= 0.7 && held < 3.0);
    CHECK(logged(" s: 1 new levels were not kept meanwhile, more than 64 MiB waiting to be written\n"));
    u64 late, skipped, unkept;
    vk_bc7_cache_late(&late, &skipped, &unkept);
    CHECK(unkept == unkept0 + 1 && skipped == skipped0 + 1);
    CHECK(reads_as(k1, a, n) && reads_as(k2, b, n));
    vk_bc7_cache_close();
    CHECK(vk_bc7_cache_open(PATH, v1, 1ull << 30)); /* a later run finds them in the file */
    CHECK(reads_as(k1, a, n) && reads_as(k2, b, n) && !vk_bc7_cache_find(k3));
    vk_bc7_cache_close();
    remove(PATH);
    remove(IDX);
    free(a);
    free(b);
    free(c);
    free(back);
    puts("PASS records put while the disk takes no write: the GPU thread goes on, the queue holds 64 MiB, and the log "
         "says how long the write took");
}
#else
static void slow_disk(void) { puts("SKIP a held read: the interposed pread is Linux's"); }
static void slow_writes(void) { puts("SKIP held writes: the interposed pwrite is Linux's"); }
#endif

int main(void) {
    keys();
    file();
    later();
    slow_disk();
    slow_writes();
    printf("All %u BC7 cache checks passed\n", checks);
    return 0;
}
