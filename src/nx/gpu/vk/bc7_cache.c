/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 blocks of ASTC levels encoded before (astc.c), kept across runs in one file: shader_cache/<game>.vkbc7.
   Where the GPU samples BCn but not ASTC, a level encoded as BC7 costs the GPU 3 to 5 ms (the full encoder on 256x256,
   the fast build on 4096x4096, docs/layers/gpu/astc.md), and the memory budget's trim evicts textures that the game then uses
   again: one game encoded the same levels 6,000 to 14,000 times in 150 seconds. The blocks never change for the same
   ASTC bytes, so the first encode's blocks are kept, read back from the GPU when its submission is done, and every
   later upload of that level copies them into the image with no decode and no encode.

   The file is a header (BC7_MAGIC, BC7_FILE_VERSION and the version the caller gives: the encoder, the shaders' SPIR-V,
   the GPU and its driver, astc.c's vk_astc_cache_open) and records appended as they are made: {u64 key[2], u64 bytes,
   u64 check} and the blocks, bytes of them, a multiple of 16. A file of another version starts over, and so does a
   file that would pass its bound. Only the index is in memory (an entry of 32 bytes a record): a hit reads its
   record's blocks from the file a piece at a time, through one buffer of BC7_PIECE bytes, checks them on the way and
   writes them into the staging memory given. A record cut short by a crash ends the reading, and the file is cut
   there; a record whose blocks don't check out on a read is dropped from the index, and the level is encoded again.
   The file is locked while it is open (snx_lock_file), so a second game process runs without the cache rather than
   writes the same file.

   Beside it, <file>.idx: the same header, then the index's entries in the file's order, {u64 key[2], u64 at, u64
   bytes} (BC7_IDX), one appended with each record. The cache opens from it, where the records' own headers lie all
   through the file: Mario Kart World's 91,000 records, 4.3 GB, took minutes to open off a disk whose cache had let
   the file go, where its index is 3 MB. An entry is taken while it lies past the last one and its blocks within the
   file; the file is cut after the last one taken (a record after it, written as a crash came before its entry, is
   lost). Without an index of the file's version, the records' headers are read as before and the index written from
   them */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <fcntl.h>
#endif
#include "common/platform/file.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

#define BC7_MAGIC        "SNXBC7\n"
#define BC7_FILE_VERSION 1
#define BC7_HEAD         32u /* the header: the magic's 8 bytes, the file version (u64) and the caller's version */
#define BC7_REC          32u /* a record's header: its key, its bytes and their check */
/* a hit's read goes through this many bytes at a time, which stay in the CPU's cache between the read and the copy */
#define BC7_PIECE        (1u << 20)
#define BC7_IDX          32u /* an index file's entry */

/* an index entry: a record's key and where its blocks start in the file */
typedef struct {
    u64 key[2];
    u64 at;   /* 0: a free slot (the header comes first) */
    u64 size; /* 0: a record that didn't check out, whose slot stays taken so that probes go on past it */
} Bc7Entry;

typedef struct {
    FILE* f;   /* NULL: no cache */
    FILE* idx; /* the index file: NULL where it couldn't be written (the next run reads the records' headers) */
    intptr_t lock;
    char path[1200];
    u64 version[2];
    u64 end; /* where the next record goes */
    u64 max; /* the file's bound */
    Bc7Entry* map;
    u32 cap; /* slots in map, a power of two */
    u32 used;
    u64 hits, stored, restarts;
    u8 piece[BC7_PIECE]; /* last: what a close clears ends before it */
} Bc7Cache;
static Bc7Cache g_bc7_cache;

void vk_bc7_content(u64 content[2], const void* p, u64 n) {
    u64 words[4] = {content[0], content[1], 0, 0};
    vk_hash128(p, n, words + 2);
    vk_hash128(words, sizeof words, content);
}

