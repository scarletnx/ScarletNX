/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Analysis cache (nx/loader/analysis_cache.c): round-trip of function lists, trailer/hash rejection, key
   sensitivity to every hashed field, soft-fail open, little-endian numbers, empty analysis, rejection of
   implausible headers, concurrent stores, an end-to-end analyze/store/load comparison, and that a truncated
   or corrupted payload never leaves partial allocations. */
#include "nx/loader/analysis_cache.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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
    if (cg->arrays_block) {
        free(cg->arrays_block);
    } else {
        for (i = 0; i < cg->function_count; ++i) {
            free(cg->functions[i].addresses);
            free(cg->functions[i].leaders);
        }
        free(cg->functions);
    }
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
        f[i].leaders = (unsigned char*)snx_alloc(f[i].count);
        for (j = 0; j < f[i].count; ++j) f[i].addresses[j] = f[i].start + j * 4;
        memset(f[i].leaders, 0, f[i].count);
        for (j = 0; j < f[i].leader_count; ++j) f[i].leaders[j] = (unsigned char)(j + 1);
    }
    /* function 1 exercises the NULL-addresses mode, when there is one */
    if (n > 1) {
        free(f[1].addresses);
        f[1].addresses = NULL;
    }
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
        for (j = 0; j < a[i].count; ++j)
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

static void put32(unsigned char* image, size_t offset, SnxU32 word) {
    unsigned i;
    for (i = 0; i < 4; ++i) image[offset + i] = (unsigned char)(word >> (8 * i));
}

static void put64(unsigned char* p, SnxU64 v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

/* Write a payload with a valid magic and hash. The tests change only the header, to check the reader's bounds. */
static void write_bad_file(const char* folder, const char* key, const unsigned char* payload, size_t size) {
    static const unsigned char magic[8] = {'S', 'N', 'X', 'A', 'N', 'L', '1', 0};
    unsigned char trailer[SNX_ANALYSIS_TRAILER];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    char* name = snx_format("%s.an", key);
    char* path = snx_path_join(folder, name);
    FILE* f;
    free(name);
    if (!ctx || !path) {
        if (ctx) EVP_MD_CTX_free(ctx);
        free(path);
        CHECK(0);
        return;
    }
    memcpy(trailer, magic, sizeof magic);
    CHECK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1);
    CHECK(EVP_DigestUpdate(ctx, key, 64) == 1);
    if (size) CHECK(EVP_DigestUpdate(ctx, payload, size) == 1);
    CHECK(EVP_DigestFinal_ex(ctx, trailer + 8, NULL) == 1);
    EVP_MD_CTX_free(ctx);
    f = snx_fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(payload, 1, size, f);
        fwrite(trailer, 1, sizeof trailer, f);
        fclose(f);
    }
    free(path);
}

static void make_bad_payload(unsigned char* p, SnxU64 nfunc, SnxU64 count, SnxU64 leader_count, SnxU64 alen,
                             size_t* size) {
    size_t off = 0;
    put64(p + off, nfunc);
    off += 8;
    if (nfunc) {
        put64(p + off, 0x2000);
        off += 8;
        put64(p + off, 0x2000 + count * 4);
        off += 8;
        put64(p + off, count);
        off += 8;
        put64(p + off, leader_count);
        off += 8;
        put64(p + off, alen);
        off += 8;
    }
    *size = off;
}

