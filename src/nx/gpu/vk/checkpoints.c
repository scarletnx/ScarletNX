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
static struct {
    PMutex lock;
    VkCk* ring;
    u64 newest;
    int reported;
} g_ck = {.lock = PMUTEX_INIT};
/* The first report freezes publication while holding lock. This view then stays immutable, including while the
   Vulkan queries and log callbacks run. A producer that already left the lock may still record its marker. */
typedef struct {
    const VkCk* ring;
    u64 newest;
} VkCkView;

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
    /* Device startup, before any producer or reporter. */
    g_ck.ring = calloc(CK_RING, sizeof *g_ck.ring);
    if (!g_ck.ring) {
        g_vk.has_checkpoints = 0;
        WARN("vulkan: vk_checkpoints: no memory for its ring\n");
    }
}
void vk_ck_(VkCommandBuffer cb, u32 kind, u32 a, u32 b, u32 c, u32 d) {
    u64 submit = vk_submit_number(), marker;
    pm_lock(&g_ck.lock);
    /* Pointer bits carry the complete generation. Never wrap them and mistake an old GPU marker for a new one. */
    if (!g_ck.ring || g_ck.reported || g_ck.newest == UINTPTR_MAX) {
        pm_unlock(&g_ck.lock);
        return;
    }
    marker = ++g_ck.newest;
    g_ck.ring[(marker - 1) % CK_RING] = (VkCk){submit, kind, a, b, c, d};
    pm_unlock(&g_ck.lock);
    /* A marker is a pointer's bits, the driver's to keep and never dereference. No recorder call holds lock. */
    rec_CmdSetCheckpointNV(cb, (const void*)(uintptr_t)marker);
}
static const VkCk* ck_find(const VkCkView* view, u64 marker) {
    /* Serialized publication has no gaps and identities never wrap. The retained interval alone proves which
       generation occupies a slot; no per-entry copy of the marker is needed. */
    if (!marker || marker > view->newest || view->newest - marker >= CK_RING) return NULL;
    return &view->ring[(marker - 1) % CK_RING];
}
static void report_marker(const VkCkView* view, u64 stage, u64 marker) {
    if (!marker) return;
    const VkCk* e = ck_find(view, marker);
    if (!e) {
        WARN("vulkan:   stage %#llx reached marker %llu of %llu: no longer in the diagnostic ring\n",
             (unsigned long long)stage, (unsigned long long)marker, (unsigned long long)view->newest);
        return;
    }
    WARN("vulkan:   stage %#llx reached marker %llu of %llu, submission %llu: %s: %u, %u, %u, %u\n",
         (unsigned long long)stage, (unsigned long long)marker, (unsigned long long)view->newest,
         (unsigned long long)e->submit, e->kind < VK_CK_KINDS ? g_ck_names[e->kind] : "?", e->a, e->b, e->c, e->d);
}
static void report_queue(const VkCkView* view, const char* name, VkQueue q) {
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
        for (u32 i = 0; i < n2; i++) report_marker(view, d2[i].stage, (u64)(uintptr_t)d2[i].pCheckpointMarker);
    }
    for (u32 i = 0; i < n; i++) report_marker(view, data[i].stage, (u64)(uintptr_t)data[i].pCheckpointMarker);
}
/* what the submissions the GPU hasn't finished hold: the first of them is the one it is stuck in */
static void report_unfinished(const VkCkView* view, u64 done, u64 submitted) {
    u32 back = view->newest < CK_RING ? (u32)view->newest : CK_RING;
    for (u32 offset = 1; offset <= 4 && offset <= UINT64_MAX - done; offset++) {
        u64 sub = done + offset;
        if (sub > submitted && sub - submitted > 1) break;
        u32 count[VK_CK_KINDS] = {0}, shown = 0, total = 0;
        for (u32 i = back; i > 0; i--) {
            const VkCk* e = ck_find(view, view->newest - i + 1);
            if (!e || e->submit != sub || e->kind >= VK_CK_KINDS) continue;
            count[e->kind]++;
            total++;
        }
        WARN("vulkan: submission %llu holds %u markers: astc %u, bc7 %u, copies %u, deswizzles %u, conversions %u, "
             "dispatches %u, passes %u\n",
             (unsigned long long)sub, total, count[VK_CK_ASTC], count[VK_CK_BC7], count[VK_CK_COPY],
             count[VK_CK_DESWIZZLE], count[VK_CK_CONVERT], count[VK_CK_DISPATCH], count[VK_CK_PASS]);
        for (u32 i = back; i > 0 && shown < 40; i--) {
            const VkCk* e = ck_find(view, view->newest - i + 1);
            if (!e || e->submit != sub || e->kind >= VK_CK_KINDS || e->kind == VK_CK_STRIP_DONE) continue;
            WARN("vulkan:   marker %llu, %s: %u, %u, %u, %u\n", (unsigned long long)(view->newest - i + 1),
                 g_ck_names[e->kind], e->a, e->b, e->c, e->d);
            shown++;
        }
    }
}
void vk_ck_report(void) {
    if (!g_vk.has_checkpoints) return;
    pm_lock(&g_ck.lock);
    if (!g_ck.ring || g_ck.reported) {
        pm_unlock(&g_ck.lock);
        return;
    }
    g_ck.reported = 1;
    VkCkView view = {g_ck.ring, g_ck.newest};
    pm_unlock(&g_ck.lock);
    u64 submitted = 0, done = 0;
    vk_submit_progress(&submitted, &done);
    WARN("vulkan: the device was lost or stopped with graphics submissions done %llu of %llu\n",
         (unsigned long long)done, (unsigned long long)submitted);
    report_unfinished(&view, done, submitted);
    report_queue(&view, "graphics", g_vk.gfx);
    if (g_vk.compute != g_vk.gfx) report_queue(&view, "compute", g_vk.compute);
}
