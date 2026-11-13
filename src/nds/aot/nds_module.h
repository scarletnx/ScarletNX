/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's compiled module, <name>_code.so in its folder: the blocks of every piece of code the card image holds at
   a known address (cartridge/rom.h's regions), compiled ahead of time as the 3DS's are. A region has a table of blocks
   for each instruction set, a slot for each place an instruction can start, and each block's length, so the run can
   tell that memory still holds the bytes the block was compiled from: the game's overlays share addresses. */
#ifndef SNX_NDS_MODULE_H
#define SNX_NDS_MODULE_H
#include "common/cpu/arm/arm.h"
#include "nds/cartridge/rom.h"

#define NDS_MODULE_MAGIC  0x3153444E584E5353ull /* "SSNXNDS1" */
#define NDS_MODULE_ABI    1u
#define NDS_MODULE_SYMBOL "snx_nds_module_v1"
#define NDS_BLOCK_UNITS   32u /* a block's most instructions (Thumb: halfwords) */
#ifdef _WIN32
#define NDS_MODULE_SUFFIX "_code.dll"
#else
#define NDS_MODULE_SUFFIX "_code.so"
#endif
/* the blocks runs compiled at run time, which the next compile starts from: a region's index and pc | 1 for Thumb */
#define NDS_HINTS_SUFFIX ".hints"

typedef struct {
    uint32_t address, size;
    uint64_t hash;
    SnxArmBlock const* arm;   /* size / 4 slots, NULL where no block starts */
    SnxArmBlock const* thumb; /* size / 2 */
    const uint8_t* arm_units; /* each block's instructions (Thumb: halfwords), 0 for none */
    const uint8_t* thumb_units;
} NdsModuleRegion;

typedef struct {
    uint64_t magic;
    uint32_t abi, arm_abi, cpu_size, pointer_size, profile, region_count;
    uint64_t identity; /* the image's (NdsRom.identity) */
    const NdsModuleRegion* regions;
} NdsModule;

/* Compiles rom's regions into the module at path, replacing an earlier one once the new one is whole. hints names the
   file of blocks earlier runs compiled, NULL for none. 0, else 1 with the reason in error */
int nds_aot_compile(const NdsRom* rom, const char* path, const char* hints, int threads, char* error,
                    size_t error_size);

/* Discovery: which instruction slots of each region start a block and how long each is */
#define NDS_PLAN_VISITED 1u
#define NDS_PLAN_ENTRY   2u
#define NDS_PLAN_STOP    4u
typedef struct {
    unsigned char *arm, *thumb; /* a region's flags by slot */
} NdsPlan;
/* plans[i] for rom->regions[i], the caller's to free with nds_plans_free. seeds: count pairs of a region's index and
   pc | 1 for Thumb. False when out of memory */
bool nds_discover(const NdsRom* rom, const uint32_t* seeds, size_t count, NdsPlan** plans, uint64_t* entries);
void nds_plans_free(NdsPlan* plans, unsigned count);
unsigned nds_plan_length(const NdsRegion* region, const NdsPlan* plan, uint32_t pc, bool thumb);
#endif