#ifndef _WIN32
static void concurrent_child(const char* folder, const char* key, int n, SnxU64 base, int barrier) {
    char error[256], c;
    SnxAnalysisIdentity id = {.features = 11, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    SnxCodegen src = {0};
    (void)read(barrier, &c, 1);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    if (!cache) _exit(1);
    src.functions = make_functions((size_t)n, base);
    src.function_count = (size_t)n;
    src.function_capacity = (size_t)n;
    (void)snx_analysis_cache_store(cache, key, &src, error, sizeof error);
    free_codegen(&src);
    snx_analysis_cache_close(cache);
    _exit(0);
}
#endif

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
    /* a large function list must round-trip with the same capacity math */
    {
        SnxCodegen big = {0}, back = {0};
        size_t n = 4096;
        big.functions = make_functions(n, 0x90000000ull);
        big.function_count = big.function_capacity = n;
        for (i = 0; i < n; ++i) big.instruction_count += big.functions[i].count;
        mod.base = 0x90000000ull;
        CHECK(snx_analysis_cache_key(cache, &mod, 1, hints, 2, key) == 0);
        CHECK(snx_analysis_cache_store(cache, key, &big, error, sizeof error) == 0);
        CHECK(snx_analysis_cache_load(cache, key, &back, error, sizeof error) == 0);
        CHECK(back.function_count == n);
        CHECK(back.instruction_count == big.instruction_count);
        CHECK(functions_equal(big.functions, n, back.functions, back.function_count));
        free_codegen(&big);
        free_codegen(&back);
    }
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
    char error[256], k1[65], k2[65], k3[65];
    SnxAnalysisIdentity id = {.features = 0xabc, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache *c1, *c2;
    unsigned char text[48], text2[48];
    SnxCodeImage mod, mod2;
    SnxU64 hints[1] = {0x42};
    SnxU64 routed[1] = {0x99};
    SnxU64 seed[1] = {0x55}, rel[1] = {0x66};
    SnxFde fde[1] = {{0, 8}};
    size_t i;
    for (i = 0; i < sizeof text; ++i) {
        text[i] = (unsigned char)(0xaa + i);
        text2[i] = (unsigned char)(0x20 + i);
    }
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
    mod.routed = NULL;
    mod.routed_count = 0;
    /* base change */
    mod.base += 1;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    mod.base -= 1;
    /* text_off and text_size change */
    mod.text_off = 4;
    mod.text_size = sizeof text - 4;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    mod.text_off = 0;
    mod.text_size = sizeof text;
    /* image_size change: the key hashes the whole image, not only text */
    mod.image_size = sizeof text - 1;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    CHECK(strcmp(k1, k2) != 0);
    mod.image_size = sizeof text;
    /* function seeds */
    mod.functions = seed;
    mod.function_count = 1;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    seed[0] = 0x57;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k3) == 0);
    CHECK(strcmp(k2, k3) != 0);
    seed[0] = 0x55;
    mod.functions = NULL;
    mod.function_count = 0;
    /* relptrs */
    mod.relptrs = rel;
    mod.relptr_count = 1;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    rel[0] = 0x67;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k3) == 0);
    CHECK(strcmp(k2, k3) != 0);
    rel[0] = 0x66;
    mod.relptrs = NULL;
    mod.relptr_count = 0;
    /* FDE */
    mod.fde = fde;
    mod.fde_count = 1;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
    fde[0].size = 9;
    CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k3) == 0);
    CHECK(strcmp(k2, k3) != 0);
    fde[0].size = 8;
    mod.fde = NULL;
    mod.fde_count = 0;
    /* module count and module order, with two different modules */
    fill_module(&mod2, text2, sizeof text2, 0x9000);
    {
        SnxCodeImage mods[2], swapped[2];
        mods[0] = mod;
        mods[1] = mod2;
        swapped[0] = mod2;
        swapped[1] = mod;
        CHECK(snx_analysis_cache_key(c2, mods, 2, hints, 1, k2) == 0);
        CHECK(snx_analysis_cache_key(c2, swapped, 2, hints, 1, k3) == 0);
        CHECK(strcmp(k2, k3) != 0);
        CHECK(snx_analysis_cache_key(c2, &mod, 1, hints, 1, k2) == 0);
        CHECK(strcmp(k2, k3) != 0);
    }
    snx_analysis_cache_close(c2);
    snx_analysis_cache_close(c1);
    return 1;
}

