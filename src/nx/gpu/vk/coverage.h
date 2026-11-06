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
/* A nonzero color sample's factor in sixteenths. Ratios are powers of two no greater than 16, so both the default
   factor and every command-table coefficient are represented exactly. Zero coverage is killed before modulation;
   returning zero for it must not be used to turn it into a blend with a zero source. -1 denotes invalid state. */
static inline int vk_coverage_factor16(const VkCoverage* key, u32 color, u32 covered) {
    int ratio = vk_coverage_ratio(key->raster_samples, color);
    if (!ratio || covered > (u32)ratio) return -1;
    if (!covered) return 0;
    if (!key->table_enable) return (int)(covered * 16 / (u32)ratio);
    u32 value = key->table[(covered - 1) * 16 / (u32)ratio];
    return value <= 16 ? (int)value : -1;
}
/* The component bits multiplied by the factor: R,G,B,A in bits 0..3. NONE leaves all components unmodified, but
   coverage still decides whether a color sample is written. */
static inline u32 vk_coverage_components(u32 mode) {
    return mode < 4 ? ((mode & 1) ? 7u : 0u) | ((mode & 2) ? 8u : 0u) : 0;
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
    for (u32 covered = 1; covered <= (u32)ratio; covered++) {
        int factor = vk_coverage_factor16(key, color, covered);
        if (factor < 0) return 0;
        output[covered - 1] = (float)factor / 16.0f;
    }
    return (u32)ratio;
}
#endif
