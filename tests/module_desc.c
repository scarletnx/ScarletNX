/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An AOT module's descriptor for a RISC host (nx/loader/module_x64.c over mach/risc_desc.inc), for each little-endian
   one under Unicorn (support/host_unicorn.c; loongarch64 in its interpreter): the descriptor of a made-up program of
   three functions and two imports loaded with them (each function a trap), then
     snx_module()    its MOD, ENT_ADDR the entries' addresses and ENT_PTR their functions
     snx_routes()    its table's version
     snx_aot_<a>     a tail call of IMPORT_FN's pointer, the arguments as they were
     snx_import_dyn  CPU.disp's function for an address in the code, aligned, with one; else rt->miss
   A big-endian host's descriptor is the same code; its data is the host's numbers, which a test here can't read.
     snx-test-module-desc [MACHINE...]   (riscv64 ppc64le mips64el loongarch64 by default) */
#include "nx/loader/module_x64.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/loader.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
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
    const SnxNativeDesc* desc = snx_native_desc_named(name);
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
    size_t size = 0, entries = 0;
    char error[256], sym[48];
    int failures = 0;
    HostUc* u;
    if (!desc || !loaded) {
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
    if (snx_x64_descriptor_for(desc, &codegen, &options, loaded, imports, 2, &object, &size, &entries, error,
                               sizeof error)) {
        fprintf(stderr, "FAIL %s: the descriptor: %s\n", name, error);
        free(loaded);
        return 1;
    }
    free(loaded);
    if (!(u = hostuc_new(arch, 1u << 20))) {
        printf("module_desc %s: no Unicorn of it: skipped\n", name);
        free(object);
        return 0;
    }
    for (int i = 0; i < 3; ++i) {
        snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)starts[i]);
        hostuc_native(u, sym, fns[i]);
    }
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

int main(int argc, char** argv) {
    static const struct {
        const char* name;
        HostUcArch arch;
    } all[] = {{"riscv64", HOSTUC_RV64}, {"ppc64le", HOSTUC_PPC64LE}, {"mips64el", HOSTUC_MIPS64EL},
               {"loongarch64", HOSTUC_LA64}};
    int failed = 0;
    for (size_t k = 0; k < sizeof all / sizeof *all; ++k) {
        int asked = argc < 2;
        for (int a = 1; a < argc; ++a) asked |= !strcmp(argv[a], all[k].name);
        if (asked) failed |= machine(all[k].name, all[k].arch);
    }
    return failed;
}
