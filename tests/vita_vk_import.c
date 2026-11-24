/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's device (gpu/vk.c) where not every device imports guest memory (VK_EXT_external_memory_host): the first
   that imports is taken before one ahead of it that does not, and one that does not when there is none. The device is
   listed twice and the extension hidden from the first, then from both, at link time (--wrap). Where the host lists a
   GPU before a CPU, the GPU without the extension is taken before the CPU with it. Skips (77) without a device that
   has the extension */
#include "vita/gpu/vk.h"
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    unsigned lists;   /* the extension lists asked for so far, each device's in turn */
    unsigned devices; /* the devices listed */
    unsigned hide;    /* the first devices the extension is hidden from */
    bool twice;       /* the first device listed twice, and no other */
    bool cpu_after;   /* the host lists a CPU after a first device that is none */
} g_test;
VkPhysicalDevice* __real_snx_vk_physical_devices(VkInstance instance, uint32_t* count);
VkPhysicalDevice* __wrap_snx_vk_physical_devices(VkInstance instance, uint32_t* count);
VkPhysicalDevice* __wrap_snx_vk_physical_devices(VkInstance instance, uint32_t* count) {
    VkPhysicalDevice* devices = __real_snx_vk_physical_devices(instance, count);
    for (uint32_t i = 0; i < *count; ++i) {
        VkPhysicalDeviceProperties of;
        vkGetPhysicalDeviceProperties(devices[i], &of);
        if (!i) g_test.cpu_after = false;
        if (!i && of.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) break;
        if (of.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) g_test.cpu_after = true;
    }
    g_test.devices = *count, g_test.lists = 0;
    VkPhysicalDevice* twice = devices && g_test.twice ? calloc(2, sizeof *twice) : NULL;
    if (!twice) return devices;
    g_test.devices = 2;
    twice[0] = twice[1] = devices[0];
    *count = 2;
    free(devices);
    return twice;
}
bool __real_snx_vk_device_extensions(VkPhysicalDevice phys, SnxVkExtensions* out);
bool __wrap_snx_vk_device_extensions(VkPhysicalDevice phys, SnxVkExtensions* out);
bool __wrap_snx_vk_device_extensions(VkPhysicalDevice phys, SnxVkExtensions* out) {
    bool listed = __real_snx_vk_device_extensions(phys, out);
    for (uint32_t i = 0; listed && g_test.lists % g_test.devices < g_test.hide && i < out->count; ++i)
        if (!strcmp(out->list[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
            out->list[i] = out->list[--out->count];
            break;
        }
    ++g_test.lists;
    return listed;
}
/* the device made with the extension hidden from the first hide devices: whether it imports, -1 for none */
static int imports(bool twice, unsigned hide) {
    char why[512];
    g_test.hide = hide, g_test.twice = twice;
    if (!vita_vk_init(NULL, why, sizeof why)) {
        printf("no Vulkan device (%s)\n", why);
        return -1;
    }
    int imported = vita_vk()->host_import;
    vita_vk_free();
    return imported;
}

int main(void) {
    if (imports(true, 0) < 1) {
        puts("SKIP no device imports guest memory");
        return 77;
    }
    int second = imports(true, 1), none = imports(true, UINT32_MAX), gpu = g_test.cpu_after ? imports(false, 1) : 0;
    if (second != 1) fprintf(stderr, "FAIL the first device was taken, which does not import, before one that does\n");
    if (none) fprintf(stderr, "FAIL where none imports, %s\n", none < 0 ? "there is no device" : "one is said to");
    if (gpu) fprintf(stderr, "FAIL a CPU that imports was taken before a GPU that does not\n");
    if (second != 1 || none || gpu) return 1;
    printf("PASS a device that imports guest memory comes first, and one that does not is taken where none does%s\n",
           g_test.cpu_after ? ", and before a CPU" : "");
    return 0;
}