void vk_bc7_key(const u64 content[2], const VkBc7Level* lv, u64 key[2]) {
    const u64 words[7] = {content[0],
                          content[1],
                          lv->w | (u64)lv->h << 32,
                          lv->depth | (u64)lv->layers << 32,
                          lv->bw | (u64)lv->bh << 32,
                          lv->srgb | (u64)lv->build << 32,
                          VK_BC7_ENCODER_VERSION};
    vk_hash128(words, sizeof words, key);
}

/* a payload's check: its pieces' hashes, folded */
static u64 bc7_check_step(u64 h, u64 piece) { return snx_fnv_step(h, piece); }
static u64 bc7_check(const u8* p, u64 size) {
    u64 h = SNX_FNV_BASIS;
    for (u64 o = 0; o < size; o += BC7_PIECE)
        h = bc7_check_step(h, vk_hash(p + o, size - o < BC7_PIECE ? size - o : BC7_PIECE));
    return h;
}

/* key's slot: its entry, or the free slot where it would go */
static Bc7Entry* bc7_slot(const u64 key[2]) {
    for (u32 i = (u32)key[0] & (g_bc7_cache.cap - 1);; i = (i + 1) & (g_bc7_cache.cap - 1)) {
        Bc7Entry* e = &g_bc7_cache.map[i];
        if (!e->at || (e->key[0] == key[0] && e->key[1] == key[1])) return e;
    }
}
/* the record of key at `at` in the index, over an older one of the same key: 0 when memory ran out */
static int bc7_index(const u64 key[2], u64 at, u64 size) {
    if ((u64)(g_bc7_cache.used + 1) * 2 > g_bc7_cache.cap) {
        u32 cap = g_bc7_cache.cap * 2;
        Bc7Entry *old = g_bc7_cache.map, *map = cap ? calloc(cap, sizeof *map) : NULL;
        if (!map) return 0;
        u32 was = g_bc7_cache.cap;
        g_bc7_cache.map = map;
        g_bc7_cache.cap = cap;
        for (u32 i = 0; i < was; i++)
            if (old[i].at) *bc7_slot(old[i].key) = old[i];
        free(old);
    }
    Bc7Entry* e = bc7_slot(key);
    g_bc7_cache.used += !e->at;
    *e = (Bc7Entry){{key[0], key[1]}, at, size};
    return 1;
}

/* the header of the file and of its index */
static void bc7_head(u8 head[BC7_HEAD]) {
    const u64 version = BC7_FILE_VERSION;
    memset(head, 0, BC7_HEAD);
    memcpy(head, BC7_MAGIC, 8);
    memcpy(head + 8, &version, 8);
    memcpy(head + 16, g_bc7_cache.version, 16);
}
/* the index file given up for this run: the next one reads the records' headers */
static void bc7_idx_off(void) {
    if (!g_bc7_cache.idx) return;
    fclose(g_bc7_cache.idx);
    g_bc7_cache.idx = NULL;
    char path[1210];
    snprintf(path, sizeof path, "%s.idx", g_bc7_cache.path);
    remove(path);
}
/* the index file emptied to its header */
static void bc7_idx_start_over(void) {
    u8 head[BC7_HEAD];
    bc7_head(head);
    if (g_bc7_cache.idx && (snx_seek(g_bc7_cache.idx, 0) || fwrite(head, 1, BC7_HEAD, g_bc7_cache.idx) != BC7_HEAD ||
                            fflush(g_bc7_cache.idx) || snx_cut(g_bc7_cache.idx, BC7_HEAD)))
        bc7_idx_off();
}
/* a record's entry appended to the index file */
static void bc7_idx_put(const u64 key[2], u64 at, u64 size) {
    const u64 e[BC7_IDX / 8] = {key[0], key[1], at, size};
    if (g_bc7_cache.idx && (fseek(g_bc7_cache.idx, 0, SEEK_END) || fwrite(e, 1, BC7_IDX, g_bc7_cache.idx) != BC7_IDX ||
                            fflush(g_bc7_cache.idx)))
        bc7_idx_off();
}
/* the index from the index file, over a file of size bytes: where its records end, 0 where the index file is not
   this file's (another version, or not there) */
