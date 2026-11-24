/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The constant buffer slots bound as dynamic uniform buffers (cb_dynamic.h) */
#include "nx/gpu/vk/cb_dynamic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

VkCbDynamic g_cb_dynamic;

#define CB_MOVES_MAGIC "snx-cbmoves 1"

typedef struct {
    u32 set, slot;
    u64 score;
} CbMove;
/* One game's title screen on an RTX 4090, the new sets' moves a present of each slot: vertex 7 135, 11 27, 15 20, 9 17,
   0 13, 3 12.5, 5 11, 6 9; fragment 7 25, 0 20, 3 14, 4 13, 11 13, 15 12, 5 11. The two lists merged by those counts */
static const CbMove cb_default[] = {{0, 7, 1350}, {0, 11, 270}, {1, 7, 250}, {0, 15, 200}, {1, 0, 200},
                                    {0, 9, 170},  {1, 3, 140},  {0, 0, 130}, {1, 4, 130},  {1, 11, 130},
                                    {0, 3, 125},  {1, 15, 120}, {0, 5, 110}, {1, 5, 110},  {0, 6, 90}};
static int cb_move_order(const void* a, const void* b) {
    const CbMove *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    if (x->set != y->set) return x->set < y->set ? -1 : 1; /* a tie: the vertex set's first, then the slot's */
    return x->slot < y->slot ? -1 : x->slot > y->slot;
}
/* The file's lines past the magic one: "set slot moves dynamic", dynamic 1 where the run that wrote it had the slot
   dynamic. Their scores into m: n, or 0 for no file or none */
static u32 cb_moves_read(CbMove* m, u32 cap) {
    size_t size;
    char* text = (char*)snx_read_file(g_cb_dynamic.path, &size);
    if (!text) return 0;
    u32 n = 0;
    if (!strncmp(text, CB_MOVES_MAGIC "\n", sizeof CB_MOVES_MAGIC)) {
        const char* p = text + sizeof CB_MOVES_MAGIC;
        unsigned set, slot, dynamic;
        unsigned long long moves;
        int used;
        while (n < cap && sscanf(p, "%u %u %llu %u%n", &set, &slot, &moves, &dynamic, &used) == 4) {
            p += used;
            /* A slot the last run had dynamic keeps its place against one with up to twice its moves: another choice
               is other set layouts, which every pipeline of the driver's cache and every shader object binary
               (g_vk_dslayout_hash) is made again for. Noise between two runs then does not swap the slots at the
               end of the choice */
            if (set < 2 && slot < VK_CB_DYNAMIC_SLOTS && moves)
                m[n++] = (CbMove){set, slot, (u64)moves * (dynamic ? 2u : 1u)};
        }
    }
    free(text);
    return n;
}
void vk_cb_dynamic_choose(const char* cache_dir) {
    const u32 limit = g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic;
    g_cb_dynamic.mask[0] = g_cb_dynamic.mask[1] = 0;
    snprintf(g_cb_dynamic.path, sizeof g_cb_dynamic.path, "%s/%s.vkcbmoves", cache_dir, g_exename);
    const char* off = !g_settings.dynamic_constant_buffers ? "off (vk_dynamic_cbufs=0)"
                      : g_vk.has_descriptor_buffers        ? "off: descriptor buffers hold the sets"
                      : g_vk.has_descriptor_heap_support   ? "off: descriptor heaps can hold the sets"
                                                           : NULL;
#if defined(__ANDROID__)
    /* Turnip keeps dynamic buffers in the descriptor set the runtime takes back for a fifth (gpu_driver_load.c's
       turnip_sets), where they read as zeros: none on Android until that is checked on the phones (#2780) */
    if (!off)
        off = "off on Android, where vk_dynamic_cbufs=1 is not taken: Turnip keeps dynamic buffers in the descriptor "
              "set the runtime takes back for a fifth (#2780)";
#endif
    g_cb_dynamic.counting = !off;
    if (off) {
        LOG("vulkan: dynamic constant buffers %s, the device's limit %u\n", off, limit);
        return;
    }
    CbMove m[2 * VK_CB_DYNAMIC_SLOTS + sizeof cb_default / sizeof *cb_default];
    u32 n = cb_moves_read(m, 2 * VK_CB_DYNAMIC_SLOTS);
    const int from_file = n != 0;
    if (!from_file)
        for (u32 i = 0; i < sizeof cb_default / sizeof *cb_default; i++) m[n++] = cb_default[i];
    qsort(m, n, sizeof *m, cb_move_order);
    /* the most moved first, up to the device's limit on the dynamic uniform buffers of a pipeline layout: the vertex
       and fragment sets are both in each, and no other set has any. Not a slot past the uniform buffers the device
       takes (cbufs_fit_limits), which is a storage buffer */
    u32 taken = 0;
    for (u32 i = 0; i < n && taken < limit; i++) {
        if (vk_cbuf_type(m[i].slot) != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (g_cb_dynamic.mask[m[i].set] >> m[i].slot & 1))
            continue;
        g_cb_dynamic.mask[m[i].set] |= 1u << m[i].slot;
        taken++;
    }
    char list[2][96];
    for (int s = 0; s < 2; s++) {
        size_t at = 0;
        list[s][0] = 0;
        for (u32 i = 0; i < n; i++) /* in the order of the choice */
            if (m[i].set == (u32)s && (g_cb_dynamic.mask[s] >> m[i].slot & 1) && at < sizeof list[s]) {
                int w = snprintf(list[s] + at, sizeof list[s] - at, " %u", m[i].slot);
                if (w > 0) at += (size_t)w;
            }
        if (!at) snprintf(list[s], sizeof list[s], " none");
    }
    LOG("vulkan: dynamic constant buffers, %u of the device's limit %u, from %s: vertex set%s, fragment set%s\n", taken,
        limit, from_file ? g_cb_dynamic.path : "the default order", list[0], list[1]);
}
void vk_cb_dynamic_save(void) {
    if (!g_cb_dynamic.counting) return;
    CbMove m[2 * VK_CB_DYNAMIC_SLOTS];
    u32 n = 0;
    for (u32 s = 0; s < 2; s++)
        for (u32 i = 0; i < VK_CB_DYNAMIC_SLOTS; i++) {
            u64 moves = atomic_load_explicit(&g_cb_dynamic.moves[s][i], memory_order_relaxed);
            if (moves) m[n++] = (CbMove){s, i, moves};
        }
    if (!n) return; /* a run that drew nothing keeps the last run's */
    qsort(m, n, sizeof *m, cb_move_order);
    char text[sizeof CB_MOVES_MAGIC + 2 * VK_CB_DYNAMIC_SLOTS * 40];
    size_t at = (size_t)snprintf(text, sizeof text, "%s\n", CB_MOVES_MAGIC);
    for (u32 i = 0; i < n && at < sizeof text; i++) {
        int w = snprintf(text + at, sizeof text - at, "%u %u %llu %u\n", m[i].set, m[i].slot,
                         (unsigned long long)m[i].score, g_cb_dynamic.mask[m[i].set] >> m[i].slot & 1);
        if (w > 0) at += (size_t)w;
    }
    if (at >= sizeof text) at = sizeof text - 1; /* snprintf cut the last line: not its NUL */
    char tmp[sizeof g_cb_dynamic.path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_cb_dynamic.path);
    if (snx_write_file(tmp, text, at) || plat_replace_file(tmp, g_cb_dynamic.path)) {
        remove(tmp);
        WARN("vulkan: the constant buffers' moves could not be written to %s\n", g_cb_dynamic.path);
    }
}
