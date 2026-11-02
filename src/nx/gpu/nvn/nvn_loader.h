/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_LOADER_H
#define SNX_NVN_LOADER_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
#include "common/filesystem/host.h"

/* the functions the game resolved that aren't implemented: their addresses and names, for the message when one is
   called. Filled under lock, read by the handler of a call, which only runs once its entry was published */
typedef struct {
    u64 addr;
    char name[128];
} NvnMissing;
#define NVN_MISSING_SLOTS 1024 /* a power of two above the ~530 functions NVN has */
/* the file's state: the missing functions, the lock their table and the slots are changed under, the resolvers'
   recompiled code, the game's NVN API version (nvn_api_pack, 0 unknown) once read (under lock) and whether the
   runtime runs the game's NVN calls: from nvn_loader_init on, when the game has NVN.
   g_nvn.loader (gpu/nvn/nvn_state.h). A 32-bit game (a32) has its resolvers as blocks of its A32 code (the *32 ones),
   its NVN functions' slots in its block table, and 4-byte pointers */
typedef struct {
    NvnMissing missing[NVN_MISSING_SLOTS];
    PMutex lock;
    GuestFn bootstrap_original, getproc_original;
    SnxArmBlock bootstrap_original32, getproc_original32;
    u64 bootstrap_entry;
    u64 flag; /* nvnPrivate0's address, 0 when the driver exports none */
    u32 api;
    int active, decided, a32;
    /* the driver's recompiled code for the NvRm sync conversions and their addresses, which the game resolved and
       nvn took over (nvn_object.c runs them on a sync in the driver's layout); 0 until resolved */
    GuestFn nvrm_get, nvrm_init;
    u64 nvrm_get_at, nvrm_init_at;
} NvnLoaderState;

/* The runtime runs the game's NVN calls itself, and the game's NVN driver doesn't run (gpu/nvn/nvn_loader.c). Init
   runs once the game's symbols and dispatch table are loaded, before any guest thread starts. A game without NVN
   (nvnBootstrapLoader) draws nothing. A Switch 2 program keeps its own NVN (NVN2) driver, whose command streams the
   GPU's front end decodes */
void nvn_loader_init(void);
/* 1 once nvn_loader_init took the game's NVN loader: the NVN functions the game resolves are the runtime's */
int nvn_loader_active(void);
/* The driver's code fn, recompiled for its function at (whose slot the loader took), run on c from inside an NVN
   call with the arguments a0 to a2, as the driver runs a callback: its return to the NVN call's return address ends
   the run. Its x0 */
u64 nvn_loader_run_driver(CPU* c, GuestFn fn, u64 at, u64 a0, u64 a1, u64 a2);

#endif
