/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A module packed for iOS (--target ios: recompiler/object/objects_pack.h), made on this host by the arm64 back end
   (mach/native.h's snx_native_target): the pack's every object is an arm64
   ELF relocatable one, and no function is refused. Then the pack as the app's loader takes it (code_loader.c): its
   objects loaded into an arena of the regions they need, and where Unicorn is, snx_module() called and the program's
   functions run through the descriptor's entries, across the objects. Other triples are no native back end's
     ios_pack PACK   (the file it writes) */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/loader/code_loader_int.h"
#include "common/cpu/recompiler/object/elf_object.h"
#include "common/cpu/recompiler/object/objects_pack.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/runtime/interface.h"
#include "common/utilities/common.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_a64.h"
#ifdef SNX_TEST_UNICORN
#include "support/host_unicorn.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_OFF  0x1000
#define TEXT_SIZE 0x1000
#define TRIPLE    "arm64-apple-ios16.0-elf"

static struct {
    int failures;
    unsigned char memory[TEXT_OFF + TEXT_SIZE];
} g_ios_pack;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++g_ios_pack.failures;                                                                                     \
        }                                                                                                              \
    } while (0)

/* f0: x9 = x30; bl f1; x30 = x9; ret. f1: x0 += 1; b f2. f2: x0 += 1; ret. Each function a unit, so each call crosses
   objects (CALL26, JUMP26) */
static void program(void) {
    static const SnxU32 code[] = {0xAA1E03E9u, 0x94000003u, 0xAA0903FEu, 0xD65F03C0u,
                                  0x91000400u, 0x14000001u, 0x91000400u, 0xD65F03C0u};
    for (size_t i = 0; i < sizeof code / sizeof *code; ++i)
        for (size_t j = 0; j < 4; ++j) g_ios_pack.memory[TEXT_OFF + 4 * i + j] = (unsigned char)(code[i] >> (8 * j));
}

static int build(const char* output, const char* triple) {
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxModuleBuild job = {0};
    image.name = "test";
    image.base = SNX_CODE_BASE;
    image.size = sizeof g_ios_pack.memory;
    image.image = g_ios_pack.memory;
    image.image_size = sizeof g_ios_pack.memory;
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
    job.output = output;
    job.opt = '2';
    job.jobs = 1;
    job.quiet = 1;
    job.triple = triple;
    return snx_build_module_a64(&loaded, &options, &job);
}

/* the pack's objects loaded as the app does (main.c's load_pack: the JIT loader's arena of the regions they need): the
   module's descriptor, 0 where it is not loaded; NULL loader where the arena could not be made */
static SnxU64 load(const SnxLinkInput* inputs, size_t count, SnxCodeLoader** out) {
    const char* names[1] = {"snx_module"};
    SnxU64 address = 0;
    size_t need[3];
    char error[256] = "";
    SnxCodeLoader* loader = NULL;
    if (snx_code_need(inputs, count, need, error, sizeof error) ||
        !(loader = snx_code_loader_new_regions_machine(need, 183, error, sizeof error)) /* EM_AARCH64 */ ||
        snx_code_load(loader, inputs, count, names, 1, &address, error, sizeof error)) {
        fprintf(stderr, "the pack does not load: %s\n", error);
        ++g_ios_pack.failures;
        if (loader) snx_code_loader_free(loader);
        *out = NULL;
        return 0;
    }
    *out = loader;
    return address;
}

