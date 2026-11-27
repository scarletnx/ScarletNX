/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AOT build with a profile cache: the cache's unit is linked into the module,
   snx_variants() hands the runtime its table, and the cached function's direct call
   to a static AOT entry goes through the module's snx_aot_<addr> alias. */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/recompiler/jit/jit_service.h"
#include "common/cpu/recompiler/variants.h"
#include "common/utilities/common.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define STATIC_OFF 0x1000
#define CACHE_OFF  0x3000
#define TEXT_SIZE  0x1000

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned char memory[0x4000];

/* the AOT entry the cached function calls: x0 += 1 */
static void aot_stub(CPU* c, u64 entry) {
    (void)entry;
    c->x[0] += 1;
}

static SnxJitService* profile(const char* path, char opt) {
    static const u64 aot_addresses[] = {SNX_CODE_BASE + STATIC_OFF};
    static const _Atomic GuestFn aot_functions[] = {aot_stub};
    static _Atomic GuestFn dispatch[TEXT_SIZE / 4];
    static SnxJitConfig config;
    static SnxCodeImage image;
    char error[512];
    SnxJitService* service;
    memset(dispatch, 0, sizeof(dispatch));
    memset(&image, 0, sizeof(image));
    image.image = memory;
    image.image_size = sizeof(memory);
    image.base = SNX_CODE_BASE;
    image.text_off = CACHE_OFF;
    image.text_size = TEXT_SIZE;
    memset(&config, 0, sizeof(config));
    config.images = &image;
    config.image_count = 1;
    config.dispatch = dispatch;
    config.code_lo = SNX_CODE_BASE + CACHE_OFF;
    config.code_hi = SNX_CODE_BASE + CACHE_OFF + TEXT_SIZE;
    config.aot_addresses = aot_addresses;
    config.aot_functions = aot_functions;
    config.aot_count = 1;
    config.direct_svc = 1;
    config.batch = 1;
    config.threads = 1;
    config.opt = opt;
    config.cache_path = path;
    service = snx_jit_service_new(&config, error, sizeof(error));
    if (!service) fprintf(stderr, "jit: %s\n", error);
    return service;
}

static void* open_module(const char* path) {
#ifdef _WIN32
    return (void*)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* symbol(void* module, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)module, name);
#else
    return dlsym(module, name);
#endif
}

static void close_module(void* module) {
#ifdef _WIN32
    FreeLibrary((HMODULE)module);
#else
    dlclose(module);
#endif
}

static const SnxVariantFunction* find_variant(const SnxVariants* variants, SnxU64 start) {
    SnxU32 i;
    for (i = 0; i < variants->count; ++i)
        if (variants->functions[i].start == start) return &variants->functions[i];
    return NULL;
}

/* the valid cache with the last record's tail cut off: the reader must refuse it */
static int copy_truncated(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    FILE* out;
    long size, cut;
    unsigned char* data;
    size_t got;
    if (!in) return -1;
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return -1;
    }
    size = ftell(in);
    if (size <= 0) {
        fclose(in);
        return -1;
    }
    rewind(in);
    data = (unsigned char*)malloc((size_t)size);
    if (!data) {
        fclose(in);
        return -1;
    }
    got = fread(data, 1, (size_t)size, in);
    fclose(in);
    if (got != (size_t)size) {
        free(data);
        return -1;
    }
    cut = size > 8 ? size - 8 : 0;
    out = fopen(to, "wb");
    if (!out) {
        free(data);
        return -1;
    }
    if (cut && fwrite(data, 1, (size_t)cut, out) != (size_t)cut) {
        fclose(out);
        free(data);
        return -1;
    }
    fclose(out);
    free(data);
    return 0;
}

