/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 blocks of ASTC levels encoded before (astc.c), kept in memory while the game runs. Where the GPU samples BCn
   but not ASTC, a level encoded as BC7 costs the GPU 3 to 5 ms (the full encoder on 256x256, the fast build on
   4096x4096, docs/layers/gpu/astc.md), and the memory budget's trim evicts textures that the game then uses again:
   one game encoded the same levels 6,000 to 14,000 times in 150 seconds. The blocks never change for the same ASTC
   bytes, so an encode's blocks are kept, read back from the GPU when its submission is done, and a later upload of
   that level copies them into the image with no decode and no encode.

   Not every level is kept: the cache holds max bytes of blocks (vk_bc7_cache_open), and of the levels the game uses
   the ones it uses most. Every key met has an entry with its count of uses (an encode stored, a read), whether its
   blocks are held or not; a level whose blocks find no room takes the room of held levels used less than it, and is
   not kept when there are none (BC7_SWEEP entries looked at for each, from where the last look ended: the least used
   of the ones seen, never of all). The counts halve every BC7_AGE_PRESENTS presents, so a scene the game left makes
   room for the one it is in. At the system's word that its memory runs short (vk_bc7_cache_release) the blocks no
   recent present used all go. A level not held is encoded again, as if the cache had never seen it.

   A hit of an upload whose command buffer the recording thread records (record.c) is copied into staging on that
   thread (vk_bc7_cache_read_later), in order with the copy it is for: the GPU thread only looks the entry up. Blocks
   that leave the cache are freed by that thread too, after the copies queued before (bc7_free): everything else here
   is the GPU thread's */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

/* the bound where the caller names none: this share of the machine's memory, between the two, and BC7_MAX_UNKNOWN on
   a system that doesn't tell how much it has */
#define BC7_MAX_SHARE    8u
#define BC7_MAX_LEAST    (64ull << 20)
#define BC7_MAX_MOST     (2048ull << 20)
#define BC7_MAX_UNKNOWN  (256ull << 20)
#define BC7_SWEEP        4096u /* entries looked at for the room of one level */
#define BC7_AGE_PRESENTS 3600u

typedef struct {
    u64 key[2];
    u8* blocks;  /* NULL: not held */
    u64 size;    /* the blocks' bytes; of a level not held, those of its last ones, 0 before any */
    u32 uses;    /* 0: a free slot */
    u32 present; /* g_vk_present at its last use */
} Bc7Entry;

