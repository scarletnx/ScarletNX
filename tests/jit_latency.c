/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Offline compilation latency through the real service, at O0. No generated guest function is called.
   Usage: snx-bench-jit-latency MANIFEST AOT_ADDRESSES ROOTS BUDGET BATCH THREADS demand|burst REGION_LIMIT [CACHE]
   REGION_LIMIT 0 uses the previous unbounded discovery, auto the adaptive region limit.
   MANIFEST rows: hex base, text offset, text size, added-image flag, file relative to the manifest.
   AOT_ADDRESSES is the host-endian u64 table of known entries. ROOTS has hex PCs, one per line.
   Captured images, tables, profiles and reports stay outside the repository. */
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Known AOT entries stop discovery and resolve links. This benchmark never calls the stand-in or generated code. */
static void aot(CPU* cpu, u64 entry) {
    (void)cpu;
    (void)entry;
    abort();
}

static unsigned number(const char* text, unsigned max) {
    char* end;
    unsigned long long value = strtoull(text, &end, 10);
    return *text && !*end && value && value <= max ? (unsigned)value : 0;
}

int main(int argc, char** argv) {
    if (argc < 9 || argc > 10) return 2;
    unsigned budget = number(argv[4], 1000000), batch = number(argv[5], 4096), threads = number(argv[6], SNX_JIT_THREADS);
    if (!budget || !batch || !threads || (strcmp(argv[7], "demand") && strcmp(argv[7], "burst"))) return 2;
    SnxCodeImage images[SNX_JIT_IMAGES + 1] = {{0}};
    unsigned added[SNX_JIT_IMAGES + 1] = {0};
    size_t image_count = 0, modules = 0;
    char error[512], file[1024];
    char* dir = snx_dirname(argv[1]);
    FILE* manifest = fopen(argv[1], "r");
    if (!manifest || !dir) return 2;
    unsigned long long base, off, size;
    unsigned is_added;
    while (fscanf(manifest, "%llx %llx %llx %u %1023s", &base, &off, &size, &is_added, file) == 5) {
        if (image_count == SNX_JIT_IMAGES + 1 || (is_added && !modules)) return 2;
        char* path = snx_path_join(dir, file);
        size_t n;
        unsigned char* bytes = snx_read_file(path, &n);
        free(path);
        if (!bytes || off > n || size > n - off || base > ~(u64)0 - off - size) return 2;
        images[image_count] = (SnxCodeImage){0};
        images[image_count].image = bytes;
        images[image_count].image_size = n;
        images[image_count].base = base;
        images[image_count].text_off = off;
        images[image_count].text_size = size;
        added[image_count++] = is_added;
        if (!is_added) modules++;
    }
    fclose(manifest);
    free(dir);
    if (!modules || !image_count) return 2;
    size_t aot_size;
    u64* addresses = (u64*)(void*)snx_read_file(argv[2], &aot_size);
    if (!addresses || aot_size % sizeof *addresses) return 2;
    size_t aot_count = aot_size / sizeof *addresses;
    for (size_t i = 1; i < aot_count; i++) if (addresses[i] < addresses[i - 1]) return 2;
    GuestFn* functions = calloc(aot_count, sizeof *functions);
    if (!functions) return 2;
    /* Include the complete AOT address range even when a live snapshot omitted a writable image. */
    u64 lo = aot_count ? addresses[0] & ~0xfffull : images[0].base;
    u64 hi = aot_count ? (addresses[aot_count - 1] + 0x1000) & ~0xfffull : lo + 0x1000;
    for (size_t i = 0; i < modules; i++) {
        u64 begin = images[i].base + images[i].text_off, end = begin + images[i].text_size;
        if (begin < lo) lo = begin & ~0xfffull;
        if (end > hi) hi = (end + 0xfff) & ~0xfffull;
    }
    if (hi <= lo || hi - lo > (1ull << 30)) return 2;
    GuestFn* dispatch = calloc((size_t)(hi - lo) / 4, sizeof *dispatch);
    if (!dispatch) return 2;
    for (size_t i = 0; i < aot_count; i++) {
        functions[i] = aot;
        dispatch[(addresses[i] - lo) / 4] = aot;
    }
    FILE* input = fopen(argv[3], "r");
    u64 roots[4096];
    double waits[4096];
    size_t count = 0;
    unsigned long long pc;
    if (!input) return 2;
    while (count < 4096 && fscanf(input, "%llx", &pc) == 1) roots[count++] = pc;
    fclose(input);
    if (!count) return 2;
    SnxJitConfig config = {0};
    config.images = images;
    config.image_count = modules;
    config.dispatch = dispatch;
    config.code_lo = lo;
    config.code_hi = hi;
    config.aot_addresses = addresses;
    config.aot_functions = functions;
    config.aot_count = aot_count;
    config.direct_svc = 1;
    config.opt = '0';
    config.budget = budget;
    config.live_budget = SIZE_MAX; /* measure exactly the requested budget */
    config.batch = batch;
    config.threads = threads;
    unsigned region = number(argv[8], 200001);
    if (!region && strcmp(argv[8], "0") && strcmp(argv[8], "auto")) return 2;
    config.region_instructions = !strcmp(argv[8], "auto") ? 0 : region ? region : SIZE_MAX;
    config.cache_path = argc == 10 ? argv[9] : NULL;
    snx_ir_fpsr_flags(1);
    SnxJitService* service = snx_jit_service_new(&config, error, sizeof error);
    if (!service) { fprintf(stderr, "%s\n", error); return 1; }
    for (size_t i = modules; i < image_count; i++)
        if (!added[i] || snx_jit_add_image(service, &images[i])) return 2;
    double started = snx_seconds();
    if (!strcmp(argv[7], "burst"))
        for (size_t i = 0; i < count; i++) snx_jit_resolve(service, roots[i]);
    unsigned missing = 0;
    for (size_t i = 0; i < count; i++) {
        double start = snx_seconds();
        if (!snx_jit_wait(service, roots[i])) missing++;
        waits[i] = (snx_seconds() - start) * 1000;
    }
    snx_jit_drain(service);
    double elapsed = (snx_seconds() - started) * 1000;
    SnxJitStats stats;
    snx_jit_stats(service, &stats);
    printf("{\"elapsed_ms\":%.6f,\"missing\":%u,\"batches\":%lu,\"functions\":%lu,"
           "\"instructions\":%lu,\"code_bytes\":%zu,\"compile_s\":%.6f,\"cached\":%lu,\"waits_ms\":[",
           elapsed, missing, stats.batches, stats.functions, stats.instructions, stats.bytes, stats.seconds, stats.cached);
    for (size_t i = 0; i < count; i++) printf("%s%.6f", i ? "," : "", waits[i]);
    puts("]}");
    snx_jit_service_free(service);
    for (size_t i = 0; i < image_count; i++) free((void*)images[i].image);
    free(addresses);
    free(functions);
    free(dispatch);
    return missing || stats.failures;
}
