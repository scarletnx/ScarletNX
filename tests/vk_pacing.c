/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* frame_pacing in a console's window (common/gpu/vk/pacing.c): each present carries its id, from 1 on each swapchain,
   and with the wait asked it waits first for the one before, until a wait times out; nothing where the device was
   made without the extensions; and the line a run says */
#include "common/gpu/vk/pacing.h"
#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vk_pacing: %s:%d: %s\n", __FILE__, __LINE__, #x);                                         \
            g_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static uint64_t g_waited[8];
static int g_waits;
static VkResult g_answer = VK_SUCCESS;
static VKAPI_ATTR VkResult VKAPI_CALL fake_wait(VkDevice device, VkSwapchainKHR sc, uint64_t id, uint64_t timeout) {
    (void)device, (void)sc;
    if (g_waits < 8) g_waited[g_waits] = id;
    ++g_waits;
    return timeout == 1000000000ull ? g_answer : VK_ERROR_UNKNOWN;
}

static uint64_t presented(const VkPresentInfoKHR* present) {
    const VkPresentIdKHR* id = present->pNext;
    return id && id->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR && id->swapchainCount == 1 ? id->pPresentIds[0] : 0;
}

/* the device's features: present id and present wait */
static VKAPI_ATTR void VKAPI_CALL fake_features(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2* f) {
    (void)phys;
    for (VkBaseOutStructure* s = f->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)
            ((VkPhysicalDevicePresentIdFeaturesKHR*)s)->presentId = VK_TRUE;
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR)
            ((VkPhysicalDevicePresentWaitFeaturesKHR*)s)->presentWait = VK_TRUE;
    }
}
/* a device that has both extensions, made with a swapchain or headless: only the one that presents takes them, as they
   require VK_KHR_swapchain (VUID-vkCreateDevice-ppEnabledExtensionNames-01387) */
static void choose(void) {
    vkGetPhysicalDeviceFeatures2 = fake_features;
    VkExtensionProperties has[3] = {{VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1},
                                    {VK_KHR_PRESENT_ID_EXTENSION_NAME, 1},
                                    {VK_KHR_PRESENT_WAIT_EXTENSION_NAME, 1}};
    SnxVkExtensions list = {has, 3};
    for (int windowed = 0; windowed < 2; ++windowed) {
        SnxVkChoice c = {.list = &list};
        if (windowed) c.extensions[c.extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        SnxVkPacing p;
        int head = 0;
        void* chain = snx_vk_pacing_choose(&p, &c, 1, &head);
        CHECK(p.on == windowed && c.extension_count == (windowed ? 3u : 0u) && (chain == &head) == !windowed);
    }
}

int main(void) {
    choose();
    vkWaitForPresentKHR = fake_wait;
    SnxVkPacing off = {0}, on = {.on = true};
    SnxVkPaced w = {0};
    VkSwapchainKHR a = (VkSwapchainKHR)(uintptr_t)0x10, b = (VkSwapchainKHR)(uintptr_t)0x20;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    /* a device without the extensions: no id, no wait */
    snx_vk_pacing_present(&off, &w, VK_NULL_HANDLE, a, true, &present, "test");
    CHECK(!present.pNext && !g_waits);
    /* ids 1, 2, 3 on one swapchain, each after the wait for the one before */
    for (uint64_t k = 1; k <= 3; ++k) {
        present.pNext = NULL;
        snx_vk_pacing_present(&on, &w, VK_NULL_HANDLE, a, true, &present, "test");
        CHECK(presented(&present) == k);
    }
    CHECK(g_waits == 2 && g_waited[0] == 1 && g_waited[1] == 2);
    /* a new swapchain starts again from 1, with no wait; unasked (vsync off, another pace) no wait either */
    present.pNext = NULL;
    snx_vk_pacing_present(&on, &w, VK_NULL_HANDLE, b, true, &present, "test");
    CHECK(presented(&present) == 1 && g_waits == 2);
    present.pNext = NULL;
    snx_vk_pacing_present(&on, &w, VK_NULL_HANDLE, b, false, &present, "test");
    CHECK(presented(&present) == 2 && g_waits == 2);
    /* a wait that times out stops the window's waits: the next frames go on unwaited */
    g_answer = VK_TIMEOUT;
    present.pNext = NULL;
    snx_vk_pacing_present(&on, &w, VK_NULL_HANDLE, b, true, &present, "test");
    CHECK(presented(&present) == 3 && g_waits == 3 && g_waited[2] == 2 && w.stopped);
    present.pNext = NULL;
    snx_vk_pacing_present(&on, &w, VK_NULL_HANDLE, b, true, &present, "test");
    CHECK(presented(&present) == 4 && g_waits == 3);
    /* what the run says */
    char line[256];
    snx_vk_pacing_said(&on, 1, line, sizeof line);
    CHECK(!strcmp(line, "frame_pacing=1: with vsync each frame waits for the one before to be on the display "
                        "(VK_KHR_present_wait)"));
    snx_vk_pacing_said(&off, 2, line, sizeof line);
    CHECK(strstr(line, "frame_pacing=2: the device has no VK_KHR_present_id and VK_KHR_present_wait") == line);
    snx_vk_pacing_said(&off, 0, line, sizeof line);
    CHECK(!strcmp(line, "frame_pacing=0: the console's timer alone paces the frames"));
    if (!g_failed) puts("vk_pacing: ok");
    return g_failed;
}
