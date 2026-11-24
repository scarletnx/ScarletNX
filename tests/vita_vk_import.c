/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's device (gpu/vk.c) where not every device imports guest memory (VK_EXT_external_memory_host): the first
   that imports is taken before one ahead of it that does not, and one that does not when there is none. The device is
   listed twice and the extension hidden from the first, then from both, in the choice of the program (device.c), which
   this test compiles in with the two calls that list them renamed: a symbol wrapped at link time is undone by a linker
   that inlines it across files (SNX_LTO). Where the host lists a GPU before a CPU, the GPU without the extension is
   taken before the CPU with it. Skips (77) without a device that has the extension */
#include "vita/gpu/vk.h"
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    unsigned lists;   /* the extension lists asked for so far, each device's in turn */
    unsigned devices; /* the devices listed */
    unsigned hide;    /* the first devices the extension is hidden from */
    unsigned hidden;  /* how many lists it was taken out of */
    bool twice;       /* the first device listed twice, and no other */
    bool cpu_after;   /* the host lists a CPU after a first device that is none */
} g_test;
static VkPhysicalDevice* listed_devices(VkInstance instance, uint32_t* count) {
    VkPhysicalDevice* devices = snx_vk_physical_devices(instance, count);
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
static bool listed_extensions(VkPhysicalDevice phys, SnxVkExtensions* out) {
    bool listed = snx_vk_device_extensions(phys, out);
    for (uint32_t i = 0; listed && g_test.lists % g_test.devices < g_test.hide && i < out->count; ++i)
        if (!strcmp(out->list[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
            out->list[i] = out->list[--out->count];
            ++g_test.hidden;
            break;
        }
    ++g_test.lists;
    return listed;
}
#define snx_vk_physical_devices  listed_devices
#define snx_vk_device_extensions listed_extensions
#include "common/gpu/vk/device.c"
#undef snx_vk_physical_devices
#undef snx_vk_device_extensions
/* the device made with the extension hidden from the first hide devices: whether it imports, -1 for none */
static int imports(bool twice, unsigned hide) {
    char why[512];
    g_test.hide = hide, g_test.twice = twice, g_test.hidden = 0;
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
    int second = imports(true, 1);
    unsigned hidden_once = g_test.hidden;
    int none = imports(true, UINT32_MAX);
    unsigned hidden_all = g_test.hidden;
    int gpu = g_test.cpu_after ? imports(false, 1) : 0;
    unsigned hidden_gpu = g_test.cpu_after ? g_test.hidden : 1;
    /* the checks below pass on a device that was asked nothing, so none of them counts without this */
    if (!hidden_once || !hidden_all || !hidden_gpu) {
        fprintf(stderr, "FAIL the extension was hidden %u, %u and %u times: no device was chosen from these lists\n",
                hidden_once, hidden_all, hidden_gpu);
        return 1;
    }
    if (second != 1) fprintf(stderr, "FAIL the first device was taken, which does not import, before one that does\n");
    if (none) fprintf(stderr, "FAIL where none imports, %s\n", none < 0 ? "there is no device" : "one is said to");
    if (gpu) fprintf(stderr, "FAIL a CPU that imports was taken before a GPU that does not\n");
    if (second != 1 || none || gpu) return 1;
    printf("PASS a device that imports guest memory comes first, and one that does not is taken where none does%s\n",
           g_test.cpu_after ? ", and before a CPU" : "");
    return 0;
}
