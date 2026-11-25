/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write marks (mach.h's sgx_mark, or LLVM's ir_marks.c) as a game's code is made: with the setting on, code made
   ahead of time (snx_build_module_llvm, O2 and O0) and by the JIT marks the pages its stores write, whichever back end
   makes it; a module says the setting it was made with (snx_write_marks), and a module made without is the one it would
   be, with no such symbol; a JIT cache's key differs with the setting, as units made without marks must not be loaded by
   a run that reads them. The stores off the stack pointer mark nothing */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/image.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/jit_cache.h"
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/cpu/recompiler/mach/native.h"
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
#define SUFFIX ".dll"
#else
#include <dlfcn.h>
#define SUFFIX ".so"
#endif

#define TEXT_OFF  0x1000
#define TEXT_SIZE 0x1000
#define PAGES     16 /* guest memory of the stores: [0, PAGES * 4 KiB) */
#define CODE_LO   0x1000
#define CODE_HI   0x2000

static int g_failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

/* str x1, [x0] (page 5); str x1, [x2] (page 9); stp x1, x1, [sp, #-16]! and add sp, sp, #16 (the stack: no mark);
   ret. The image's own memory is the module's */
static const SnxU32 g_code[] = {0xF9000001u, 0xF9000041u, 0xA9BF07E1u, 0x910043FFu, 0xD65F03C0u};
static const unsigned g_marked[] = {5, 9};
static void code_at(unsigned char* memory, size_t at) {
    for (size_t i = 0; i < sizeof g_code / sizeof *g_code; ++i)
        for (size_t j = 0; j < 4; ++j) memory[at + 4 * i + j] = (unsigned char)(g_code[i] >> (8 * j));
}

/* guest memory with the marks' bytes below it, and the function at fn run on it: the pages marked are those written */
static void check_run(GuestFn fn, u64 entry, CPU* cpu, const char* what) {
    unsigned char* block = (unsigned char*)calloc(1, SNX_WMARK_BELOW + PAGES * 4096);
    CHECK(block);
    if (!block) return;
    unsigned char *mem = block + SNX_WMARK_BELOW, *marks = block;
    cpu->mem = mem;
    cpu->x[0] = 5 * 4096 + 8;
    cpu->x[1] = 0x1122334455667788ull;
    cpu->x[2] = 9 * 4096;
    cpu->x[31] = 8 * 4096;
    fn(cpu, entry);
    for (unsigned pg = 0; pg < PAGES; ++pg) {
        int want = 0;
        for (size_t i = 0; snx_ir_write_marks_on() && i < sizeof g_marked / sizeof *g_marked; ++i)
            want |= g_marked[i] == pg;
        if (marks[pg] != want) {
            fprintf(stderr, "%s: page %u's mark is %u, expected %d\n", what, pg, marks[pg], want);
            ++g_failures;
        }
    }
    CHECK(mem[5 * 4096 + 8] == 0x88);
    free(block);
}

static void missed(CPU* c, u64 pc) {
    (void)pc;
    c->x[0] = 0xBAD;
}

