/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A whole program through the backends: a synthetic NRO with a loop, BL/RET, an indirect branch inside its function
   (the D_ dispatch), a tail call into another function, SVC, exclusives, FP and SIMD, recompiled by the CLI with each
   backend in small chunks (calls cross units). Every module runs it from the same state, AOT and through the reference
   interpreter of the templates' C (snx_a64_interp, built here: modules carry none since the runtime JIT replaced it),
   which also takes the runs' misses; every register, the flags, FP state, stack memory and the SVC calls must match
   the LLVM AOT run.
   CPU.poll is set when each run starts: the AOT code must call rt->poll at the loop's backward branch, resuming at
   its head (the interpreter polls on any taken branch). A second LLVM module built with --hook-direct-calls must run
   the same, with main's BL to func reaching func through CPU.disp, where a wrapper counts it.
     llvm_module SCARLETNX REPO CLANG [BACKEND...]
   llvm always, the others named after CLANG too (gcc). */
#include "common/cpu/aarch64/a64.h"
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/loader/module_abi.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* assembled with clang -target aarch64-linux-gnu: main at 0x80 (reached by the B at 0), func, tail at 0x200 */
static const u32 program[] = {0xD2800000, 0xD2800181, 0x1E6F1001, 0x6F00E402, 0x94000016, 0x9E620003, 0x1E632800,
                              0x1E610800, 0x4EA28442, 0x4E040C24, 0x4EA48442, 0xF1000421, 0x54FFFF01, 0x10000062,
                              0xD61F0040, 0xD4200020, 0xF81F0FE0, 0xA8C113E3, 0xB4000043, 0x910FA000, 0x885FFFE7,
                              0x8808FFE0, 0x37180040, 0xD2401C00, 0xAA1F03FE, 0x14000047, 0x8B010000, 0xD37FF804,
                              0xCA440C00, 0x9B011005, 0x9AC108A6, 0x9A868000, 0xD65F03C0};
static const u32 tail[] = {0xAA0003E6, 0xD40000E1, 0x9E780009, 0xD65F03C0};

#define IMAGE  0x400
#define MEMORY 0x10000

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static void put32(unsigned char* p, size_t off, unsigned long n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[off + i] = (unsigned char)(n >> (8 * i));
}

typedef struct {
    u64 imm[8], x0[8];
    int count, undefined;
} Calls;
static Calls* calls;
static void svc(CPU* c, u32 imm) {
    if (calls->count < 8) {
        calls->imm[calls->count] = imm;
        calls->x0[calls->count++] = c->x[0];
    }
}
static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
    ++calls->undefined;
}

typedef struct {
    CPU cpu;
    unsigned char memory[MEMORY];
    Calls calls;
    int polls; /* rt->poll calls, and c->pc at the first */
    u64 poll_pc;
    int watched; /* calls through the dispatch entry run() wrapped */
} Run;
static Run* current;
static GuestFn watched_entry;
static void watch(CPU* c, u64 e) {
    ++current->watched;
    watched_entry(c, e);
}
/* the runtime takes the thread off its core here, then gives it back: once, the flag cleared */
static void on_poll(CPU* c) {
    if (!current->polls++) current->poll_pc = c->pc;
    c->poll = 0;
}

static GuestFn reference; /* the templates' interpreter, built by build_reference */

