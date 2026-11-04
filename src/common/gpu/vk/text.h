/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Text over the presented frame, for the frontends that have no menu of their own: the 3DS, the Vita and the GBA. It is
   drawn as rectangles of the common 8x16 VGA font (utilities/font.c) in the target the caller has just drawn, so a
   hotkey's choice or the frame rate shows where the game is. A toast (toast.h) and the stats overlay (stats.h) both
   draw through it, each with a renderer of its own so their vertices stay apart. */
#ifndef SNX_COMMON_GPU_VK_TEXT_H
#define SNX_COMMON_GPU_VK_TEXT_H
#include <stdint.h>
#include <volk.h>

typedef struct SnxText SnxText;
/* one per device, beside the effects: slots vertex buffers, one a recording slot (the frontend's ring slot), so two
   windows or two renderers can draw before the first submission is done */
SnxText* snx_text_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                         uint32_t slots);
void snx_text_destroy(SnxText* text);
/* Draws lines one under the other, the top left of the first at x and y in pixels, a font pixel a scale square, with a
   black box at three quarters behind them. view is in COLOR_ATTACHMENT_OPTIMAL and comes back so. white is the
   letters' value in the target's space (1.0 for an SDR or scRGB image, the paper white in nits for a linear
   composition). slot is the caller's recording slot: its buffer and the pipeline of the target format are kept until
   that slot is used again. A line wider than the target is cut by the target's edge. 0 when nothing was drawn or the
   pipeline could not be made. */
int snx_text_draw(SnxText* text, VkCommandBuffer cb, VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                  float white, uint32_t slot, float x, float y, int scale, const char* const* lines, unsigned count);
#endif
