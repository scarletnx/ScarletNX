/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C library's functions the game exports (hooks.hook), on guest memory: a flat mapping at c->mem, so a guest
   range is the host range at the same offset. The functions' behavior is the C standard's, which leaves nothing for
   the guest's own code to differ on but a copy between overlapping ranges, undefined for memcpy: memmove gives the
   bytes as they were before the copy, which is what a game that overlaps them (by mistake) most plausibly meant.
   A range the game hasn't mapped faults here as it would in the guest's code (crash.c reports it) */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"

u64 hook_memmove(CPU* c, u64 dst, u64 src, u64 size) {
    if (size) memmove(c->mem + dst, c->mem + src, (size_t)size);
    return dst;
}

u64 hook_memset(CPU* c, u64 dst, i32 value, u64 size) {
    if (size) memset(c->mem + dst, value, (size_t)size); /* the value as an unsigned char, as both take it */
    return dst;
}
