/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A short message over the presented frame, for the frontends that have no menu of their own: the 3DS, the Vita and
   the GBA. It is drawn as rectangles of the common 8x16 VGA font (utilities/font.c), in the target the caller has just
   drawn, so a hotkey's choice shows where the game is. The Switch's own overlay (runtime/platform/overlay.c) keeps its
   richer toasts. */
#ifndef SNX_COMMON_GPU_VK_TOAST_H
#define SNX_COMMON_GPU_VK_TOAST_H
#include <stdint.h>
#include <volk.h>

typedef struct SnxToast SnxToast;
/* one per device, beside the effects */
SnxToast* snx_toast_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                           uint32_t slots);
void snx_toast_destroy(SnxToast* toast);
/* the message, shown from now_ns for a couple of seconds */
void snx_toast_show(SnxToast* toast, const char* text, uint64_t now_ns);
int snx_toast_active(const SnxToast* toast, uint64_t now_ns);
/* Draw the message over view, which is in COLOR_ATTACHMENT_OPTIMAL and comes back so. white is the text's value in
   the target's space (1.0 for an SDR or scRGB image, the paper white in nits for a linear composition), and the box
   behind it is black at three quarters. slot is the caller's recording slot: its buffer and the pipeline of the target
   format are kept until the next use of that slot, so two windows can draw before the first is submitted. 0 when
   nothing shows or the pipeline could not be made. */
int snx_toast_draw(SnxToast* toast, VkCommandBuffer cb, VkImageView view, VkFormat format, uint32_t width,
                   uint32_t height, float white, uint32_t slot, uint64_t now_ns);
#endif
