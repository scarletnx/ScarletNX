/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Content-addressed cache of the result of snx_codegen_analyze / snx_nx_codegen_analyze: the SnxFunction list
   (starts, ends, addresses, leaders) produced from a program's code images, features and hints. Keyed by a SHA-256
   of an identity (analysis format version + features) plus each module's size, whole image bytes, text offsets,
   seeds, relptrs, routed, FDEs and hints.
   Parallel to unit_cache: a hit skips the dense discovery walk. Soft-fail on open so an unwritable folder never
   aborts a build. All multi-byte integers on disk are little-endian. Any change to discovery heuristics, leader
   computation, address collection, or the meaning of any key field must bump SNX_ANALYSIS_VERSION. */
#ifndef SNX_NX_ANALYSIS_CACHE_H
#define SNX_NX_ANALYSIS_CACHE_H

#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/image.h"
#include "common/platform/file.h"
#include <stddef.h>

/* Bump when the on-disk layout of a .an file or the meaning of any field in the key changes. */
#define SNX_ANALYSIS_VERSION 4

/* Trailer: "SNXANL1" (8 bytes) + SHA-256 of (key digits || payload). A function's leaders are one flag per instruction:
   the payload holds count bytes, of which leader_count are set. */
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
/* Count a build that could not form the key or the module views, so the report shows a silent disable. */
void snx_analysis_cache_setup_failed(SnxAnalysisCache* cache);

/* The keys of the unit objects a build made, kept beside its analysis so the next build can read the objects without
   emitting their IR. options identifies everything outside the analysis that changes a unit's IR. Silently ignores a
   missing, short or mismatched sidecar: a build with no keys still emits every unit. */
/* SHA-256 of a blob, as 64 hex digits, for the options digest of the unit-key sidecar. */
int snx_analysis_cache_digest(const void* data, size_t size, char out[65]);

int snx_analysis_cache_load_unit_keys(const SnxAnalysisCache* cache, const char* key, const char* options,
                                      char (*unit_keys)[65], size_t count);
int snx_analysis_cache_store_unit_keys(const SnxAnalysisCache* cache, const char* key, const char* options,
                                       char (*unit_keys)[65], size_t count);

/* The compiled module descriptor, kept with the values emit_module would have returned. A miss emits it as usual. */
/* On success *object points into *map, which the caller keeps until the object is no longer used, then unmaps. */
int snx_analysis_cache_load_descriptor(const SnxAnalysisCache* cache, const char* key, const unsigned char** object,
                                       size_t* size, size_t* wrapped, size_t* entries, size_t* variants,
                                       PFileMap* map);
int snx_analysis_cache_store_descriptor(const SnxAnalysisCache* cache, const char* key, const unsigned char* object,
                                        size_t size, size_t wrapped, size_t entries, size_t variants);

/* A stamp that the linked module on disk is the one these objects make. The caller checks the output file's size,
   time and a sample hash before skipping the link, so an edited or replaced module is never trusted. */
int snx_analysis_cache_load_link_stamp(const SnxAnalysisCache* cache, const char* key, const char* desc_key,
                                       const char* identity, char (*unit_keys)[65], size_t count,
                                       const char* output, SnxU64* size, SnxU64* mtime, char sample[65]);
int snx_analysis_cache_store_link_stamp(const SnxAnalysisCache* cache, const char* key, const char* desc_key,
                                        const char* identity, char (*unit_keys)[65], size_t count,
                                        const char* output, SnxU64 size, SnxU64 mtime, const char sample[65]);

#endif
