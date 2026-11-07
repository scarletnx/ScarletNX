/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_checkpoints=1 (VK_NV_device_diagnostic_checkpoints): what the GPU was doing when the device was lost. Each piece
   of work worth naming records a marker in its command buffer, an index into a ring of what it was. After a loss the
   driver says which markers each queue's pipeline stages reached, and the report logs them. */
#include <stdlib.h>
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"

#define CK_RING (1u << 18)
typedef struct {
    u64 submit;
    u32 kind, a, b, c, d;
} VkCk;
static VkCk* g_ck;
static u32 g_ck_next; /* atomic: compute records on its own thread */
static int g_ck_reported; /* atomic */

static const char* const g_ck_names[VK_CK_KINDS] = {
    "ASTC decode (blocks wide, high, slices, footprint)",
    "BC7 encode (blocks wide, high, slices, texels wide)",
    "copy of a strip to its image (width, height, slices, level)",
    "strip done (width, height, slices, level)",
    "deswizzle (row bytes, rows, slices, to block linear)",
    "format conversion (groups wide, rows, slices)",
    "game compute dispatch (groups x, y, z)",
    "rendering pass (width, height, color targets)",
    "end of a submission (reason)",
};

void vk_ck_init(void) {
    g_ck = calloc(CK_RING, sizeof *g_ck);
    if (!g_ck) {
        g_vk.has_checkpoints = 0;
        WARN("vulkan: vk_checkpoints: no memory for its ring\n");
    }
}
void vk_ck_(VkCommandBuffer cb, u32 kind, u32 a, u32 b, u32 c, u32 d) {
    u32 at = __atomic_fetch_add(&g_ck_next, 1, __ATOMIC_RELAXED);
    VkCk* e = &g_ck[at % CK_RING];
    e->submit = vk_submit_number();
    e->kind = kind;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    /* at + 1: a marker is a pointer's bits, the driver's to keep and never read */
    rec_CmdSetCheckpointNV(cb, (const void*)(uintptr_t)((u64)at + 1));
}
static void report_queue(const char* name, VkQueue q) {
    enum { MAX = 64 };
    VkCheckpointDataNV data[MAX];
    u32 n = 0;
    vkGetQueueCheckpointDataNV(q, &n, NULL);
    if (n > MAX) n = MAX;
    for (u32 i = 0; i < n; i++) data[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    vkGetQueueCheckpointDataNV(q, &n, data);
    WARN("vulkan: checkpoints of the %s queue: %u stages reached a marker\n", name, n);
    if (vkGetQueueCheckpointData2NV) { /* what a queue submitted to with vkQueueSubmit2 may keep only this way */
        VkCheckpointData2NV d2[MAX];
        u32 n2 = 0;
        vkGetQueueCheckpointData2NV(q, &n2, NULL);
        if (n2 > MAX) n2 = MAX;
        for (u32 i = 0; i < n2; i++) d2[i] = (VkCheckpointData2NV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV};
        vkGetQueueCheckpointData2NV(q, &n2, d2);
        WARN("vulkan: its second query: %u stages reached a marker\n", n2);
        for (u32 i = 0; i < n2; i++) {
            u64 marker = (u64)(uintptr_t)d2[i].pCheckpointMarker;
            if (!marker) continue;
            const VkCk* e = &g_ck[(u32)(marker - 1) % CK_RING];
            WARN("vulkan:   stage %#llx reached marker %llu, submission %llu: %s: %u, %u, %u, %u\n",
                 (unsigned long long)d2[i].stage, (unsigned long long)(marker - 1), (unsigned long long)e->submit,
                 e->kind < VK_CK_KINDS ? g_ck_names[e->kind] : "?", e->a, e->b, e->c, e->d);
        }
    }
    u32 newest = __atomic_load_n(&g_ck_next, __ATOMIC_RELAXED);
    for (u32 i = 0; i < n; i++) {
        u64 marker = (u64)(uintptr_t)data[i].pCheckpointMarker;
        if (!marker) continue;
        u32 at = (u32)(marker - 1);
        const VkCk* e = &g_ck[at % CK_RING];
        WARN("vulkan:   stage %#x reached marker %u of %u, submission %llu: %s: %u, %u, %u, %u\n",
             (unsigned)data[i].stage, at, newest, (unsigned long long)e->submit,
             e->kind < VK_CK_KINDS ? g_ck_names[e->kind] : "?", e->a, e->b, e->c, e->d);
    }
}
/* what the submissions the GPU hasn't finished hold: the first of them is the one it is stuck in */
static void report_unfinished(u64 done, u64 submitted) {
    u32 newest = __atomic_load_n(&g_ck_next, __ATOMIC_RELAXED);
    u32 back = newest < CK_RING ? newest : CK_RING;
    for (u64 sub = done + 1; sub <= submitted + 1 && sub <= done + 4; sub++) {
        u32 count[VK_CK_KINDS] = {0}, shown = 0, total = 0;
        for (u32 i = back; i > 0; i--) {
            const VkCk* e = &g_ck[(newest - i) % CK_RING];
            if (e->submit != sub || e->kind >= VK_CK_KINDS) continue;
            count[e->kind]++;
            total++;
        }
        WARN("vulkan: submission %llu holds %u markers: astc %u, bc7 %u, copies %u, deswizzles %u, conversions %u, "
             "dispatches %u, passes %u\n",
             (unsigned long long)sub, total, count[VK_CK_ASTC], count[VK_CK_BC7], count[VK_CK_COPY],
             count[VK_CK_DESWIZZLE], count[VK_CK_CONVERT], count[VK_CK_DISPATCH], count[VK_CK_PASS]);
        for (u32 i = back; i > 0 && shown < 40; i--) {
            const VkCk* e = &g_ck[(newest - i) % CK_RING];
            if (e->submit != sub || e->kind >= VK_CK_KINDS || e->kind == VK_CK_STRIP_DONE) continue;
            WARN("vulkan:   %s: %u, %u, %u, %u\n", g_ck_names[e->kind], e->a, e->b, e->c, e->d);
            shown++;
        }
    }
}
void vk_ck_report(void) {
    if (!g_vk.has_checkpoints || __atomic_exchange_n(&g_ck_reported, 1, __ATOMIC_SEQ_CST)) return;
    u64 submitted = 0, done = 0;
    vk_submit_progress(&submitted, &done);
    WARN("vulkan: the device was lost with graphics submissions done %llu of %llu\n", (unsigned long long)done,
         (unsigned long long)submitted);
    report_unfinished(done, submitted);
    report_queue("graphics", g_vk.gfx);
    if (g_vk.compute != g_vk.gfx) report_queue("compute", g_vk.compute);
}
