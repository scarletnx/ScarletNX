/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An AOT module's descriptor for a RISC host (nx/loader/module_x64.c over mach/risc_desc.inc), for each little-endian
   one under Unicorn (support/host_unicorn.c; loongarch64 in its interpreter), and arm64's (module_x64.c's own, which a
   pack of iOS's has whatever host makes it): the descriptor of a made-up program of
   three functions and two imports loaded with them (each function a trap), then
     snx_module()    its MOD, ENT_ADDR the entries' addresses and ENT_PTR their functions
     snx_routes()    its table's version
     snx_aot_<a>     a tail call of IMPORT_FN's pointer, the arguments as they were
     snx_import_dyn  CPU.disp's function for an address in the code, aligned, with one; else rt->miss
     snx_variants   a profile run's unit's functions: their data, and each one's tail call through the unit's table
     snx_aot_<e>    (with a profile run's units) a tail call of the entry's function
   A big-endian host's descriptor is the same code; its data is the host's numbers, which a test here can't read. With
   SNX_TEST_WIN64 (snx-test-module-desc-win64) it is x86-64's for Windows, a COFF object of its calling convention, run by
   an x86-64 host itself (support/host_win64.c); with SNX_TEST_ARM64WIN (snx-test-module-desc-arm64win) arm64's for
   Windows, and arm64's of a pack beside it, which stays ELF.
     snx-test-module-desc [MACHINE...]   (riscv64 ppc64le mips64el loongarch64 arm64 by default) */
#include "nx/loader/module_x64.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/loader.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "common/cpu/recompiler/variants.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "support/host_unicorn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_called, g_args[2];
static uint64_t fn0(uint64_t a, uint64_t b) { return g_called = 0xF0, g_args[0] = a, g_args[1] = b, 0xF0; }
static uint64_t fn1(uint64_t a, uint64_t b) { return g_called = 0xF1, g_args[0] = a, g_args[1] = b, 0xF1; }
static uint64_t fn2(uint64_t a, uint64_t b) { return g_called = 0xF2, g_args[0] = a, g_args[1] = b, 0xF2; }
static uint64_t imported(uint64_t a, uint64_t b) { return g_called = 0x1A, g_args[0] = a, g_args[1] = b, 0x1A; }
static uint64_t missed(uint64_t a, uint64_t b) { return g_called = 0x5A, g_args[0] = a, g_args[1] = b, 0x5A; }

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s: ", name);                                                                        \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static int machine(const char* name, HostUcArch arch) {
#if defined(SNX_TEST_WIN64)
    const SnxNativeDesc* desc = NULL; /* module_x64.c's own */
    const int found = snx_x64_win64();
#elif defined(SNX_TEST_ARM64WIN)
    const SnxNativeDesc* desc = NULL;
    const int found = snx_arm64_windows();
#else
    const SnxNativeDesc* desc = !strcmp(name, "arm64") ? &snx_a64_desc : snx_native_desc_named(name);
    const int found = desc != NULL;
#endif
    static const SnxU64 starts[3] = {SNX_CODE_BASE + 0x100, SNX_CODE_BASE + 0x200, SNX_CODE_BASE + 0x300};
    static void* const fns[3] = {(void*)fn0, (void*)fn1, (void*)fn2};
    /* the entries: function 0's words 0 and 2, function 1's 0, function 2's 0 and 1 */
    unsigned char leaders[3][4] = {{1, 0, 1, 0}, {1, 0, 0, 0}, {1, 1, 0, 0}};
    static const SnxU64 want_addr[5] = {SNX_CODE_BASE + 0x100, SNX_CODE_BASE + 0x108, SNX_CODE_BASE + 0x200,
                                        SNX_CODE_BASE + 0x300, SNX_CODE_BASE + 0x304};
    static const int want_fn[5] = {0, 0, 1, 2, 2};
    const SnxU64 imports[2] = {0x7100001000ull, 0x7100002000ull};
    SnxFunction functions[3];
    SnxCodegen codegen;
    SnxCodegenOptions options;
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    unsigned char* object = NULL;
    size_t size = 0, entries = 0, variants = 0;
    char error[256], sym[48];
    int failures = 0;
    HostUc* u;
    if (!found || !loaded) {
        fprintf(stderr, "FAIL %s: no descriptor of the machine's\n", name);
        free(loaded);
        return 1;
    }
    memset(&codegen, 0, sizeof codegen);
    memset(&options, 0, sizeof options);
    for (int i = 0; i < 3; ++i) {
        memset(&functions[i], 0, sizeof functions[i]);
        functions[i].start = starts[i];
        functions[i].end = starts[i] + 16;
        functions[i].count = 4;
        functions[i].leaders = leaders[i];
        functions[i].leader_count = (size_t)(leaders[i][0] + leaders[i][1] + leaders[i][2] + leaders[i][3]);
    }
    codegen.functions = functions;
    codegen.function_count = 3;
    /* a profile run's unit of two functions, its table UNIT_T (the test's: two traps) */
    static const SnxU64 vleaders[2] = {0x7200000000ull, 0x7200000010ull};
    static const SnxVariantRun vruns[1] = {{0x7200000000ull, 8}};
    static const SnxJitCacheFunction vfns[2] = {{0x7200000000ull, 2, vleaders, 1, vruns, 0x1111, 0x2222},
                                                {0x7200000100ull, 0, NULL, 0, NULL, 0x3333, 0x4444}};
    SnxJitCacheRecord record = {"UNIT_T", 2, vfns, NULL, 0};
    if (snx_x64_descriptor_for(desc, &codegen, &options, loaded, imports, 2, &record, 1, &object, &size, &entries,
                               &variants, error, sizeof error)) {
        fprintf(stderr, "FAIL %s: the descriptor: %s\n", name, error);
        free(loaded);
        return 1;
    }
    free(loaded);
    if (!(u = hostuc_new(arch, 1u << 20))) {
        printf("SKIP: no Unicorn of %s: nothing to run\n", name);
        free(object);
        return 77;
    }
    for (int i = 0; i < 3; ++i) {
        snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)starts[i]);
        hostuc_native(u, sym, fns[i]);
    }
    /* the unit's table: fn0's and fn2's traps */
    uint64_t* unit = (uint64_t*)hostuc_alloc(u, 16);
    if (!unit || hostuc_define(u, "UNIT_T", (uint64_t)(uintptr_t)unit)) {
        fprintf(stderr, "FAIL %s: out of memory\n", name);
        free(object);
        hostuc_free(u);
        return 1;
    }
    snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)starts[0]);
    unit[0] = hostuc_trap_of(u, sym);
    snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)starts[2]);
    unit[1] = hostuc_trap_of(u, sym);
    if (hostuc_load(u, object, size, error, sizeof error)) {
        fprintf(stderr, "FAIL %s: loading the descriptor: %s\n", name, error);
        free(object);
        hostuc_free(u);
        return 1;
    }
    free(object);
    /* snx_module() */
    {
        uint64_t got = 0;
        const SnxModule* mod;
        CHECK(entries == 5, "%zu entries, not 5", entries);
        CHECK(!hostuc_call(u, hostuc_symbol(u, "snx_module"), 0, 0, &got), "snx_module stopped");
        mod = (const SnxModule*)(uintptr_t)got;
        CHECK(got && got == hostuc_symbol(u, "MOD"), "snx_module gave %llx, MOD is %llx", (unsigned long long)got,
              (unsigned long long)hostuc_symbol(u, "MOD"));
        if (got && got == hostuc_symbol(u, "MOD")) {
            CHECK(mod->nentries == 5, "MOD.nentries %u", mod->nentries);
            for (int i = 0; i < 5 && mod->nentries == 5; ++i) {
                uint64_t fn = (uint64_t)(uintptr_t)mod->entry_fn[i];
                snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)starts[want_fn[i]]);
                CHECK(mod->entry_addr[i] == want_addr[i], "entry %d at %llx, not %llx", i,
                      (unsigned long long)mod->entry_addr[i], (unsigned long long)want_addr[i]);
                CHECK(fn == hostuc_trap_of(u, sym), "entry %d's function %llx, not %s's %llx", i,
                      (unsigned long long)fn, sym, (unsigned long long)hostuc_trap_of(u, sym));
            }
        }
    }
    /* snx_variants() and the wrappers */
    {
        uint64_t got = 0;
        const SnxVariants* v;
        CHECK(variants == 2, "%zu variants, not 2", variants);
        CHECK(!hostuc_call(u, hostuc_symbol(u, "snx_variants"), 0, 0, &got), "snx_variants stopped");
        v = (const SnxVariants*)(uintptr_t)got;
        CHECK(v && v->version == SNX_VARIANTS_VERSION && v->count == 2, "snx_variants' table wrong");
        if (v && v->count == 2) {
            const SnxVariantFunction* f = v->functions;
            CHECK(f[0].start == vfns[0].start && f[0].leader_count == 2 && f[0].leaders[1] == vleaders[1] &&
                      f[0].run_count == 1 && f[0].runs[0].words == 8 && f[0].h1 == 0x1111 && f[0].h2 == 0x2222,
                  "variant 0's data");
            CHECK(f[1].start == vfns[1].start && !f[1].leader_count && !f[1].leaders && !f[1].runs &&
                      f[1].h2 == 0x4444,
                  "variant 1's data");
            for (int k = 0; k < 2; ++k) {
                uint64_t want = k ? 0xF2 : 0xF0;
                g_called = 0;
                CHECK(!hostuc_call(u, (uint64_t)(uintptr_t)f[k].fn, 0xC0 + k, 0xE0 + k, &got), "variant %d stopped",
                      k);
                CHECK(g_called == want && g_args[0] == 0xC0u + k && g_args[1] == 0xE0u + k,
                      "variant %d called %llx", k, (unsigned long long)g_called);
            }
        }
        for (int i = 0; i < 5; ++i) {
            uint64_t want = 0xF0 + (uint64_t)want_fn[i];
            snprintf(sym, sizeof sym, "snx_aot_%llx", (unsigned long long)want_addr[i]);
            g_called = 0;
            CHECK(hostuc_symbol(u, sym) && !hostuc_call(u, hostuc_symbol(u, sym), 7, want_addr[i], &got) &&
                      g_called == want && g_args[1] == want_addr[i],
                  "%s called %llx, not %llx", sym, (unsigned long long)g_called, (unsigned long long)want);
        }
    }
    /* snx_routes() */
    {
        uint64_t got = 0;
        CHECK(!hostuc_call(u, hostuc_symbol(u, "snx_routes"), 0, 0, &got), "snx_routes stopped");
        CHECK(got && ((const SnxRoutes*)(uintptr_t)got)->version == SNX_ROUTES_VERSION, "snx_routes' table wrong");
    }
    /* the imports' stubs: IMPORT_FN[1] made imported's trap */
    {
        uint64_t* table = (uint64_t*)(uintptr_t)hostuc_symbol(u, "IMPORT_FN");
        uint64_t got = 0, stub;
        snprintf(sym, sizeof sym, "snx_aot_%llx", (unsigned long long)imports[1]);
        stub = hostuc_symbol(u, sym);
        CHECK(table && stub, "no IMPORT_FN or %s", sym);
        if (table && stub) {
            table[1] = hostuc_native(u, "imported", (void*)imported);
            g_called = 0;
            CHECK(!hostuc_call(u, stub, 0x1234, 0x5678, &got), "%s stopped", sym);
            CHECK(got == 0x1A && g_called == 0x1A && g_args[0] == 0x1234 && g_args[1] == 0x5678,
                  "%s: %llx called %llx with %llx, %llx", sym, (unsigned long long)got, (unsigned long long)g_called,
                  (unsigned long long)g_args[0], (unsigned long long)g_args[1]);
        }
    }
    /* snx_import_dyn: CPU.disp's function, else rt->miss */
    {
        CPU* c = (CPU*)hostuc_alloc(u, sizeof *c);
        RtAPI* rt = (RtAPI*)hostuc_alloc(u, sizeof *rt);
        uint64_t* disp = (uint64_t*)hostuc_alloc(u, 64 * 8);
        uint64_t dyn = hostuc_symbol(u, "snx_import_dyn");
        static const struct {
            uint64_t e, called;
        } cases[] = {{0x10008, 0xF1}, {0x10010, 0x5A}, {0x1000A, 0x5A}, {0x0FFFC, 0x5A}, {0x10100, 0x5A}};
        CHECK(c && rt && disp && dyn, "no state or no snx_import_dyn");
        if (c && rt && disp && dyn) {
            c->code_lo = 0x10000, c->code_hi = 0x10100;
            c->disp = (SNX_ATOMIC(GuestFn)*)(void*)disp;
            c->rt = rt;
            rt->miss = (GuestFn)(uintptr_t)hostuc_native(u, "missed", (void*)missed);
            snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)starts[1]);
            disp[2] = hostuc_trap_of(u, sym); /* 0x10008 */
            for (size_t k = 0; k < sizeof cases / sizeof *cases; ++k) {
                uint64_t got = 0;
                g_called = 0;
                CHECK(!hostuc_call(u, dyn, (uint64_t)(uintptr_t)c, cases[k].e, &got), "snx_import_dyn(%llx) stopped",
                      (unsigned long long)cases[k].e);
                CHECK(g_called == cases[k].called && g_args[0] == (uint64_t)(uintptr_t)c && g_args[1] == cases[k].e,
                      "snx_import_dyn(%llx) called %llx (want %llx) with %llx", (unsigned long long)cases[k].e,
                      (unsigned long long)g_called, (unsigned long long)cases[k].called,
                      (unsigned long long)g_args[1]);
            }
        }
    }
    hostuc_free(u);
    printf("module_desc %s: %s\n", name, failures ? "FAILED" : "ok");
    return failures != 0;
}

