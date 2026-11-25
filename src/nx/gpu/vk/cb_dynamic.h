/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The vertex and fragment programs' constant buffer slots that the set layouts bind as dynamic uniform buffers
   (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC), the GPU plan's M3b. A constant buffer's copy moves at most draws: with
   its offset in the descriptor, each move made a new set (81 to 86% of one game's new sets differed from a set of the
   same frame only in those offsets). A dynamic slot's descriptor holds the buffer and the range, and the bind of the set
   gives the offset (vkCmdBindDescriptorSets' pDynamicOffsets), so the set stays.

   Only with descriptor sets: descriptor buffers and heaps have no dynamic descriptors. The device caps the dynamic
   uniform buffers of a pipeline layout (maxDescriptorSetUniformBuffersDynamic: 15 on NVIDIA's, 16 on RADV, 8 on ANV),
   so the slots that move most take them. Which slots move most is the game's: each run counts the moves of each slot
   and keeps them in the game's shader cache as it closes (<game>.vkcbmoves), with the slots it had dynamic, and the next
   start chooses from them, before the set layouts are made: the same slots, unless others move far more. With no file,
   one game's order (cb_dynamic.c) */
#ifndef SNX_VK_CB_DYNAMIC_H
#define SNX_VK_CB_DYNAMIC_H
#include <stdatomic.h>
#include "nx/gpu/vk/vk.h"

#define VK_CB_DYNAMIC_SLOTS 18 /* the constant buffer slots of a stage's set, from SNX_VKB_CB */

typedef struct {
    /* by the stage's set (0 vertex, 1 fragment): a bit for each slot bound as a dynamic uniform buffer. Set once at
       startup, before the layouts (vk_cb_dynamic_choose) */
    u32 mask[2];
    int counting;    /* the moves are counted and saved: the setting on and the descriptors in sets */
    char path[1200]; /* the file of the moves */
    /* the moves of each slot, by the set: the thread with vk_lock counts them, the exit's save reads them */
    _Atomic u64 moves[2][VK_CB_DYNAMIC_SLOTS];
    u64 last[2][VK_CB_DYNAMIC_SLOTS]; /* each slot's last offset (vk_lock) */
} VkCbDynamic;
extern VkCbDynamic g_cb_dynamic;

/* Chooses the dynamic slots (g_cb_dynamic.mask) from the file in cache_dir, else from the default order, and logs the
   line that says so. After cbufs_fit_limits (vk_cbuf_type), before the set layouts */
void vk_cb_dynamic_choose(const char* cache_dir);
/* Writes this run's moves to the file, as the game closes (any thread) */
void vk_cb_dynamic_save(void);
/* the descriptor type of constant buffer slot `slot` in the stage's set `set` */
static inline VkDescriptorType vk_cb_dynamic_type(int set, u32 slot) {
    return (u32)set < 2 && slot < VK_CB_DYNAMIC_SLOTS && (g_cb_dynamic.mask[set] >> slot & 1)
               ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
               : vk_cbuf_type(slot);
}
/* a set's write of slot `slot` at offset off (vk_lock): a move when off is not the slot's last */
static inline void vk_cb_dynamic_move(int set, u32 slot, u64 off) {
    if (!g_cb_dynamic.counting || (u32)set >= 2 || slot >= VK_CB_DYNAMIC_SLOTS || g_cb_dynamic.last[set][slot] == off)
        return;
    g_cb_dynamic.last[set][slot] = off;
    _Atomic u64* m = &g_cb_dynamic.moves[set][slot];
    atomic_store_explicit(m, atomic_load_explicit(m, memory_order_relaxed) + 1, memory_order_relaxed);
}
#endif
