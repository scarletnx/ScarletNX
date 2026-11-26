/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT's cache file. Later runtime sessions load compatible units from it (jit_service.c). The AOT
   build can also link its objects into the game module and put its metadata in the module's variant table. */
#ifndef SNX_JIT_CACHE_H
#define SNX_JIT_CACHE_H
#include "common/cpu/recompiler/variants.h"
#include "common/utilities/common.h"
#include <stddef.h>

#define SNX_JIT_CACHE_MAGIC  "SNXJIT01"
#define SNX_JIT_RECORD_MAGIC 0x524A4E53u         /* "SNJR" */
#define SNX_JIT_CACHE_MAX    ((size_t)256 << 20) /* a larger file is refused */

typedef struct SnxJitCache SnxJitCache;

typedef struct {
    SnxU64 start;
    size_t leader_count;
    const SnxU64* leaders;
    size_t run_count;
    const SnxVariantRun* runs;
    SnxU64 h1, h2;
} SnxJitCacheFunction;

typedef struct {
    const char* name;
    size_t function_count;
    const SnxJitCacheFunction* functions;
    const unsigned char* object;
    size_t object_size;
} SnxJitCacheRecord;

/* What the objects depend on beyond the code: the ABI, the IR object version,
   the host target triple, LLVM, the runtime library, the host CPU and its
   features, the -O level, hooked direct calls, direct SVCs, SLP vectorization,
   FPSR flag exactness, volatile guest accesses, the guest's A64 features (0: the
   Switch's) and the decoder's tables. The profile run and the AOT build must
   pass the same. */
char* snx_jit_cache_key(char opt, int hook, int svc, SnxU64 features);

/* Opens and validates the whole file: a wrong key, a corrupt record, or a
   trailing unfinished record returns NULL with the reason in error. An empty
   cache (the header and no records) opens with count 0. */
SnxJitCache* snx_jit_cache_open(const char* path, const char* key, char* error, size_t error_size);
void snx_jit_cache_close(SnxJitCache* cache);
size_t snx_jit_cache_count(const SnxJitCache* cache);
const SnxJitCacheRecord* snx_jit_cache_record(const SnxJitCache* cache, size_t index);

#endif
