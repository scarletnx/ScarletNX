/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bounded pools (hle/hle.h has the rules): a static array whose all-zero slots are free, taken and given back under
   the lock that guards it. The HLE's pools are in g_hle, nvn_direct's in g_nvn (gpu/nvn/nvn_state.h). hle/hle.c has
   the functions */
#ifndef SNX_HLE_POOL_H
#define SNX_HLE_POOL_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* a free slot of the count at base, each size bytes, searched from *next on and wrapping: the slot, *next past it. A
   full pool asserts, and gives NULL where asserts are off (NDEBUG). The caller holds the lock that guards the pool,
   and makes the slot non-zero before it lets go of it */
void* pool_take(void* base, size_t size, u32 count, u32* next, const char* what);
/* the slot freed (zeroed), under the pool's lock */
void pool_put(void* slot, size_t size);
/* the same for a pool that is an array, arr, whose search starts at next (a u32) */
#define POOL_TAKE(arr, next, what) pool_take((arr), sizeof *(arr), (u32)(sizeof(arr) / sizeof *(arr)), &(next), (what))
#define POOL_PUT(p)                pool_put((p), sizeof *(p))
#define POOL_INDEX(arr, p)         ((u32)((p) - (arr)))
#endif
