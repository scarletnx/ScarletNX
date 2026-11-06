/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* CPU checks of sample-location keys. No Vulkan device is required. */
#include "nx/gpu/vk/sample_locations.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int pass, const char* text) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", text);
}
static VkSampleLocationCaps full_caps(void) {
    return (VkSampleLocationCaps){.available = 1, .counts = 31, .subpixel_bits = 4, .variable = 1,
                                  .coordinate_min = 0, .coordinate_max = 15.0f / 16,
                                  .grid = {{4, 4}, {2, 4}, {2, 2}, {1, 2}, {1, 1}}};
}
static void check_zero(const VkSampleLocations* key, const char* text) {
    const VkSampleLocations zero = {0};
    check(!memcmp(key, &zero, sizeof zero), text);
}
static void test_disabled_and_gates(void) {
    DrawCall call = {0};
    VkSampleLocationCaps caps = {0};
    VkSampleLocations key;
    memset(&key, 0xa5, sizeof key);
    check(vk_sample_locations_key(&key, &call, 0, &caps, 0, 0), "disabled state needs no device feature");
    check_zero(&key, "disabled state clears the entire cache key");
    call.multisample_flags = DS_MS_SAMPLE_LOCATIONS;
    caps = full_caps();
    const u32 invalid_counts[] = {0, 3, 6, 17, 32};
    for (u32 i = 0; i < sizeof invalid_counts / sizeof invalid_counts[0]; i++) {
        memset(&key, 0xa5, sizeof key);
        check(!vk_sample_locations_key(&key, &call, invalid_counts[i], &caps, 0, 8),
              "invalid sample count is rejected");
        check_zero(&key, "rejected count leaves a zero key");
    }
    caps.available = 0;
    check(!vk_sample_locations_key(&key, &call, 4, &caps, 0, 8), "extension feature is required");
    caps = full_caps();
    caps.variable = 0;
    check(!vk_sample_locations_key(&key, &call, 4, &caps, 0, 8), "variable sample locations are required");
    caps = full_caps();
    caps.counts &= ~4u;
    check(!vk_sample_locations_key(&key, &call, 4, &caps, 0, 8), "device must support the active sample count");
    caps = full_caps();
    caps.subpixel_bits = 3;
    check(!vk_sample_locations_key(&key, &call, 4, &caps, 0, 8), "four sample-coordinate fraction bits are required");
    caps = full_caps();
    caps.coordinate_min = 1.0f / 16;
    check(!vk_sample_locations_key(&key, &call, 4, &caps, 0, 8), "coordinates below the device range are rejected");
    caps = full_caps();
    caps.coordinate_max = 14.0f / 16;
    call.sample_locations[0] = 0xff;
    check(!vk_sample_locations_key(&key, &call, 4, &caps, 0, 8), "coordinates above the device range are rejected");
}
static void test_grid_shapes(void) {
    static const u32 counts[] = {1, 2, 4, 8, 16};
    static const u32 widths[] = {4, 2, 2, 1, 1};
    static const u32 heights[] = {4, 4, 2, 2, 1};
    VkSampleLocationCaps caps = full_caps();
    for (u32 c = 0; c < 5; c++) {
        DrawCall call = {.multisample_flags = DS_MS_SAMPLE_LOCATIONS | DS_MS_SAMPLE_GRID};
        for (u32 i = 0; i < 16; i++) call.sample_locations[i] = (u8)(0x80 | i);
        VkSampleLocations key;
        check(vk_sample_locations_key(&key, &call, counts[c], &caps, 0, 8), "complete sample grid is accepted");
        check(key.samples == counts[c] && key.width == widths[c] && key.height == heights[c],
              "sample count selects the documented pixel-grid shape");
        check(!memcmp(key.location, call.sample_locations, 16), "full-grid sample order remains pixel then sample");
        caps.grid[c].width = widths[c] > 1 ? widths[c] - 1 : widths[c];
        caps.grid[c].height = heights[c] > 1 ? heights[c] - 1 : heights[c];
        if (widths[c] > 1 || heights[c] > 1)
            check(!vk_sample_locations_key(&key, &call, counts[c], &caps, 0, 8),
                  "non-repeating grid cannot exceed the device grid");
        caps = full_caps();
        call.multisample_flags = DS_MS_SAMPLE_LOCATIONS;
        check(vk_sample_locations_key(&key, &call, counts[c], &caps, 0, 8), "single-pixel pattern is accepted");
        check(key.samples == counts[c] && key.width == 1 && key.height == 1,
              "grid-disabled state uses one pixel");
        check(!memcmp(key.location, call.sample_locations, counts[c]), "single-pixel pattern preserves sample order");
        for (u32 i = counts[c]; i < 16; i++) check(!key.location[i], "unused pattern bytes are zero");
    }
}
static void test_exact_repetition(void) {
    VkSampleLocationCaps caps = full_caps();
    caps.grid[0] = (VkExtent2D){1, 1};
    DrawCall call = {.multisample_flags = DS_MS_SAMPLE_LOCATIONS | DS_MS_SAMPLE_GRID};
    for (u32 i = 0; i < 16; i++) call.sample_locations[i] = 0x86;
    VkSampleLocations key;
    check(vk_sample_locations_key(&key, &call, 1, &caps, 0, 8), "uniform pattern fits a one-pixel device grid");
    check(key.width == 1 && key.height == 1 && key.location[0] == 0x86, "uniform grid reduces to one pixel");
    VkSampleLocations grid_key = key;
    call.multisample_flags = DS_MS_SAMPLE_LOCATIONS;
    check(vk_sample_locations_key(&key, &call, 1, &caps, 0, 8) && !memcmp(&key, &grid_key, sizeof key),
          "equivalent uniform states share one cache key");
    call.multisample_flags |= DS_MS_SAMPLE_GRID;
    caps = full_caps();
    const u8 pattern[4] = {0x11, 0x22, 0x33, 0x44};
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) call.sample_locations[y * 4 + x] = pattern[(y % 2) * 2 + x % 2];
    check(vk_sample_locations_key(&key, &call, 1, &caps, 0, 8), "exact two-by-two repetition is accepted");
    check(key.width == 2 && key.height == 2 && !memcmp(key.location, pattern, 4),
          "two-dimensional reduction packs rows without stale columns");
    caps.grid[0] = (VkExtent2D){1, 1};
    check(!vk_sample_locations_key(&key, &call, 1, &caps, 0, 8), "distinct pattern is never approximated by one pixel");
    check_zero(&key, "grid rejection leaves a zero cache key");
}
static void test_lower_left(void) {
    VkSampleLocationCaps caps = full_caps();
    DrawCall call = {.multisample_flags = DS_MS_SAMPLE_LOCATIONS | DS_MS_SAMPLE_GRID};
    for (u32 i = 0; i < 16; i++) call.sample_locations[i] = (u8)(0x10 + i);
    VkSampleLocations key;
    const u8 even_expected[16] = {0xfc, 0xfd, 0xfe, 0xff, 0xf8, 0xf9, 0xfa, 0xfb,
                                0xf4, 0xf5, 0xf6, 0xf7, 0xf0, 0xf1, 0xf2, 0xf3};
    const u8 odd_expected[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xfc, 0xfd, 0xfe, 0xff,
                               0xf8, 0xf9, 0xfa, 0xfb, 0xf4, 0xf5, 0xf6, 0xf7};
    check(vk_sample_locations_key(&key, &call, 1, &caps, 1, 8) &&
              !memcmp(key.location, even_expected, 16), "lower-left transform reverses rows and subpixel y");
    check(vk_sample_locations_key(&key, &call, 1, &caps, 1, 9) &&
              !memcmp(key.location, odd_expected, 16), "lower-left grid phase follows the attachment height");
    call.sample_locations[0] = 0;
    check(!vk_sample_locations_key(&key, &call, 1, &caps, 1, 8), "reflected coordinate beyond the device range is rejected");
    caps.coordinate_max = 1;
    check(!vk_sample_locations_key(&key, &call, 1, &caps, 1, 8), "one-pixel coordinate cannot wrap in the packed key");
}
static void test_vulkan_info(void) {
    VkSampleLocations key = {.samples = 2, .width = 2, .height = 2,
                             .location = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
    VkSampleLocationEXT output[16];
    for (u32 i = 0; i < 16; i++) output[i] = (VkSampleLocationEXT){-1, -1};
    VkSampleLocationsInfoEXT info = vk_sample_locations_info(&key, output);
    check(info.sType == VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT && !info.pNext &&
              info.sampleLocationsPerPixel == VK_SAMPLE_COUNT_2_BIT &&
              info.sampleLocationGridSize.width == 2 && info.sampleLocationGridSize.height == 2 &&
              info.sampleLocationsCount == 8 && info.pSampleLocations == output,
          "Vulkan info preserves the key shape and output lifetime");
    check(output[0].x == 2.0f / 16 && output[0].y == 1.0f / 16 &&
              output[7].x == 0 && output[7].y == 15.0f / 16, "packed coordinates convert exactly to Vulkan floats");
    for (u32 i = 8; i < 16; i++) check(output[i].x == -1 && output[i].y == -1, "info preserves unused output entries");
    for (u32 samples = 1; samples <= 16; samples *= 2) {
        key = vk_sample_locations_standard(samples);
        check(key.samples == samples && key.width == 1 && key.height == 1, "standard patterns use one pixel");
        for (u32 i = samples; i < 16; i++) check(!key.location[i], "standard patterns clear unused coordinates");
        for (u32 i = 0; i < samples; i++)
            for (u32 j = 0; j < i; j++) check(key.location[i] != key.location[j], "standard positions are distinct");
    }
    key = vk_sample_locations_standard(4);
    const u8 four_samples[] = {0x26, 0x6e, 0xa2, 0xea};
    check(!memcmp(key.location, four_samples, 4), "standard four-sample positions retain sample-index order");
    key = vk_sample_locations_standard(3);
    check_zero(&key, "invalid standard count returns no pattern");
}
int main(void) {
    test_disabled_and_gates();
    test_grid_shapes();
    test_exact_repetition();
    test_lower_left();
    test_vulkan_info();
    printf("vk sample locations: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