typedef struct {
    Bc7Entry* map;
    u32 cap; /* slots in map, a power of two; 0: no cache */
    u32 used;
    u32 hand;  /* where the next look for room starts */
    u32 aged;  /* g_vk_present when the counts last halved */
    u64 max;   /* the bound, in bytes of blocks */
    u64 bytes; /* held now */
    u64 hits, stored, evicted, refused, released;
    _Atomic u64 later; /* atomic, the recording thread's: the hits it copied */
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

/* blocks out of the cache, freed after the copies of them the recording thread still has queued */
static void bc7_free_run(const void* arg) {
    u8* p;
    memcpy(&p, arg, sizeof p);
    free(p);
}
static void bc7_free(u8* blocks) { vk_rec_call(bc7_free_run, &blocks, sizeof blocks); }
static void bc7_drop(Bc7Entry* e) {
    g_bc7_cache.bytes -= e->size;
    bc7_free(e->blocks);
    e->blocks = NULL;
}

/* key's slot: its entry, or the free slot where it would go */
static Bc7Entry* bc7_slot(const u64 key[2]) {
    for (u32 i = (u32)key[0] & (g_bc7_cache.cap - 1);; i = (i + 1) & (g_bc7_cache.cap - 1)) {
        Bc7Entry* e = &g_bc7_cache.map[i];
        if (!e->uses || (e->key[0] == key[0] && e->key[1] == key[1])) return e;
    }
}
/* key's entry, made where it has none: NULL when memory ran out */
static Bc7Entry* bc7_entry(const u64 key[2]) {
    Bc7Entry* e = bc7_slot(key);
    if (e->uses) return e;
    if ((u64)(g_bc7_cache.used + 1) * 2 > g_bc7_cache.cap) {
        const u32 was = g_bc7_cache.cap, cap = was * 2;
        Bc7Entry *old = g_bc7_cache.map, *grown = cap ? calloc(cap, sizeof *grown) : NULL;
        if (!grown) return NULL;
        g_bc7_cache.map = grown;
        g_bc7_cache.cap = cap;
        g_bc7_cache.hand = 0;
        for (u32 i = 0; i < was; i++)
            if (old[i].uses) *bc7_slot(old[i].key) = old[i];
        free(old);
        e = bc7_slot(key);
    }
    *e = (Bc7Entry){.key = {key[0], key[1]}};
    g_bc7_cache.used++;
    return e;
}

/* the bound for a machine of total bytes of memory (known 0: the system didn't tell) */
static u64 bc7_share(int known, u64 total) {
    const u64 share = total / BC7_MAX_SHARE;
    if (!known) return BC7_MAX_UNKNOWN;
    return share < BC7_MAX_LEAST ? BC7_MAX_LEAST : share > BC7_MAX_MOST ? BC7_MAX_MOST : share;
}

int vk_bc7_cache_old_file(const char* path) {
    const u64 size = snx_file_size(path);
    if (!size) return 0;
    WARN("vulkan: %s (%.1f MB) is a BC7 cache file of an earlier version, which this one does not read or write: "
         "the BC7 cache is held in memory now. The game's Delete Old Caches in the launcher deletes it\n",
         path, (double)size / 1048576.0);
    return 1;
}

void vk_bc7_cache_close(void) {
    for (u32 i = 0; i < g_bc7_cache.cap; i++)
        if (g_bc7_cache.map[i].blocks) bc7_free(g_bc7_cache.map[i].blocks);
    free(g_bc7_cache.map);
    g_bc7_cache.map = NULL;
    g_bc7_cache.cap = g_bc7_cache.used = g_bc7_cache.hand = 0;
    g_bc7_cache.max = g_bc7_cache.bytes = 0;
}

int vk_bc7_cache_open(u64 max) {
    vk_bc7_cache_close();
    g_bc7_cache.hits = g_bc7_cache.stored = g_bc7_cache.evicted = g_bc7_cache.refused = g_bc7_cache.released = 0;
    atomic_store_explicit(&g_bc7_cache.later, 0, memory_order_relaxed);
    g_bc7_cache.map = calloc(1024, sizeof *g_bc7_cache.map);
    if (!g_bc7_cache.map) {
        WARN("vulkan: no memory for the BC7 cache's table: every ASTC level is encoded each time it loads\n");
        return 0;
    }
    g_bc7_cache.cap = 1024;
    SnxU64 total = 0;
    const int known = snx_memory_total(&total);
    g_bc7_cache.max = max ? max : bc7_share(known, total);
    g_bc7_cache.aged = g_vk_present;
    LOG("vulkan: the BC7 cache keeps the ASTC levels the game uses most in memory, %llu MB of them at most\n",
        (unsigned long long)(g_bc7_cache.max >> 20));
    return 1;
}
u64 vk_bc7_cache_max(void) { return g_bc7_cache.cap ? g_bc7_cache.max : 0; }

/* the counts halved, once every BC7_AGE_PRESENTS presents: one that would reach 0 stays 1, its slot taken */
static void bc7_age(void) {
    if (g_vk_present - g_bc7_cache.aged < BC7_AGE_PRESENTS) return;
    g_bc7_cache.aged = g_vk_present;
    for (u32 i = 0; i < g_bc7_cache.cap; i++) {
        Bc7Entry* e = &g_bc7_cache.map[i];
        if (e->uses > 1) e->uses /= 2;
    }
}
static void bc7_used(Bc7Entry* e) {
    bc7_age();
    if (e->uses < UINT32_MAX) e->uses++;
    e->present = g_vk_present;
}

u64 vk_bc7_cache_find(const u64 key[2]) {
    if (!g_bc7_cache.cap) return 0;
    const Bc7Entry* e = bc7_slot(key);
    return e->uses && e->blocks ? e->size : 0;
}
static Bc7Entry* bc7_held(const u64 key[2], u64 size) {
    if (!g_bc7_cache.cap || !size) return NULL;
    Bc7Entry* e = bc7_slot(key);
    return e->uses && e->blocks && e->size == size ? e : NULL;
}

int vk_bc7_cache_read(const u64 key[2], u8* dst, u64 size) {
    Bc7Entry* e = bc7_held(key, size);
    if (!e) return 0;
    memcpy(dst, e->blocks, (size_t)size);
    bc7_used(e);
    g_bc7_cache.hits++;
    return 1;
}

/* a hit's copy on the recording thread: the blocks into staging, then the copy that they are for */
typedef struct {
    const u8* blocks;
    u8* dst;
    u64 size, off;
    VkCommandBuffer cb;
    VkBuffer buf;
    VkImage img;
    VkImageLayout layout; /* img's, VK_LAYOUT's of the GPU thread */
    DevMem mem;
    VkBufferImageCopy copy;
} Bc7Later;
static void bc7_later_run(const void* p) {
    const Bc7Later* l = p;
    memcpy(l->dst, l->blocks, (size_t)l->size);
    /* staging may not be host coherent, and its submission's flush (cmd.c's rings_flush) came before this */
    vk_devmem_flush(l->mem, l->off, l->size);
    if (l->img) vkCmdCopyBufferToImage(l->cb, l->buf, l->img, l->layout, 1, &l->copy);
    atomic_fetch_add_explicit(&g_bc7_cache.later, 1, memory_order_relaxed);
}
int vk_bc7_cache_read_later(VkCommandBuffer cb, const u64 key[2], u64 size, u8* dst, DevMem mem, u64 off, VkBuffer buf,
                            VkImage img, VkImageLayout layout, const VkBufferImageCopy* copy) {
    if (!vk_rec_threaded()) return -1;
    Bc7Entry* e = bc7_held(key, size);
    if (!e) return 0;
    const Bc7Later l = {e->blocks, dst, size, off, cb, buf, img, layout, mem, *copy};
    bc7_used(e);
    vk_rec_call(bc7_later_run, &l, sizeof l);
    return 1;
}

/* an entry whose blocks may go for a level used `uses` times: used less, and by no present since the one before this
   (the game is on those) */
static int bc7_yields(const Bc7Entry* e, u32 uses) {
    return e->blocks && e->uses < uses && g_vk_present - e->present > 1;
}
/* room for size more bytes of a level used `uses` times, taken from the entries that yield to it among the
   BC7_SWEEP after the hand: 0 when they leave it short, and then none went */
static int bc7_room(u64 size, u32 uses) {
    if (g_bc7_cache.bytes + size <= g_bc7_cache.max) return 1;
    const u64 need = g_bc7_cache.bytes + size - g_bc7_cache.max;
    const u32 n = g_bc7_cache.cap < BC7_SWEEP ? g_bc7_cache.cap : BC7_SWEEP, from = g_bc7_cache.hand;
    u64 found = 0;
    u32 seen = 0;
    for (; seen < n && found < need; seen++) {
        const Bc7Entry* e = &g_bc7_cache.map[(from + seen) & (g_bc7_cache.cap - 1)];
        if (bc7_yields(e, uses)) found += e->size;
    }
    g_bc7_cache.hand = (from + seen) & (g_bc7_cache.cap - 1);
    if (found < need) return 0;
    for (u32 k = 0; k < seen; k++) {
        Bc7Entry* e = &g_bc7_cache.map[(from + k) & (g_bc7_cache.cap - 1)];
        if (!bc7_yields(e, uses)) continue;
        bc7_drop(e);
        g_bc7_cache.evicted++;
    }
    return 1;
}

void vk_bc7_cache_put(const u64 key[2], const u8* p, u64 size) {
    if (!g_bc7_cache.cap || !size || size % 16 || size > g_bc7_cache.max) return;
    Bc7Entry* e = bc7_entry(key);
    if (!e) return;
    bc7_used(e);
    if (e->blocks) return;
    e->size = size;
    u8* blocks = bc7_room(size, e->uses) ? malloc((size_t)size) : NULL;
    if (!blocks) { /* not among the most used yet, or no memory: encoded again at its next use, and counted again */
        g_bc7_cache.refused++;
        return;
    }
    memcpy(blocks, p, (size_t)size);
    e->blocks = blocks;
    g_bc7_cache.bytes += size;
    g_bc7_cache.stored++;
}

u64 vk_bc7_cache_release(u32 before) {
    u64 went = 0;
    for (u32 i = 0; i < g_bc7_cache.cap; i++) {
        Bc7Entry* e = &g_bc7_cache.map[i];
        if (!e->blocks || (int32_t)(e->present - before) >= 0) continue;
        went += e->size;
        bc7_drop(e);
    }
    g_bc7_cache.released += went;
    return went;
}

void vk_bc7_cache_stats(VkBc7CacheStats* s) {
    *s = (VkBc7CacheStats){g_bc7_cache.hits + atomic_load_explicit(&g_bc7_cache.later, memory_order_relaxed),
                           g_bc7_cache.stored,
                           g_bc7_cache.evicted,
                           g_bc7_cache.refused,
                           g_bc7_cache.released,
                           g_bc7_cache.bytes,
                           g_bc7_cache.used};
}
