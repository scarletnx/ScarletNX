/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* CPU resource work under vk_lock. These categories exclude time recorded in nested categories of this file.
   They can still include waits or pipeline work recorded by other telemetry; those totals are not additive. */
#ifndef SNX_VK_RESOURCE_STATS_H
#define SNX_VK_RESOURCE_STATS_H
#include "nx/runtime/platform/plat.h"

enum {
    VK_RESOURCE_IMAGE_QUERY,
    VK_RESOURCE_IMAGE_CREATE,
    VK_RESOURCE_IMAGE_DESTROY,
    VK_RESOURCE_SURFACE_CREATE,
    VK_RESOURCE_RT_RELOAD,
    VK_RESOURCE_SURFACE_RECLAIM,
    VK_RESOURCE_BUFFER_MAINTENANCE,
    VK_RESOURCE_BUFFER_CACHE_BUILD,
    VK_RESOURCE_CMD_RESET,
    VK_RESOURCE_N
};
typedef struct {
    u64 calls[VK_RESOURCE_N], ns[VK_RESOURCE_N];
} VkResourceStats;
extern VkResourceStats g_vk_resource_stats;

typedef struct { u64 start, nested; } VkResourceScope;
static inline u64 vk_resource_total(void) {
    u64 total = 0;
    for (u32 i = 0; i < VK_RESOURCE_N; i++) total += g_vk_resource_stats.ns[i];
    return total;
}
static inline VkResourceScope vk_resource_begin(void) {
    VkResourceScope scope = {plat_ns(), vk_resource_total()};
    return scope;
}
static inline void vk_resource_end(u32 kind, VkResourceScope scope) {
    u64 elapsed = plat_ns() - scope.start, nested = vk_resource_total() - scope.nested;
    g_vk_resource_stats.calls[kind]++;
    g_vk_resource_stats.ns[kind] += elapsed > nested ? elapsed - nested : 0;
}
#endif