/* the interpreter of a64_templates.inc's C, compiled into a library of its own with compiler and loaded: 0, or -1 */
static int build_reference(const char* root, const char* repo, const char* compiler) {
    char *source = snx_path_join(root, "reference.c"), *include = snx_path_join(repo, "src"),
         *output = NULL;
#ifdef _WIN32
    char* library = snx_path_join(root, "reference.dll");
    HMODULE handle;
#else
    char* library = snx_path_join(root, "reference.so");
    void* handle;
#endif
    const char* args[16];
    FILE* file = snx_fopen(source, "wb");
    int n = 0, status;
    if (!file) return -1;
    fputs("#include \"common/cpu/aarch64/cpu.h\"\n#include \"common/cpu/aarch64/cpu_helpers.c\"\n", file);
    snx_a64_interp(file, 0);
    fputs("SNX_EXPORT void snx_reference(CPU* c, u64 e) { snx_interp(c, e); }\n", file);
    if (fclose(file)) return -1;
    args[n++] = compiler;
    args[n++] = "-O1";
    args[n++] = "-shared";
#ifndef _WIN32
    args[n++] = "-fPIC";
#endif
    args[n++] = "-fno-strict-aliasing";
    args[n++] = "-fwrapv";
#if defined(__x86_64__) || defined(_M_X64)
    args[n++] = "-mcx16";
#endif
    args[n++] = "-w";
    args[n++] = "-I";
    args[n++] = include;
    args[n++] = source;
    args[n++] = "-o";
    args[n++] = library;
    args[n] = NULL;
    status = snx_run(args, root, NULL, &output);
    if (status && output) fputs(output, stderr);
    free(output);
    if (status) return -1;
#ifdef _WIN32
    handle = LoadLibraryA(library);
    reference = handle ? (GuestFn)(void*)GetProcAddress(handle, "snx_reference") : NULL;
#else
    handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    reference = handle ? (GuestFn)dlsym(handle, "snx_reference") : NULL;
#endif
    free(source);
    free(include);
    free(library);
    return reference ? 0 : -1;
}

/* watch_at: an offset in the image whose dispatch entry counts its calls (0: none) */
static int run(const char* path, const unsigned char* image, int interp, u64 watch_at, Run* out) {
    typedef const SnxModule* (*ModuleFn)(void);
    ModuleFn get;
    const SnxModule* mod;
    RtAPI rt;
    unsigned char* memory;
    size_t i;
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module") : NULL;
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "%s\n", dlerror());
    get = handle ? (ModuleFn)dlsym(handle, "snx_module") : NULL;
#endif
    if (!get) return -1;
    mod = get();
    memset(out, 0, sizeof(*out));
    memset(&rt, 0, sizeof(rt));
    rt.svc = svc;
    rt.undef = undefined;
    rt.poll = on_poll;
    rt.miss = reference; /* a pc a module has no function for */
    calls = &out->calls;
    current = out;
    memory = (unsigned char*)calloc(1, (size_t)mod->mods[0].base + MEMORY);
    out->cpu.disp = (GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4, sizeof(GuestFn));
    memcpy(memory + mod->mods[0].base, image, IMAGE);
    for (i = 0; i < mod->nentries; ++i) out->cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
    if (watch_at) {
        GuestFn* slot = &out->cpu.disp[(mod->mods[0].base + watch_at - mod->code_lo) / 4];
        watched_entry = *slot;
        if (watched_entry) *slot = watch;
    }
    out->cpu.mem = memory;
    out->cpu.rt = &rt;
    out->cpu.code_lo = mod->code_lo;
    out->cpu.code_hi = mod->code_hi;
    out->cpu.x[31] = mod->mods[0].base + MEMORY / 2;
    out->cpu.x[30] = 0xDEAD0000;
    out->cpu.poll = 1;
    if (interp)
        reference(&out->cpu, mod->mods[0].base);
    else if (out->cpu.disp[0])
        out->cpu.disp[0](&out->cpu, mod->mods[0].base);
    memcpy(out->memory, memory + mod->mods[0].base, MEMORY);
    free(memory);
    free(out->cpu.disp);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return 0;
}

static int same(const Run* a, const Run* b) {
    return !memcmp(a->cpu.x, b->cpu.x, sizeof(a->cpu.x)) && a->cpu.pc == b->cpu.pc && a->cpu.n == b->cpu.n &&
           a->cpu.z == b->cpu.z && a->cpu.cf == b->cpu.cf && a->cpu.vf == b->cpu.vf &&
           !memcmp(a->cpu.v, b->cpu.v, sizeof(a->cpu.v)) && a->cpu.fpsr == b->cpu.fpsr &&
           a->cpu.excl_on == b->cpu.excl_on && !memcmp(a->memory, b->memory, MEMORY) &&
           !memcmp(&a->calls, &b->calls, sizeof(a->calls));
}

