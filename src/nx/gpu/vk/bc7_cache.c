/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 blocks of ASTC levels encoded before (astc.c), kept across runs in one file: shader_cache/<game>.vkbc7.
   Where the GPU samples BCn but not ASTC, a level encoded as BC7 costs the GPU 3 to 5 ms (the full encoder on 256x256,
   the fast build on 4096x4096, docs/ASTC.md), and the memory budget's trim evicts textures that the game then uses
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
   writes the same file */
#include <stdlib.h>
#include <string.h>
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

/* an index entry: a record's key and where its blocks start in the file */
typedef struct {
    u64 key[2];
    u64 at;   /* 0: a free slot (the header comes first) */
    u64 size; /* 0: a record that didn't check out, whose slot stays taken so that probes go on past it */
} Bc7Entry;

typedef struct {
    FILE* f; /* NULL: no cache */
    intptr_t lock;
    char path[1200];
    u64 version[2];
    u64 end; /* where the next record goes */
    u64 max; /* the file's bound */
    Bc7Entry* map;
    u32 cap; /* slots in map, a power of two */
    u32 used;
    u8* piece; /* BC7_PIECE bytes */
    u64 hits, stored, restarts;
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

/* the file emptied to its header, and the index with it: 0 when it couldn't be written */
static int bc7_start_over(void) {
    u8 head[BC7_HEAD] = {0};
    const u64 version = BC7_FILE_VERSION;
    memcpy(head, BC7_MAGIC, 8);
    memcpy(head + 8, &version, 8);
    memcpy(head + 16, g_bc7_cache.version, 16);
    memset(g_bc7_cache.map, 0, (size_t)g_bc7_cache.cap * sizeof *g_bc7_cache.map);
    g_bc7_cache.used = 0;
    g_bc7_cache.end = BC7_HEAD;
    return !snx_seek(g_bc7_cache.f, 0) && fwrite(head, 1, BC7_HEAD, g_bc7_cache.f) == BC7_HEAD &&
           !fflush(g_bc7_cache.f) && !snx_cut(g_bc7_cache.f, BC7_HEAD);
}

void vk_bc7_cache_close(void) {
    if (!g_bc7_cache.f) return;
    fclose(g_bc7_cache.f);
    snx_unlock_file(g_bc7_cache.lock);
    free(g_bc7_cache.map);
    free(g_bc7_cache.piece);
    memset(&g_bc7_cache, 0, sizeof g_bc7_cache); /* its path makes it over a kilobyte */
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
        memset(&g_bc7_cache, 0, sizeof g_bc7_cache);
        return 0;
    }
    g_bc7_cache.map = calloc(1024, sizeof *g_bc7_cache.map);
    g_bc7_cache.piece = malloc(BC7_PIECE);
    if (!g_bc7_cache.map || !g_bc7_cache.piece) {
        bc7_off("out of memory");
        return 0;
    }
    g_bc7_cache.cap = 1024;
    g_bc7_cache.max = max;
    memcpy(g_bc7_cache.version, version, 16);
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
    /* the records' headers, each where the one before it ends, until one is cut short or makes no sense */
    if (fseek(g_bc7_cache.f, 0, SEEK_END)) {
        bc7_off("its size could not be read");
        return 0;
    }
    const int64_t told = snx_tell(g_bc7_cache.f);
    const u64 size = told < 0 ? 0 : (u64)told;
    u64 at = BC7_HEAD, bytes = 0;
    while (size - at >= BC7_REC) {
        u64 rec[BC7_REC / 8];
        if (snx_seek(g_bc7_cache.f, at) || fread(rec, 1, BC7_REC, g_bc7_cache.f) != BC7_REC) break;
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
    if (snx_seek(g_bc7_cache.f, e->at - BC7_REC) || fread(rec, 1, BC7_REC, g_bc7_cache.f) != BC7_REC) {
        bc7_off("a record could not be read");
        return 0;
    }
    u64 h = SNX_FNV_BASIS;
    for (u64 o = 0; o < size && rec[0] == key[0] && rec[1] == key[1] && rec[2] == size; o += BC7_PIECE) {
        size_t n = (size_t)(size - o < BC7_PIECE ? size - o : BC7_PIECE);
        if (fread(g_bc7_cache.piece, 1, n, g_bc7_cache.f) != n) break;
        h = bc7_check_step(h, vk_hash_copy(dst + o, g_bc7_cache.piece, n));
    }
    if (rec[0] != key[0] || rec[1] != key[1] || rec[2] != size || h != rec[3]) {
        LOG_LIMIT(8, "vulkan: a record of the BC7 cache did not check out (dropped)\n");
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
    g_bc7_cache.end += BC7_REC + size;
}

void vk_bc7_cache_stats(u64* hits, u64* stored, u64* restarts, u64* bytes) {
    *hits = g_bc7_cache.hits;
    *stored = g_bc7_cache.stored;
    *restarts = g_bc7_cache.restarts;
    *bytes = g_bc7_cache.f ? g_bc7_cache.end : 0;
}