static u64 bc7_idx_load(u64 size) {
    u8 head[BC7_HEAD], have[BC7_HEAD];
    bc7_head(head);
    if (!g_bc7_cache.idx || snx_seek(g_bc7_cache.idx, 0) || fread(have, 1, BC7_HEAD, g_bc7_cache.idx) != BC7_HEAD ||
        memcmp(head, have, BC7_HEAD))
        return 0;
    u64 end = BC7_HEAD, n = 0, e[(64u << 10) / 8];
    size_t got;
    int done = 0;
    while (!done && (got = fread(e, 1, sizeof e, g_bc7_cache.idx)) >= BC7_IDX) {
        for (size_t i = 0; i + BC7_IDX <= got && !done; i += BC7_IDX) {
            const u64* r = e + i / 8;
            /* past the last record, its blocks a whole number of 16 bytes within the file */
            if (r[2] < end + BC7_REC || !r[3] || r[3] % 16 || r[3] > size || r[2] > size - r[3] ||
                !bc7_index(r, r[2], r[3]))
                done = 1;
            else {
                end = r[2] + r[3];
                n++;
            }
        }
        done |= got < sizeof e;
    }
    /* the entries taken, and nothing after them */
    if (snx_cut(g_bc7_cache.idx, BC7_HEAD + n * BC7_IDX)) return 0;
    return end;
}
/* the index file written anew from the index, its entries in the file's order */
static int bc7_at_order(const void* a, const void* b) {
    const u64 x = ((const Bc7Entry*)a)->at, y = ((const Bc7Entry*)b)->at;
    return (x > y) - (x < y);
}
static void bc7_idx_write(void) {
    bc7_idx_start_over();
    if (!g_bc7_cache.idx) return;
    Bc7Entry* all = malloc(((size_t)g_bc7_cache.used + 1) * sizeof *all);
    if (!all) {
        bc7_idx_off();
        return;
    }
    u32 n = 0;
    for (u32 i = 0; i < g_bc7_cache.cap; i++)
        if (g_bc7_cache.map[i].at) all[n++] = g_bc7_cache.map[i];
    qsort(all, n, sizeof *all, bc7_at_order);
    for (u32 i = 0; i < n && g_bc7_cache.idx; i++) {
        const u64 e[BC7_IDX / 8] = {all[i].key[0], all[i].key[1], all[i].at, all[i].size};
        if (fwrite(e, 1, BC7_IDX, g_bc7_cache.idx) != BC7_IDX) bc7_idx_off();
    }
    if (g_bc7_cache.idx && fflush(g_bc7_cache.idx)) bc7_idx_off();
    free(all);
}

/* the file emptied to its header, and the index with it: 0 when it couldn't be written */
static int bc7_start_over(void) {
    u8 head[BC7_HEAD];
    bc7_head(head);
    memset(g_bc7_cache.map, 0, (size_t)g_bc7_cache.cap * sizeof *g_bc7_cache.map);
    g_bc7_cache.used = 0;
    g_bc7_cache.end = BC7_HEAD;
    bc7_idx_start_over();
    return !snx_seek(g_bc7_cache.f, 0) && fwrite(head, 1, BC7_HEAD, g_bc7_cache.f) == BC7_HEAD &&
           !fflush(g_bc7_cache.f) && !snx_cut(g_bc7_cache.f, BC7_HEAD);
}

void vk_bc7_cache_close(void) {
    if (!g_bc7_cache.f) return;
    if (g_bc7_cache.idx) fclose(g_bc7_cache.idx);
    fclose(g_bc7_cache.f);
    snx_unlock_file(g_bc7_cache.lock);
    free(g_bc7_cache.map);
    memset(&g_bc7_cache, 0, offsetof(Bc7Cache, piece)); /* its path and buffer make it over a megabyte */
}