static int test_soft_fail_open(const char* folder) {
    char error[256], *file, *path;
    SnxAnalysisIdentity id = {.features = 0, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    /* a regular file cannot be a parent directory, on any host */
    file = snx_path_join(folder, "not-a-folder");
    CHECK(file && !snx_write_file(file, "x", 1));
    if (!file) return 1;
    path = snx_format("%s/analysis", file);
    cache = snx_analysis_cache_open(path, &id, error, sizeof error);
    CHECK(cache == NULL);
    CHECK(error[0] != 0);
    free(path);
    free(file);
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

static int test_bad_header(const char* folder) {
    char error[256], key[65];
    SnxAnalysisIdentity id = {.features = 9, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    SnxCodegen dst = {0};
    unsigned char text[16] = {0}, payload[128];
    SnxCodeImage mod;
    size_t size;
    fill_module(&mod, text, sizeof text, 0);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    make_bad_payload(payload, 1000001ull, 0, 0, 0, &size);
    write_bad_file(folder, key, payload, size);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    make_bad_payload(payload, 1, 10000001ull, 0, 10000001ull, &size);
    write_bad_file(folder, key, payload, size);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    make_bad_payload(payload, 1, 4, 0, 5, &size);
    write_bad_file(folder, key, payload, size);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    make_bad_payload(payload, 1, 4, 5, 0, &size);
    write_bad_file(folder, key, payload, size);
    CHECK(snx_analysis_cache_load(cache, key, &dst, error, sizeof error) != 0);
    CHECK(dst.functions == NULL);
    free_codegen(&dst);
    snx_analysis_cache_close(cache);
    return 1;
}

#ifndef _WIN32
static int test_concurrent_store(const char* folder) {
    char error[256], key[65];
    SnxAnalysisIdentity id = {.features = 11, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    unsigned char text[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    SnxCodeImage mod;
    SnxCodegen a = {0}, b = {0}, loaded = {0};
    int iter, barrier[2], status;
    pid_t pid1, pid2;
    fill_module(&mod, text, sizeof text, 0x1000);
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    a.functions = make_functions(3, 0x2000);
    a.function_count = a.function_capacity = 3;
    b.functions = make_functions(5, 0x4000);
    b.function_count = b.function_capacity = 5;
    for (iter = 0; iter < 20; ++iter) {
        CHECK(pipe(barrier) == 0);
        pid1 = fork();
        if (pid1 == 0) {
            close(barrier[1]);
            concurrent_child(folder, key, 3, 0x2000, barrier[0]);
        }
        pid2 = fork();
        if (pid2 == 0) {
            close(barrier[1]);
            concurrent_child(folder, key, 5, 0x4000, barrier[0]);
        }
        close(barrier[0]);
        (void)write(barrier[1], "xx", 2);
        close(barrier[1]);
        CHECK(waitpid(pid1, &status, 0) == pid1);
        CHECK(waitpid(pid2, &status, 0) == pid2);
        memset(&loaded, 0, sizeof loaded);
        CHECK(snx_analysis_cache_load(cache, key, &loaded, error, sizeof error) == 0);
        CHECK(functions_equal(a.functions, 3, loaded.functions, loaded.function_count) ||
              functions_equal(b.functions, 5, loaded.functions, loaded.function_count));
        free_codegen(&loaded);
        {
            char* name = snx_format("%s.an", key);
            char* path = snx_path_join(folder, name);
            char* tmp1 = snx_format("%s.%lu.tmp", path, (unsigned long)pid1);
            char* tmp2 = snx_format("%s.%lu.tmp", path, (unsigned long)pid2);
            CHECK(!snx_path_exists(tmp1));
            CHECK(!snx_path_exists(tmp2));
            free(tmp1);
            free(tmp2);
            free(path);
            free(name);
        }
    }
    free_codegen(&a);
    free_codegen(&b);
    snx_analysis_cache_close(cache);
    return 1;
}
#else
static int test_concurrent_store(const char* folder) {
    (void)folder;
    return 1;
}
#endif

static int test_analyze_roundtrip(const char* folder) {
    static const SnxU32 words[] = {0xAA1E03F3UL, 0x91000400UL, 0x9400000AUL, 0xF1002C1FUL, 0x540000A1UL,
                                   0x91000800UL, 0xAA1303FEUL, 0xD65F03C0UL, 0xD503201FUL, 0x91019000UL,
                                   0x17FFFFFCUL, 0xD4200000UL, 0x91002800UL, 0xD65F03C0UL};
    char error[256], key[65];
    SnxAnalysisIdentity id = {.features = 13, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    unsigned char image[sizeof words];
    SnxCodeImage mod = {0};
    SnxFde fde = {0, 44};
    SnxU64 seed = 20;
    SnxCodegen live1 = {0}, loaded = {0}, live2 = {0};
    size_t i, j;
    for (i = 0; i < sizeof words / sizeof words[0]; ++i) put32(image, i * 4, words[i]);
    mod.image = image;
    mod.image_size = sizeof image;
    mod.base = 0x100000;
    mod.text_size = sizeof image;
    mod.fde = &fde;
    mod.fde_count = 1;
    mod.functions = &seed;
    mod.function_count = 1;
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) return 0;
    CHECK(!snx_codegen_analyze(&live1, &mod, 1, NULL, 0, 1, 0));
    CHECK(live1.function_count > 0);
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    CHECK(snx_analysis_cache_store(cache, key, &live1, error, sizeof error) == 0);
    CHECK(snx_analysis_cache_load(cache, key, &loaded, error, sizeof error) == 0);
    CHECK(!snx_codegen_analyze(&live2, &mod, 1, NULL, 0, 1, 0));
    CHECK(functions_equal(live1.functions, live1.function_count, live2.functions, live2.function_count));
    CHECK(functions_equal(live2.functions, live2.function_count, loaded.functions, loaded.function_count));
    CHECK(loaded.instruction_count == live2.instruction_count);
    /* a loaded analysis must get its image views back, or every word decodes as undefined */
    CHECK(snx_codegen_attach_images(&loaded, &mod, 1) == 0);
    loaded.features = 0;
    for (i = 0; i < loaded.function_count; ++i) {
        const SnxFunction* fn = &loaded.functions[i];
        CHECK(!!fn->addresses == !!live2.functions[i].addresses);
        for (j = 0; j < fn->count; ++j) {
            SnxU64 addr = snx_function_address(fn, j);
            SnxU32 word;
            CHECK(snx_codegen_word(&loaded, addr, &word));
            CHECK(word == snx_ld_le32(image + (size_t)(addr - mod.base)));
        }
    }
    snx_codegen_free(&live1);
    snx_codegen_free(&live2);
    snx_codegen_free(&loaded);
    snx_analysis_cache_close(cache);
    return 1;
}

static int test_unit_keys(const char* folder) {
    char error[256], key[65], options[65], other[65], back[3][65];
    SnxAnalysisIdentity id = {.features = 21, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    unsigned char text[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    SnxCodeImage mod;
    char (*keys)[65] = (char (*)[65])snx_alloc(3 * 65);
    size_t i;
    fill_module(&mod, text, sizeof text, 0);
    CHECK(keys != NULL);
    if (!keys) return 0;
    memset(keys, 0, 3 * 65);
    memset(back, 0, sizeof back);
    for (i = 0; i < 3; ++i) {
        snprintf(keys[i], 65, "%064x", (unsigned)(0xabc + i));
        keys[i][64] = 0;
    }
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) {
        free(keys);
        return 0;
    }
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    memset(options, 'a', 64);
    options[64] = 0;
    memset(other, 'b', 64);
    other[64] = 0;
    CHECK(snx_analysis_cache_store_unit_keys(cache, key, options, keys, 3) == 0);
    CHECK(snx_analysis_cache_load_unit_keys(cache, key, options, back, 3) == 0);
    for (i = 0; i < 3; ++i) CHECK(strcmp(keys[i], back[i]) == 0);
    /* a sidecar made under different build options must not be used */
    CHECK(snx_analysis_cache_load_unit_keys(cache, key, other, back, 3) != 0);
    /* neither must one for a different number of units */
    CHECK(snx_analysis_cache_load_unit_keys(cache, key, options, back, 4) != 0);
    free(keys);
    snx_analysis_cache_close(cache);
    return 1;
}

static int test_link_stamp(const char* folder) {
    char error[256], key[65], desc[65], identity[65], other[65], sample[65], back[65];
    char (*keys)[65];
    SnxAnalysisIdentity id = {.features = 23, .version = SNX_ANALYSIS_VERSION};
    SnxAnalysisCache* cache;
    unsigned char text[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    SnxCodeImage mod;
    SnxU64 size = 0, mtime = 0;
    size_t i;
    fill_module(&mod, text, sizeof text, 0);
    keys = (char (*)[65])snx_alloc(2 * 65);
    CHECK(keys != NULL);
    if (!keys) return 0;
    memset(keys, 0, 2 * 65);
    for (i = 0; i < 2; ++i) {
        snprintf(keys[i], 65, "%064x", (unsigned)(0xdef0 + i));
        keys[i][64] = 0;
    }
    memset(desc, 'c', 64); desc[64] = 0;
    memset(identity, 'i', 64); identity[64] = 0;
    memset(other, 'x', 64); other[64] = 0;
    memset(sample, 's', 64); sample[64] = 0;
    cache = snx_analysis_cache_open(folder, &id, error, sizeof error);
    CHECK(cache != NULL);
    if (!cache) {
        free(keys);
        return 0;
    }
    CHECK(snx_analysis_cache_key(cache, &mod, 1, NULL, 0, key) == 0);
    CHECK(snx_analysis_cache_store_link_stamp(cache, key, desc, identity, keys, 2, "/game/code.so", 4096, 1234,
                                              sample) == 0);
    memset(back, 0, sizeof back);
    CHECK(snx_analysis_cache_load_link_stamp(cache, key, desc, identity, keys, 2, "/game/code.so", &size, &mtime,
                                             back) == 0);
    CHECK(size == 4096 && mtime == 1234 && !strcmp(sample, back));
    CHECK(snx_analysis_cache_load_link_stamp(cache, key, other, identity, keys, 2, "/game/code.so", &size, &mtime,
                                             back) != 0);
    CHECK(snx_analysis_cache_load_link_stamp(cache, key, desc, identity, keys, 2, "/other/code.so", &size, &mtime,
                                             back) != 0);
    free(keys);
    snx_analysis_cache_close(cache);
    return 1;
}

int main(void) {
    char* root = snx_temp_dir(NULL), *folder;
    failures = 0;
    if (!root) {
        fprintf(stderr, "cannot make temp folder name\n");
        return 1;
    }
    folder = snx_format("%s/snx-analysis-cache-test", root);
    free(root);
    if (!folder) {
        fprintf(stderr, "cannot make temp folder name\n");
        return 1;
    }
    snx_remove_tree(folder);
    test_roundtrip(folder);
    test_empty(folder);
    test_trailer_reject(folder);
    test_key_sensitivity(folder);
    test_bad_header(folder);
    test_concurrent_store(folder);
    test_analyze_roundtrip(folder);
    test_unit_keys(folder);
    test_link_stamp(folder);
    test_soft_fail_open(folder);
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
