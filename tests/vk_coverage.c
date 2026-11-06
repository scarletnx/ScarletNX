/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Validate mixed-sample coverage's feature gates and the independent16-entry NVN to N/C-entry Vulkan table map. */
#include "nx/gpu/vk/coverage.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int pass, const char* text) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", text);
}
int main(void) {
    for (u32 color = 1; color <= 8; color *= 2)
        for (u32 raster = 2; raster <= 16; raster *= 2)
            for (u32 device = 0; device < 2; device++)
                for (u32 depth = 0; depth < 3; depth++) {
                    u32 depth_samples = depth == 0 ? 0 : depth == 1 ? raster : 32;
                    DrawCall call = {.raster_samples = raster, .coverage_mode = 3, .coverage_table_enable = 1};
                    for (u32 i = 0; i < 16; i++) call.coverage_table[i] = (u8)(i + 1);
                    VkCoverage key;
                    int made = vk_coverage_key(&key, &call, color, 1, depth_samples, (int)device, 31);
                    check(made == (raster >= color && (device || raster == color) && depth != 2),
                          "native reduction or equal-count shader gates");
                    if (!made) continue;
                    check(key.raster_samples == raster && key.mode == 3 && key.table_enable == 1 &&
                              (raster == color ? key.table[0] == call.coverage_table[0]
                                               : !memcmp(key.table, call.coverage_table, sizeof key.table)),
                          "active coverage state preserved");
                    float table[16];
                    for (u32 i = 0; i < 16; i++) table[i] = -1;
                    u32 count = vk_coverage_table(&key, color, table);
                    check(count == raster / color, "compact table has one coefficient per nonzero coverage count");
                    for (u32 covered = 1; covered <= count; covered++) {
                        u32 index = (covered - 1) * 16 * color / raster;
                        check(table[covered - 1] == (float)(index + 1) / 16, "compact factor uses the NVN index rule");
                    }
                    for (u32 i = count; i < 16; i++)
                        check(table[i] == -1, "compact map preserves unused output entries");
                }
    DrawCall call = {.raster_samples = 4, .coverage_mode = 3, .coverage_table_enable = 1};
    VkCoverage key;
    check(!vk_coverage_key(&key, &call, 1, 0, 0, 1, 31), "mixed coverage requires color attachments");
    check(!vk_coverage_key(&key, &call, 1, 1, 0, 1, 1), "unsupported raster count is rejected");
    call.coverage_mode = 4;
    check(!vk_coverage_key(&key, &call, 1, 1, 0, 1, 31), "invalid modulation mode is rejected");
    call.coverage_mode = 3;
    call.coverage_table[3] = 17;
    check(!vk_coverage_key(&key, &call, 1, 1, 0, 1, 31), "coefficient past one is rejected");
    call.coverage_mode = 0;
    check(vk_coverage_key(&key, &call, 1, 1, 0, 1, 31) && !key.mode && !key.table_enable,
          "mode NONE removes inert table values from the cache key");
    call.coverage_mode = 3;
    call.coverage_table_enable = 0;
    check(vk_coverage_key(&key, &call, 1, 1, 0, 1, 31) && key.mode == 3 && !key.table_enable,
          "linear coverage uses no table");
    call.raster_samples = 3;
    check(!vk_coverage_key(&key, &call, 1, 1, 0, 1, 31), "non-power-of-two sample count is rejected");
    call.raster_samples = 32;
    check(!vk_coverage_key(&key, &call, 1, 1, 0, 1, 63), "NVN raster counts stop at16");
    call = (DrawCall){.raster_samples = 4, .coverage_mode = 3, .coverage_table_enable = 1, .coverage_table = {5}};
    check(vk_coverage_key(&key, &call, 4, 1, 4, 1, 31) && key.raster_samples == 4 && key.table[0] == 5 &&
              vk_coverage_factor16(&key, 4, 1) == 5, "ratio-one custom modulation consumes table entry zero");
    call.coverage_table[0] = 16;
    check(vk_coverage_key(&key, &call, 4, 1, 4, 0, 31) && !key.raster_samples,
          "ratio-one unit modulation needs no native extension or distinct pipeline");
    call.coverage_table_enable = 0;
    check(vk_coverage_key(&key, &call, 4, 1, 4, 0, 31) && !key.raster_samples,
          "ratio-one linear modulation is inert");
    check(vk_coverage_key(&key, &call, 4, 0, 4, 0, 31) && !key.raster_samples,
          "equal-count depth-only rasterization needs no color modulation");
    check(!vk_coverage_key(&key, &call, 4, 1, 2, 1, 31), "bound depth must use the raster count");
    call.raster_samples = 0;
    check(!vk_coverage_key(&key, &call, 1, 1, 4, 1, 31), "unlike attachments require explicit raster samples");
    printf("vk coverage: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