#if defined(SNX_TEST_ARM64WIN)
/* a module of so many imports that IMPORT_FN's slots go past what COFF's ADRP holds: the stubs on both sides of that
   place each read their own */
static int many_imports(void) {
    enum { COUNT = 140000, SIDE = (1 << 20) / 8 };
    static const size_t asked[] = {0, SIDE - 1, SIDE, SIDE + 1, COUNT - 1};
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    SnxU64* imports = (SnxU64*)calloc(COUNT, sizeof *imports);
    SnxCodegen codegen = {0};
    SnxCodegenOptions options = {0};
    const char* name = "arm64 imports";
    unsigned char* object = NULL;
    size_t size = 0, entries = 0, variants = 0;
    char error[256] = "", sym[48];
    int failures = 0;
    HostUc* u = hostuc_new(HOSTUC_ARM64_WIN, 1u << 20);
    uint64_t* table = NULL;
    if (!u) {
        free(loaded), free(imports);
        return 77;
    }
    for (size_t k = 0; imports && k < COUNT; ++k) imports[k] = 0x7100000000ull + 16 * k;
    if (!loaded || !imports ||
        snx_x64_descriptor_for(NULL, &codegen, &options, loaded, imports, COUNT, NULL, 0, &object, &size, &entries,
                               &variants, error, sizeof error) ||
        hostuc_load(u, object, size, error, sizeof error) ||
        !(table = (uint64_t*)(uintptr_t)hostuc_symbol(u, "IMPORT_FN")))
        CHECK(0, "the descriptor of %d imports: %s", COUNT, error);
    else {
        const uint64_t to = hostuc_native(u, "imported", (void*)imported);
        const uint64_t other = hostuc_native(u, "missed", (void*)missed);
        for (size_t k = 0; k < COUNT; ++k) table[k] = other;
        for (size_t k = 0; k < sizeof asked / sizeof *asked; ++k) {
            uint64_t got = 0;
            table[asked[k]] = to;
            snprintf(sym, sizeof sym, "snx_aot_%llx", (unsigned long long)imports[asked[k]]);
            CHECK(!hostuc_call(u, hostuc_symbol(u, sym), 1, 2, &got) && got == 0x1A, "%s gave %llx", sym,
                  (unsigned long long)got);
            table[asked[k]] = other;
        }
    }
    printf("module_desc %s: %s\n", name, failures ? "FAILED" : "ok");
    free(object), free(loaded), free(imports);
    hostuc_free(u);
    return failures != 0;
}
#endif

