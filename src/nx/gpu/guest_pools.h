/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's memory pools, as a front end that has them makes them (NVN's memory pools): each one's range of GPU
   addresses. The renderer has no part in a pool's life, and reads them here to measure how much of each its draws
   bind as buffers. A front end without pools (a game that writes its own command buffers) adds none. */
#ifndef SNX_GPU_GUEST_POOLS_H
#define SNX_GPU_GUEST_POOLS_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u64 gva, size;
    int virtual; /* it reserves its addresses only: mapping requests fill it */
} GpuPool;

/* a pool made at gva, and the pool at gva gone. From any thread */
void gpu_pool_add(u64 gva, u64 size, int virtual);
void gpu_pool_remove(u64 gva);
/* the live pools copied to out, cap at most, in no order: how many there are */
u32 gpu_pools(GpuPool* out, u32 cap);
#endif
