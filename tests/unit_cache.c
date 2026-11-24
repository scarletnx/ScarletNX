/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's units held in memory (common/cpu/recompiler/x64/x64_cache.h) under the module build: a program
   of two functions, each a unit of its own, built in one process. A second build takes both units from memory and
   links the same module byte for byte; a program whose second function differs makes that unit alone, and runs it.
   The limit is 1% of the machine's memory, at most the ceiling; the unit used longest ago leaves first; a unit larger
   than the limit is made and not kept, said with its size and the limit; a ceiling of 0 holds nothing. Where the back
   end is not built it passes, saying so */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/utilities/common.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_llvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define TEXT_OFF  0x1000
#define TEXT_SIZE 0x1000

/* the failures, the program's memory, and what snx_error said since the last forget() */
static struct {
    int failures;
    unsigned char memory[TEXT_OFF + TEXT_SIZE];
    char said[16][1024];
    int said_count;
    size_t wrappers; /* the last build's snx_aot_ wrappers */
} g_unit_cache;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++g_unit_cache.failures;                                                                                   \
        }                                                                                                              \
    } while (0)

static void sink(const char* line) {
    fprintf(stderr, "said: %s\n", line);
    if (g_unit_cache.said_count < 16)
        snprintf(g_unit_cache.said[g_unit_cache.said_count++], sizeof g_unit_cache.said[0], "%s", line);
}
static void forget(void) { g_unit_cache.said_count = 0; }
static int said(const char* a, const char* b) {
    int i;
    for (i = 0; i < g_unit_cache.said_count; ++i)
        if (strstr(g_unit_cache.said[i], a) && (!b || strstr(g_unit_cache.said[i], b))) return 1;
    return 0;
}

/* f0: x9 = x30; bl f1; x30 = x9; ret. f1: x0 += add; ret */
static void program(unsigned add) {
    const SnxU32 code[] = {0xAA1E03E9u, 0x94000003u, 0xAA0903FEu, 0xD65F03C0u, 0x91000000u | (add << 10), 0xD65F03C0u};
    unsigned char* memory = g_unit_cache.memory;
    size_t i, j;
    memset(memory, 0, sizeof g_unit_cache.memory);
    for (i = 0; i < sizeof code / sizeof *code; ++i)
        for (j = 0; j < 4; ++j) memory[TEXT_OFF + 4 * i + j] = (unsigned char)(code[i] >> (8 * j));
}

/* the module of program's code, each function a unit */
static int build(const char* output) {
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxLlvmBuild llvm = {0};
    image.name = "test";
    image.base = SNX_CODE_BASE;
    image.size = sizeof g_unit_cache.memory;
    image.image = g_unit_cache.memory;
    image.image_size = sizeof g_unit_cache.memory;
    image.text_off = TEXT_OFF;
    image.text_size = TEXT_SIZE;
    image.is_nro = 1;
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    loaded.npdm.is64 = 1;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.chunk_instrs = 1; /* each function a unit */
    llvm.output = output;
    llvm.opt = '2';
    llvm.jobs = 1;
    llvm.quiet = 1;
    llvm.wrappers = &g_unit_cache.wrappers;
    return snx_build_module_llvm(&loaded, &options, &llvm);
}

static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
}
static void poll_clear(CPU* c) { c->poll = 0; }
static void missed(CPU* c, u64 entry) {
    (void)entry;
    c->x[0] = 0xBAD;
}

/* the module's f0 run on x0 = 41: x0 after it, 0 when the module does not load */
static u64 run(const char* output) {
    typedef const SnxModule* (*ModuleFn)(void);
    char* path = snx_absolute(output);
    const SnxModule* mod;
    ModuleFn get;
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    CPU cpu = {0};
    u64 x0 = 0;
    size_t i;
#ifdef _WIN32
    HMODULE handle = path ? LoadLibraryA(path) : NULL;
    get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module") : NULL;
#else
    void* handle = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    get = handle ? (ModuleFn)dlsym(handle, "snx_module") : NULL;
#endif
    free(path);
    if (!get) return 0;
    mod = get();
    rt.undef = undefined;
    rt.poll = poll_clear;
    rt.miss = missed;
    cpu.rt = &rt;
    cpu.mem = g_unit_cache.memory;
    cpu.code_lo = mod->code_lo;
    cpu.code_hi = mod->code_hi;
    cpu.disp = (_Atomic GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4 + 1, sizeof(GuestFn));
    for (i = 0; i < mod->nentries; ++i) cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
    cpu.x[0] = 41;
    cpu.x[30] = 0xDEAD0000;
    for (i = 0; i < mod->nentries; ++i)
        if (mod->entry_addr[i] == SNX_CODE_BASE + TEXT_OFF) {
            mod->entry_fn[i](&cpu, mod->entry_addr[i]);
            x0 = cpu.x[0];
        }
    free(cpu.disp);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return x0;
}