#if defined(SNX_TEST_ARM64WIN)
/* a Windows host's own descriptor is a COFF object, and a pack's (snx_a64_desc: a phone's) an ELF one all the same */
static int packed(void) {
    const char* name = "a pack's arm64";
    const SnxU64 imports[1] = {0x7100000000ull};
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    SnxCodegen codegen = {0};
    SnxCodegenOptions options = {0};
    int failures = 0;
    for (int own = 0; loaded && own < 2; ++own) {
        unsigned char* object = NULL;
        size_t size = 0, entries = 0, variants = 0;
        char error[256] = "";
        const int rc = snx_x64_descriptor_for(own ? NULL : &snx_a64_desc, &codegen, &options, loaded, imports, 1, NULL,
                                              0, &object, &size, &entries, &variants, error, sizeof error);
        CHECK(!rc, "%s descriptor: %s", own ? "the host's own" : "a pack's", error);
        if (!rc && own)
            CHECK(size > 2 && object[0] == 0x64 && object[1] == 0xAA, "the host's own descriptor is not COFF's arm64");
        if (!rc && !own)
            CHECK(size > 20 && !memcmp(object, "\177ELF", 4) && object[18] == 183 && object[19] == 0,
                  "a pack's descriptor is not ELF's arm64");
        free(object);
    }
    if (!loaded) ++failures;
    printf("module_desc %s: %s\n", name, failures ? "FAILED" : "ok");
    free(loaded);
    return failures != 0;
}
#endif

