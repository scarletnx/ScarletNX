/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presentation every frontend picks for the vsync setting (common/gpu/vk/swapchain.h's snx_vk_present_mode), on the
   mode lists surfaces give: with vsync FIFO; without, mailbox, else immediate, else FIFO all the same */
#include "common/gpu/vk/swapchain.h"
#include <stdio.h>
#include <stdlib.h>

static int checks;
static void expect(const char* what, const VkPresentModeKHR* modes, uint32_t count, bool vsync, VkPresentModeKHR want) {
    VkPresentModeKHR got = snx_vk_present_mode(modes, count, vsync);
    ++checks;
    if (got != want) {
        fprintf(stderr, "%s, vsync=%d: mode %d, expected %d\n", what, vsync, (int)got, (int)want);
        exit(1);
    }
}
int main(void) {
    static const VkPresentModeKHR all[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR,
                                           VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    static const VkPresentModeKHR no_mailbox[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    static const VkPresentModeKHR fifo_only[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    expect("every mode", all, 4, true, VK_PRESENT_MODE_FIFO_KHR);
    expect("every mode", all, 4, false, VK_PRESENT_MODE_MAILBOX_KHR);
    expect("no mailbox", no_mailbox, 2, false, VK_PRESENT_MODE_IMMEDIATE_KHR);
    expect("FIFO alone", fifo_only, 2, false, VK_PRESENT_MODE_FIFO_KHR);
    expect("no list", NULL, 0, false, VK_PRESENT_MODE_FIFO_KHR);
    printf("vk present mode: %d checks\n", checks);
    return 0;
}
