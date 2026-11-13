/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The calculations of the texture cache's measurements (gpu/vk/tex_census.c): a key's hash, the table of evicted
   keys, a texture's place among the guest's live texture objects, and the sums by host format and ASTC block size. */
#include "nx/gpu/vk/tex_census.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a hash of set `set` of the table: vk_tex_evicted_add takes the set from all the bits, the two halves folded */
static u64 in_set(u32 set, u32 n) { return (u64)(n + 1) << 32 | ((n + 1) ^ set); }

int main(void) {
    /* a key's hash: the same for the same members, another for each member that differs, never 0 */
    VkTexKey k;
    memset(&k, 0, sizeof k);
    k.gva = 0x4000000;
    k.layer_size = 0x10000;
    k.w = 256, k.h = 128, k.layers = 6, k.depth = 1, k.levels = 9, k.bl = 1, k.bh = 4, k.fkey = 0x24;
    const u64 h = vk_tex_key_hash(&k);
    VkTexKey same = k;
    assert(h && vk_tex_key_hash(&same) == h);
    {
        VkTexKey o = k;
        o.gva += 0x200;
        assert(vk_tex_key_hash(&o) != h);
        o = k, o.w = 128, o.h = 256;
        assert(vk_tex_key_hash(&o) != h);
        o = k, o.levels = 1;
        assert(vk_tex_key_hash(&o) != h);
        o = k, o.fkey |= 1u << 31; /* the sRGB twin */
        assert(vk_tex_key_hash(&o) != h);
        o = k, o.msaa = 1;
        assert(vk_tex_key_hash(&o) != h);
        o = k, o.tile_w = 3;
        assert(vk_tex_key_hash(&o) != h);
    }

    /* the evicted keys: one taken is there one time, however often it went in */
    VkTexEvicted* e = calloc(1, sizeof *e);
    assert(e);
    assert(!vk_tex_evicted_take(e, h));
    vk_tex_evicted_add(e, h);
    vk_tex_evicted_add(e, h);
    assert(vk_tex_evicted_take(e, h) && !vk_tex_evicted_take(e, h));
    /* a set keeps its newest VK_TEX_EVICTED_WAYS: one more pushes the oldest out, and no other set's */
    for (u32 i = 0; i < VK_TEX_EVICTED_WAYS + 1; i++) vk_tex_evicted_add(e, in_set(7, i));
    vk_tex_evicted_add(e, in_set(8, 0));
    assert(!vk_tex_evicted_take(e, in_set(7, 0)));
    for (u32 i = 1; i < VK_TEX_EVICTED_WAYS + 1; i++) assert(vk_tex_evicted_take(e, in_set(7, i)));
    assert(vk_tex_evicted_take(e, in_set(8, 0)));
    /* one taken from the middle leaves the others in their order: the oldest of them goes first */
    for (u32 i = 0; i < VK_TEX_EVICTED_WAYS; i++) vk_tex_evicted_add(e, in_set(9, i));
    assert(vk_tex_evicted_take(e, in_set(9, 1)));
    vk_tex_evicted_add(e, in_set(9, 10)); /* into the way that freed */
    vk_tex_evicted_add(e, in_set(9, 11)); /* pushes out hash 0, the oldest */
    assert(!vk_tex_evicted_take(e, in_set(9, 0)) && !vk_tex_evicted_take(e, in_set(9, 1)));
    assert(vk_tex_evicted_take(e, in_set(9, 2)) && vk_tex_evicted_take(e, in_set(9, 3)));
    assert(vk_tex_evicted_take(e, in_set(9, 10)) && vk_tex_evicted_take(e, in_set(9, 11)));
    /* the way one leaves is the one the next key takes: the oldest stays, with a set that was full */
    for (u32 i = 0; i < VK_TEX_EVICTED_WAYS; i++) vk_tex_evicted_add(e, in_set(10, i));
    assert(vk_tex_evicted_take(e, in_set(10, 2)));
    vk_tex_evicted_add(e, in_set(10, 10));
    assert(vk_tex_evicted_take(e, in_set(10, 0)) && vk_tex_evicted_take(e, in_set(10, 1)));
    assert(vk_tex_evicted_take(e, in_set(10, 3)) && vk_tex_evicted_take(e, in_set(10, 10)));
    for (u32 s = 0; s < VK_TEX_EVICTED_SETS; s++) /* all taken: the table is empty again */
        for (u32 w = 0; w < VK_TEX_EVICTED_WAYS; w++) assert(!e->hash[s][w]);
    free(e);

    /* a texture among the live objects: at one's first address, inside one (a later layer), or dead. Two objects
       that touch, one inside the first, and one with no bytes */
    VkRange live[4] = {{0x1000, 0x800}, {0x1800, 0x800}, {0x1200, 0x100}, {0x9000, 0}};
    const u64 starts[4] = {0x1000, 0x1200, 0x1800, 0x9000};
    const u32 n = vk_range_join(live, 4);
    assert(n == 1);
    assert(vk_tex_live_class(starts, 4, live, n, 0x1000) == VK_TEX_LIVE);
    assert(vk_tex_live_class(starts, 4, live, n, 0x1200) == VK_TEX_LIVE);
    assert(vk_tex_live_class(starts, 4, live, n, 0x1800) == VK_TEX_LIVE);
    assert(vk_tex_live_class(starts, 4, live, n, 0x9000) == VK_TEX_LIVE); /* at its address, whatever its size */
    assert(vk_tex_live_class(starts, 4, live, n, 0x1400) == VK_TEX_INSIDE);
    assert(vk_tex_live_class(starts, 4, live, n, 0x1fff) == VK_TEX_INSIDE);
    assert(vk_tex_live_class(starts, 4, live, n, 0x2000) == VK_TEX_DEAD); /* the first byte past the last object */
    assert(vk_tex_live_class(starts, 4, live, n, 0xfff) == VK_TEX_DEAD);
    assert(vk_tex_live_class(starts, 4, live, n, 0x9001) == VK_TEX_DEAD);
    assert(vk_tex_live_class(NULL, 0, NULL, 0, 0x1000) == VK_TEX_DEAD); /* no objects */

    /* the sums by format: one row a format, the most bytes first, the formats past the table's rows in other */
    VkTexFormats f;
    memset(&f, 0, sizeof f);
    vk_tex_formats_add(&f, 37, 100);
    vk_tex_formats_add(&f, 145, 4000);
    vk_tex_formats_add(&f, 37, 50);
    vk_tex_formats_add(&f, 131, 4000);
    assert(f.n == 3);
    vk_tex_formats_sort(&f);
    assert(f.row[0].format == 131 && f.row[0].bytes == 4000 && f.row[0].count == 1); /* a tie: the lower format */
    assert(f.row[1].format == 145 && f.row[2].format == 37 && f.row[2].bytes == 150 && f.row[2].count == 2);
    for (u32 i = 0; i < VK_TEX_FORMATS; i++) vk_tex_formats_add(&f, 1000 + i, 1);
    assert(f.n == VK_TEX_FORMATS && f.other.count == 3 && f.other.bytes == 3);
    vk_tex_formats_add(&f, 37, 1); /* a format with a row keeps it */
    assert(f.other.count == 3);

    /* the ASTC sums: by block size and by what holds the texture, nothing for a size ASTC has not */
    VkAstcSizes a;
    memset(&a, 0, sizeof a);
    vk_astc_sizes_add(&a, 4, 4, VK_ASTC_AS_BC7, 10);
    vk_astc_sizes_add(&a, 4, 4, VK_ASTC_AS_RGBA8, 20);
    vk_astc_sizes_add(&a, 4, 4, VK_ASTC_AS_BC7, 5);
    vk_astc_sizes_add(&a, 12, 12, VK_ASTC_AS_ASTC, 7);
    vk_astc_sizes_add(&a, 10, 5, VK_ASTC_AS_RGBA8, 3);
    assert(a.bytes[0][0][VK_ASTC_AS_BC7] == 15 && a.bytes[0][0][VK_ASTC_AS_RGBA8] == 20);
    assert(a.bytes[8][8][VK_ASTC_AS_ASTC] == 7 && a.bytes[6][1][VK_ASTC_AS_RGBA8] == 3);
    VkAstcSizes was = a;
    vk_astc_sizes_add(&a, 3, 4, VK_ASTC_AS_BC7, 1);
    vk_astc_sizes_add(&a, 4, 13, VK_ASTC_AS_BC7, 1);
    vk_astc_sizes_add(&a, 4, 4, VK_ASTC_AS_N, 1);
    assert(!memcmp(&a, &was, sizeof a));

    puts("vk_tex_census: ok");
    return 0;
}
