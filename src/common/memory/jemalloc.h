/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_MEMORY_JEMALLOC_H
#define SNX_MEMORY_JEMALLOC_H
#include <stdint.h>
/* The host heap the allocator's caches manage: the bytes live in it now (jemalloc's stats.allocated, when the program
   links it), 0 where the allocator is not jemalloc's or says nothing. The process's resident set and the memory the
   system can still give are beside this one, in utilities/common.h (snx_footprint, snx_memory_available) */
uint64_t snx_heap_used(void);
#endif
