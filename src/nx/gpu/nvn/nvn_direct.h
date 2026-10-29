/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DIRECT_H
#define SNX_NVN_DIRECT_H
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h"

/* the functions the game resolved that aren't implemented: their addresses and names, for the message when one is
   called. Filled under lock, read by the handler of a call, which only runs once its entry was published */
typedef struct {
    u64 addr;
    char name[128];
} NvnMissing;
#define NVN_MISSING_SLOTS 1024 /* a power of two above the ~530 functions NVN has */
/* the file's state: the missing functions, the lock their table and the slots are changed under, the resolvers'
   recompiled code, the game's NVN API version (nvn_api_pack, 0 unknown) once decided (under lock) and whether the
   runtime runs the game's NVN calls: from nvn_direct_init on, and with auto until the first bootstrap call says no.
   g_nvn.direct (gpu/nvn/nvn_state.h) */
typedef struct {
    NvnMissing missing[NVN_MISSING_SLOTS];
    PMutex lock;
    GuestFn bootstrap_original, getproc_original;
    u64 bootstrap_entry;
    u64 flag; /* nvnPrivate0's address, 0 when the driver exports none */
    u32 api;
    int active, decided;
} NvnDirectState;

/* nvn_direct=1: the runtime runs the game's NVN calls itself, and the game's NVN driver doesn't run; 2 (auto): where
   the game's NVN API version is one nvn_direct has measured layouts for (gpu/nvn/nvn_direct.c). Init runs once the
   game's symbols and dispatch table are loaded, before any guest thread starts, and before nvn_trace_init, which
   stands aside under it (auto with nvn_trace leaves the driver to the trace) */
void nvn_direct_init(void);
/* 1 once nvn_direct_init took the game's NVN loader: the NVN functions the game resolves are the runtime's. With auto,
   0 again from the game's first NVN call on when its API version isn't described (the driver runs then) */
int nvn_direct_active(void);
/* For nvn_verify (gpu/nvn/nvn_verify.c), which runs the driver beside nvn_direct: the driver's own lookup of name on c
   (it puts the name below c's stack pointer and returns to c's link register), the function's address or 0; and the
   driver's nvnPrivate0 word (0 when it exports none), read and put back */
u64 nvn_direct_lookup(CPU* c, const char* name);
u32 nvn_direct_flag(void);
void nvn_direct_flag_set(u32 v);

#endif
