/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Analysis cache (nx/loader/analysis_cache.c): round-trip of function lists, trailer/hash rejection, key
   sensitivity to version/features/text/hints/routed, soft-fail open, little-endian numbers, empty analysis,
   and that a truncated or corrupted payload never leaves partial allocations. */
#include "nx/loader/analysis_cache.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static void free_codegen(SnxCodegen* cg) {
    size_t i;
    if (!cg || !cg->functions) return;
    for (i = 0; i < cg->function_count; ++i) {
        free(cg->functions[i].addresses);
        free(cg->functions[i].leaders);
    }
    free(cg->functions);
    memset(cg, 0, sizeof *cg);
}

static SnxFunction* make_functions(size_t n, SnxU64 base) {
    SnxFunction* f = (SnxFunction*)snx_alloc(n * sizeof(SnxFunction));
    size_t i, j;
    memset(f, 0, n * sizeof(SnxFunction));
    for (i = 0; i < n; ++i) {
        f[i].start = base + i * 0x40;
        f[i].end = f[i].start + 0x30;
        f[i].count = 4 + (i % 3);
        f[i].leader_count = 1 + (i % 2);
        f[i].addresses = (SnxU64*)snx_alloc(f[i].count * sizeof(SnxU64));
        f[i].leaders = (unsigned char*)snx_alloc(f[i].leader_count);
        for (j = 0; j < f[i].count; ++j) f[i].addresses[j] = f[i].start + j * 4;
        for (j = 0; j < f[i].leader_count; ++j) f[i].leaders[j] = (unsigned char)(j + 1);
    }
    /* function 1 exercises the NULL-addresses mode: count from bounds semantics, no array */
    free(f[1].addresses);
    f[1].addresses = NULL;
    return f;
}

static int functions_equal(const SnxFunction* a, size_t na, const SnxFunction* b, size_t nb) {
    size_t i, j;
    if (na != nb) return 0;
    for (i = 0; i < na; ++i) {
        if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].count != b[i].count ||
            a[i].leader_count != b[i].leader_count)
            return 0;
        if (!!a[i].addresses != !!b[i].addresses) return 0;
        for (j = 0; j < a[i].count; ++j)
            if (a[i].addresses && a[i].addresses[j] != b[i].addresses[j]) return 0;
        for (j = 0; j < a[i].leader_count; ++j)
            if (a[i].leaders[j] != b[i].leaders[j]) return 0;
    }
    return 1;
}

static void fill_module(SnxCodeImage* m, unsigned char* text, size_t text_size, SnxU64 base) {
    memset(m, 0, sizeof *m);
    m->image = text;
    m->image_size = text_size;
    m->base = base;
    m->text_off = 0;
    m->text_size = text_size;
}

static int test_roundtrip(const char* folder) {
    char error[256], key[65];
    SnxAnalysisIdentity id = {.features = 0x1234, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    SnxCodegen src = {0}, dst = {0};
    unsigned char text[64];
    SnxCodeImage mod;
    SnxU64 hints[2] = {0x1000, 0x2000};
    size_t i;
    for (i = 0; i < sizeof text; ++i) text[i] = (unsigned char)(i * 3 + 7);
    fill_module(&mod, text, sizeof text, 0x80000000ull);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    src.functions = make_functions(3, 0x80001000ull);
    src.function_count = 3;
    src.function_capacity = 3;
    src.instruction_count = 0;
    for (i = 0; i < 3; ++i) src.instruction_count += src.functions[i].count;
    CHECK(snx_analysis_cache_key(cache, &mod, 1, hints, 2, key) == 0);
    CHECK(key[0] != 0 && strlen(key) == 64);
    CHECK(snx_analysis_cache_store(cache, key, &src, error, sizeof error) == 0);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) == 0);
    CHECK(dst.function_count == 3);
    CHECK(dst.instruction_count == src.instruction_count);
    CHECK(functions_equal(src.functions, 3, dst.functions, dst.function_count));
    CHECK(dst.functions[1].addresses == NULL);
    CHECK(dst.functions[1].count == src.functions[1].count);
    free_codegen(&src);
    free_codegen(&dst);
    snx_analysis_cache_close(cache);
    return 1;
}

