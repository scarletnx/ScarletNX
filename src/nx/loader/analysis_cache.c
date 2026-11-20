/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/analysis_cache.h"
#include "common/cpu/aarch64/cpu.h"
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
    size_t hits, misses, stores, store_fail, setup_fail;
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
    if (cache->setup_fail)
        snx_error("analysis cache: %zu setup(s) failed in %s", cache->setup_fail, cache->folder);
}

void snx_analysis_cache_setup_failed(SnxAnalysisCache* cache) {
    if (cache) ++cache->setup_fail;
}

int snx_analysis_cache_digest(const void* data, size_t size, char out[65]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned char dig[32];
    size_t i;
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, size) != 1 || EVP_DigestFinal_ex(ctx, dig, NULL) != 1) {
        if (ctx) EVP_MD_CTX_free(ctx);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    for (i = 0; i < sizeof dig; ++i) {
        out[2 * i] = g_digits[dig[i] >> 4];
        out[2 * i + 1] = g_digits[dig[i] & 15];
    }
    out[64] = 0;
    return 0;
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
        field(ctx, &m->image_size, sizeof m->image_size);
        if (m->image_size && m->image) field(ctx, m->image, m->image_size);
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
    unsigned char *file = NULL, *payload, *block = NULL, *cursor;
    size_t size = 0, i, off, bytes;
    SnxU64 nfunc;
    SnxFunction* functions = NULL;
    FILE* f;
    unsigned char expect[32];
    char reason[256];
    if (!cache || !key || !codegen || !error) return -1;
    if (codegen->function_count || codegen->functions) {
        snprintf(error, error_size, "codegen is not empty");
        return -1;
    }
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
    if (check_hash(key, payload, size, expect) || memcmp(expect, file + size + 8, 32) != 0) {
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
    bytes = (size_t)nfunc * sizeof(SnxFunction);
    if (size > SIZE_MAX - bytes - 1) {
        free(file);
        cache->misses++;
        snprintf(error, error_size, "analysis payload is too large");
        return -1;
    }
    block = (unsigned char*)snx_alloc(bytes + size + 1);
    if (!block) {
        free(file);
        cache->misses++;
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    functions = (SnxFunction*)block;
    cursor = block + bytes;
    memset(functions, 0, bytes);
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
        if (count > 10000000ull || (alen && alen != count) || leader_count > count) goto truncated;
        fn->start = start;
        fn->end = end;
        fn->count = (size_t)count;
        fn->leader_count = (size_t)leader_count;
        if (alen) {
            size_t j;
            if (fn->count > (size - off) / 8) goto truncated;
            fn->addresses = (SnxU64*)cursor;
            cursor += fn->count * sizeof(SnxU64);
            for (j = 0; j < fn->count; ++j) {
                fn->addresses[j] = get64(payload + off);
                off += 8;
            }
        }
        if (leader_count) {
            if (fn->count > size - off) goto truncated;
            fn->leaders = cursor;
            cursor += fn->count;
            memcpy(fn->leaders, payload + off, fn->count);
            off += fn->count;
        }
    }
    free(file);
    codegen->functions = functions;
    codegen->arrays_block = block;
    codegen->function_count = (size_t)nfunc;
    codegen->function_capacity = (size_t)nfunc;
    codegen->instruction_count = 0;
    for (i = 0; i < (size_t)nfunc; ++i) codegen->instruction_count += functions[i].count;
    cache->hits++;
    return 0;

truncated:
    free(block);
    free(file);
    cache->misses++;
    snprintf(error, error_size, "truncated analysis payload");
    return -1;
}

/* The unit-key sidecar: the options digest, the count, then one 64-character key per unit. A key that does not name a
   whole object only costs a miss, so no hash is needed: the options digest rejects a sidecar from another build. */
int snx_analysis_cache_load_unit_keys(const SnxAnalysisCache* cache, const char* key, const char* options,
                                      char (*unit_keys)[65], size_t count) {
    char *name, *path;
    unsigned char* file;
    size_t size = 0, i;
    int rc = -1;
    if (!cache || !key || !options || !unit_keys) return -1;
    name = snx_format("%s.units", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    if (!path) return -1;
    file = snx_read_file(path, &size);
    free(path);
    if (!file) return -1;
    if (size >= 72 && !memcmp(file, options, 64) && get64(file + 64) == count && size >= 72 + count * 64) {
        for (i = 0; i < count; ++i) memcpy(unit_keys[i], file + 72 + i * 64, 64);
        rc = 0;
    }
    free(file);
    return rc;
}

int snx_analysis_cache_store_unit_keys(const SnxAnalysisCache* cache, const char* key, const char* options,
                                       char (*unit_keys)[65], size_t count) {
    char *name, *path, *tmp;
    unsigned char* file;
    size_t size = 72 + count * 64, i;
    FILE* f;
    int rc = -1, written;
    if (!cache || !key || !options || !unit_keys) return -1;
    file = (unsigned char*)snx_alloc(size);
    if (!file) return -1;
    memcpy(file, options, 64);
    put64(file + 64, (SnxU64)count);
    for (i = 0; i < count; ++i) {
        if (!unit_keys[i][0] || strlen(unit_keys[i]) != 64) goto done;
        memcpy(file + 72 + i * 64, unit_keys[i], 64);
    }
    name = snx_format("%s.units", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    tmp = path ? snx_format("%s.%lu.tmp", path, process_id()) : NULL;
    if (!tmp) {
        free(path);
        goto done;
    }
    f = snx_fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        free(path);
        goto done;
    }
    written = fwrite(file, 1, size, f) == size;
    if (fclose(f)) written = 0;
    if (written && !plat_replace_file(tmp, path)) rc = 0;
    if (rc) {
        snx_error("analysis cache: cannot keep the unit keys of %s in %s", key, path);
        if (snx_path_exists(tmp)) snx_remove_tree(tmp);
    }
    free(tmp);
    free(path);
done:
    free(file);
    return rc;
}

/* The descriptor file: magic, the descriptor key, wrapped, entries, object size, the object, then its hash. */
static const unsigned char g_descriptor_magic[8] = {'S', 'N', 'X', 'D', 'S', 'C', 'R', '1'};

int snx_analysis_cache_load_descriptor(const SnxAnalysisCache* cache, const char* key, const unsigned char** object,
                                       size_t* size, size_t* wrapped, size_t* entries, size_t* variants,
                                       PFileMap* map) {
    const unsigned char* file;
    unsigned char hash[32];
    char *name, *path;
    size_t file_size, object_size, off;
    int rc = -1;
    if (!cache || !key || !object || !size || !wrapped || !entries || !variants || !map) return -1;
    *map = (PFileMap){0};
    name = snx_format("%s.desc", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    if (!path) return -1;
    if (plat_file_map(map, path)) {
        free(path);
        return -1;
    }
    free(path);
    file = map->data;
    file_size = (size_t)map->size;
    if (!file || file_size < 8 + 64 + 32 + 32) goto done;
    if (memcmp(file, g_descriptor_magic, 8) || memcmp(file + 8, key, 64)) goto done;
    off = 8 + 64;
    *wrapped = (size_t)get64(file + off);
    off += 8;
    *entries = (size_t)get64(file + off);
    off += 8;
    *variants = (size_t)get64(file + off);
    off += 8;
    object_size = (size_t)get64(file + off);
    off += 8;
    if (object_size > file_size - off - 32) goto done;
    if (check_hash(key, file + 8 + 64, off - (8 + 64) + object_size, hash) ||
        memcmp(hash, file + off + object_size, 32))
        goto done;
    *object = file + off;
    *size = object_size;
    rc = 0;
done:
    if (rc != 0) plat_file_unmap(map);
    return rc;
}

int snx_analysis_cache_store_descriptor(const SnxAnalysisCache* cache, const char* key, const unsigned char* object,
                                        size_t size, size_t wrapped, size_t entries, size_t variants) {
    char *name, *path, *tmp;
    unsigned char *file, hash[32];
    size_t total = 8 + 64 + 32 + size + 32, off = 8 + 64;
    FILE* f;
    int written, rc = -1;
    if (!cache || !key || !object) return -1;
    file = (unsigned char*)snx_alloc(total);
    if (!file) return -1;
    memcpy(file, g_descriptor_magic, 8);
    memcpy(file + 8, key, 64);
    put64(file + off, (SnxU64)wrapped);
    off += 8;
    put64(file + off, (SnxU64)entries);
    off += 8;
    put64(file + off, (SnxU64)variants);
    off += 8;
    put64(file + off, (SnxU64)size);
    off += 8;
    memcpy(file + off, object, size);
    off += size;
    if (check_hash(key, file + 8 + 64, off - (8 + 64), hash)) goto done;
    memcpy(file + off, hash, 32);
    name = snx_format("%s.desc", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    tmp = path ? snx_format("%s.%lu.tmp", path, process_id()) : NULL;
    if (!tmp) {
        free(path);
        goto done;
    }
    f = snx_fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        free(path);
        goto done;
    }
    written = fwrite(file, 1, total, f) == total;
    if (fclose(f)) written = 0;
    if (written && !plat_replace_file(tmp, path)) rc = 0;
    if (rc && snx_path_exists(tmp)) snx_remove_tree(tmp);
    free(tmp);
    free(path);
done:
    free(file);
    return rc;
}

#define SNX_LINK_STAMP_VERSION 1
static const unsigned char g_link_magic[8] = {'S', 'N', 'X', 'L', 'N', 'K', '1', 0};

int snx_analysis_cache_load_link_stamp(const SnxAnalysisCache* cache, const char* key, const char* desc_key,
                                       const char* identity, char (*unit_keys)[65], size_t count,
                                       const char* output, SnxU64* size, SnxU64* mtime, char sample[65]) {
    unsigned char *file, hash[32];
    char *name, *path;
    size_t file_size, off = 0, length, i;
    int rc = -1;
    if (!cache || !key || !desc_key || !identity || !output || !size || !mtime || !sample ||
        (count && !unit_keys))
        return -1;
    name = snx_format("%s.link", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    if (!path) return -1;
    file = snx_read_file(path, &file_size);
    free(path);
    if (!file || file_size < 16 + 8 + 64 + 64 + 8 + 8 + 8 + 8 + 64 + 32) goto done;
    if (memcmp(file, g_link_magic, 8) || get64(file + 8) != SNX_LINK_STAMP_VERSION ||
        get64(file + 16) != (SnxU64)SNX_ABI || memcmp(file + 24, desc_key, 64) ||
        memcmp(file + 88, identity, 64) || get64(file + 152) != count)
        goto done;
    off = 160;
    for (i = 0; i < count; ++i) {
        if (off + 64 > file_size || memcmp(file + off, unit_keys[i], 64)) goto done;
        off += 64;
    }
    if (off + 8 > file_size) goto done;
    length = (size_t)get64(file + off);
    off += 8;
    if (length > file_size - off || length != strlen(output) || memcmp(file + off, output, length)) goto done;
    off += length;
    if (off + 8 + 8 + 32 + 32 > file_size) goto done;
    *size = get64(file + off);
    off += 8;
    *mtime = get64(file + off);
    off += 8;
    memcpy(sample, file + off, 64);
    sample[64] = 0;
    off += 64;
    if (check_hash(key, file, off, hash) || memcmp(hash, file + off, 32)) goto done;
    rc = 0;
done:
    free(file);
    return rc;
}

int snx_analysis_cache_store_link_stamp(const SnxAnalysisCache* cache, const char* key, const char* desc_key,
                                        const char* identity, char (*unit_keys)[65], size_t count,
                                        const char* output, SnxU64 size, SnxU64 mtime, const char sample[65]) {
    unsigned char *file, hash[32];
    char *name, *path, *tmp;
    size_t total, off = 0, length, i;
    FILE* f;
    int written, rc = -1;
    if (!cache || !key || !desc_key || !identity || !output || !sample || (count && !unit_keys)) return -1;
    length = strlen(output);
    total = 16 + 8 + 64 + 64 + 8 + count * 64 + 8 + length + 8 + 8 + 64 + 32;
    file = (unsigned char*)snx_alloc(total);
    if (!file) return -1;
    memcpy(file, g_link_magic, 8);
    put64(file + 8, SNX_LINK_STAMP_VERSION);
    put64(file + 16, (SnxU64)SNX_ABI);
    memcpy(file + 24, desc_key, 64);
    memcpy(file + 88, identity, 64);
    put64(file + 152, (SnxU64)count);
    off = 160;
    for (i = 0; i < count; ++i) {
        memcpy(file + off, unit_keys[i], 64);
        off += 64;
    }
    put64(file + off, (SnxU64)length);
    off += 8;
    memcpy(file + off, output, length);
    off += length;
    put64(file + off, size);
    off += 8;
    put64(file + off, mtime);
    off += 8;
    memcpy(file + off, sample, 64);
    off += 64;
    if (check_hash(key, file, off, hash)) goto done;
    memcpy(file + off, hash, 32);
    off += 32;
    name = snx_format("%s.link", key);
    path = snx_path_join(cache->folder, name);
    free(name);
    tmp = path ? snx_format("%s.%lu.tmp", path, process_id()) : NULL;
    if (!tmp) {
        free(path);
        goto done;
    }
    f = snx_fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        free(path);
        goto done;
    }
    written = fwrite(file, 1, total, f) == total;
    if (fclose(f)) written = 0;
    if (written && off == total && !plat_replace_file(tmp, path)) rc = 0;
    if (rc && snx_path_exists(tmp)) snx_remove_tree(tmp);
    free(tmp);
    free(path);
done:
    free(file);
    return rc;
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
        capacity += 40 + (fn->addresses ? fn->count * 8 : 0);
        /* leaders is one flag per instruction: count bytes, of which leader_count are set. */
        if (fn->leader_count) capacity += fn->count;
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
            memcpy(payload + used, fn->leaders, fn->count);
            used += fn->count;
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