/* the cache off from now on, for why (an error writing or reading the file) */
static void bc7_off(const char* why) {
    WARN("vulkan: the BC7 cache %s is off for this run: %s\n", g_bc7_cache.path, why);
    vk_bc7_cache_close();
}

int vk_bc7_cache_open(const char* path, const u64 version[2], u64 max) {
    vk_bc7_cache_close();
    if (snprintf(g_bc7_cache.path, sizeof g_bc7_cache.path, "%s", path) >= (int)sizeof g_bc7_cache.path) return 0;
    int locked = snx_lock_file(path, &g_bc7_cache.lock);
    if (locked) {
        LOG("vulkan: the BC7 cache %s is %s: none this run\n", path,
            locked > 0 ? "in use by another process" : "not to be locked");
        return 0;
    }
    g_bc7_cache.f = snx_fopen(path, "r+b");
    if (!g_bc7_cache.f) {
        snx_unlock_file(g_bc7_cache.lock);
        memset(&g_bc7_cache, 0, offsetof(Bc7Cache, piece));
        return 0;
    }
    g_bc7_cache.map = calloc(1024, sizeof *g_bc7_cache.map);
    if (!g_bc7_cache.map) {
        bc7_off("out of memory");
        return 0;
    }
    g_bc7_cache.cap = 1024;
    g_bc7_cache.max = max;
    memcpy(g_bc7_cache.version, version, 16);
    {
        char idx[1210];
        snprintf(idx, sizeof idx, "%s.idx", path);
        g_bc7_cache.idx = snx_fopen(idx, "r+b");
        if (!g_bc7_cache.idx) g_bc7_cache.idx = snx_fopen(idx, "w+b");
    }
    u64 head[BC7_HEAD / 8];
    int same = fread(head, 1, BC7_HEAD, g_bc7_cache.f) == BC7_HEAD && !memcmp(head, BC7_MAGIC, 8) &&
               head[1] == BC7_FILE_VERSION && head[2] == version[0] && head[3] == version[1];
    if (!same) {
        if (!bc7_start_over()) {
            bc7_off("its header could not be written");
            return 0;
        }
        LOG("vulkan: the BC7 cache %s starts empty\n", path);
        return 1;
    }
    /* the records' headers, each where the one before it ends, until one is cut short or makes no sense. Read where
       they lie (plat_pread), not through the stream: after a seek each read of 32 bytes filled the stream's buffer of a
       block (128 KiB on ZFS), and Mario Kart World's cache of 84,000 records, 4.3 GB, took minutes to open */
    if (fseek(g_bc7_cache.f, 0, SEEK_END)) {
        bc7_off("its size could not be read");
        return 0;
    }
    const int64_t told = snx_tell(g_bc7_cache.f);
    const u64 size = told < 0 ? 0 : (u64)told;
    u64 at = bc7_idx_load(size), bytes = 0;
    const int from_idx = at != 0;
    if (from_idx)
        for (u32 i = 0; i < g_bc7_cache.cap; i++) bytes += g_bc7_cache.map[i].at ? g_bc7_cache.map[i].size : 0;
    else
        at = BC7_HEAD;
    while (!from_idx && size - at >= BC7_REC) {
        u64 rec[BC7_REC / 8];
        if (plat_pread(g_bc7_cache.f, rec, BC7_REC, at) != BC7_REC) break;
        if (!rec[2] || rec[2] % 16 || rec[2] > size - at - BC7_REC) break;
        if (!bc7_index(rec, at + BC7_REC, rec[2])) break;
        bytes += rec[2];
        at += BC7_REC + rec[2];
    }
    g_bc7_cache.end = at;
    if (at < size && snx_cut(g_bc7_cache.f, at)) {
        bc7_off("a record cut short could not be cut off");
        return 0;
    }
    if (!from_idx) bc7_idx_write();
#if defined(__linux__)
    /* a hit reads one level's records, wherever they lie: the kernel's readahead past them (64 MiB a read on one USB
       disk of 40 MB/s) took that disk from the game's own reads for seconds while a very large level's pieces loaded
       (astc.c's bg_load) */
    posix_fadvise(fileno(g_bc7_cache.f), 0, 0, POSIX_FADV_RANDOM);
#endif
    LOG("vulkan: the BC7 cache %s holds %u levels of %llu MB\n", path, g_bc7_cache.used,
        (unsigned long long)(bytes >> 20));
    return 1;
}