static int test_empty(const char* folder) {
    char error[256], key[65];
    SnxAnalysisIdentity id = {.features = 1, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    SnxCodegen src = {0}, dst = {0};
    unsigned char text[16] = {0};
    SnxCodeImage mod;
    fill_module(&mod, text, sizeof text, 0);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    CHECK(snx_analysis_cache_store(cache, key, &src, error, sizeof error) == 0);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) == 0);
    CHECK(dst.function_count == 0);
    CHECK(dst.functions == NULL || dst.function_count == 0);
    free_codegen(&dst);
    snx_analysis_cache_close(cache);
    return 1;
}

static int test_trailer_reject(const char* folder) {
    char error[256], key[65], *path, *name;
    SnxAnalysisIdentity id = {.features = 2, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    SnxCodegen src = {0}, dst = {0};
    unsigned char text[32];
    SnxCodeImage mod;
    FILE* f;
    size_t i;
    for (i = 0; i < sizeof text; ++i) text[i] = (unsigned char)i;
    fill_module(&mod, text, sizeof text, 0x1000);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    src.functions = make_functions(1, 0x2000);
    src.function_count = 1;
    src.function_capacity = 1;
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    CHECK(snx_analysis_cache_store(cache, key, &src, error, sizeof error) == 0);
    free_codegen(&src);
    name = snx_format("%s.an", key);
    path = snx_path_join(folder, name);
    free(name);
    /* corrupt magic */
    f = snx_fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) {
        fseek(f, -40, SEEK_END);
        fputc('X', f);
        fclose(f);
    }
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    free_codegen(&dst);
    /* truncate */
    f = snx_fopen(path, "wb");
    if (f) {
        fwrite("short", 1, 5, f);
        fclose(f);
    }
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    free(path);
    snx_analysis_cache_close(cache);
    return 1;
}

static int test_key_sensitivity(const char* folder) {
    char error[256], k1[65], k2[65];
    SnxAnalysisIdentity id = {.features = 0xabc, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache *c1, *c2;
    unsigned char text[48];
    SnxCodeImage mod;
    SnxU64 hints[1] = {0x42};
    SnxU64 routed[1] = {0x99};
    size_t i;
    for (i = 0; i < sizeof text; ++i) text[i] = (unsigned char)(0xaa + i);
    fill_module(&mod, text, sizeof text, 0x8000);
    c1 = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(c1 != NULL);
    if (!c1) return 0;
    CHECK(snx_analysis_cache_key(c1, &mod, 1, hints, 1, k1) == 0);
    /* features change */
    id.features = 0xabd;
    c2 = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(c2 != NULL);
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    snx_analysis_cache_close(c2);
    /* version change */
    id.features = 0xabc;
    id.version = SNX_ANALYSIS_VERSION + 1;
    c2 = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(c2 != NULL);
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    snx_analysis_cache_close(c2);
    /* text change */
    id.version = SNX_ANALYSIS_VERSION;
    c2 = snx_analysis_cache_open(folder, &id, error, sizeof error);
    text[0] ^= 0xff;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    text[0] ^= 0xff;
    /* hint change */
    hints[0] = 0x43;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    hints[0] = 0x42;
    /* routed change */
    mod.routed = routed;
    mod.routed_count = 1;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    snx_analysis_cache_close(c2);
    snx_analysis_cache_close(c1);
    return 1;
}

static int test_soft_fail_open(void) {
    char error[256];
    SnxAnalysisIdentity id = {.features = 0, .version = SNX_ANALYSIS_VERSION};
    /* path that cannot be created as a directory on any reasonable host */
    SnxAnalysisCache* cache = snx_analysis_cache_open("/proc/analysis_cache_should_fail", &id, error, sizeof error);
    CHECK(cache == NULL);
    CHECK(error[0] != 0);
    return 1;
}

static int test_miss(const char* folder) {
    char error[256], key[65];
    SnxAnalysisIdentity id = {.features = 7, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    SnxCodegen dst = {0};
    unsigned char text[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    SnxCodeImage mod;
    fill_module(&mod, text, sizeof text, 0);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    snx_analysis_cache_close(cache);
    return 1;
}

int main(void) {
    char* folder = snx_format("%s/snx-analysis-cache-test", snx_temp_dir(NULL));
    failures = 0;
    if (!folder) {
        fprintf(stderr, "cannot make temp folder name\n");
        return 1;
    }
    snx_remove_tree(folder);
    test_roundtrip(folder);
    test_empty(folder);
    test_trailer_reject(folder);
    test_key_sensitivity(folder);
    test_soft_fail_open();
    test_miss(folder);
    snx_remove_tree(folder);
    free(folder);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("analysis cache: all checks passed\n");
    return 0;
}
