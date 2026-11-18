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
#define SNX_VK_CB_WINDOW_BYTES       65536u
/* lavapipe hands its linear rasterizer the push constants of every stage a draw binds as one buffer, as many bytes as
   the stages' ranges add up to, and the rasterizer (Mesa's lp_fs_linear_run) copies a byte of each of its floats into
   a stack array of LP_MAX_LINEAR_CONSTANTS * 4: 256 bytes of push constants, its own maxPushConstantsSize */
#define SNX_VK_LAVAPIPE_LINEAR_BYTES 256u

typedef struct {
    uint32_t push_bytes;    /* maxPushConstantsSize */
    uint32_t uniform_range; /* maxUniformBufferRange */
    uint32_t uniform_cbufs; /* the constant buffers bound as uniform buffers (g_spirv_translate.uniform_cbufs) */
    int heaps;              /* descriptor heaps hold the sets */
    int lavapipe;           /* the driver is lavapipe (VK_DRIVER_ID_MESA_LLVMPIPE) */
} SnxVkCbWindowDevice;

/* 1 where the windows are on, their offsets in push constants up to window_push (SH_PUSH_BYTES_WINDOWS); out holds
   the line either way */
static inline int snx_vk_cb_windows_say(char* out, size_t n, const SnxVkCbWindowDevice* d, uint32_t window_push) {
    char why[400] = "";
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
    else if (d->lavapipe)
        snprintf(why, sizeof why,
                 "lavapipe hands its linear rasterizer the push constants of every stage a draw binds as one buffer, "
                 "%u bytes a stage with windows, and the rasterizer copies them into a stack array that holds %u "
                 "bytes' worth: a vertex and a fragment program's %u smash its stack (issue #2461)",
                 (unsigned)window_push, SNX_VK_LAVAPIPE_LINEAR_BYTES, (unsigned)(2 * window_push));
    if (n)
        snprintf(out, n, "vulkan: %s%s",
                 why[0] ? "constant buffers bound one by one: "
                        : "constant buffers bound as windows of the staging ring, their offsets pushed",
                 why);
    return !why[0];
}

#endif
