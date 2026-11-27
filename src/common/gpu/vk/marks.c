/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/marks.h"
#include "common/gpu/vk/ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKS       16              /* a slot's timestamps: the parts of its submission and its end */
#define NAMES       16              /* the names summed */
#define PROFILE_NS  10000000000ull  /* the profile said every 10 seconds */

struct SnxVkMarks {
    VkQueryPool pool;          /* MARKS timestamps a slot, VK_NULL_HANDLE without the profile */
    double period;             /* a timestamp's tick in ns */
    uint64_t valid;            /* the mask of a timestamp's valid bits */
    bool checkpoints;
    const char* who;
    const char* work;
    const char* names[SNX_VK_RING_SLOTS][MARKS]; /* each slot's parts, in order */
    unsigned count[SNX_VK_RING_SLOTS];           /* each slot's timestamps written */
    struct {
        const char* name;
        double ns;
    } sum[NAMES];
    unsigned sums;
    uint64_t submissions, since; /* summed since since (ns), 0 before the first */
};

bool snx_vk_marks_choose(SnxVkChoice* c, int checkpoints) {
    return checkpoints && snx_vk_choice_has(c, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) &&
           snx_vk_choice_enable(c, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
}

SnxVkMarks* snx_vk_marks_new(VkPhysicalDevice phys, VkDevice device, uint32_t family, int profile, int checkpoints,
                             bool checkpoints_made, const char* who, const char* work) {
    if (checkpoints && !checkpoints_made)
        fprintf(stderr, "%s: vk_checkpoints=%d: the device has no VK_NV_device_diagnostic_checkpoints: no checkpoints\n",
                who, checkpoints);
    VkQueryPool pool = VK_NULL_HANDLE;
    double period = 0;
    uint64_t valid = 0;
    if (profile) {
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, NULL);
        VkQueueFamilyProperties families[32];
        if (n > 32) n = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, families);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys, &props);
        uint32_t bits = family < n ? families[family].timestampValidBits : 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = SNX_VK_RING_SLOTS * MARKS};
        if (!bits || props.limits.timestampPeriod <= 0)
            fprintf(stderr, "%s: vk_gpu_profile=%d: the queue has no timestamps: no profile\n", who, profile);
        else if (vkCreateQueryPool(device, &qi, NULL, &pool) != VK_SUCCESS)
            fprintf(stderr, "%s: vk_gpu_profile=%d: the timestamps' query pool is not made: no profile\n", who, profile);
        else {
            period = props.limits.timestampPeriod;
            valid = bits >= 64 ? UINT64_MAX : (1ull << bits) - 1;
        }
    }
    if (!pool && !checkpoints_made) return NULL;
    SnxVkMarks* m = calloc(1, sizeof *m);
    if (!m) {
        if (pool) vkDestroyQueryPool(device, pool, NULL);
        fprintf(stderr, "%s: no memory for the GPU's profile and checkpoints\n", who);
        return NULL;
    }
    *m = (SnxVkMarks){.pool = pool, .period = period, .valid = valid, .checkpoints = checkpoints_made, .who = who,
                      .work = work};
    if (pool)
        fprintf(stderr, "%s: vk_gpu_profile=%d: the GPU's time of each part of its work said every 10 seconds\n", who,
                profile);
    return m;
}

void snx_vk_marks_free(SnxVkMarks* m, VkDevice device) {
    if (!m) return;
    if (m->pool) vkDestroyQueryPool(device, m->pool, NULL);
    free(m);
}

/* a part begun (or the recording's end, name NULL): its timestamp, its checkpoint */
static void mark(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot, const char* name) {
    unsigned k = m->count[slot];
    if (m->checkpoints && name) vkCmdSetCheckpointNV(cb, name);
    if (!m->pool || k >= MARKS) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m->pool, slot * MARKS + k);
    m->names[slot][k] = name;
    m->count[slot] = k + 1;
}
void snx_vk_marks_begin(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot) {
    m->count[slot] = 0;
    if (m->pool) vkCmdResetQueryPool(cb, m->pool, slot * MARKS, MARKS);
    mark(m, cb, slot, m->work);
}
void snx_vk_marks_pass(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot, const char* name) {
    /* the last timestamp kept for the recording's end */
    if (m->count[slot] + 1 < MARKS || !m->pool) mark(m, cb, slot, name);
}
void snx_vk_marks_end(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot) { mark(m, cb, slot, NULL); }

static void add(SnxVkMarks* m, const char* name, double ns) {
    unsigned i = 0;
    while (i < m->sums && strcmp(m->sum[i].name, name)) ++i;
    if (i == m->sums) {
        if (m->sums == NAMES) return;
        m->sum[m->sums++] = (typeof(m->sum[0])){name, 0};
    }
    m->sum[i].ns += ns;
}
void snx_vk_marks_line(const SnxVkMarks* m, double seconds, char* out, size_t size) {
    int n = snprintf(out, size, "the GPU's time of its work, the last %.1f s, %llu submissions:", seconds,
                     (unsigned long long)m->submissions);
    for (unsigned i = 0; i < m->sums && n > 0 && (size_t)n < size; ++i)
        n += snprintf(out + n, size - (size_t)n, "%s %s %.2f ms", i ? "," : "", m->sum[i].name,
                      m->sum[i].ns / 1e6 / (double)(m->submissions ? m->submissions : 1));
}
void snx_vk_marks_retire(SnxVkMarks* m, VkDevice device, unsigned slot, uint64_t now_ns) {
    unsigned k = m->count[slot];
    m->count[slot] = 0;
    if (!m->pool || k < 2) return;
    uint64_t t[MARKS];
    if (vkGetQueryPoolResults(device, m->pool, slot * MARKS, k, sizeof t, t, sizeof *t, VK_QUERY_RESULT_64_BIT) !=
        VK_SUCCESS)
        return;
    for (unsigned i = 0; i + 1 < k; ++i)
        add(m, m->names[slot][i], (double)((t[i + 1] - t[i]) & m->valid) * m->period);
    ++m->submissions;
    if (!m->since) m->since = now_ns;
    if (now_ns - m->since < PROFILE_NS) return;
    char line[512];
    snx_vk_marks_line(m, (double)(now_ns - m->since) / 1e9, line, sizeof line);
    fprintf(stderr, "%s: %s\n", m->who, line);
    m->sums = 0, m->submissions = 0, m->since = now_ns;
}

void snx_vk_marks_lost(SnxVkMarks* m, VkQueue queue) {
    if (!m->checkpoints) return;
    uint32_t n = 0;
    vkGetQueueCheckpointDataNV(queue, &n, NULL);
    VkCheckpointDataNV data[32];
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; ++i) data[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    vkGetQueueCheckpointDataNV(queue, &n, data);
    fprintf(stderr, "%s: the device is lost; the queue's checkpoints reached: %u\n", m->who, n);
    for (uint32_t i = 0; i < n; ++i)
        fprintf(stderr, "%s:   %s at stage 0x%x\n", m->who, data[i].pCheckpointMarker ? (const char*)data[i].pCheckpointMarker : "?",
                (unsigned)data[i].stage);
}
