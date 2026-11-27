/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* frame_pacing in a console's window, the Switch's setting: where the device has VK_KHR_present_id and
   VK_KHR_present_wait, a frame's present waits for the one before to be on the display, so frames do not queue for
   it. 1 and 2 alike: a console's frame is one refresh, which 2's hold has nothing to even out; 0 leaves the frames to
   the console's timer alone */
#ifndef SNX_COMMON_VK_PACING_H
#define SNX_COMMON_VK_PACING_H
#include "common/gpu/vk/device.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the device's: both extensions' features, chained into the device made */
typedef struct {
    VkPhysicalDevicePresentIdFeaturesKHR id;
    VkPhysicalDevicePresentWaitFeaturesKHR wait;
    bool on; /* frame_pacing asked for it and the device is made with both extensions */
} SnxVkPacing;
/* a window's: its presents' ids, from 1 on each swapchain */
typedef struct {
    VkPresentIdKHR present;
    uint64_t id;       /* the last present's */
    VkSwapchainKHR sc; /* the swapchain those ids are of */
    bool stopped;      /* a wait failed or timed out: the window's frames go on unwaited, said once */
} SnxVkPaced;

/* In a renderer's choose, before snx_vk_choice_create: where frame_pacing (pacing) asks and the device has both
   extensions and their features, both enabled and their features ahead of next. The chain's head to hand on (next
   where the device has them not) */
void* snx_vk_pacing_choose(SnxVkPacing* p, SnxVkChoice* c, int pacing, void* next);
/* A present of sc, a window's whose ids w keeps: its id ahead of present's pNext, after a wait of a second at most for the one before to be shown
   where wait (vsync on, at the console's pace). Nothing where the device was made without them; a wait that fails or
   times out stops the window's waits, said once on stderr after who */
void snx_vk_pacing_present(const SnxVkPacing* p, SnxVkPaced* w, VkDevice device, VkSwapchainKHR sc, bool wait,
                           VkPresentInfoKHR* present, const char* who);
/* what a run says of it at start, with the value asked: "frame_pacing=1: each frame waits for the one before to be on
   the display (VK_KHR_present_wait)", or that the device has not the extensions */
void snx_vk_pacing_said(const SnxVkPacing* p, int pacing, char* out, size_t size);
#endif
