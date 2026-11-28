/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A texture whose levels are drawn through render targets keeps every level's memory. Under memory pressure a new
   texture with levels is made with its finest level never bound and sampled from the next one on (texture_half_res),
   which suits one loaded from guest memory. A game that blurs its frame down a chain of targets samples each level
   through one texture header over them: made so while its targets were being made again each frame, the copy of the
   first level's target went into memory that was not there, every pass down the chain read zeros and the frame it
   was added to came out white. Here texture_half_res is Always: a 512 by 512 texture of ten levels made over a drawn
   target is whole and holds the target's pixels */
#define TAKE_LEVELS_GVA 0xCC8000000ull
#define TAKE_LEVELS_CPU (HEAP_BASE + 0x40400000)

/* a 512 by 512 block linear RGBA8 header at gva, blocks 16 GOBs high, ten levels: level 0 is 1 MiB */
static void take_levels_tic(u32* tic, u64 gva) {
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 words[8] = {rgba8, (u32)gva, (u32)(gva >> 32) | 3u << 21, 4u << 3 | 9u << 28, 511 | 1u << 23, 511,
                          0,     9u << 4};
    memcpy(tic, words, sizeof words);
}

static int check_take_levels(void) {
    const u64 over = TAKE_LEVELS_GVA, before = TAKE_LEVELS_GVA + 0x400000;
    static const u8 red[4] = {255, 0, 0, 255};
    const int half = g_settings.texture_half_res;
    u32 tic_over[8], tic_before[8];
    u8 px[4] = {0};
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(TAKE_LEVELS_CPU, 0x800000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(TAKE_LEVELS_GVA, TAKE_LEVELS_CPU, 0x800000);
    take_levels_tic(tic_over, over);
    take_levels_tic(tic_before, before);
    g_settings.texture_half_res = 2;
    /* made with nothing drawn over it: at half its resolution, where the device makes such a texture */
    vk_lock();
    Surf* late = vk_tex_from_tic(tic_before);
    const int made_half = late && late->shed && late->lod_floor == 1;
    vk_unlock();
    if (!made_half) {
        g_settings.texture_half_res = half;
        printf("texture levels from targets: not checked, this device makes no texture at half its resolution "
               "(sparse binding %d, residency %d, view min LOD %d)\n",
               g_vk.sparse, g_vk.resident, g_vk.has_view_min_lod);
        free(ch);
        return 0;
    }
    /* made over a drawn target: whole, with the target's pixels */
    rt_clear(ch, over, 512, 512, 4, 0);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic_over);
    const int whole = t && !t->is_rt && t->levels == 10 && !t->shed && t->lod_floor == 0;
    const int read = t && !read_pixel0(t, px);
    vk_unlock();
    if (!whole || !read || memcmp(px, red, 4))
        fprintf(stderr,
                "texture levels from targets: the texture made over a drawn target is shed %d with its floor at level "
                "%u (0 and 0 expected) and its first pixel is %02x%02x%02x%02x, not the target's ff0000ff\n",
                t ? t->shed : -1, t ? t->lod_floor : 99, px[0], px[1], px[2], px[3]);
    CHECK(whole && read && !memcmp(px, red, 4));
    g_settings.texture_half_res = half;
    puts("texture levels from targets: made whole over a drawn target");
    free(ch);
    return 0;
}