/* what same() found apart, with both values, for a log from a machine the test can't be rerun on */
static void differences(const Run* a, const Run* b, const char* what) {
    size_t i;
    for (i = 0; i < sizeof(a->cpu.x) / sizeof(a->cpu.x[0]); ++i)
        if (a->cpu.x[i] != b->cpu.x[i])
            fprintf(stderr, "  %s x%lu=%llx, llvm %llx\n", what, (unsigned long)i, (unsigned long long)b->cpu.x[i],
                    (unsigned long long)a->cpu.x[i]);
    for (i = 0; i < sizeof(a->cpu.v) / sizeof(a->cpu.v[0]); ++i)
        if (memcmp(&a->cpu.v[i], &b->cpu.v[i], sizeof(a->cpu.v[i])))
            fprintf(stderr, "  %s v%lu=%llx:%llx, llvm %llx:%llx\n", what, (unsigned long)i,
                    (unsigned long long)b->cpu.v[i].d[1], (unsigned long long)b->cpu.v[i].d[0],
                    (unsigned long long)a->cpu.v[i].d[1], (unsigned long long)a->cpu.v[i].d[0]);
    if (a->cpu.pc != b->cpu.pc || a->cpu.n != b->cpu.n || a->cpu.z != b->cpu.z || a->cpu.cf != b->cpu.cf ||
        a->cpu.vf != b->cpu.vf || a->cpu.fpsr != b->cpu.fpsr || a->cpu.excl_on != b->cpu.excl_on)
        fprintf(stderr, "  %s pc=%llx nzcv=%u%u%u%u fpsr=%x excl=%u, llvm pc=%llx nzcv=%u%u%u%u fpsr=%x excl=%u\n",
                what, (unsigned long long)b->cpu.pc, b->cpu.n, b->cpu.z, b->cpu.cf, b->cpu.vf, (unsigned)b->cpu.fpsr,
                b->cpu.excl_on, (unsigned long long)a->cpu.pc, a->cpu.n, a->cpu.z, a->cpu.cf, a->cpu.vf,
                (unsigned)a->cpu.fpsr, a->cpu.excl_on);
    for (i = 0; i < MEMORY; ++i)
        if (a->memory[i] != b->memory[i]) {
            fprintf(stderr, "  %s memory differs from offset %lx\n", what, (unsigned long)i);
            break;
        }
    if (memcmp(&a->calls, &b->calls, sizeof(a->calls))) fprintf(stderr, "  %s calls differ\n", what);
}

