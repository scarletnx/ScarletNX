/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where a device's memory is (common/gpu/vk/vk_common.h's snx_vk_memory_model), which the Switch's and the 3DS's
   devices choose their path for guest memory by, on the heaps and types drivers give: a discrete GPU with a 256 MiB
   window and with resizable BAR (RADV on an RX 6700 XT), an integrated GPU, a CPU's (lavapipe), and GPUs that are
   neither, which share the system's memory only when the CPU maps their biggest heap and it holds most of it. */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <stdlib.h>

#define MIB (1ull << 20)
#define GIB (1ull << 30)
enum { LOCAL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VISIBLE = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT };

static struct {
    int failures, devices;
} g_model;
/* heaps sizes[i] (device-local where local has bit i), and a type for each of types' pairs {heap, properties} */
static VkPhysicalDeviceMemoryProperties memory(unsigned heaps, const uint64_t* sizes, unsigned local, unsigned count,
                                               const unsigned (*types)[2]) {
    VkPhysicalDeviceMemoryProperties m = {0};
    m.memoryHeapCount = heaps;
    for (unsigned h = 0; h < heaps; ++h)
        m.memoryHeaps[h] = (VkMemoryHeap){sizes[h], local >> h & 1 ? VK_MEMORY_HEAP_DEVICE_LOCAL_BIT : 0};
    m.memoryTypeCount = count;
    for (unsigned t = 0; t < count; ++t) m.memoryTypes[t] = (VkMemoryType){types[t][1], types[t][0]};
    return m;
}
static void expect(const char* what, VkPhysicalDeviceType type, const VkPhysicalDeviceMemoryProperties* m,
                   SnxVkMemoryModel want) {
    static const char* const names[] = {"SMALL_BAR", "REBAR", "UMA"};
    SnxVkMemoryModel got = snx_vk_memory_model(type, m);
    ++g_model.devices;
    if (got != want) {
        fprintf(stderr, "%s: %s, %s expected\n", what, names[got], names[want]);
        ++g_model.failures;
    }
}
int main(void) {
    /* RADV, an RX 6700 XT without resizable BAR: VRAM less the window, the system's memory, the 256 MiB window */
    const uint64_t radv[3] = {12 * GIB - 256 * MIB, 16 * GIB, 256 * MIB};
    const unsigned radv_types[4][2] = {{0, LOCAL}, {1, VISIBLE}, {2, LOCAL | VISIBLE}, {1, VISIBLE}};
    VkPhysicalDeviceMemoryProperties m = memory(3, radv, 5, 4, radv_types);
    expect("a discrete GPU with a 256 MiB window", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, &m, SNX_VK_SMALL_BAR);
    /* the same with resizable BAR: the whole VRAM is the CPU's to map */
    const uint64_t rebar[2] = {12 * GIB, 16 * GIB};
    const unsigned rebar_types[3][2] = {{0, LOCAL}, {1, VISIBLE}, {0, LOCAL | VISIBLE}};
    m = memory(2, rebar, 1, 3, rebar_types);
    expect("a discrete GPU with resizable BAR", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, &m, SNX_VK_REBAR);
    /* a discrete GPU with more memory of its own than the system's, all of it mapped, is still discrete */
    const uint64_t big[2] = {24 * GIB, 8 * GIB};
    m = memory(2, big, 1, 3, rebar_types);
    expect("a discrete GPU holding most of the memory", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, &m, SNX_VK_REBAR);
    /* an integrated GPU, whatever its heaps say, and a CPU's (lavapipe: one heap, mapped, device-local) */
    m = memory(3, radv, 5, 4, radv_types);
    expect("an integrated GPU", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, &m, SNX_VK_UMA);
    const uint64_t one[1] = {16 * GIB};
    const unsigned one_type[1][2] = {{0, LOCAL | VISIBLE}};
    m = memory(1, one, 1, 1, one_type);
    expect("a CPU's", VK_PHYSICAL_DEVICE_TYPE_CPU, &m, SNX_VK_UMA);
    /* neither: a virtual GPU whose one mapped heap is the memory shares it; one with a small heap of its own does not,
       nor one with no device-local heap */
    expect("a virtual GPU on the system's memory", VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU, &m, SNX_VK_UMA);
    m = memory(3, radv, 5, 4, radv_types);
    expect("a virtual GPU with a window", VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU, &m, SNX_VK_SMALL_BAR);
    const unsigned host_type[1][2] = {{0, VISIBLE}};
    m = memory(1, one, 0, 1, host_type);
    expect("another device with no device-local heap", VK_PHYSICAL_DEVICE_TYPE_OTHER, &m, SNX_VK_SMALL_BAR);
    if (g_model.failures) return 1;
    printf("vk_memory_model: %d devices\n", g_model.devices);
    return 0;
}
