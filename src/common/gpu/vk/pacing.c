/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/pacing.h"
#include <stdio.h>

void* snx_vk_pacing_choose(SnxVkPacing* p, SnxVkChoice* c, int pacing, void* next) {
    *p = (SnxVkPacing){0}; /* asked anew of each device choose is called for */
    if (pacing <= 0 || !snx_vk_choice_has(c, VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
        !snx_vk_choice_has(c, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
        return next;
    VkPhysicalDevicePresentIdFeaturesKHR id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR wait = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                   .pNext = &id};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &wait};
    vkGetPhysicalDeviceFeatures2(c->phys, &features);
    if (!id.presentId || !wait.presentWait || !snx_vk_choice_enable(c, VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
        !snx_vk_choice_enable(c, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
        return next;
    p->id = (VkPhysicalDevicePresentIdFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = next, .presentId = VK_TRUE};
    p->wait = (VkPhysicalDevicePresentWaitFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .pNext = &p->id, .presentWait = VK_TRUE};
    p->on = true;
    return &p->wait;
}

void snx_vk_pacing_present(const SnxVkPacing* p, SnxVkPaced* w, VkDevice device, VkSwapchainKHR sc, bool wait,
                           VkPresentInfoKHR* present, const char* who) {
    if (!p->on) return;
    if (sc != w->sc) { /* a new swapchain's ids start again */
        w->sc = sc;
        w->id = 0;
    }
    if (wait && w->id && !w->stopped) {
        /* a display that never says a frame was shown would hold every frame a second: the window stops waiting */
        VkResult r = vkWaitForPresentKHR(device, sc, w->id, 1000000000ull);
        if (r != VK_SUCCESS) {
            w->stopped = true;
            fprintf(stderr, "%s: frame_pacing: the wait for present %llu %s: the window's frames go on unwaited\n", who,
                    (unsigned long long)w->id, r == VK_TIMEOUT ? "took over a second" : "failed");
        }
    }
    ++w->id;
    w->present = (VkPresentIdKHR){
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = present->pNext, .swapchainCount = 1, .pPresentIds = &w->id};
    present->pNext = &w->present;
}

void snx_vk_pacing_said(const SnxVkPacing* p, int pacing, char* out, size_t size) {
    if (pacing <= 0)
        snprintf(out, size, "frame_pacing=%d: the console's timer alone paces the frames", pacing);
    else if (p->on)
        snprintf(out, size,
                 "frame_pacing=%d: with vsync each frame waits for the one before to be on the display "
                 "(VK_KHR_present_wait)",
                 pacing);
    else
        snprintf(out, size,
                 "frame_pacing=%d: the device has no VK_KHR_present_id and VK_KHR_present_wait: the console's timer "
                 "alone paces the frames",
                 pacing);
}
