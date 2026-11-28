/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile and vk_checkpoints in a console's renderer (common/gpu/vk/marks.c), on fakes of the driver's calls:
   a submission's parts timed by its timestamps and summed per name, a checkpoint at each part where the device has
   them, the ones a lost queue reached read back; none made without timestamps or checkpoints */
#include "common/gpu/vk/marks.h"
#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vk_marks: %s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            g_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static uint32_t g_bits = 64;
static int g_stamps, g_resets, g_checkpoints, g_lost_reads;
static const char* g_marked[8];
static VKAPI_ATTR void VKAPI_CALL families(VkPhysicalDevice phys, uint32_t* n, VkQueueFamilyProperties* out) {
    (void)phys;
    if (out && *n) out[0] = (VkQueueFamilyProperties){.queueCount = 1, .timestampValidBits = g_bits};
    *n = 1;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice phys, VkPhysicalDeviceProperties* p) {
    (void)phys;
    *p = (VkPhysicalDeviceProperties){0};
    p->limits.timestampPeriod = 1.0f;
}
static VKAPI_ATTR VkResult VKAPI_CALL pool_make(VkDevice d, const VkQueryPoolCreateInfo* i, const VkAllocationCallbacks* a,
                                                VkQueryPool* pool) {
    (void)d, (void)i, (void)a;
    *pool = (VkQueryPool)(uintptr_t)0x40;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL pool_free(VkDevice d, VkQueryPool pool, const VkAllocationCallbacks* a) {
    (void)d, (void)pool, (void)a;
}
static VKAPI_ATTR void VKAPI_CALL reset(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    (void)cb, (void)pool, (void)first, (void)count;
    ++g_resets;
}
static VKAPI_ATTR void VKAPI_CALL stamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool,
                                       uint32_t query) {
    (void)cb, (void)stage, (void)pool, (void)query;
    ++g_stamps;
}
static VKAPI_ATTR VkResult VKAPI_CALL results(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                              void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)d, (void)pool, (void)first, (void)size, (void)stride, (void)flags;
    static const uint64_t t[3] = {1000, 1501000, 4001000}; /* the work 1.5 ms, then the 3D 2.5 ms */
    memcpy(data, t, count * sizeof *t);
    return count == 3 ? VK_SUCCESS : VK_NOT_READY;
}
static VKAPI_ATTR void VKAPI_CALL checkpoint(VkCommandBuffer cb, const void* marker) {
    (void)cb;
    if (g_checkpoints < 8) g_marked[g_checkpoints] = marker;
    ++g_checkpoints;
}
static VKAPI_ATTR void VKAPI_CALL reached(VkQueue queue, uint32_t* n, VkCheckpointDataNV* data) {
    (void)queue;
    ++g_lost_reads;
    if (data && *n) data[0].pCheckpointMarker = "3d", data[0].stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    *n = 1;
}

int main(void) {
    vkGetPhysicalDeviceQueueFamilyProperties = families;
    vkGetPhysicalDeviceProperties = properties;
    vkCreateQueryPool = pool_make;
    vkDestroyQueryPool = pool_free;
    vkCmdResetQueryPool = reset;
    vkCmdWriteTimestamp = stamp;
    vkGetQueryPoolResults = results;
    vkCmdSetCheckpointNV = checkpoint;
    vkGetQueueCheckpointDataNV = reached;
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)0x50;
    /* neither asked: none */
    CHECK(!snx_vk_marks_new(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0, 0, false, "test", "work"));
    /* the profile without timestamps, checkpoints without the extension: none */
    g_bits = 0;
    CHECK(!snx_vk_marks_new(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 1, 1, false, "test", "work"));
    g_bits = 64;
    /* a submission of two parts, its work and its 3D, each with its checkpoint */
    SnxVkMarks* m = snx_vk_marks_new(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 1, 1, true, "test", "work");
    CHECK(m);
    if (!m) return 1;
    snx_vk_marks_begin(m, cb, 2);
    snx_vk_marks_pass(m, cb, 2, "3d");
    snx_vk_marks_end(m, cb, 2);
    CHECK(g_resets == 1 && g_stamps == 3 && g_checkpoints == 2 && !strcmp(g_marked[0], "work") &&
          !strcmp(g_marked[1], "3d"));
    snx_vk_marks_retire(m, VK_NULL_HANDLE, 2, 5);
    char line[256];
    snx_vk_marks_line(m, 10.0, line, sizeof line);
    CHECK(!strcmp(line, "the GPU's time of its work, the last 10.0 s, 1 submissions: work 1.50 ms, 3d 2.50 ms"));
    /* a lost queue: the checkpoints it reached read back */
    snx_vk_marks_lost(m, VK_NULL_HANDLE);
    CHECK(g_lost_reads == 2);
    snx_vk_marks_free(m, VK_NULL_HANDLE);
    if (!g_failed) puts("vk_marks: ok");
    return g_failed;
}
