/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_LOADER_H
#define SNX_NVN_LOADER_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
#include "common/platform/thread.h"

/* the functions the game resolved, by address: each one's name, for the message when one that isn't implemented is
   called, and for a save state's load in a process that hasn't resolved them (nvn_loader_state_install). Filled under
   lock, read by the handler of a call, which only runs once its entry was published */
typedef struct {
    u64 addr;
    char name[128];
} NvnResolved;
#define NVN_RESOLVED_SLOTS 1024 /* a power of two above the ~530 functions NVN has */
/* the file's state: the functions resolved, the lock their table and the slots are changed under, the resolvers'
   recompiled code, the game's NVN API version (nvn_api_pack, 0 unknown) once read (under lock) and whether the
   runtime runs the game's NVN calls: from nvn_loader_init on, when the game has NVN.
   g_nvn.loader (gpu/nvn/nvn_state.h). A 32-bit game (a32) has its resolvers as blocks of its A32 code (the *32 ones),
   its NVN functions' slots in its block table, and 4-byte pointers */
typedef struct {
    NvnResolved resolved[NVN_RESOLVED_SLOTS];
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
   (nvnBootstrapLoader) draws nothing */
void nvn_loader_init(void);
/* 1 once nvn_loader_init took the game's NVN loader: the NVN functions the game resolves are the runtime's */
int nvn_loader_active(void);
/* 1 when fn is a function the loader puts in a dispatch slot in the driver's place: an NVN function's
   implementation, the handler of one the runtime has not, or the wrappers of the driver's two lookups. A slot keeps
   one until the loader gives it another, which it tells of (guest_jit.h's jit_slot_taken) */
int nvn_loader_handler(GuestFn fn);
/* The driver's code fn, recompiled for its function at (whose slot the loader took), run on c from inside an NVN
   call with the arguments a0 to a2, as the driver runs a callback: its return to the NVN call's return address ends
   the run. Its x0 */
u64 nvn_loader_run_driver(CPU* c, GuestFn fn, u64 at, u64 a0, u64 a1, u64 a2);
/* A save state's loader (gpu/nvn/nvn_save.c): whether what it resolved is this game's (its functions in the game's
   code, by names), checked before the load can't be refused; and, once the state's game is in place, each taken again
   here, as this process may not have resolved them yet (a state loaded as the game starts), and the state's API
   version kept where this process hasn't read the driver's */
int nvn_loader_state_ok(const NvnLoaderState* saved);
void nvn_loader_state_install(const NvnLoaderState* saved);

#endif
