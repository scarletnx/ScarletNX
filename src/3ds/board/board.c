/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/board.h"

/* Layout follows Azahar src/core/memory.h; see docs/platforms/3ds/README.md for provenance.
   No Azahar interpreter/JIT or privileged ARM9 firmware is embedded here. */
#define COMMON_REGIONS                                                                                                 \
    {"IO", 0x10100000u, 0x00400000u, SNX_CTR_MMIO}, {"MPCore", 0x17e00000u, 0x00002000u, SNX_CTR_MMIO}, {              \
        "VRAM", 0x18000000u, 0x00600000u, SNX_CTR_VRAM                                                                 \
    }
#define WORK_REGIONS                                                                                                   \
    {"DSP RAM", 0x1ff00000u, 0x00080000u, SNX_CTR_DSP_RAM}, { "AXI WRAM", 0x1ff80000u, 0x00080000u, SNX_CTR_RAM }
static const SnxCtrRegion old_regions[] = {
    COMMON_REGIONS,
    WORK_REGIONS,
    {"FCRAM", 0x20000000u, 0x08000000u, SNX_CTR_RAM},
};
static const SnxCtrRegion new_regions[] = {
    COMMON_REGIONS,
    {"Extra RAM", 0x1f000000u, 0x00400000u, SNX_CTR_RAM},
    WORK_REGIONS,
    {"FCRAM", 0x20000000u, 0x10000000u, SNX_CTR_RAM},
};
static const SnxCtrProfile profiles[] = {
    {SNX_CTR_OLD, "Old 3DS", 2, 0x08000000u, old_regions, sizeof old_regions / sizeof *old_regions},
    {SNX_CTR_NEW, "New 3DS", 4, 0x10000000u, new_regions, sizeof new_regions / sizeof *new_regions},
};

const SnxCtrProfile* snx_ctr_profile(SnxCtrModel model) {
    switch (model) {
        case SNX_CTR_OLD: return &profiles[0];
        case SNX_CTR_NEW: return &profiles[1];
        default: return NULL;
    }
}
const SnxCtrRegion* snx_ctr_physical_region(const SnxCtrProfile* profile, uint32_t address, uint32_t size) {
    if (!profile || !size) return NULL;
    for (size_t i = 0; i < profile->region_count; ++i) {
        const SnxCtrRegion* region = &profile->regions[i];
        if (address >= region->base && (uint64_t)address + size <= (uint64_t)region->base + region->size) return region;
    }
    return NULL;
}
bool snx_ctr_board_init(SnxCtrBoard* board, SnxCtrModel model) {
    if (!board) return false;
    *board = (SnxCtrBoard){.profile = snx_ctr_profile(model)};
    if (!board->profile) return false;
    board->powered = true;
    return true;
}
bool snx_ctr_board_reset(SnxCtrBoard* board) {
    if (!board || !board->powered || !board->profile) return false;
    board->ticks = 0;
    return true;
}
bool snx_ctr_board_advance(SnxCtrBoard* board, uint64_t ticks) {
    if (!board || !board->powered || !board->profile || ticks > UINT64_MAX - board->ticks) return false;
    board->ticks += ticks;
    return true;
}
void snx_ctr_board_shutdown(SnxCtrBoard* board) {
    if (board) *board = (SnxCtrBoard){0};
}
