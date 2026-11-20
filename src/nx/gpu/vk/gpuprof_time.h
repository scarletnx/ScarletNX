/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The time between two timestamps of a queue (gpuprof.c): a timestamp is written once the queue reaches it, so the
   start of one piece of work may be read before the end of the piece recorded before it, and the counter wraps at the
   queue family's timestampValidBits */
#ifndef SNX_VK_GPUPROF_TIME_H
#define SNX_VK_GPUPROF_TIME_H
#include <stdint.h>

/* to's ticks after from's, below 0 before it, within the queue's valid bits */
static inline int64_t vk_gpuprof_ticks(uint32_t bits, uint64_t from, uint64_t to) {
    uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1, d = (to - from) & mask;
    return d > mask / 2 ? -(int64_t)((from - to) & mask) : (int64_t)d;
}
/* the nanoseconds from one timestamp to a later one, a tick being period ns: 0 when to is not after from, where the
   unsigned difference would read as the whole counter */
static inline uint64_t vk_gpuprof_ns(uint32_t bits, uint64_t from, uint64_t to, double period) {
    int64_t d = vk_gpuprof_ticks(bits, from, to);
    return d > 0 ? (uint64_t)((double)d * period) : 0;
}
#endif