int main(int argc, char** argv) {
    static const struct {
        const char* name;
        HostUcArch arch;
    } all[] = {
#if defined(SNX_TEST_WIN64)
        {"x86-64", HOSTUC_WIN64}
#elif defined(SNX_TEST_ARM64WIN)
        {"arm64", HOSTUC_ARM64_WIN}
#else
        {"riscv64", HOSTUC_RV64},
        {"ppc64le", HOSTUC_PPC64LE},
        {"mips64el", HOSTUC_MIPS64EL},
        {"loongarch64", HOSTUC_LA64},
        {"arm64", HOSTUC_ARM64}
#endif
    };
    int failed = 0, skipped = 0, ran = 0;
#if defined(SNX_TEST_WIN64)
    snx_x64_set_win64(1);
#elif defined(SNX_TEST_ARM64WIN)
    snx_arm64_set_windows(1);
#endif
    for (size_t k = 0; k < sizeof all / sizeof *all; ++k) {
        int asked = argc < 2;
        for (int a = 1; a < argc; ++a) asked |= !strcmp(argv[a], all[k].name);
        if (!asked) continue;
        int rc = machine(all[k].name, all[k].arch);
        failed |= rc == 1, skipped += rc == 77, ran += rc != 77;
    }
#if defined(SNX_TEST_ARM64WIN)
    failed |= many_imports() == 1;
    failed |= packed();
#endif
    return failed ? 1 : skipped && !ran ? 77 : 0;
}
