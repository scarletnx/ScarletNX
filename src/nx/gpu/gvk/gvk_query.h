/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What vkGetQueryPoolResults writes into a game's memory (gvk_api.c) */
#ifndef SNX_GVK_QUERY_H
#define SNX_GVK_QUERY_H
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* The bytes from its first to past its last, for n queries of values results each, stride apart: the driver writes the
   last query's results at (n - 1) * stride whatever the size the game names. UINT64_MAX where that passes 64 bits */
static inline u64 gvk_query_bytes(u32 n, u32 values, VkQueryResultFlags flags, u64 stride) {
    if (!n) return 0;
    u64 one = ((u64)values + ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0)) *
              (flags & VK_QUERY_RESULT_64_BIT ? 8 : 4);
    if (n > 1 && stride > (UINT64_MAX - one) / (n - 1)) return UINT64_MAX;
    return (n - 1) * stride + one;
}

#endif
