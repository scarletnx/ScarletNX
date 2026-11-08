/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_SAMPLE_LOCATIONS_H
#define SNX_VK_SAMPLE_LOCATIONS_H
#include "nx/gpu/draw_state.h"
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include <string.h>

typedef struct {
    u32 available, counts, subpixel_bits, variable;
    float coordinate_min, coordinate_max;
    VkExtent2D grid[5]; /* counts1,2,4,8,16, queried with vkGetPhysicalDeviceMultisamplePropertiesEXT */
} VkSampleLocationCaps;
typedef struct {
    u32 samples, width, height;
    u8 location[16]; /* x low nibble, y high nibble, in sixteenths of a pixel */
} VkSampleLocations;

static inline int vk_sample_locations_key(VkSampleLocations* key, const DrawCall* call, u32 samples,
                                           const VkSampleLocationCaps* caps, int lower_left, u32 height) {
    memset(key, 0, sizeof *key);
    if (!(call->multisample_flags & DS_MS_SAMPLE_LOCATIONS)) return 1;
    if (!caps->available || !caps->variable || !samples || samples > 16 || (samples & (samples - 1)) || !(caps->counts & samples) ||
        caps->subpixel_bits < 4)
        return 0;
    u32 log2 = 0;
    while ((1u << log2) < samples) log2++;
    u32 width = 1, rows = 1;
    if (call->multisample_flags & DS_MS_SAMPLE_GRID) {
        width = samples < 4 ? (samples == 1 ? 4 : 2) : samples == 4 ? 2 : 1;
        rows = 16 / (samples * width);
    }
    u8 locations[16] = {0};
    for (u32 y = 0; y < rows; y++)
        for (u32 x = 0; x < width; x++)
            for (u32 sample = 0; sample < samples; sample++) {
                /* The published sample-location extensions index pixel*N+sample; the API prose omits N. */
                u32 source_y = lower_left ? (height - 1 - y) & (rows - 1) : y;
                u8 value = call->sample_locations[(source_y * width + x) * samples + sample];
                u32 sx = value & 15, sy = value >> 4;
                if (lower_left) sy = 16 - sy;
                if (sy > 15) return 0;
                if ((float)sx / 16 < caps->coordinate_min || (float)sx / 16 > caps->coordinate_max ||
                    (float)sy / 16 < caps->coordinate_min || (float)sy / 16 > caps->coordinate_max)
                    return 0;
                locations[(y * width + x) * samples + sample] = (u8)(sx | sy << 4);
            }
    /* A repeated pattern has a smaller exact grid, including a uniform grid on devices with a1x1 limit. */
    u32 exact_width = width, exact_rows = rows;
    for (u32 candidate = 1; candidate < width; candidate <<= 1) {
        int same = 1;
        for (u32 y = 0; y < rows; y++)
            for (u32 x = 0; x < width; x++)
                if (memcmp(locations + (y * width + x) * samples,
                           locations + (y * width + x % candidate) * samples, samples))
                    same = 0;
        if (same) { exact_width = candidate; break; }
    }
    for (u32 candidate = 1; candidate < rows; candidate <<= 1) {
        int same = 1;
        for (u32 y = 0; y < rows; y++)
            if (memcmp(locations + y * width * samples, locations + (y % candidate) * width * samples,
                       width * samples))
                same = 0;
        if (same) { exact_rows = candidate; break; }
    }
    if (exact_width > caps->grid[log2].width || exact_rows > caps->grid[log2].height) return 0;
    key->samples = samples;
    key->width = exact_width;
    key->height = exact_rows;
    for (u32 y = 0; y < exact_rows; y++)
        memcpy(key->location + y * exact_width * samples, locations + y * width * samples, exact_width * samples);
    return 1;
}
/* Vulkan's standard sample positions, in sample-index order, in sixteenths of one pixel. */
static inline VkSampleLocations vk_sample_locations_standard(u32 samples) {
    static const u8 positions[5][16] = {
        {0x88}, {0xcc, 0x44}, {0x26, 0x6e, 0xa2, 0xea},
        {0x59, 0xb7, 0x9d, 0x35, 0xd3, 0x71, 0xfb, 0x1f},
        {0x99, 0x57, 0xa5, 0x7c, 0x63, 0xda, 0xbd, 0x3b, 0xe6, 0x18, 0x24, 0xc2, 0x80, 0x4f, 0xfe, 0x01}};
    VkSampleLocations result = {0};
    if (!samples || samples > 16 || (samples & (samples - 1))) return result;
    u32 index = 0;
    while ((1u << index) < samples) index++;
    result.samples = samples;
    result.width = result.height = 1;
    memcpy(result.location, positions[index], samples);
    return result;
}
static inline VkSampleLocationsInfoEXT vk_sample_locations_info(const VkSampleLocations* key,
                                                                 VkSampleLocationEXT output[16]) {
    u32 count = key->samples * key->width * key->height;
    for (u32 i = 0; i < count; i++)
        output[i] = (VkSampleLocationEXT){(float)(key->location[i] & 15) / 16, (float)(key->location[i] >> 4) / 16};
    return (VkSampleLocationsInfoEXT){.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
                                      .sampleLocationsPerPixel = (VkSampleCountFlagBits)key->samples,
                                      .sampleLocationGridSize = {key->width, key->height},
                                      .sampleLocationsCount = count,
                                      .pSampleLocations = output};
}
/* The sample locations a layout transition of a depth image made SAMPLE_LOCATIONS_COMPATIBLE names in its barrier's
   pNext (into out and pos), the ones key holds: NULL for none or for the image's standard ones (samples a pixel), which
   a transition that names none takes. Recording names nothing there that it can leave out: a barrier with a pNext
   chain waits for the recording thread to catch up (record.c barrier_chained) */
static inline const VkSampleLocationsInfoEXT* vk_sample_locations_barrier(const VkSampleLocations* key, u32 samples,
                                                                          VkSampleLocationsInfoEXT* out,
                                                                          VkSampleLocationEXT pos[16]) {
    VkSampleLocations standard = vk_sample_locations_standard(samples);
    if (!key->samples || !memcmp(key, &standard, sizeof standard)) return NULL;
    *out = vk_sample_locations_info(key, pos);
    return out;
}
#endif
