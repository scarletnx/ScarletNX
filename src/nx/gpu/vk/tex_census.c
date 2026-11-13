/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/vk/tex_census.h"
#include "common/utilities/bits.h"

/* the key's bytes are hashed, so it has none that no member names */
typedef char vk_tex_key_has_no_padding[sizeof(VkTexKey) == 2 * sizeof(u64) + 14 * sizeof(u32) ? 1 : -1];

u64 vk_tex_key_hash(const VkTexKey* k) {
    const u64 h = snx_fnv1a(SNX_FNV_BASIS, k, sizeof *k);
    return h ? h : 1;
}

/* the set of a hash: all its bits choose it */
static u64* evicted_set(VkTexEvicted* e, u64 hash) { return e->hash[(hash ^ hash >> 32) & (VK_TEX_EVICTED_SETS - 1)]; }
/* way w of set leaves it: the older ones move up to it, and the free end is the oldest's */
static void evicted_drop(u64* set, u32 w) {
    for (; w; w--) set[w] = set[w - 1];
    set[0] = 0;
}

void vk_tex_evicted_add(VkTexEvicted* e, u64 hash) {
    u64* set = evicted_set(e, hash);
    u32 w = 0;
    while (w < VK_TEX_EVICTED_WAYS && set[w] != hash) w++;
    if (w < VK_TEX_EVICTED_WAYS) return;
    /* the newest goes last: the others move down, over a free way or over the oldest */
    for (w = 0; w + 1 < VK_TEX_EVICTED_WAYS; w++) set[w] = set[w + 1];
    set[VK_TEX_EVICTED_WAYS - 1] = hash;
}

int vk_tex_evicted_take(VkTexEvicted* e, u64 hash) {
    u64* set = evicted_set(e, hash);
    for (u32 w = 0; w < VK_TEX_EVICTED_WAYS; w++)
        if (set[w] == hash) {
            evicted_drop(set, w);
            return 1;
        }
    return 0;
}

int vk_tex_live_class(const u64* starts, u32 nstarts, const VkRange* joined, u32 njoined, u64 gva) {
    u32 lo = 0, hi = nstarts;
    while (lo < hi) { /* the first start not below gva */
        const u32 mid = lo + (hi - lo) / 2;
        if (starts[mid] < gva)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < nstarts && starts[lo] == gva) return VK_TEX_LIVE;
    return vk_range_inside(joined, njoined, gva, 1) ? VK_TEX_INSIDE : VK_TEX_DEAD;
}

void vk_tex_formats_add(VkTexFormats* f, u32 format, u64 bytes) {
    VkTexFormatRow* r = &f->other;
    u32 i = 0;
    while (i < f->n && f->row[i].format != format) i++;
    if (i < f->n)
        r = &f->row[i];
    else if (f->n < VK_TEX_FORMATS) {
        r = &f->row[f->n++];
        r->format = format;
    }
    r->count++;
    r->bytes += bytes;
}

static int by_bytes(const void* a, const void* b) {
    const VkTexFormatRow *x = a, *y = b;
    if (x->bytes != y->bytes) return x->bytes > y->bytes ? -1 : 1;
    return x->format < y->format ? -1 : x->format > y->format;
}
void vk_tex_formats_sort(VkTexFormats* f) { qsort(f->row, f->n, sizeof f->row[0], by_bytes); }

void vk_astc_sizes_add(VkAstcSizes* a, u32 bw, u32 bh, int as, u64 bytes) {
    if (bw < 4 || bh < 4 || bw >= 4 + VK_ASTC_SIDES || bh >= 4 + VK_ASTC_SIDES || as < 0 || as >= VK_ASTC_AS_N) return;
    a->bytes[bw - 4][bh - 4][as] += bytes;
}
