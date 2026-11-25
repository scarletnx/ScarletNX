/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pool map of bindless textures in shadow mode (docs/dev/draw-cost.md, item 7, step 3; the setting
   pool_map_shadow). A record for each TIC index of a texture pool, a sampler record for each TSC index of a sampler
   pool, each with a valid bit. A slot's resolve fills an invalid record. Events clear a record: its header written (by
   the CPU, by the GPU, or a result pending over it), the surface table changed at its texture's address or at the target
   it crops (surf_changed, which a freed surface moves too), a new draw to the target it crops, the scale, an image swap
   of its texture and a view of its record destroyed. The renderer still draws through its resolves: the map only
   compares a valid record with each resolve, and counts what agrees and what does not, by cause. All on the CPU, under
   vk_lock. With the setting off, the renderer calls none of this in a draw, and the events return at once */
#ifndef SNX_VK_POOL_MAP_H
#define SNX_VK_POOL_MAP_H
#include "nx/gpu/vk/vk.h"

/* what today's resolve gave a slot */
typedef struct {
    const Surf* t;          /* the texture, NULL for none */
    const u32* tic;         /* its two headers, 8 words each */
    const u32* tsc;
    const Surf* over;       /* the target the texture crops or copies (render.c TexOver), NULL for none */
    u32 over_wgen;          /* that target's write generation then */
    int keepable;           /* the texture memo keeps it: a record may hold it */
    VkImageView view;       /* the view the slot binds of t itself, VK_NULL_HANDLE for none or another surface's */
    u32 view_kind;          /* how the slot reads it (view type and number kind): another kind compares no view */
} PoolSlot;
/* why a record and a resolve differ, or why a record was cleared */
enum { POOL_DIFF_SURFACE, POOL_DIFF_HEADER, POOL_DIFF_SAMPLER, POOL_DIFF_OVER, POOL_DIFF_VIEW, POOL_DIFF_N };
enum { POOL_CLEAR_HEADER, POOL_CLEAR_SURFACE, POOL_CLEAR_DRAWN, POOL_CLEAR_SCALE, POOL_CLEAR_SWAP, POOL_CLEAR_VIEW,
       POOL_CLEAR_N };
typedef struct {
    u64 agree, samplers_agree; /* valid records the resolve agreed with */
    u64 differ[POOL_DIFF_N];
    u64 filled, unkept, past; /* records filled, resolves no record may hold, handles past a pool's limit */
    u64 cleared[POOL_CLEAR_N];
} PoolMapCounts;

/* slot's resolve of handle, with the draw's pools and their limits: compared with the valid records, or filled into
   them (vk_lock held, the setting on) */
void vk_pool_map_slot(u64 tex_pool, u32 tex_max, u64 smp_pool, u32 smp_max, u32 handle, const PoolSlot* slot);
/* events from outside a draw (they return at once with the setting off) */
void vk_pool_map_swapped(const Surf* t);    /* t's image is another one now: its views are gone */
void vk_pool_map_view_gone(VkImageView v);  /* v is destroyed */
void vk_pool_map_counts(PoolMapCounts* c);
void vk_pool_map_log(u64 presents);         /* the counts since the last line, with vk_render_stats_log */
/* tests: whether the record of TIC index of the texture pool at tex_pool is valid now (the events taken in first) */
int vk_pool_map_valid(u64 tex_pool, u32 index);
void vk_pool_map_forget(void);              /* every record cleared and every pool forgotten */

/* The pool map that SM86 programs read (pool_map_gpu.c, the setting pool_map). vk_pool_gpu_draw at a draw's first
   stage that reads it: the pages, directories and header of the draw's pools as the draw's records hold them, written
   into this frame's arena where they changed, with the arena's address and the header's word index; 0 where it has
   no map (the arena full, no memory), and the draw's slots take their words. vk_pool_gpu_take: whether the record of
   the handle holds view in class cls and smp (shadow: the depth compare sampler), as the draw's map does, with view's
   slot; vk_pool_gpu_learn: the record holds them from now, for later draws. vk_pool_gpu_empty: the empty map, which a
   draw that reads none names */
enum {
    VK_POOL_GPU_MAP,     /* a slot through the map */
    VK_POOL_GPU_MISS,    /* its own word: the record did not hold its view and sampler, or the draw changed it */
    VK_POOL_GPU_HANDLE,  /* a handle from memory or an indexed table (the program reads its word) */
    VK_POOL_GPU_VIEW,    /* a view of another surface (a copy, a feedback loop) or another layout */
    VK_POOL_GPU_PAST,    /* a handle past a pool's limit */
    VK_POOL_GPU_COMPARE, /* a depth compare of a colour texture, which the program takes with the plain sampler */
    VK_POOL_GPU_NONE,    /* no map for the draw */
    VK_POOL_GPU_WHY_N
};
typedef struct {
    u64 why[VK_POOL_GPU_WHY_N];
    u64 learned, conflicts, forgotten, pages_written, words_written, versions, full, switches, gpu_bytes;
} VkPoolGpuCounts;
int vk_pool_gpu_draw(u64 tex_pool, u32 tex_max, u64 smp_pool, u32 smp_max, u64* address, u32* header);
int vk_pool_gpu_take(u32 handle, u32 cls, int shadow, VkImageView view, VkSampler smp, u32* image_slot);
void vk_pool_gpu_learn(u32 handle, u32 cls, int shadow, VkImageView view, VkSampler smp, u32 image_slot,
                       u32 sampler_slot);
u64 vk_pool_gpu_empty(u32* header);
void vk_pool_gpu_view_gone(VkImageView v);
void vk_pool_gpu_count(int why);
void vk_pool_gpu_counts(VkPoolGpuCounts* c);
void vk_pool_gpu_forget(void);
void vk_pool_gpu_log(u64 presents);
#endif