static int same_file(const char* a, const char* b) {
    size_t na = 0, nb = 0, at = 0, i;
    unsigned char *x = snx_read_file(a, &na), *y = snx_read_file(b, &nb);
    int same = x && y && na == nb && !memcmp(x, y, na);
    if (!same) { /* where they part and the bytes there, for a platform only CI runs */
        while (x && y && at < na && at < nb && x[at] == y[at]) ++at;
        fprintf(stderr, "%s: %zu bytes, %s: %zu bytes, the first difference at %zu:", a, x ? na : 0, b, y ? nb : 0, at);
        for (i = at; i < at + 16 && x && i < na; ++i) fprintf(stderr, " %02x", x[i]);
        fprintf(stderr, " /");
        for (i = at; i < at + 16 && y && i < nb; ++i) fprintf(stderr, " %02x", y[i]);
        fprintf(stderr, "\n");
    }
    free(x);
    free(y);
    return same;
}

static SnxX64CacheStats stats(void) {
    SnxX64CacheStats s;
    snx_x64_cache_stats(&s);
    return s;
}

int main(void) {
#ifdef _WIN32
#define SUFFIX ".dll"
#else
#define SUFFIX ".so"
#endif
    /* a module of its own for each build, as one loaded before is not loaded again from a file of its name */
    static const char* const dirs[] = {"snx-test-unit-cache.0", "snx-test-unit-cache.1", "snx-test-unit-cache.2",
                                       "snx-test-unit-cache.3", "snx-test-unit-cache.4", "snx-test-unit-cache.5"};
    static const char* const out[] = {"snx-test-unit-cache.0/module" SUFFIX, "snx-test-unit-cache.1/module" SUFFIX,
                                      "snx-test-unit-cache.2/module" SUFFIX, "snx-test-unit-cache.3/module" SUFFIX,
                                      "snx-test-unit-cache.4/module" SUFFIX, "snx-test-unit-cache.5/module" SUFFIX};
    SnxX64CacheStats s;
    SnxU64 held;
    size_t i;
    if (!snx_x64_available()) {
        printf("unit cache: no x86-64 back end on this host: passed\n");
        return 0;
    }
    snx_error_sink(sink);
    for (i = 0; i < sizeof dirs / sizeof *dirs; ++i) {
        snx_remove_tree(dirs[i]);
        CHECK(!snx_mkdirs(dirs[i]));
    }
    /* 1% of the memory, at most the ceiling */
    snx_x64_cache_memory(8000000000ull);
    CHECK(snx_x64_cache_limit() == 80000000ull);
    snx_x64_cache_ceiling(50000000ull);
    CHECK(snx_x64_cache_limit() == 50000000ull);
    snx_x64_cache_ceiling(SNX_X64_CACHE_CEILING_DEFAULT);
    snx_x64_cache_memory(1000000000000ull);
    CHECK(snx_x64_cache_limit() == SNX_X64_CACHE_CEILING_DEFAULT);

    /* the first build makes both units and keeps them; the second takes them from memory, the same module */
    snx_x64_cache_clear();
    program(1);
    CHECK(!build(out[0]) && run(out[0]) == 42);
    s = stats();
    CHECK(s.misses == 2 && s.hits == 0 && s.units == 2 && s.bytes > 0);
    CHECK(!build(out[1]) && run(out[1]) == 42);
    s = stats();
    CHECK(s.hits == 2 && s.misses == 2 && s.units == 2);
    CHECK(same_file(out[0], out[1]));
    /* f1 changed: its unit made and kept beside the old one, f0's from memory */
    program(5);
    CHECK(!build(out[2]) && run(out[2]) == 46);
    s = stats();
    CHECK(s.hits == 3 && s.misses == 3 && s.units == 3);

    /* room for two units: f1's +5 unit, made with the +1 ones held, leaves room by letting go f1's +1 unit, used
       longest ago (f0's was just used). Built again, the +5 program takes both its units from memory: had f0's gone
       instead, it would be made again */
    snx_x64_cache_clear();
    program(1);
    CHECK(!build(out[3]));
    held = stats().bytes;
    snx_x64_cache_ceiling(held);
    program(5);
    CHECK(!build(out[4]) && run(out[4]) == 46);
    s = stats();
    CHECK(s.evicted == 1 && s.units == 2 && s.hits == 1 && s.misses == 3);
    CHECK(!build(out[5]) && run(out[5]) == 46);
    s = stats();
    CHECK(s.hits == 3 && s.misses == 3 && s.evicted == 1);
    CHECK(same_file(out[4], out[5]));

    /* a limit no unit fits: each made and not kept, said once with its size and the limit */
    program(1);
    snx_x64_cache_clear();
    snx_x64_cache_ceiling(16);
    forget();
    CHECK(!build(out[0]) && run(out[0]) == 42);
    s = stats();
    CHECK(s.units == 0 && s.too_large == 2 && s.bytes == 0);
    CHECK(said("x86-64 unit cache: a unit of ", "bytes is more than the 16 bytes it holds"));
    CHECK(g_unit_cache.said_count == 1);
    /* a ceiling of 0: nothing held, every unit made */
    snx_x64_cache_clear();
    snx_x64_cache_ceiling(0);
    CHECK(snx_x64_cache_limit() == 0);
    CHECK(!build(out[1]) && run(out[1]) == 42);
    s = stats();
    CHECK(s.units == 0 && s.hits == 0 && s.misses == 0);

    for (i = 0; i < sizeof dirs / sizeof *dirs; ++i) snx_remove_tree(dirs[i]);
    printf("unit cache: %s\n", g_unit_cache.failures ? "FAILED" : "passed");
    return g_unit_cache.failures != 0;
}
