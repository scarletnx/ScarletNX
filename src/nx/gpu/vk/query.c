/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's samples-passed counter (3D 0x545 SampleCounterEnable, 0x54C ResetCounter 1, a counter report of
   selector 0x15: NVN's and deko3d's occlusion queries), counted with Vulkan occlusion queries.

   The count runs from its reset. It is kept in segments: a segment is one query, begun at the first draw counted in a
   rendering pass and ended with the pass, at a reset, or at a report. At a submit (and at a report), the results of
   the segments ended so far are copied into host-cached memory and their queries reset, in the same command buffer:
   once per submit, as each copy waits for its queries on the GPU. A report is a release (release.c) gated on a submit
   that holds those copies. When it is performed, the GPU has counted, and the segments since the epoch's previous
   report are added to the epoch's sum, which is its value: a game that resets once and subtracts reports keeps only
   the segments since its last report live. Whatever reads the report (the render enable's comparison, a semaphore
   acquire, the CPU) waits for that release, as for any other.

   Counting starts at the first reset: deko3d and NVN turn the counter on at startup, and a game that never resets it
   never reads it, so it costs those games nothing. A report before any reset, or in an epoch that outgrew the ring,
   keeps the made-up value the front end writes (maxwell/engines.c), over which a render enable draws.

   Render targets scaled by the resolution setting pass that many more samples: the sum is divided back, and a count
   that wasn't zero stays at least 1, as an object behind it is visible. Targets are single-sampled here, so a
   multisampled guest target counts pixels where the console counts samples.
   Everything here runs with vk_lock held. */
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"

#define QN    4096 /* segments the ring holds: queries in the pool, and result slots */
#define QWAIT 1024 /* reports queued and not performed yet, at most (the release queue's size) */

static VkQueryPool g_qpool;
static VkBuffer g_qbuf;
static VmaAllocation g_qmem;
static u64* g_qres;   /* segment s's count at s % QN, once its copy has run */
static int g_qfailed; /* the pool couldn't be made: reports keep the made-up value */
static struct {
    int disabled; /* SampleCounterEnable is off: draws aren't counted */
    int started;  /* a reset came: counting is on from there */
    int open;     /* the last segment's query is active in the rendering pass */
    int bad;      /* the epoch outgrew the ring: its reports keep the made-up value until the next reset */
    u64 next;     /* segments begun */
    u64 copied;   /* segments whose results have their copy recorded */
    u64 first;    /* the epoch's first segment */
    u64 reported; /* the end of the epoch's last report queued, its first before one */
    /* the oldest segment each queued report not performed yet adds to its epoch's sum, in order: their slots aren't
       used again until then */
    u64 wait[QWAIT];
    u32 whead, wtail;
    /* the reports performed: the sum of epoch sum_first's segments up to sum_end */
    u64 sum, sum_first, sum_end;
} g_q;

static int pool_make(void) {
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = QN};
    if (!VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &g_qpool))) {
        g_qpool = VK_NULL_HANDLE;
        return 0;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = QN * sizeof(u64),
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_qbuf, &g_qmem, &info))) {
        vkDestroyQueryPool(g_vk.dev, g_qpool, NULL);
        g_qpool = VK_NULL_HANDLE;
        return 0;
    }
    g_qres = info.pMappedData;
    /* a query is reset before its first use, outside a rendering pass */
    vk_render_end();
    vkCmdResetQueryPool(vk_cmd(), g_qpool, 0, QN);
    if (!g_vk.feat.occlusionQueryPrecise)
        WARN("vulkan: the device's occlusion queries aren't precise: samples-passed counts may be any nonzero value\n");
    return 1;
}

/* the oldest segment a report may still add: the oldest queued report's, or the next report's */
static u64 live_from(void) { return g_q.whead != g_q.wtail ? g_q.wait[g_q.wtail % QWAIT] : g_q.reported; }

void vk_query_draw(VkCommandBuffer cb) {
    if (!g_q.started || g_q.disabled || g_q.bad || g_q.open) return;
    /* the slot is still needed by a report, or its query not reset yet: this epoch counts no further */
    if (g_q.next - live_from() >= QN || g_q.next - g_q.copied >= QN) {
        g_q.bad = 1;
        LOG_LIMIT(4, "vulkan: a samples-passed count spans more than %u passes, its reports are made up\n", QN);
        return;
    }
    vkCmdBeginQuery(cb, g_qpool, (u32)(g_q.next % QN),
                    g_vk.feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    g_q.next++;
    g_q.open = 1;
}
void vk_query_pass_end(VkCommandBuffer cb) {
    if (!g_q.open) return;
    vkCmdEndQuery(cb, g_qpool, (u32)((g_q.next - 1) % QN));
    g_q.open = 0;
}
void vk_query_flush(VkCommandBuffer cb) {
    if (g_q.copied == g_q.next) return;
    while (g_q.copied < g_q.next) { /* in runs that don't wrap around the ring */
        u32 at = (u32)(g_q.copied % QN), n = QN - at;
        if (n > g_q.next - g_q.copied) n = (u32)(g_q.next - g_q.copied);
        vkCmdCopyQueryPoolResults(cb, g_qpool, at, n, g_qbuf, (VkDeviceSize)at * sizeof(u64), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        vkCmdResetQueryPool(cb, g_qpool, at, n);
        g_q.copied += n;
    }
    /* for the host's reads, and for a later copy into a slot of a segment no report read */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

void vk_query_reset(void) {
    if (!g_qpool) {
        if (g_qfailed) return;
        if (!pool_make()) {
            g_qfailed = 1;
            return;
        }
    }
    if (g_q.open) vk_query_pass_end(vk_cmd()); /* the segment open now counted the epoch before */
    if (!g_q.started) LOG("vulkan: the game counts samples passed, with occlusion queries\n");
    g_q.started = 1;
    g_q.bad = 0;
    g_q.first = g_q.reported = g_q.next;
}
void vk_query_enable(int on) {
    if (!on && g_q.open) vk_query_pass_end(vk_cmd());
    g_q.disabled = !on;
}
int vk_query_report(u64 gva, u32 len, u64 ticks) {
    if (!g_q.started || g_q.bad || g_q.whead - g_q.wtail == QWAIT) return 0;
    if (g_q.open) vk_query_pass_end(vk_cmd());
    vk_render_end();
    vk_query_flush(vk_cmd()); /* the segments counted so far, copied before the submit the report waits for */
    g_q.wait[g_q.whead++ % QWAIT] = g_q.reported;
    g_q.reported = g_q.next;
    vk_release_samples(gva, len, g_q.first, g_q.next, ticks);
    return 1;
}
/* a report being performed, in order: the count of its epoch's segments [first, end), whose copies have run */
u64 vk_query_value(u64 first, u64 end) {
    vmaInvalidateAllocation(g_vk.vma, g_qmem, 0, VK_WHOLE_SIZE);
    if (first != g_q.sum_first) { /* a new epoch (one with segments starts past the last one's) */
        g_q.sum_first = g_q.sum_end = first;
        g_q.sum = 0;
    }
    for (u64 s = g_q.sum_end; s < end; s++) g_q.sum += g_qres[s % QN];
    if (end > g_q.sum_end) g_q.sum_end = end;
    if (g_q.whead != g_q.wtail) g_q.wtail++;
    double area = (double)g_vk_scale * (double)g_vk_scale;
    u64 v = area == 1.0 ? g_q.sum : (u64)((double)g_q.sum / area + 0.5);
    return g_q.sum && !v ? 1 : v;
}