#ifdef SNX_TEST_UNICORN
static void poll_clear(CPU* c) { c->poll = 0; }
static void missed(CPU* c, u64 entry) {
    (void)entry;
    c->x[0] = 0xBAD;
}
/* the descriptor's entry at address run from the state: 1, 0 where the module has none there or it stops */
static int run_entry(HostUc* u, const SnxModule* mod, u64 address, CPU* c) {
    uint64_t got = 0;
    for (u32 i = 0; i < mod->nentries; ++i)
        if (mod->entry_addr[i] == address)
            return !hostuc_call(u, (uint64_t)(uintptr_t)mod->entry_fn[i], (uint64_t)(uintptr_t)c, address, &got);
    return 0;
}
/* the loaded arena's code, run: f0 takes 41 through f1 and f2 */
static void run(SnxCodeLoader* loader, SnxU64 get_module) {
    HostUc* u = hostuc_new(HOSTUC_ARM64, 1u << 16);
    uint64_t got = 0;
    if (!u) {
        printf("ios_pack: no Unicorn of arm64: its code is not run here\n");
        return;
    }
    CHECK(!hostuc_map(u, loader->base, loader->reserved));
    CHECK(!hostuc_call(u, get_module, 0, 0, &got));
    const SnxModule* mod = (const SnxModule*)(uintptr_t)got;
    CPU* c = (CPU*)hostuc_alloc(u, sizeof *c);
    RtAPI* rt = (RtAPI*)hostuc_alloc(u, sizeof *rt);
    uint64_t* disp = (uint64_t*)hostuc_alloc(u, TEXT_SIZE / 4 * 8);
    CHECK(mod && c && rt && disp);
    if (mod && c && rt && disp) {
        CHECK(mod->nentries >= 3);
        rt->poll = (void (*)(CPU*))(uintptr_t)hostuc_native(u, "poll_clear", (void*)poll_clear);
        rt->miss = (void (*)(CPU*, u64))(uintptr_t)hostuc_native(u, "missed", (void*)missed);
        c->rt = rt;
        c->code_lo = SNX_CODE_BASE + TEXT_OFF, c->code_hi = SNX_CODE_BASE + TEXT_OFF + TEXT_SIZE;
        c->disp = (void*)disp;
        c->x[0] = 41;
        CHECK(run_entry(u, mod, SNX_CODE_BASE + TEXT_OFF, c));
        CHECK(c->x[0] == 43 && c->x[30] == 0);
        if (c->x[0] != 43) fprintf(stderr, "x0 is %llu, not 43\n", (unsigned long long)c->x[0]);
    }
    hostuc_free(u);
}
#endif

int main(int argc, char** argv) {
    size_t size = 0, count = 0;
    unsigned char* data;
    SnxLinkInput* inputs;
    if (argc != 2) {
        fprintf(stderr, "usage: ios_pack PACK\n");
        return 2;
    }
    CHECK(!snx_native_target("arm64-apple-macos13-elf"));
    CHECK(!snx_native_target("aarch64-linux-gnu"));
    CHECK(!snx_native_target("arm64-apple-ios16.0"));
    if (!snx_native_target(TRIPLE)) {
        printf("ios_pack: this host's build makes no native iOS pack: nothing to check\n");
        return 77;
    }
    program();
    if (build(argv[1], TRIPLE)) {
        fprintf(stderr, "the pack was not built\n");
        return 1;
    }
    data = (unsigned char*)snx_read_file(argv[1], &size);
    inputs = data ? snx_objects_unpack(data, size, "pack", &count) : NULL;
    CHECK(inputs && count >= 3); /* the descriptor, f0's unit and f1's, with f2 in it or of its own */
    for (size_t i = 0; inputs && i < count; ++i) {
        SnxElfObject object;
        unsigned machine = 183, abi = 0; /* EM_AARCH64 */
        char error[256] = "";
        int bad = snx_elf_parse(&object, &inputs[i], &machine, &abi, error, sizeof error);
        if (bad) fprintf(stderr, "object %zu: %s\n", i, error);
        CHECK(!bad);
        if (!bad) snx_elf_free(&object);
    }
    if (inputs) {
        SnxCodeLoader* loader;
        SnxU64 get_module = load(inputs, count, &loader);
        if (get_module) {
#ifdef SNX_TEST_UNICORN
            run(loader, get_module);
#else
            printf("ios_pack: its code is not run here (no Unicorn)\n");
#endif
        }
        if (loader) snx_code_loader_free(loader);
    }
    free(inputs);
    free(data);
    printf("ios_pack: %zu objects, %d failures\n", count, g_ios_pack.failures);
    return g_ios_pack.failures != 0;
}
