/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_BOARD_H
#define SNX_CTR_BOARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Board descriptions only: these are physical addresses, NOT process VAs or
   permission to dereference SnxArmCPU.mem. The memory controller owns mappings. */
typedef enum { SNX_CTR_OLD, SNX_CTR_NEW } SnxCtrModel;
typedef enum { SNX_CTR_RAM, SNX_CTR_VRAM, SNX_CTR_DSP_RAM, SNX_CTR_MMIO } SnxCtrRegionKind;
typedef struct {
    const char* name;
    uint32_t base, size;
    SnxCtrRegionKind kind;
} SnxCtrRegion;
typedef struct {
    SnxCtrModel model;
    const char* name;
    unsigned arm11_cores;
    uint32_t fcram_size;
    const SnxCtrRegion* regions;
    size_t region_count;
} SnxCtrProfile;

/* Timing uses Azahar's base ARM11 tick domain, including on New 3DS.
   CPU overclock/scaling and actual device event scheduling are not wired yet. */
#define SNX_CTR_BASE_CLOCK_HZ   268111856ull
#define SNX_CTR_TICKS_PER_FRAME 4481136ull

typedef struct {
    const SnxCtrProfile* profile;
    uint64_t ticks;
    bool powered;
} SnxCtrBoard;

const SnxCtrProfile* snx_ctr_profile(SnxCtrModel model);
/* Return a region only when the entire nonempty physical range fits in it. */
const SnxCtrRegion* snx_ctr_physical_region(const SnxCtrProfile* profile, uint32_t address, uint32_t size);
/* Initialize fresh storage; no guest RAM, OS or devices are booted by this API. */
bool snx_ctr_board_init(SnxCtrBoard* board, SnxCtrModel model);
bool snx_ctr_board_reset(SnxCtrBoard* board);
bool snx_ctr_board_advance(SnxCtrBoard* board, uint64_t ticks);
void snx_ctr_board_shutdown(SnxCtrBoard* board);
#endif
