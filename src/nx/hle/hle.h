/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HLE's state: every variable of static duration the kernel, the SVCs, the services and the file system keep is a
   member of g_hle, one struct per file (g_hle.ipc is ipc.c's), whose type is declared in the header beside that file.
   g_hle has no initializer, so it is all zero in .bss: hle_init sets what starts out otherwise (the locks, the
   defaults), first thing in the program, before anything reads or writes g_hle. Only .c files include this header: the owners' headers never do, so they can't form a cycle.

   The objects the HLE makes and frees (kernel objects, service objects and their data, sessions, domains...) live in
   pools: a static array with a bound, in g_hle. An all-zero slot is free (a live object never is: the code that takes
   a slot makes it non-zero before it lets go of the pool's lock), a new object takes the first free slot from the one
   after the last taken, wrapping around, and a freed one is zeroed. A full pool is a bound below what a game uses,
   asserted */
#ifndef SNX_HLE_H
#define SNX_HLE_H
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/vi.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/network/network.h"
#include "nx/hle/filesystem/fs.h"

typedef struct {
    /* the kernel and the SVCs */
    HleKernel kernel;
    HleThread thread;
    HleSync sync;
    HleDebug debug;
    HleSvc svc;
    HleSvcMem svcmem;
    HleIpc ipc;
    /* the services */
    HleAcc acc;
    HleAm am;
    HleAudio audio;
    HleAudren audren;
    HleAudrenDsp audren_dsp;
    HleHid hid;
    HleLdr ldr;
    HleMisc misc;
    HleNvdrv nvdrv;
    HlePl pl;
    HleSet set;
    HleSm sm;
    HleTime time;
    HleVi vi;
    HleBsd bsd;
    HleLdn ldn;
    HleNifm nifm;
    HleSsl ssl;
    /* the file system */
    HleFs fs;
} Hle;

extern Hle g_hle;
void hle_init(void);

/* a thread as a number, which the kernel's lists hold: its slot in g_hle.thread.threads plus 1, 0 for none */
static inline u32 kt_ix(const KThread* t) { return t ? (u32)(t - g_hle.thread.threads) + 1 : 0; }
static inline KThread* kt_at(u32 i) { return i ? &g_hle.thread.threads[i - 1] : NULL; }

/* a free slot of the count at base, each size bytes, searched from *next on and wrapping: the slot, *next past it.
   The caller holds the lock that guards the pool, and makes the slot non-zero before it lets go of it */
void* pool_take(void* base, size_t size, u32 count, u32* next, const char* what);
/* the slot freed (zeroed), under the pool's lock */
void pool_put(void* slot, size_t size);
/* the same for a pool that is an array, arr, whose search starts at next (a u32) */
#define POOL_TAKE(arr, next, what) pool_take((arr), sizeof *(arr), (u32)(sizeof(arr) / sizeof *(arr)), &(next), (what))
#define POOL_PUT(p)                pool_put((p), sizeof *(p))
#define POOL_INDEX(arr, p)         ((u32)((p) - (arr)))
#endif
