/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Content-addressed cache of the result of snx_codegen_analyze / snx_nx_codegen_analyze: the SnxFunction list
   (starts, ends, addresses, leaders) produced from a program's code images, features and hints. Keyed by a SHA-256
   of an identity (analysis format version + features) plus the modules' text, seeds, routed, FDEs and hints.
   Parallel to unit_cache: a hit skips the dense discovery walk. Soft-fail on open so an unwritable folder never
   aborts a build. All multi-byte integers on disk are little-endian. Any change to discovery heuristics, leader
   computation, address collection, or the meaning of any key field must bump SNX_ANALYSIS_VERSION. */
#ifndef SNX_NX_ANALYSIS_CACHE_H
#define SNX_NX_ANALYSIS_CACHE_H

#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/image.h"
#include <stddef.h>

/* Bump when the on-disk layout of a .an file or the meaning of any field in the key changes. */
#define SNX_ANALYSIS_VERSION 2

/* Trailer: "SNXANL1" (8 bytes) + SHA-256 of (key digits || payload). */
#define SNX_ANALYSIS_TRAILER 40

typedef struct SnxAnalysisCache SnxAnalysisCache;

typedef struct {
    SnxU64 features;
    unsigned version; /* SNX_ANALYSIS_VERSION */
} SnxAnalysisIdentity;

/* Open the cache in folder (created if needed). Soft-fail: returns NULL with reason in error; caller must continue
   without the cache. identity is hashed into every key. */
SnxAnalysisCache* snx_analysis_cache_open(const char* folder, const SnxAnalysisIdentity* identity, char* error,
                                          size_t error_size);
void snx_analysis_cache_close(SnxAnalysisCache* cache);

int snx_analysis_cache_key(const SnxAnalysisCache* cache, const SnxCodeImage* modules, size_t module_count,
                           const SnxU64* hints, size_t hint_count, char key[65]);

/* Load into an empty codegen. On success codegen owns the functions allocations exactly as snx_codegen_analyze
   would. Partial allocations are never left on failure. 0 hit, -1 miss or error. */
int snx_analysis_cache_load(SnxAnalysisCache* cache, const char* key, SnxCodegen* codegen, char* error,
                            size_t error_size);

int snx_analysis_cache_store(SnxAnalysisCache* cache, const char* key, const SnxCodegen* codegen, char* error,
                             size_t error_size);

void snx_analysis_cache_report(SnxAnalysisCache* cache, int quiet);

#endif
