/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* memory/own.c for tests of services that hand guest memory to the kernel (bsd.c, fs.c): nothing is GPU-owned here */
#include "nx/memory/own.h"

int g_own_any;
void own_acquire(u64 addr, u64 size) {
    (void)addr;
    (void)size;
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