/* the module of the code at base, built at opt and run */
static void check_module(char opt) {
    static unsigned char memory[TEXT_OFF + TEXT_SIZE];
    char dir[64], path[128];
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxLlvmBuild llvm = {0};
    size_t i;
    snprintf(dir, sizeof dir, "snx-test-write-marks-code.%c", opt);
    snprintf(path, sizeof path, "%s/module" SUFFIX, dir);
    snx_remove_tree(dir);
    CHECK(!snx_mkdirs(dir));
    memset(memory, 0, sizeof memory);
    code_at(memory, TEXT_OFF);
    image.name = "test";
    image.base = SNX_CODE_BASE;
    image.size = sizeof memory;
    image.image = memory;
    image.image_size = sizeof memory;
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
    llvm.output = path;
    llvm.opt = opt;
    llvm.jobs = 1;
    llvm.quiet = 1;
    CHECK(!snx_build_module_llvm(&loaded, &options, &llvm));
    typedef const SnxModule* (*ModuleFn)(void);
    char* absolute = snx_absolute(path);
#ifdef _WIN32
    HMODULE handle = absolute ? LoadLibraryA(absolute) : NULL;
    ModuleFn get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module") : NULL;
#else
    void* handle = absolute ? dlopen(absolute, RTLD_NOW | RTLD_LOCAL) : NULL;
    ModuleFn get = handle ? (ModuleFn)dlsym(handle, "snx_module") : NULL;
#endif
    CHECK(get);
    free(absolute);
    if (!get) return;
    const SnxModule* mod = get();
    /* the module says what it was made with, where it was */
    {
        typedef const SnxWriteMarks* (*MarksFn)(void);
#ifdef _WIN32
        MarksFn made = (MarksFn)(void*)GetProcAddress(handle, "snx_write_marks");
#else
        MarksFn made = (MarksFn)dlsym(handle, "snx_write_marks");
#endif
        CHECK(!!made == (snx_ir_write_marks_on() != 0));
        if (made) CHECK(made()->version == SNX_WRITE_MARKS_VERSION && made()->level == (u32)snx_ir_write_marks_on());
    }
    CPU cpu = {0};
    RtAPI rt = {.cas128 = snx_runtime_cas128, .miss = missed};
    cpu.rt = &rt;
    cpu.code_lo = mod->code_lo;
    cpu.code_hi = mod->code_hi;
    cpu.disp = (_Atomic GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4 + 1, sizeof(GuestFn));
    cpu.x[30] = 0xDEAD0000;
    for (i = 0; i < mod->nentries; ++i)
        if (mod->entry_addr[i] == SNX_CODE_BASE + TEXT_OFF) {
            char what[32];
            snprintf(what, sizeof what, "the module at O%c", opt);
            check_run(mod->entry_fn[i], mod->entry_addr[i], &cpu, what);
        }
    free(cpu.disp);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

/* the same function compiled by the JIT service */
static void check_jit(void) {
    static unsigned char memory[CODE_HI + 0x1000];
    static _Atomic GuestFn dispatch[(CODE_HI - CODE_LO) / 4];
    static SnxCodeImage image;
    SnxJitConfig config = {0};
    char error[512];
    code_at(memory, CODE_LO);
    image.image = memory;
    image.image_size = sizeof memory;
    image.text_off = CODE_LO;
    image.text_size = CODE_HI - CODE_LO;
    config.images = &image;
    config.image_count = 1;
    config.dispatch = dispatch;
    config.code_lo = CODE_LO;
    config.code_hi = CODE_HI;
    config.threads = 1;
    SnxJitService* service = snx_jit_service_new(&config, error, sizeof error);
    CHECK(service);
    if (!service) {
        fprintf(stderr, "jit: %s\n", error);
        return;
    }
    GuestFn fn = snx_jit_wait(service, CODE_LO);
    CHECK(fn);
    CPU cpu = {0};
    RtAPI rt = {.cas128 = snx_runtime_cas128, .miss = missed};
    cpu.rt = &rt;
    cpu.disp = dispatch;
    cpu.code_lo = CODE_LO;
    cpu.code_hi = CODE_HI;
    cpu.x[30] = 0xDEAD0000;
    if (fn) check_run(fn, CODE_LO, &cpu, "the JIT's code");
    snx_jit_service_free(service);
}

/* what the key is with the marks as given: malloc'd */
static char* key(int marks) {
    snx_ir_write_marks(marks);
    return snx_jit_cache_key('2', 0, 1, 0);
}

int main(void) {
    char error[512];
    CHECK(!snx_ir_init(error, sizeof error));
    char *off = key(0), *on = key(1), *both = key(2);
    CHECK(off && on && both && strcmp(off, on) && strcmp(on, both) && strcmp(off, both));
    free(off);
    free(on);
    free(both);
    printf("write_marks_code: the %s back end makes the code\n", snx_native_name());
    for (int marks = 0; marks < 2; ++marks) {
        char pc[32];
        snx_ir_write_marks(marks);
        check_module('2');
        check_module('0');
        check_jit();
        /* with the function left to LLVM, so the descriptor is LLVM's */
        snprintf(pc, sizeof pc, "%#llx", (unsigned long long)(SNX_CODE_BASE + TEXT_OFF));
        snx_ir_diagnostics(0, pc);
        check_module('0');
        snx_ir_diagnostics(0, NULL);
    }
    if (g_failures) {
        fprintf(stderr, "write_marks_code: %d failure(s)\n", g_failures);
        return 1;
    }
    puts(
        "write_marks_code: the module and the JIT marked the pages their stores wrote, and none without the setting; the "
        "cache keys differ");
    return 0;
}
