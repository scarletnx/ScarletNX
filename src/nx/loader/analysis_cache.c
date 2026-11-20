/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/analysis_cache.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static const unsigned char g_trailer_magic[8] = {'S', 'N', 'X', 'A', 'N', 'L', '1', 0};
static const char g_digits[] = "0123456789abcdef";

struct SnxAnalysisCache {
    SnxAnalysisIdentity identity;
    size_t hits, misses, stores, store_fail;
    char folder[4096];
};

static void put64(unsigned char* p, SnxU64 v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

static SnxU64 get64(const unsigned char* p) {
    SnxU64 v = 0;
    int i;
    for (i = 0; i < 8; ++i) v |= (SnxU64)p[i] << (8 * i);
    return v;
}

static int field(EVP_MD_CTX* ctx, const void* data, size_t size) {
    unsigned char length[8];
    put64(length, size);
    return EVP_DigestUpdate(ctx, length, sizeof length) == 1 && (!size || EVP_DigestUpdate(ctx, data, size) == 1);
}

static void free_functions(SnxFunction* functions, size_t count) {
    size_t i;
    if (!functions) return;
    for (i = 0; i < count; ++i) {
        free(functions[i].addresses);
        free(functions[i].leaders);
        functions[i].addresses = NULL;
        functions[i].leaders = NULL;
    }
    free(functions);
}

static int check_hash(const char* key, const unsigned char* payload, size_t size, unsigned char out[32]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        if (ctx) EVP_MD_CTX_free(ctx);
        return -1;
    }
    if (EVP_DigestUpdate(ctx, key, 64) != 1 || (size && EVP_DigestUpdate(ctx, payload, size) != 1) ||
        EVP_DigestFinal_ex(ctx, out, NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    return 0;
}

static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

SnxAnalysisCache* snx_analysis_cache_open(const char* folder, const SnxAnalysisIdentity* identity, char* error,
                                          size_t error_size) {
    SnxAnalysisCache* cache;
    char reason[256];
    if (!folder || !identity) {
        snprintf(error, error_size, "missing folder or identity");
        return NULL;
    }
    if (snx_mkdirs(folder)) {
        snprintf(error, error_size, "cannot make the folder %s: %s", folder, snx_os_error(reason, sizeof reason));
        return NULL;
    }
    cache = (SnxAnalysisCache*)snx_alloc(sizeof *cache);
    if (!cache) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    memset(cache, 0, sizeof *cache);
    cache->identity = *identity;
    snprintf(cache->folder, sizeof cache->folder, "%s", folder);
    return cache;
}

void snx_analysis_cache_close(SnxAnalysisCache* cache) {
    free(cache);
}

void snx_analysis_cache_report(SnxAnalysisCache* cache, int quiet) {
    if (!cache || quiet) return;
    if (cache->hits || cache->misses || cache->stores)
        printf("analysis cache: %zu hit(s), %zu miss(es), %zu stored in %s\n", cache->hits, cache->misses,
               cache->stores, cache->folder);
    if (cache->store_fail)
        snx_error("analysis cache: %zu store(s) failed in %s", cache->store_fail, cache->folder);
}

int snx_analysis_cache_key(const SnxAnalysisCache* cache, const SnxCodeImage* modules, size_t module_count,
                           const SnxU64* hints, size_t hint_count, char key[65]) {
    EVP_MD_CTX* ctx;
    unsigned char dig[32];
    size_t i;
    if (!cache || !key) return -1;
    ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        if (ctx) EVP_MD_CTX_free(ctx);
        return -1;
    }
    field(ctx, "snx-analysis-cache", 18);
    field(ctx, &cache->identity.version, sizeof cache->identity.version);
    field(ctx, &cache->identity.features, sizeof cache->identity.features);
    field(ctx, &module_count, sizeof module_count);
    for (i = 0; i < module_count; ++i) {
        const SnxCodeImage* m = &modules[i];
        field(ctx, &m->base, sizeof m->base);
        field(ctx, &m->text_off, sizeof m->text_off);
        field(ctx, &m->text_size, sizeof m->text_size);
        if (m->text_size && m->image) field(ctx, m->image + m->text_off, m->text_size);
        field(ctx, m->functions, m->function_count * sizeof(SnxU64));
        field(ctx, m->relptrs, m->relptr_count * sizeof(SnxU64));
        field(ctx, m->routed, m->routed_count * sizeof(SnxU64));
        field(ctx, m->fde, m->fde_count * sizeof(SnxFde));
    }
    field(ctx, hints, hint_count * sizeof(SnxU64));
    if (EVP_DigestFinal_ex(ctx, dig, NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    for (i = 0; i < 32; ++i) {
        key[2 * i] = g_digits[dig[i] >> 4];
        key[2 * i + 1] = g_digits[dig[i] & 15];
    }
    key[64] = 0;
    return 0;
}

int snx_analysis_cache_load(SnxAnalysisCache* cache, const char* key, SnxCodegen* codegen, char* error,
                            size_t error_size) {
    char *name = NULL, *path = NULL;
    unsigned char *file = NULL, *payload;
    size_t size = 0, i, off;
    SnxU64 nfunc;
    SnxFunction* functions = NULL;
    FILE* f;
    unsigned char expect[32];
    char reason[256];
    if (!cache || !key || !codegen || !error) return -1;
    name = snx_format("%s.an", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    if (!path) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    f = snx_fopen(path, "rb");
    if (!f) {
        free(path);
        cache->misses++;
        snprintf(error, error_size, "no analysis for %s", key);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) || (size = (size_t)ftell(f)) == (size_t)-1 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        free(path);
        cache->misses++;
        snprintf(error, error_size, "cannot size %s", path);
        return -1;
    }
    if (size < SNX_ANALYSIS_TRAILER + 8) {
        fclose(f);
        free(path);
        cache->misses++;
        snprintf(error, error_size, "%zu bytes, too short for trailer", size);
        return -1;
    }
    file = (unsigned char*)snx_alloc(size);
    if (!file || fread(file, 1, size, f) != size) {
        free(file);
        fclose(f);
        free(path);
        cache->misses++;
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(reason, sizeof reason));
        return -1;
    }
    fclose(f);
    free(path);
    if (memcmp(file + size - SNX_ANALYSIS_TRAILER, g_trailer_magic, 8) != 0) {
        free(file);
        cache->misses++;
        snprintf(error, error_size, "%zu bytes, which do not end in the SNXANL1 check", size);
        return -1;
    }
    payload = file;
    size -= SNX_ANALYSIS_TRAILER;
    if (check_hash(key, payload, size, expect) || memcmp(expect, file + size, 32) != 0) {
        free(file);
        cache->misses++;
        snprintf(error, error_size, "analysis payload hash mismatch");
        return -1;
    }
    nfunc = get64(payload);
    off = 8;
    if (nfunc > 1000000ull) {
        free(file);
        cache->misses++;
        snprintf(error, error_size, "implausible function count %llu", (unsigned long long)nfunc);
        return -1;
    }
    functions = (SnxFunction*)snx_alloc((size_t)nfunc * sizeof(SnxFunction));
    if (!functions && nfunc) {
        free(file);
        cache->misses++;
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    memset(functions, 0, (size_t)nfunc * sizeof(SnxFunction));
    for (i = 0; i < (size_t)nfunc; ++i) {
        SnxFunction* fn = &functions[i];
        SnxU64 start, end, count, leader_count, alen;
        if (off + 40 > size) goto truncated;
        start = get64(payload + off);
        off += 8;
        end = get64(payload + off);
        off += 8;
        count = get64(payload + off);
        off += 8;
        leader_count = get64(payload + off);
        off += 8;
        alen = get64(payload + off);
        off += 8;
        if (count > 10000000ull || (alen && alen != count)) goto truncated;
        fn->start = start;
        fn->end = end;
        fn->count = (size_t)count;
        fn->leader_count = (size_t)leader_count;
        if (alen) {
            size_t j;
            fn->addresses = (SnxU64*)snx_alloc(fn->count * sizeof(SnxU64));
            if (!fn->addresses) goto oom;
            if (off + fn->count * 8 > size) goto truncated;
            for (j = 0; j < fn->count; ++j) {
                fn->addresses[j] = get64(payload + off);
                off += 8;
            }
        }
        if (leader_count) {
            fn->leaders = (unsigned char*)snx_alloc(fn->leader_count);
            if (!fn->leaders) goto oom;
            if (off + fn->leader_count > size) goto truncated;
            memcpy(fn->leaders, payload + off, fn->leader_count);
            off += fn->leader_count;
        }
    }
    free(file);
    codegen->functions = functions;
    codegen->function_count = (size_t)nfunc;
    codegen->function_capacity = (size_t)nfunc;
    codegen->instruction_count = 0;
    for (i = 0; i < (size_t)nfunc; ++i) codegen->instruction_count += functions[i].count;
    cache->hits++;
    return 0;

truncated:
    free_functions(functions, i);
    free(file);
    cache->misses++;
    snprintf(error, error_size, "truncated analysis payload");
    return -1;
oom:
    free_functions(functions, i + 1);
    free(file);
    cache->misses++;
    snprintf(error, error_size, "out of memory");
    return -1;
}

int snx_analysis_cache_store(SnxAnalysisCache* cache, const char* key, const SnxCodegen* codegen, char* error,
                             size_t error_size) {
    char *name = NULL, *path = NULL, *tmp = NULL;
    unsigned char *payload = NULL, trailer[SNX_ANALYSIS_TRAILER];
    size_t capacity = 8, used = 0, i, j;
    FILE* f = NULL;
    SnxU64 nfunc;
    char reason[256];
    int rc = -1, written;
    if (!cache || !key || !codegen) return -1;
    nfunc = codegen->function_count;
    for (i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* fn = &codegen->functions[i];
        capacity += 40 + (fn->addresses ? fn->count * 8 : 0) + fn->leader_count;
    }
    payload = (unsigned char*)snx_alloc(capacity);
    if (!payload) {
        snprintf(error, error_size, "out of memory");
        cache->store_fail++;
        return -1;
    }
    put64(payload + used, nfunc);
    used += 8;
    for (i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* fn = &codegen->functions[i];
        SnxU64 alen = fn->addresses ? (SnxU64)fn->count : 0;
        put64(payload + used, fn->start);      used += 8;
        put64(payload + used, fn->end);        used += 8;
        put64(payload + used, (SnxU64)fn->count);        used += 8;
        put64(payload + used, (SnxU64)fn->leader_count); used += 8;
        put64(payload + used, alen);           used += 8;
        for (j = 0; j < (size_t)alen; ++j) {
            put64(payload + used, fn->addresses[j]);
            used += 8;
        }
        if (fn->leader_count) {
            memcpy(payload + used, fn->leaders, fn->leader_count);
            used += fn->leader_count;
        }
    }
    memcpy(trailer, g_trailer_magic, 8);
    if (check_hash(key, payload, used, trailer + 8)) {
        free(payload);
        snprintf(error, error_size, "cannot hash analysis");
        cache->store_fail++;
        return -1;
    }
    name = snx_format("%s.an", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    if (!path) {
        free(payload);
        snprintf(error, error_size, "out of memory");
        cache->store_fail++;
        return -1;
    }
    tmp = snx_format("%s.%lu.tmp", path, process_id());
    if (!tmp) {
        free(payload);
        free(path);
        snprintf(error, error_size, "out of memory");
        cache->store_fail++;
        return -1;
    }
    f = snx_fopen(tmp, "wb");
    if (!f) {
        snprintf(error, error_size, "cannot write %s: %s", tmp, snx_os_error(reason, sizeof reason));
        goto fail;
    }
    written = fwrite(payload, 1, used, f) == used && fwrite(trailer, 1, sizeof trailer, f) == sizeof trailer;
    if (!written) snx_os_error(reason, sizeof reason);
    if (fclose(f) && written) {
        written = 0;
        snx_os_error(reason, sizeof reason);
    }
    f = NULL;
    if (!written) {
        snprintf(error, error_size, "cannot write %s: %s", tmp, reason);
        goto fail;
    }
    if (plat_replace_file(tmp, path)) {
        snprintf(error, error_size, "cannot rename %s to %s: %s", tmp, path, snx_os_error(reason, sizeof reason));
        goto fail;
    }
    rc = 0;
    cache->stores++;
fail:
    if (rc) {
        cache->store_fail++;
        if (snx_path_exists(tmp)) snx_remove_tree(tmp);
    }
    free(payload);
    free(tmp);
    free(path);
    return rc;
}
