/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AOT step for a GBA ROM: the ARM and Thumb code of the cartridge goes through the existing ARM
   recompiler (src/common/cpu/arm) into a native module, the way NX and GNU ABI prepare their code. */
#ifndef SNX_AGB_RECOMP_H
#define SNX_AGB_RECOMP_H
#include "common/cpu/arm/arm.h"
#include <stddef.h>
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
typedef struct {
    uint32_t pc;
    uint16_t instructions;
    uint8_t thumb;
    uint64_t code; /* a hash of the words it was built from: a block in RAM runs while memory still holds them */
    uint32_t symbol; /* its function's number in the cached module (snx_agb_<n>); 0 for one compiled at run time */
    uint8_t hint;    /* the map named it as one a run compiled, which this run compiles again when it reaches it */
    SnxArmBlock fn;
} AgbBlock;

typedef struct {
    AgbBlock* blocks; /* from index 1, capacity of them, then their index: 2 * capacity slots (recomp.c) */
    size_t count, capacity;
    void* handle;            /* the loaded native module */
    struct SnxArmExec* exec; /* what compiles the blocks the module lacks, in this process at run time */
    size_t compiled;         /* the blocks the run compiled that the map did not name: its new hints */
    char* base;              /* the cache path: <base>.so and <base>.map, which names the ROM's hash */
    int dirty;               /* blocks changed since the module was linked */
    /* the module file's size and time as this run loaded or linked it: one another process recompiled since numbers
       its functions its own way */
    unsigned long long module_size, module_time;
    /* the blocks a search compared with the pc it looked for: a dispatch's cost, which the index keeps to a slot or two
       however many blocks there are (tests/agb_cpu.c) */
    uint64_t compared;
} AgbRecomp;

void agb_recomp_init(AgbRecomp* recomp, const char* base);
void agb_recomp_free(AgbRecomp* recomp);
/* The AOT step: discovers the code the ROM entry reaches and builds the module. */
int agb_recomp_build(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size);
/* The blocks of the cache's map, without its module: every block the runs reached, which a recompile (-r) builds the
   module from. 0, else -1, error saying why when a map is there but for another ROM or another build */
int agb_recomp_load_map(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size);
/* Loads the module a previous build cached in the game's folder, and its map. Returns 0 on success, else -1, error
   saying why when a cache is there but for another ROM or another build. */
int agb_recomp_load_cache(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size);
/* The block at pc in the given ISA, compiling one if the cache does not hold it yet. */
const AgbBlock* agb_recomp_lookup(AgbRecomp* recomp, AgbBoard* board, uint32_t pc, int thumb, char* error,
                                  size_t error_size);
/* Keeps the blocks compiled at run time for the next run, in the map alone, as hints: the module stays as it is, and a
   recompile builds them into it. */
int agb_recomp_save(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size);
#endif
