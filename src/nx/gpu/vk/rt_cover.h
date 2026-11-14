/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether a guest write covers a render target's bytes, which is when the target is dropped instead of written back
   (surface.c's vk_rt_before_write), apart so tests/rt_cover.c holds it to its edges: memory is the truth only when the
   write replaces every byte, and no arithmetic here may wrap */
#ifndef SNX_VK_RT_COVER_H
#define SNX_VK_RT_COVER_H
#include <stdint.h>

/* a target of bytes at s_gva, and a guest write of [gva, gva + size): 1 when every byte of the target is written. A
   target whose address has the top bit set is not the guest's, and the comparison is done as an offset so that neither
   sum can wrap */
static inline int vk_rt_write_covers(uint64_t s_gva, uint64_t bytes, uint64_t gva, uint64_t size) {
    if (s_gva >> 63) return 0;
    if (s_gva < gva || size < bytes) return 0;
    return s_gva - gva <= size - bytes;
}
#endif
