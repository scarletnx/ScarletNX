/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The tables a draw's and a dispatch's shader lookups keep their last finds in (pipeline.c's vk_shader_get_ex,
   compute.c's vk_dispatch): a key's set, of WAYS ways each. A key a set has no room for is looked up the slow way, its
   code read and hashed again (~55 us a draw), each time it comes back. With two ways in 2048 sets, 5 to 6 of one open
   world's draws a frame read their programs again, every one of them a key seen before in a full set: 0.33 ms of the
   GPU thread a frame. 64 dispatch entries of one way each read 2 dispatches' programs a frame again */
#ifndef SNX_VK_LOOKUP_WAYS_H
#define SNX_VK_LOOKUP_WAYS_H
#include "common/cpu/aarch64/cpu.h"

#define VK_SHA_SET_BITS 12 /* draws: 4096 sets */
#define VK_SHA_WAYS     4
#define VK_CSF_SETS     64 /* dispatches */
#define VK_CSF_WAYS     4

/* Fibonacci hashing (Knuth's multiplicative hashing): 2^64 divided by the golden ratio. A key times it keeps, in the
   product's top bits, a mix of every bit of the key, so the top VK_SHA_SET_BITS spread keys that differ only in a few
   bits over all the sets */
#define VK_FIB_MULT 0x9E3779B97F4A7C15ull
/* the set of a draw's key (its mix of address, key words and stage), from every bit of it: programs are 256-byte
   aligned, and a shift and xor of their addresses left most sets unused */
static inline u32 vk_sha_set(u64 hk) { return (u32)((hk * VK_FIB_MULT) >> (64 - VK_SHA_SET_BITS)); }
/* the set of a dispatch's program address */
static inline u32 vk_csf_set(u64 pa) { return (u32)(((pa >> 8) ^ (pa >> 16)) & (VK_CSF_SETS - 1)); }
#endif
