/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Resource CPU intervals must not count nested image queries/allocations a second time in RT reload or reclaim. */
#include "nx/gpu/vk/resource_stats.h"
#include <assert.h>
#include <stdio.h>

VkResourceStats g_vk_resource_stats;
static u64 now;
u64 plat_ns(void) { return now; }

int main(void) {
    VkResourceScope reload = vk_resource_begin();
    now = 10;
    VkResourceScope create = vk_resource_begin();
    now = 12;
    VkResourceScope query = vk_resource_begin();
    now = 15;
    vk_resource_end(VK_RESOURCE_IMAGE_QUERY, query);
    now = 20;
    vk_resource_end(VK_RESOURCE_IMAGE_CREATE, create);
    now = 30;
    vk_resource_end(VK_RESOURCE_RT_RELOAD, reload);
    assert(g_vk_resource_stats.ns[VK_RESOURCE_IMAGE_QUERY] == 3);
    assert(g_vk_resource_stats.ns[VK_RESOURCE_IMAGE_CREATE] == 7);
    assert(g_vk_resource_stats.ns[VK_RESOURCE_RT_RELOAD] == 20);
    assert(vk_resource_total() == 30);
    assert(g_vk_resource_stats.calls[VK_RESOURCE_IMAGE_QUERY] == 1);
    assert(g_vk_resource_stats.calls[VK_RESOURCE_IMAGE_CREATE] == 1);
    assert(g_vk_resource_stats.calls[VK_RESOURCE_RT_RELOAD] == 1);

    /* Reentrant work in the same category is exclusive too. An operation with no elapsed time still counts. */
    VkResourceScope outer = vk_resource_begin();
    now = 32;
    VkResourceScope inner = vk_resource_begin();
    now = 36;
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, inner);
    now = 40;
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, outer);
    assert(g_vk_resource_stats.ns[VK_RESOURCE_SURFACE_RECLAIM] == 10);
    assert(g_vk_resource_stats.calls[VK_RESOURCE_SURFACE_RECLAIM] == 2);
    VkResourceScope empty = vk_resource_begin();
    vk_resource_end(VK_RESOURCE_BUFFER_MAINTENANCE, empty);
    assert(g_vk_resource_stats.calls[VK_RESOURCE_BUFFER_MAINTENANCE] == 1);
    assert(!g_vk_resource_stats.ns[VK_RESOURCE_BUFFER_MAINTENANCE]);
    assert(vk_resource_total() == 40);
    puts("Resource intervals exclude nested work and retain exact CPU durations");
    return 0;
}
