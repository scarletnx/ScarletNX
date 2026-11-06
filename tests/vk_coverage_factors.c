/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The compact native table and a future portable resolve share exactly representable dyadic coefficients.
   Every raster mask is checked, including masks whose surviving sample is not sample zero. */
#include "nx/gpu/vk/coverage.h"
#include <stdio.h>

static u32 checks, failures;
static void check(int pass, const char* what) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
static u32 population(u32 mask) {
    u32 count = 0;
    for (; mask; mask /= 2) count += mask % 2;
    return count;
}
int main(void) {
    const u32 components[] = {0, 7, 8, 15};
    for (u32 mode = 0; mode < 4; mode++)
        check(vk_coverage_components(mode) == components[mode], "modulation component selection");
    check(vk_coverage_components(4) == 0, "invalid modulation mode selects no components");
    for (u32 raster = 2; raster <= 16; raster *= 2)
        for (u32 color = 1; color < raster; color *= 2) {
            VkCoverage state = {.raster_samples = raster};
            const u32 ratio = raster / color;
            float compact[16];
            check(vk_coverage_table(&state, color, compact) == ratio, "default table has the color-sample ratio");
            for (u32 count = 0; count <= ratio; count++) {
                int factor = vk_coverage_factor16(&state, color, count);
                check(factor >= 0 && (u32)factor * ratio == count * 16,
                      "default factors equal the exact covered fraction");
                if (count) check(compact[count - 1] * ratio == (float)count, "native factors use the same fraction");
            }
            check(vk_coverage_factor16(&state, color, ratio + 1) == -1, "coverage above the ratio is invalid");
            state.table_enable = 1;
            for (u32 coefficient = 0; coefficient <= 16; coefficient++) {
                for (u32 index = 0; index < 16; index++) state.table[index] = (u8)((index + coefficient) % 17);
                check(vk_coverage_table(&state, color, compact) == ratio, "custom table has the color-sample ratio");
                check(vk_coverage_factor16(&state, color, 0) == 0, "empty coverage never indexes the custom table");
                for (u32 count = 1; count <= ratio; count++) {
                    /* Derive the selected source entry by dividing its index interval into ratio equal buckets. */
                    const u32 first = (count - 1) * (16 / ratio);
                    const u32 expected = (first + coefficient) % 17;
                    check(vk_coverage_factor16(&state, color, count) == (int)expected,
                          "custom factor selects the first entry in its coverage bucket");
                    check(compact[count - 1] * 16 == (float)expected, "native custom factor is exactly representable");
                }
            }
        }
    for (u32 raster = 2; raster <= 16; raster *= 2) {
        VkCoverage state = {.raster_samples = raster};
        for (u32 mask = 0; mask < (1u << raster); mask++) {
            const u32 count = population(mask);
            check(vk_coverage_factor16(&state, 1, count) * (int)raster == (int)(count * 16),
                  "every coverage mask uses all surviving samples");
        }
    }
    VkCoverage invalid = {.raster_samples = 4, .table_enable = 1, .table = {17}};
    check(vk_coverage_factor16(&invalid, 1, 1) == -1, "custom coefficients above one are invalid");
    check(vk_coverage_factor16(&invalid, 0, 1) == -1, "zero color samples are invalid");
    invalid.raster_samples = 3;
    check(vk_coverage_factor16(&invalid, 1, 1) == -1, "non-power-of-two raster samples are invalid");
    printf("vk coverage factors: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