u64 vk_bc7_cache_find(const u64 key[2]) {
    if (!g_bc7_cache.f) return 0;
    const Bc7Entry* e = bc7_slot(key);
    return e->at ? e->size : 0;
}

int vk_bc7_cache_read(const u64 key[2], u8* dst, u64 size) {
    if (!g_bc7_cache.f) return 0;
    Bc7Entry* e = bc7_slot(key);
    u64 rec[BC7_REC / 8];
    if (!e->at || !size || e->size != size) return 0;
    /* where they lie, as the index's scan reads them: through the stream, a record of 16 KiB read a block of 128 */
    if (plat_pread(g_bc7_cache.f, rec, BC7_REC, e->at - BC7_REC) != BC7_REC) {
        bc7_off("a record could not be read");
        return 0;
    }
    u64 h = SNX_FNV_BASIS;
    for (u64 o = 0; o < size && rec[0] == key[0] && rec[1] == key[1] && rec[2] == size; o += BC7_PIECE) {
        size_t n = (size_t)(size - o < BC7_PIECE ? size - o : BC7_PIECE);
        if (plat_pread(g_bc7_cache.f, g_bc7_cache.piece, n, e->at + o) != (int64_t)n) break;
        h = bc7_check_step(h, vk_hash_copy(dst + o, g_bc7_cache.piece, n));
    }
    if (rec[0] != key[0] || rec[1] != key[1] || rec[2] != size || h != rec[3]) {
        LOG_LIMIT(8, "vulkan: a record of the BC7 cache did not check out: dropped, its level is encoded again\n");
        e->size = 0;
        return 0;
    }
    g_bc7_cache.hits++;
    return 1;
}

void vk_bc7_cache_put(const u64 key[2], const u8* p, u64 size) {
    if (!g_bc7_cache.f || !size || size % 16 || size > g_bc7_cache.max - BC7_HEAD - BC7_REC || vk_bc7_cache_find(key))
        return;
    if (g_bc7_cache.end + BC7_REC + size > g_bc7_cache.max) {
        LOG("vulkan: the BC7 cache %s reached %llu MB: it starts over\n", g_bc7_cache.path,
            (unsigned long long)(g_bc7_cache.max >> 20));
        g_bc7_cache.restarts++;
        if (!bc7_start_over()) {
            bc7_off("it could not be emptied");
            return;
        }
    }
    const u64 rec[BC7_REC / 8] = {key[0], key[1], size, bc7_check(p, size)};
    if (snx_seek(g_bc7_cache.f, g_bc7_cache.end) || fwrite(rec, 1, BC7_REC, g_bc7_cache.f) != BC7_REC ||
        fwrite(p, 1, (size_t)size, g_bc7_cache.f) != size || fflush(g_bc7_cache.f)) {
        bc7_off("a record could not be written");
        return;
    }
    if (bc7_index(key, g_bc7_cache.end + BC7_REC, size)) g_bc7_cache.stored++;
    bc7_idx_put(key, g_bc7_cache.end + BC7_REC, size);
    g_bc7_cache.end += BC7_REC + size;
}

void vk_bc7_cache_stats(u64* hits, u64* stored, u64* restarts, u64* bytes) {
    *hits = g_bc7_cache.hits;
    *stored = g_bc7_cache.stored;
    *restarts = g_bc7_cache.restarts;
    *bytes = g_bc7_cache.f ? g_bc7_cache.end : 0;
}
