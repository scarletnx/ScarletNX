/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NROs a program maps while it runs (ldr:ro), made ahead of time: the recompile finds them in the program's RomFS,
   gives each a guest address behind the program's modules (loader.h's snx_nro_slots_place: in the order of their
   hashes, so the same set of NROs is laid out the same), tells the game module where they go (its snx_nros table,
   module_abi.h) and makes each one's code a module of its own for that address, <hash>.so in <Game>_nro beside the
   game module, which ldr:ro loads when the program maps the NRO of that hash (hle/services/ldr.c) */
#ifndef SNX_DRIVER_NROS_H
#define SNX_DRIVER_NROS_H
#include "nx/loader/module.h"
#include "nx/loader/module_llvm.h"
#include "nx/runtime/driver/container.h"

/* an NRO as found: its RomFS file's name, and its image (the file up to the size its header gives) */
typedef struct {
    unsigned char hash[32];
    char* name;
    unsigned char* data;
    size_t size;
} SnxNroFile;
typedef struct {
    SnxNroFile* files;
    size_t file_count;
    SnxNroSlot* places; /* placed: one per hash, in their order */
    size_t count;
    SnxU64 end;
} SnxNroSet;

/* the NROs of an extracted RomFS's folder (every .nro under it), or of a dump's RomFS read in place, added to set,
   which starts zeroed. A file that is no NRO ldr:ro maps (not an NRO, compressed, its image or BSS not in whole
   pages) is said and left out. 0, or nonzero, said why */
int snx_nros_folder(SnxNroSet* set, const char* romfs, int quiet);
int snx_nros_dump(SnxNroSet* set, SnxHacFs* fs, int quiet);
/* gives each NRO of set its address behind program's modules. 0, or nonzero, said why */
int snx_nros_place(SnxNroSet* set, const SnxProgram* program, int quiet);
/* each placed NRO's module, <folder>/<hash>.so (.dll for Windows), made as the game module was (gen, build: its level,
   jobs and unit cache), and every other module of an earlier recompile in folder removed. 0, or nonzero, said why */
int snx_nros_build(const SnxNroSet* set, const SnxProgram* program, const SnxCodegenOptions* gen,
                   const SnxLlvmBuild* build, const char* folder, int windows);
void snx_nros_free(SnxNroSet* set);

#endif