int main(void) {
#ifdef _WIN32
    static const char* output = "snx-test-aot-variants.dll";
#else
    static const char* output = "snx-test-aot-variants.so";
#endif
    static const char* cache_path = "snx-test-aot-variants.jitcache";
    static const char* wrong_path = "snx-test-aot-variants-wrong.jitcache";
    static const char* truncated_path = "snx-test-aot-variants-truncated.jitcache";
    static const char* bad_output = "snx-test-aot-variants-bad";
    static const SnxU32 static_code[] = {0x91000400u, 0xD65F03C0u}; /* add x0, x0, #1; ret */
    static const SnxU32 cache_code[] = {0x17FFF800u};              /* b 0x08001000 */
    static const SnxU32 cache_code2[] = {0x91000800u, 0xD65F03C0u}; /* add x0, x0, #2; ret */
    SnxModuleImage module_image;
    SnxProgram program;
    SnxCodegenOptions options, wrong_options, truncated_options;
    SnxModuleBuild build, wrong_build, truncated_build;
    SnxJitService* service;
    char error[1024];
    char* absolute;
    void* handle;
    const SnxModule* (*module_fn)(void);
    const SnxVariants* (*variants_fn)(void);
    const SnxModule* module;
    const SnxVariants* variants;
    const SnxVariantFunction *function, *second;
    CPU cpu;
    int built;
    size_t wrappers = 0;

    remove(cache_path);
    remove(wrong_path);
    remove(truncated_path);
    remove(bad_output);
    remove(output);
    memset(memory, 0, sizeof(memory));
    memcpy(memory + STATIC_OFF, static_code, sizeof(static_code));
    memcpy(memory + CACHE_OFF, cache_code, sizeof(cache_code));
    memcpy(memory + CACHE_OFF + 0x400, cache_code2, sizeof(cache_code2));

    service = profile(cache_path, '2');
    CHECK(service != NULL);
    if (!service) return 1;
    CHECK(snx_jit_wait(service, SNX_CODE_BASE + CACHE_OFF) != NULL);
    CHECK(snx_jit_wait(service, SNX_CODE_BASE + CACHE_OFF + 0x400) != NULL);
    snx_jit_service_free(service);

    /* a cache profiled at another opt: its key doesn't match the AOT build's */
    service = profile(wrong_path, '1');
    CHECK(service != NULL);
    if (service) {
        CHECK(snx_jit_wait(service, SNX_CODE_BASE + CACHE_OFF) != NULL);
        snx_jit_service_free(service);
    }
    /* the valid cache with its last record cut off: the reader refuses it */
    CHECK(copy_truncated(cache_path, truncated_path) == 0);

    memset(&module_image, 0, sizeof(module_image));
    module_image.name = "test";
    module_image.base = SNX_CODE_BASE;
    module_image.size = sizeof(memory);
    module_image.image = memory;
    module_image.image_size = sizeof(memory);
    module_image.text_off = STATIC_OFF;
    module_image.text_size = TEXT_SIZE;
    module_image.is_nro = 1;
    memset(&program, 0, sizeof(program));
    program.modules = &module_image;
    program.module_count = 1;
    program.npdm.title_id = 0x0100000000000000ULL;
    program.npdm.stack = 0x100000;
    program.npdm.is64 = 1;
    strcpy(program.npdm.name, "test");

    memset(&options, 0, sizeof(options));
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.jit_cache = cache_path;
    memset(&build, 0, sizeof(build));
    build.output = output;
    build.opt = '2';
    build.jobs = 1;
    build.quiet = 1;
    build.wrappers = &wrappers;
    error[0] = 0;
    built = snx_build_module_a64(&program, &options, &build);
    CHECK(built == 0);
    CHECK(wrappers > 0); /* the profile cache's units call the module's functions through them */
    if (built) {
        fprintf(stderr, "build: %s\n", error);
        remove(cache_path);
        remove(wrong_path);
        remove(truncated_path);
        return 1;
    }

    wrong_options = options;
    wrong_options.jit_cache = wrong_path;
    wrong_build = build;
    wrong_build.output = bad_output;
    CHECK(snx_build_module_a64(&program, &wrong_options, &wrong_build) != 0);
    remove(bad_output);
    truncated_options = options;
    truncated_options.jit_cache = truncated_path;
    truncated_build = build;
    truncated_build.output = bad_output;
    CHECK(snx_build_module_a64(&program, &truncated_options, &truncated_build) != 0);
    remove(bad_output);

    absolute = snx_absolute(output);
    CHECK(absolute != NULL);
    handle = absolute ? open_module(absolute) : NULL;
    CHECK(handle != NULL);
    if (!handle) {
        fprintf(stderr, "load: %s\n", output);
        free(absolute);
        remove(cache_path);
        remove(wrong_path);
        remove(truncated_path);
        remove(output);
        return 1;
    }
    module_fn = (const SnxModule * (*)(void)) symbol(handle, "snx_module");
    variants_fn = (const SnxVariants * (*)(void)) symbol(handle, "snx_variants");
    CHECK(module_fn != NULL);
    CHECK(variants_fn != NULL);
    module = module_fn ? module_fn() : NULL;
    CHECK(module != NULL && module->abi == SNX_ABI);
    variants = variants_fn ? variants_fn() : NULL;
    CHECK(variants != NULL);
    if (variants) {
        CHECK(variants->version == SNX_VARIANTS_VERSION);
        CHECK(variants->count == 2);
        function = find_variant(variants, SNX_CODE_BASE + CACHE_OFF);
        second = find_variant(variants, SNX_CODE_BASE + CACHE_OFF + 0x400);
        CHECK(function != NULL);
        CHECK(second != NULL);
        if (function) {
            CHECK(function->leader_count == 1);
            CHECK(function->leader_count == 1 && function->leaders[0] == SNX_CODE_BASE + CACHE_OFF);
            CHECK(function->run_count == 1);
            CHECK(function->run_count == 1 && function->runs[0].address == SNX_CODE_BASE + CACHE_OFF);
            CHECK(function->run_count == 1 && function->runs[0].words == 1);
            CHECK(function->h1 != 0 && function->h2 != 0);
            CHECK(function->fn != NULL);
            memset(&cpu, 0, sizeof(cpu));
            cpu.mem = memory;
            cpu.x[0] = 41;
            function->fn(&cpu, function->start);
            CHECK(cpu.x[0] == 42);
        }
        if (second) {
            CHECK(second->leader_count == 1);
            CHECK(second->leader_count == 1 && second->leaders[0] == SNX_CODE_BASE + CACHE_OFF + 0x400);
            CHECK(second->run_count == 1);
            CHECK(second->run_count == 1 && second->runs[0].address == SNX_CODE_BASE + CACHE_OFF + 0x400);
            CHECK(second->run_count == 1 && second->runs[0].words == 2);
            CHECK(second->h1 != 0 && second->h2 != 0);
            CHECK(second->fn != NULL);
            memset(&cpu, 0, sizeof(cpu));
            cpu.mem = memory;
            cpu.x[0] = 41;
            second->fn(&cpu, second->start);
            CHECK(cpu.x[0] == 43);
        }
    }
    close_module(handle);
    free(absolute);
    remove(cache_path);
    remove(wrong_path);
    remove(truncated_path);
    remove(output);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("aot_variants: ok");
    return 0;
}
