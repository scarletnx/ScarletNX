/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether a guest write covers a render target (vk/rt_cover.h), whose answer decides between dropping the target and
   writing it back: every byte, or a part of it; and no size that wraps */
#include <stdio.h>
#include "nx/gpu/vk/rt_cover.h"

static unsigned checks, failures;
static void check(int ok, const char* what, unsigned long long a, unsigned long long b) {
    checks++;
    if (ok) return;
    if (failures++ < 8) fprintf(stderr, "FAIL: %s: %llu against %llu\n", what, a, b);
}

int main(void) {
    /* the write is exactly the target */
    check(vk_rt_write_covers(0x80000000ull, 0x1000, 0x80000000ull, 0x1000), "a write of exactly the target covers it",
          0, 0);
    /* more than the target is still every byte of it */
    check(vk_rt_write_covers(0x80000000ull, 0x1000, 0x7FFFF000ull, 0x3000), "a write around the target covers it", 0,
          0);
    /* a byte short at either end is a part */
    check(!vk_rt_write_covers(0x80000000ull, 0x1000, 0x80000000ull, 0xFFF), "a write short of the end is a part", 0, 0);
    check(!vk_rt_write_covers(0x80000000ull, 0x1000, 0x80000001ull, 0x1000), "a write past the start is a part", 0, 0);
    check(!vk_rt_write_covers(0x80001000ull, 0x1000, 0x80000000ull, 0x1FFF), "one byte short of the end is a part", 0,
          0);
    /* before and after the target: neither reaches a byte */
    check(!vk_rt_write_covers(0x80001000ull, 0x1000, 0x80000000ull, 0x1000), "a write before the target covers none", 0,
          0);
    check(!vk_rt_write_covers(0x80000000ull, 0x1000, 0x80001000ull, 0x1000), "a write after the target covers none", 0,
          0);
    /* the top of the guest's space: neither the start nor the end may wrap */
    check(vk_rt_write_covers(0x7FFFFFFFFFFFF000ull, 0x1000, 0x7FFFFFFFFFFFF000ull, 0x1000),
          "a target at the guest's top is covered", 0, 0);
    check(!vk_rt_write_covers(0x7FFFFFFFFFFFF000ull, 0x1000, 0x7FFFFFFFFFFFF000ull, 0xFFF),
          "a write short at the guest's top is a part", 0, 0);
    check(!vk_rt_write_covers(0x7FFFFFFFFFFF0000ull, 0x20000, 0x7FFFFFFFFFFF8000ull, 0x18000),
          "a write that ends at the guest's top is a part", 0, 0);
    /* a target whose address is not the guest's is never dropped */
    check(!vk_rt_write_covers(0x8000000000000000ull, 0x1000, 0, 0xFFFFFFFFFFFFFFFFull), "a host address is not a target",
          0, 0);
    if (failures) {
        printf("%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("rt_cover: %u checks: a write covers a target only from its first byte to its last, and nothing wraps\n",
           checks);
    return 0;
}