int main(int argc, char** argv) {
    static const char* backends[8] = {"llvm"};
    static Run runs[8][2], hooked, unhooked;
    int nbackends = 1;
    unsigned char image[IMAGE];
    char *root, *input, *self, *out[9], *module[9], *text = NULL;
    const char* args[24];
    int b, k, interp;
    size_t i;
    if (argc < 4) return 2;
    for (b = 4; b < argc && nbackends < 8; ++b) backends[nbackends++] = argv[b];
    root = snx_temp_dir(NULL);
    if (!root) return 1;
    memset(image, 0, sizeof(image));
    put32(image, 0, 0x14000020); /* B main */
    memcpy(image + 0x10, "NRO0", 4);
    put32(image, 0x18, IMAGE);
    put32(image, 0x24, 0x300);
    put32(image, 0x28, 0x300);
    put32(image, 0x2c, 0x80);
    put32(image, 0x30, 0x380);
    put32(image, 0x34, 0x80);
    for (i = 0; i < sizeof(program) / sizeof(program[0]); ++i) put32(image, 0x80 + 4 * i, program[i]);
    for (i = 0; i < sizeof(tail) / sizeof(tail[0]); ++i) put32(image, 0x200 + 4 * i, tail[i]);
    input = snx_path_join(root, "program.nro");
    self = snx_executable(argv[0]);
    check(!snx_write_file(input, image, sizeof(image)), "write NRO");
    if (build_reference(root, argv[2], argv[3])) {
        check(0, "build the reference interpreter");
        return 1;
    }
    for (b = 0; b <= nbackends; ++b) { /* the last: LLVM again, with --hook-direct-calls */
        out[b] = snx_path_join(root, b < nbackends ? backends[b] : "llvm-hooked");
#ifdef _WIN32
        module[b] = snx_path_join(out[b], "Program/Program_code.dll");
#else
        module[b] = snx_path_join(out[b], "Program/Program_code.so");
#endif
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "Program";
        args[k++] = "--outdir";
        args[k++] = out[b];
        args[k++] = "--runtime";
        args[k++] = self;
        args[k++] = "--chunk";
        args[k++] = "4";
        args[k++] = "--backend";
        args[k++] = b < nbackends ? backends[b] : "llvm";
        if (b == nbackends) args[k++] = "--hook-direct-calls";
        args[k++] = "-q";
        args[k] = NULL;
        if (snx_run(args, root, NULL, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            fprintf(stderr, "%s build failed\n", b < nbackends ? backends[b] : "hooked llvm");
            check(0, "build");
        }
        free(text);
        text = NULL;
        if (b == nbackends) break;
        for (interp = 0; interp < 2; ++interp)
            check(!run(module[b], image, interp, 0, &runs[b][interp]), interp ? "interpreter run" : "AOT run");
    }
    /* func, which main's BL at 0x90 calls on each pass of the loop */
    check(!run(module[0], image, 0, 0xE8, &unhooked) && !run(module[nbackends], image, 0, 0xE8, &hooked),
          "hooked and unhooked runs");
    check(same(&runs[0][0], &hooked), "the --hook-direct-calls module runs the same");
    check(!unhooked.watched && hooked.watched > 0,
          "a direct BL reaches its function through CPU.disp only with --hook-direct-calls");
    check(runs[0][0].cpu.pc == 0 && runs[0][0].calls.count == 1 && runs[0][0].calls.imm[0] == 7 &&
              !runs[0][0].calls.undefined,
          "the program returns to 0 after one SVC #7");
    check(same(&runs[0][0], &runs[0][1]), "LLVM AOT and the reference interpreter agree");
    for (b = 0; b < nbackends; ++b) { /* the loop: b.ne back to main+0x10 */
        if (runs[b][0].polls != 1 || runs[b][0].poll_pc != runs[0][0].cpu.code_lo + 0x90)
            fprintf(stderr, "%s: %d polls, the first at %llx\n", backends[b], runs[b][0].polls,
                    (unsigned long long)runs[b][0].poll_pc);
        check(runs[b][0].polls == 1 && runs[b][0].poll_pc == runs[0][0].cpu.code_lo + 0x90,
              "AOT code polls at the loop's backward branch");
        check(runs[b][1].polls == 1, "the interpreter polls");
    }
    for (b = 1; b < nbackends; ++b) {
        if (!same(&runs[0][0], &runs[b][0])) {
            fprintf(stderr, "%s AOT differs from LLVM AOT\n", backends[b]);
            differences(&runs[0][0], &runs[b][0], "AOT");
        }
        if (!same(&runs[0][0], &runs[b][1])) {
            fprintf(stderr, "%s interpreter differs from LLVM AOT\n", backends[b]);
            differences(&runs[0][0], &runs[b][1], "interpreter");
        }
        check(same(&runs[0][0], &runs[b][0]) && same(&runs[0][0], &runs[b][1]), "backends agree");
    }
    printf("x0=%llx x9=%llx d0=%llx v2=%llx:%llx\n", (unsigned long long)runs[0][0].cpu.x[0],
           (unsigned long long)runs[0][0].cpu.x[9], (unsigned long long)runs[0][0].cpu.v[0].d[0],
           (unsigned long long)runs[0][0].cpu.v[2].d[1], (unsigned long long)runs[0][0].cpu.v[2].d[0]);
    check(!snx_remove_tree(root), "fixture cleanup");
    printf("LLVM module: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
