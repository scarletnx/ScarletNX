/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* memory/own.c for tests of services that hand guest memory to the kernel (bsd.c, fs.c): nothing is GPU-owned here */
#include <string.h>
#include "nx/memory/own.h"
#include "nx/memory/mem.h"

int g_own_any;
void own_acquire(u64 addr, u64 size) {
    (void)addr;
    (void)size;
}
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    (void)addr;
    (void)size;
    (void)out;
    (void)max;
    return 0;
}
/* the GPU's reads of guest memory (gpu.c): every byte the CPU's */
u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max) {
    (void)out;
    (void)max;
    memcpy(dst, GP(addr), (size_t)size);
    return 0;
}
/* platform/plat_posix.c: no page here is protected for the write tracking */
void plat_ww_open(u8* p, u64 size) {
    (void)p;
    (void)size;
}
void plat_ww_close(u8* p, u64 size) {
    (void)p;
    (void)size;
}
