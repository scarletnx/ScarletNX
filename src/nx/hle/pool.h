/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bounded pools (hle/hle.h has the rules): a static array whose all-zero slots are free, taken and given back under
   the lock that guards it. The HLE's pools are in g_hle, nvn's in g_nvn (gpu/nvn/nvn_state.h). hle/hle.c has
   the functions */
#ifndef SNX_HLE_POOL_H
#define SNX_HLE_POOL_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* a free slot of the count at base, each size bytes, searched from *next on and wrapping: the slot, *next past it. The
   caller holds the lock that guards the pool, and makes the slot non-zero before it lets go of it. A full pool ends
   the program after a line that names it: the bound is the runtime's own, with no answer of the console's for it */
void* pool_take(void* base, size_t size, u32 count, u32* next, const char* what);
/* the same for an object the console refuses with a result when it has no more of them (a thread, an event, a
   domain): NULL from a full pool, and the caller answers that result and logs it with the pool and its count */
void* pool_try(void* base, size_t size, u32 count, u32* next);
/* the slot freed (zeroed), under the pool's lock */
void pool_put(void* slot, size_t size);
/* the same for a pool that is an array, arr, whose search starts at next (a u32) */
#define POOL_TAKE(arr, next, what) pool_take((arr), sizeof *(arr), (u32)(sizeof(arr) / sizeof *(arr)), &(next), (what))
#define POOL_TRY(arr, next)        pool_try((arr), sizeof *(arr), (u32)(sizeof(arr) / sizeof *(arr)), &(next))
#define POOL_PUT(p)                pool_put((p), sizeof *(p))
#define POOL_INDEX(arr, p)         ((u32)((p) - (arr)))
#endif
