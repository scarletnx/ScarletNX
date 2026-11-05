/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN target-independent rasterization mapped to the native mixed-sample coverage path. The initial path has
   color attachments only: the shared target ABI does not describe separate depth and color sample counts. */
#ifndef SNX_VK_COVERAGE_H
#define SNX_VK_COVERAGE_H
#include "nx/gpu/draw_state.h"
#include <string.h>

typedef struct {
    u32 raster_samples, mode, table_enable;
    u8 table[16]; /* coefficients in units of1/16, clamped to0..16 by NVN command replay */
} VkCoverage;

static inline int vk_coverage_ratio(u32 raster, u32 color) {
    if (!color || (color & (color - 1)) || !raster || (raster & (raster - 1)) || raster > 16 || raster <= color)
        return 0;
    return (int)(raster / color);
}
static inline int vk_coverage_key(VkCoverage* key, const DrawCall* call, u32 color, u32 attachments, int depth,
                                  int available, u32 raster_counts) {
    memset(key, 0, sizeof *key); /* all bytes participate in the pipeline key */
    if (!call->raster_samples || call->raster_samples <= color) return 1;
    if (!available || depth || !attachments || attachments > DS_TARGETS_MAX ||
        !vk_coverage_ratio(call->raster_samples, color) || !(raster_counts & call->raster_samples) ||
        call->coverage_mode > 3)
        return 0;
    key->raster_samples = call->raster_samples;
    key->mode = call->coverage_mode;
    /* A table cannot affect mode NONE; do not make distinct pipelines for those inert table binds. */
    if (key->mode && call->coverage_table_enable) {
        for (u32 i = 0; i < 16; i++)
            if (call->coverage_table[i] > 16) return 0;
        key->table_enable = 1;
        memcpy(key->table, call->coverage_table, sizeof key->table);
    }
    return 1;
}
static inline u32 vk_coverage_table(const VkCoverage* key, u32 color, float output[16]) {
    int ratio = vk_coverage_ratio(key->raster_samples, color);
    if (!ratio) return 0;
    for (u32 covered = 1; covered <= (u32)ratio; covered++)
        output[covered - 1] = (float)key->table[(covered - 1) * 16 / (u32)ratio] / 16.0f;
    return (u32)ratio;
}
#endif
