/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's units held in memory while the process runs (x64_cache.c): what snx_x64_object made of a unit,
   under the SHA-256 of what it made it from (snx_x64_unit_facts), so that a unit made again in the same process (an
   NRO a game loads again, a JIT unit, the launcher's next recompile) is copied rather than made. The back end makes a
   unit in about the time reading one back from a disk took, so nothing is kept on a disk. It holds at most 1% of the
   machine's memory and at most its ceiling (the setting unit_cache_ceil_mb); the units used longest ago leave first */
#ifndef SNX_RECOMPILER_X64_CACHE_H
#define SNX_RECOMPILER_X64_CACHE_H

#include "common/cpu/recompiler/x64/x64_codegen.h"

/* the ceiling in bytes, 512 MB until it is set: 0 holds nothing */
#define SNX_X64_CACHE_CEILING_DEFAULT 512000000ull
void snx_x64_cache_ceiling(SnxU64 bytes);
/* the machine's memory the 1% is of, as a test sets it: 0 for the system's reading (snx_memory_total) */
void snx_x64_cache_memory(SnxU64 bytes);
/* the most it holds now: 1% of the machine's memory, at most the ceiling; the ceiling alone where the system cannot
   tell its memory, said once */
SnxU64 snx_x64_cache_limit(void);
/* snx_x64_object through the cache: a unit it holds is copied into made, *object and *size (*reused 1, the unit now
   the one used last), else made and kept, the ones used longest ago leaving for its room. A unit larger than the
   limit is made and not kept, said once with its size and the limit. 0, or -1 with why in error (snx_x64_object's) */
int snx_x64_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options,
                          unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                          size_t error_size);

typedef struct {
    size_t units;                 /* held now */
    SnxU64 bytes, limit;          /* their objects' bytes, and the limit in effect */
    size_t hits, misses, evicted; /* since the process started or the last clear */
    size_t too_large;             /* units made and not kept, each larger than the limit */
} SnxX64CacheStats;
void snx_x64_cache_stats(SnxX64CacheStats* stats);
/* every unit let go and the counts started again: a test's */
void snx_x64_cache_clear(void);
/* the SHA-256 (FIPS 180-4) the keys are, of size bytes: here, as the recompiler's library links the system's libraries
   alone (tests/recompiler_symbols.cmake) */
void snx_x64_cache_sha256(const void* data, size_t size, unsigned char out[32]);

#endif
