/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_MODULE_H
#define SNX_VITA_MODULE_H
#include "common/cpu/arm/arm.h"
#include "vita/loader/image.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A game's compiled code: <title ID>_code.so (.dll on Windows) in its folder, the blocks the compiler found in the
   image as it loads (vita/loader/image.h), for the A9 profile on the flat guest memory (cpu/arm/README.md): the
   Vita's Cortex-A9 is ARMv7-A, whose user-mode A32, Thumb-2, VFPv3 and NEON the ARMv8-A AArch32 instruction set holds,
   with the UNPREDICTABLE forms its games rely on run as the A9 runs them */
#define VITA_MODULE_MAGIC 0x31415449564e5853ull /* "SNXVITA1" */
/* raised when the compiled blocks change. 2 added Thumb literal loads, saturation, parallel arithmetic and DSP
   multiplies, 3 TBH, 4 no flags from 16-bit data processing in an IT block, 5 a 16-bit LDM loading its base, 6 the
   A9 profile (stores with writeback into the register they store), 7 blocks that go on to the next by a tail call
   and count their backward edges at VITA_POLL_OFFSET from the cpu */
#define VITA_MODULE_ABI   7u
/* where a chained block finds the thread's poll count, right after the cpu (VitaThread) */
#define VITA_POLL_OFFSET  sizeof(SnxArmCPU)
#ifdef _WIN32
#define VITA_MODULE_SUFFIX "_code.dll"
#else
#define VITA_MODULE_SUFFIX "_code.so"
#endif
/* the blocks earlier runs compiled at run time, one a line in hex with bit 0 for Thumb: discovery starts there too */
#define VITA_HINTS_SUFFIX ".hints"
/* the most code one block may run through: the block compiler's limit, in instructions (A32) or halfwords (Thumb) */
#define VITA_BLOCK_LIMIT 65536u

typedef struct {
    uint64_t magic;
    uint32_t abi, descriptor_size, cpu_size, arm_abi, pointer_size, count;
    unsigned char identity[32];   /* the image's (VitaImage.identity) */
    const uint32_t* keys;         /* count block addresses, ascending, bit 0 set for Thumb */
    SnxArmBlock const* blocks;    /* each key's block */
} VitaModule;

bool vita_module_compatible(const VitaModule* module, const VitaImage* image);
/* Whether the descriptor's fields before its count are those this build compiles for image */
bool vita_module_fields(const VitaModule* module, const VitaImage* image);
/* The block at key (address | Thumb), NULL when the module lacks it */
SnxArmBlock vita_module_block(const VitaModule* module, uint32_t key);

typedef struct {
    void* library;
    const VitaModule* module;
} VitaNative;
/* The module at path, opened and checked against image. Trusted native code: the driver compiled it itself */
bool vita_native_open(const char* path, const VitaImage* image, VitaNative* native, char* error, size_t error_size);
void vita_native_close(VitaNative* native);
/* Whether the module at path was compiled for image by this build, read from its descriptor in the file without
   loading it: the driver compiles first when it was not, so it loads a module once (a library unloaded and loaded
   again by its path can be the old one, as under the Tracy client, whose dlclose does nothing) */
bool vita_module_current(const char* path, const VitaImage* image);

/* The hints (VITA_HINTS_SUFFIX): the blocks in folder's file, ascending and each once (count 0 and NULL for none, the
   caller's to free); and the file written with keys (sorted in place): how many it holds, each once, SIZE_MAX when it
   can't be written */
uint32_t* vita_hints_read(const char* folder, size_t* count);
size_t vita_hints_write(const char* folder, uint32_t* keys, size_t count);
/* folder's file named after folder with suffix (recompiled/PCSE00001/PCSE00001_code.so), the caller's to free */
char* vita_game_file(const char* folder, const char* suffix);
/* Compiles image into folder's module, seeded with the hints there. 0, or 1 with error filled */
int vita_aot_compile(const char* folder, const VitaImage* image, char* error, size_t error_size);
#endif
