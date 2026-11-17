/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What shading_rate.c decides with no device call: the extension it takes, and the conversion's push constants
   (shading_rate.comp). A test links this file alone (tests/vk_shading_rate.c) */
#include <stdio.h>
#include "nx/gpu/vk/shading_rate.h"

#define GAME_TEXEL 16u /* the console's texel, in pixels */

int vk_vrs_choose(const VkVrsDevice* d, VkExtent2D* texel, char* line, size_t n) {
    *texel = (VkExtent2D){0, 0};
    if (!d->ounce) {
        snprintf(line, n, "vulkan: no shading rate images: the game is no Ounce program");
        return VK_VRS_NONE;
    }
    /* the game's palettes have 16 entries, which vk_vrs_draw sets as they are */
    if (d->nv && d->nv_palette >= 16 && d->nv_texel.width && d->nv_texel.height) {
        *texel = d->nv_texel;
        snprintf(line, n,
                 "vulkan: an Ounce's shading rate images through VK_NV_shading_rate_image (texels of %ux%u, palettes "
                 "of %u entries)",
                 texel->width, texel->height, d->nv_palette);
        return VK_VRS_NV;
    }
    if (d->khr && d->khr_min.width && d->khr_min.height) {
        /* the console's 16x16 where the device takes it, else the nearest size: both powers of two in the range */
        u32 w = GAME_TEXEL, h = GAME_TEXEL;
        if (w < d->khr_min.width) w = d->khr_min.width;
        if (w > d->khr_max.width) w = d->khr_max.width;
        if (h < d->khr_min.height) h = d->khr_min.height;
        if (h > d->khr_max.height) h = d->khr_max.height;
        if (w && h && !(w & (w - 1)) && !(h & (h - 1)) &&
            (!d->khr_aspect || (w >= h ? w / h : h / w) <= d->khr_aspect)) {
            *texel = (VkExtent2D){w, h};
            snprintf(line, n,
                     "vulkan: an Ounce's shading rate images through the attachment of VK_KHR_fragment_shading_rate "
                     "(texels of %ux%u, viewport 0's palette only)",
                     w, h);
            return VK_VRS_KHR;
        }
    }
    snprintf(line, n,
             "vulkan: no VK_NV_shading_rate_image or attachment of VK_KHR_fragment_shading_rate: an Ounce's shading "
             "rate images are shaded at the full rate");
    return VK_VRS_NONE;
}

u32 vk_vrs_game_texel(u32 texel, float scale, u32 w, u32 sw) {
    u32 t = sw ? (u32)((float)texel * scale * (float)w / (float)sw + 0.5f) : 0;
    return t ? t : 1;
}

int vk_vrs_reuse(int vrs, int new_image, int valid, u64 palette0_was, u64 palette0) {
    if (new_image || !valid) return VK_VRS_CONVERT;
    /* VK_NV_shading_rate_image reads the palettes at each draw: a new palette alone needs no new image */
    if (vrs == VK_VRS_NV) return VK_VRS_NEW_ID;
    /* the KHR image holds viewport 0's rates: the game sets each viewport's palette in each frame (five in one game),
       and the others change nothing in it */
    return palette0 == palette0_was ? VK_VRS_SAME_ID : VK_VRS_CONVERT;
}

u32 vk_vrs_khr_rate(u32 e) {
    switch (e) {
        case 0: return 2u << 2 | 2u;  /* no invocations: the coarsest rate that all devices give (4x4) */
        case 6: return 1u << 2;       /* one invocation for 2x1 pixels */
        case 7: return 1u;            /* 1x2 */
        case 8: return 1u << 2 | 1u;  /* 2x2 */
        case 9: return 2u << 2 | 1u;  /* 4x2 */
        case 10: return 1u << 2 | 2u; /* 2x4 */
        case 11: return 2u << 2 | 2u; /* 4x4 */
        default: return 0;            /* one or more invocations for each pixel */
    }
}

void vk_vrs_push(int vrs, VkExtent2D texel, u32 game_w, u32 game_h, u32 game_texel, u64 palette0, VkVrsPush* o) {
    *o = (VkVrsPush){0};
    /* the pixels that the game's image covers, in texels of the device's */
    o->size_x = (u32)(((u64)game_w * game_texel + texel.width - 1) / texel.width);
    o->size_y = (u32)(((u64)game_h * game_texel + texel.height - 1) / texel.height);
    if (!o->size_x) o->size_x = 1;
    if (!o->size_y) o->size_y = 1;
    o->game_w = game_w, o->game_h = game_h;
    o->texel_w = texel.width, o->texel_h = texel.height;
    o->game_texel = game_texel;
    o->finest = vrs == VK_VRS_KHR;
    /* the index itself for VK_NV_shading_rate_image, whose palettes the draws set; viewport 0's rate as
       VK_KHR_fragment_shading_rate encodes it */
    for (u32 i = 0; i < 16; i++) {
        u32 v = vrs == VK_VRS_NV ? i : vk_vrs_khr_rate((u32)(palette0 >> (4 * i)) & 15u);
        o->table[i / 4] |= v << (8 * (i % 4));
    }
}
