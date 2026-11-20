/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether a device binds the vertex and fragment programs' constant buffers through windows of the staging ring
   (ShCaps.cb_windows, render.c's cb_window_place), and the line that says so or why not: the device's value against
   what the windows need */
#ifndef SNX_VK_CB_WINDOWS_H
#define SNX_VK_CB_WINDOWS_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* a window's reach, which a uniform range must take whole */
#define SNX_VK_CB_WINDOW_BYTES 65536u

typedef struct {
    uint32_t push_bytes;    /* maxPushConstantsSize */
    uint32_t uniform_range; /* maxUniformBufferRange */
    uint32_t uniform_cbufs; /* the constant buffers bound as uniform buffers (g_spirv_translate.uniform_cbufs) */
    int heaps;              /* descriptor heaps hold the sets */
    /* the extension bans' line on maxPushConstantsSize that holds for the driver, as vk_ban_why says it; NULL for
       none */
    const char* ban;
} SnxVkCbWindowDevice;

/* vk_cb_windows: what the setting asks for */
#define SNX_VK_CB_WINDOWS_OFF  0
#define SNX_VK_CB_WINDOWS_ON   1 /* over a ban too */
#define SNX_VK_CB_WINDOWS_AUTO 2

/* 1 where the windows are on, their offsets in push constants up to window_push (SH_PUSH_BYTES_WINDOWS), as asked
   (SNX_VK_CB_WINDOWS_*); out holds the line either way */
static inline int snx_vk_cb_windows_say(char* out, size_t n, const SnxVkCbWindowDevice* d, uint32_t window_push,
                                        int asked) {
    char why[1400] = "";
    int over_ban = 0;
    if (d->push_bytes < window_push)
        snprintf(why, sizeof why, "the GPU pushes %u bytes of constants, and the windows' offsets end at byte %u",
                 (unsigned)d->push_bytes, (unsigned)window_push);
    else if (d->uniform_range < SNX_VK_CB_WINDOW_BYTES)
        snprintf(why, sizeof why, "the GPU binds uniform ranges of %u bytes, and a window takes %u",
                 (unsigned)d->uniform_range, SNX_VK_CB_WINDOW_BYTES);
    else if (!d->uniform_cbufs)
        snprintf(why, sizeof why, "none of the 18 is bound as a uniform buffer");
    else if (d->heaps)
        snprintf(
            why, sizeof why,
            "descriptor heaps push their sets' addresses from byte 128 (VK_DESCRIPTOR_HEAP_PUSH_OFFSET), where the "
            "windows' offsets go");
    else if (asked == SNX_VK_CB_WINDOWS_OFF)
        snprintf(why, sizeof why, "vk_cb_windows=0 (Off)");
    else if (d->ban && asked == SNX_VK_CB_WINDOWS_ON)
        over_ban = 1;
    else if (d->ban)
        snprintf(why, sizeof why, "%s", d->ban);
    if (!n) return !why[0];
    if (over_ban)
        snprintf(out, n,
                 "vulkan: constant buffers bound as windows of the staging ring, their offsets pushed, as "
                 "vk_cb_windows=1 (On) asks over the ban: %s",
                 d->ban);
    else if (why[0] && asked == SNX_VK_CB_WINDOWS_ON)
        snprintf(out, n, "vulkan: constant buffers bound one by one, where vk_cb_windows=1 (On) asks for windows: %s",
                 why);
    else
        snprintf(out, n, "vulkan: %s%s",
                 why[0] ? "constant buffers bound one by one: "
                        : "constant buffers bound as windows of the staging ring, their offsets pushed",
                 why);
    return !why[0];
}

#endif
