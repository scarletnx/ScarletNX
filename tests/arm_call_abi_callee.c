/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime library's functions a native AArch32 block calls (the real ones) and a runtime's callbacks behind them,
   as a compiler makes them for a machine the back ends make code of: arm_x64.c's --callee runs the blocks against this
   object under Unicorn. What a compiled function takes a narrow argument's upper bits for is the machine's ABI's, which
   a host function called in its place cannot show */
#include "common/cpu/arm/rt_helpers.c"
#include "support/arm_rt_callbacks.h"

/* no library is under the object: volatile, so that the compiler makes no call of the function it is */
void* memcpy(void* restrict to, const void* restrict from, size_t size) {
    volatile unsigned char* t = to;
    const volatile unsigned char* f = from;
    for (size_t k = 0; k < size; ++k) t[k] = f[k];
    return to;
}
