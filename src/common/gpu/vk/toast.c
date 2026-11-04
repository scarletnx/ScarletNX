/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The toast of the frontends with no menu (toast.h): a message in the common 8x16 VGA font, over the frame the
   presenter has just drawn, through the shared text renderer (text.c). */
#include "common/gpu/vk/toast.h"
#include "common/gpu/vk/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOAST_MS   2200u /* how long a message shows */
#define TOAST_TEXT 96    /* the message kept, its NUL included */

struct SnxToast {
    SnxText* text;
    char message[TOAST_TEXT];
    uint64_t until_ns;
};

SnxToast* snx_toast_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                           uint32_t slots) {
    SnxToast* t = (SnxToast*)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->text = snx_text_create(phys, dev, queue, family, cache, slots);
    if (!t->text) {
        free(t);
        return NULL;
    }
    return t;
}

void snx_toast_destroy(SnxToast* t) {
    if (!t) return;
    snx_text_destroy(t->text);
    free(t);
}

void snx_toast_show_ms(SnxToast* t, const char* text, uint64_t now_ns, unsigned ms) {
    if (!t || !text) return;
    snprintf(t->message, sizeof t->message, "%s", text);
    t->until_ns = now_ns + (uint64_t)ms * 1000000u;
}
void snx_toast_show(SnxToast* t, const char* text, uint64_t now_ns) {
    snx_toast_show_ms(t, text, now_ns, TOAST_MS);
}

int snx_toast_active(const SnxToast* t, uint64_t now_ns) { return t && t->message[0] && now_ns < t->until_ns; }

int snx_toast_draw(SnxToast* t, VkCommandBuffer cb, VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                   float white, uint32_t slot, uint64_t now_ns) {
    const char* lines[1];
    unsigned len, scale;
    float x, y;
    if (!snx_toast_active(t, now_ns) || !width || !height) return 0;
    len = (unsigned)strlen(t->message);
    /* two pixels a font pixel where the target is tall enough, one where the message would not fit */
    scale = height >= 400 ? 2u : 1u;
    while (scale > 1 && (len * 8u * scale) + 16u * scale > width) --scale;
    x = ((float)width - (float)(len * 8u * scale)) * 0.5f;
    y = 8.0f * (float)scale;
    lines[0] = t->message;
    return snx_text_draw(t->text, cb, view, format, width, height, white, slot, x, y, (int)scale, lines, 1);
}
