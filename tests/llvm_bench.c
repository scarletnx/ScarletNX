/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Speed of the generated code. Each kernel is AArch64 assembly (assembled with CLANG's aarch64 target), recompiled by
   the CLI with each backend, and run AOT with x19 iterations; the final registers of all backends must agree.
     llvm_bench SCARLETNX REPO CLANG [--iterations N] [--backend NAME]... [--kernel NAME] [-O LEVEL]
   Default backend: llvm (add gcc with --backend). Prints one line per kernel and backend: seconds and ns per
   iteration. */
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

typedef struct {
    const char *name, *source;
} Kernel;

/* each kernel: main at 0x80 loops x19 times and returns through x30 = 0; x3 points at scratch memory */
static const Kernel kernels[] = {
    {"integer", "main:\n mov x1, #0\n mov x2, #1\nloop:\n add x1, x1, x2\n eor x2, x2, x1, lsl #3\n"
                " and x4, x1, #0xff00ff00ff00ff00\n orr x5, x4, #0x3\n adds x6, x5, x2\n csel x7, x6, x1, mi\n"
                " ubfx x8, x7, #5, #17\n madd x1, x8, x2, x1\n str x1, [x3, #8]\n ldr x9, [x3, #8]\n"
                " add x2, x2, x9, lsr #7\n and w10, w2, #0x7ffffff0\n eor x2, x2, x10\n tbz x2, #5, 1f\n"
                " sub x2, x2, #3\n1:\n subs x19, x19, #1\n b.ne loop\n ret\n"},
    {"fp", "main:\n fmov d0, #1.0\n fmov d1, #1.5\n fmov d2, #0.5\n fmov s3, #2.0\n fmov s4, #1.25\nloop:\n"
           " fadd d0, d0, d2\n fmul d5, d0, d1\n fdiv d6, d5, d1\n fsub d0, d6, d2\n fmadd d7, d0, d1, d2\n"
           " fsqrt d8, d7\n fmul d0, d0, d2\n fadd d0, d0, d8\n fadd s3, s3, s4\n fdiv s3, s3, s4\n"
           " fmul s5, s3, s4\n fsub s3, s5, s3\n fcvtzs x9, d8\n scvtf d9, x9\n fcmp d9, d8\n cset x10, gt\n"
           " add x1, x1, x10\n subs x19, x19, #1\n b.ne loop\n ret\n"},
    {"simd", "main:\n fmov v2.4s, #1.5\n fmov v1.4s, #0.5\n movi v5.4s, #3\n movi v6.4s, #1\nloop:\n"
             " fadd v1.4s, v1.4s, v2.4s\n fmul v3.4s, v1.4s, v2.4s\n fmla v4.4s, v3.4s, v2.4s\n"
             " fdiv v1.4s, v3.4s, v2.4s\n add v5.4s, v5.4s, v6.4s\n st1 {v5.4s}, [x3]\n ld1 {v6.4s}, [x3]\n"
             " uzp1 v7.4s, v5.4s, v6.4s\n cnt v8.16b, v7.16b\n rbit v9.16b, v8.16b\n add v6.4s, v6.4s, v9.4s\n"
             " dup v10.4s, w19\n eor v4.16b, v4.16b, v10.16b\n subs x19, x19, #1\n b.ne loop\n ret\n"},
    {"crypto", "main:\n dup v1.2d, x3\n dup v0.4s, w3\nloop:\n aese v0.16b, v1.16b\n aesmc v0.16b, v0.16b\n"
               " aesd v2.16b, v1.16b\n aesimc v2.16b, v2.16b\n pmull v3.1q, v0.1d, v1.1d\n pmull2 v4.1q, v0.2d, v1.2d\n"
               " fmov x6, d3\n crc32x w5, w5, x6\n crc32cx w7, w7, x6\n crc32b w5, w5, w6\n crc32ch w7, w7, w6\n"
               " eor v1.16b, v1.16b, v4.16b\n eor v2.16b, v2.16b, v3.16b\n subs x19, x19, #1\n b.ne loop\n ret\n"},
    {"tricks", "main:\n movi v1.4s, #7\n movi v2.2d, #0xff\n movi v3.16b, #200\n dup v4.16b, w3\nloop:\n"
               " sqadd v5.4s, v5.4s, v1.4s\n sqsub v6.2d, v6.2d, v2.2d\n uqadd v7.16b, v7.16b, v3.16b\n"
               " srhadd v8.16b, v8.16b, v4.16b\n urhadd v9.8h, v9.8h, v3.8h\n bsl v10.16b, v4.16b, v3.16b\n"
               " bif v11.16b, v5.16b, v4.16b\n rbit v12.16b, v8.16b\n ushr v13.16b, v7.16b, #3\n"
               " sshr v14.16b, v8.16b, #2\n rbit x9, x19\n crc32x w5, w5, x9\n crc32w w6, w6, w19\n"
               " ursra v15.16b, v12.16b, #5\n eor v4.16b, v4.16b, v13.16b\n subs x19, x19, #1\n b.ne loop\n ret\n"},
    {"calls", "main:\n mov x21, x30\n adr x20, f2\n mov x1, #0\nloop:\n bl f1\n blr x20\n subs x19, x19, #1\n"
              " b.ne loop\n mov x30, x21\n ret\n bl f2\nf1:\n add x1, x1, #1\n eor x2, x2, x1, ror #7\n ret\n"
              "f2:\n add x2, x2, x1, lsl #2\n ret\n"},
};

#define MEMORY 0x40000
static int failures;

/* .text of an ELF64 relocatable object */
static unsigned char* text_of(const unsigned char* o, size_t size, size_t* length) {
    SnxU64 shoff;
    unsigned i, n, entsize, strndx;
    const unsigned char *sh, *names;
    if (size < 64) return NULL;
    memcpy(&shoff, o + 40, 8);
    entsize = o[58] | o[59] << 8;
    n = o[60] | o[61] << 8;
    strndx = o[62] | o[63] << 8;
    names = o + *(const SnxU64*)(const void*)(o + shoff + (SnxU64)strndx * entsize + 24);
    for (i = 0; i < n; ++i) {
        SnxU64 off, len;
        SnxU32 name;
        sh = o + shoff + (SnxU64)i * entsize;
        memcpy(&name, sh, 4);
        memcpy(&off, sh + 24, 8);
        memcpy(&len, sh + 32, 8);
        if (!strcmp((const char*)names + name, ".text")) {
            unsigned char* t = (unsigned char*)malloc(len);
            memcpy(t, o + off, len);
            *length = len;
            return t;
        }
    }
    return NULL;
}

static void put32(unsigned char* p, size_t off, unsigned long n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[off + i] = (unsigned char)(n >> (8 * i));
}

/* RtAPI.miss: none is expected, the kernels are whole functions the recompiler finds */
static void no_miss(CPU* c, u64 pc) {
    (void)c;
    fprintf(stderr, "llvm_bench: unexpected miss at %llx\n", (unsigned long long)pc);
    exit(1);
}

static int run(const char* path, const unsigned char* image, size_t image_size, u64 iterations, CPU* out,
               double* seconds) {
    typedef const SnxModule* (*ModuleFn)(void);
    ModuleFn get;
    const SnxModule* mod;
    RtAPI rt;
    unsigned char* memory;
    size_t i;
    double start;
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
    rt.miss = no_miss; /* every kernel is found whole */
    memory = (unsigned char*)calloc(1, (size_t)mod->mods[0].base + MEMORY);
    out->disp = (GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4, sizeof(GuestFn));
    memcpy(memory + mod->mods[0].base, image, image_size);
    for (i = 0; i < mod->nentries; ++i) out->disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
    out->mem = memory;
    out->rt = &rt;
    out->code_lo = mod->code_lo;
    out->code_hi = mod->code_hi;
    out->x[31] = mod->mods[0].base + MEMORY / 2;
    out->x[3] = mod->mods[0].base + MEMORY / 4;
    out->x[19] = iterations;
    start = snx_seconds();
    out->disp[0](out, mod->mods[0].base);
    *seconds = snx_seconds() - start;
    free(memory);
    free(out->disp);
    out->disp = NULL;
    out->mem = NULL;
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return 0;
}

int main(int argc, char** argv) {
    const char* backends[8];
    const char *only = NULL, *opt = NULL;
    size_t nbackends = 0, k, b;
    u64 iterations = 2000000;
    char *root, *text = NULL;
    int a;
    if (argc < 4) return 2;
    for (a = 4; a < argc; ++a) {
        if (a + 1 < argc && !strcmp(argv[a], "--iterations"))
            iterations = strtoull(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--backend") && nbackends < 8)
            backends[nbackends++] = argv[++a];
        else if (a + 1 < argc && !strcmp(argv[a], "--kernel"))
            only = argv[++a];
        else if (a + 1 < argc && !strcmp(argv[a], "-O"))
            opt = argv[++a];
        else
            return snx_error("unknown argument %s", argv[a]);
    }
    if (!nbackends) backends[nbackends++] = "llvm";
    root = snx_temp_dir(NULL);
    for (k = 0; k < sizeof(kernels) / sizeof(kernels[0]); ++k) {
        char *source, *object, *assembly, *input;
        unsigned char *bytes, *code, image[0x2000];
        size_t size, length = 0;
        const char* args[12];
        CPU reference;
        int have_reference = 0;
        if (only && strcmp(only, kernels[k].name)) continue;
        source = snx_format("    .text\n    .globl _start\n_start:\n    b main\n    .org 0x80\n%s", kernels[k].source);
        assembly = snx_path_join(root, "kernel.s");
        object = snx_path_join(root, "kernel.o");
        input = snx_path_join(root, "kernel.nro");
        snx_write_file(assembly, source, strlen(source));
        args[0] = argv[3];
        args[1] = "-target";
        args[2] = "aarch64-linux-gnu";
        args[3] = "-march=armv8-a+crypto+crc";
        args[4] = "-c";
        args[5] = assembly;
        args[6] = "-o";
        args[7] = object;
        args[8] = NULL;
        if (snx_run(args, root, NULL, &text)) return snx_error("assembling %s:\n%s", kernels[k].name, text);
        free(text);
        text = NULL;
        bytes = snx_read_file(object, &size);
        code = text_of(bytes, size, &length);
        if (!code || length > 0x1000) return snx_error("no usable .text in %s", kernels[k].name);
        memset(image, 0, sizeof(image));
        memcpy(image, code, length);
        memcpy(image + 0x10, "NRO0", 4);
        put32(image, 0x18, sizeof(image));
        put32(image, 0x24, 0x1000);
        put32(image, 0x28, 0x1000);
        put32(image, 0x2c, 0x800);
        put32(image, 0x30, 0x1800);
        put32(image, 0x34, 0x800);
        snx_write_file(input, image, sizeof(image));
        for (b = 0; b < nbackends; ++b) {
            char *out = snx_path_join(root, backends[b]), *module;
            CPU result;
            double seconds;
            int n = 0;
            const char* rargs[20];
#ifdef _WIN32
            module = snx_path_join(out, "Kernel/Kernel_code.dll");
#else
            module = snx_path_join(out, "Kernel/Kernel_code.so");
#endif
            rargs[n++] = argv[1];
            rargs[n++] = "--repo";
            rargs[n++] = argv[2];
            rargs[n++] = "-r";
            rargs[n++] = input;
            rargs[n++] = "Kernel";
            rargs[n++] = "--outdir";
            rargs[n++] = out;
            rargs[n++] = "--runtime";
            rargs[n++] = argv[1];
            rargs[n++] = "--backend";
            rargs[n++] = backends[b];
            rargs[n++] = "-q";
            if (opt) {
                rargs[n++] = "-O";
                rargs[n++] = opt;
            }
            rargs[n] = NULL;
            if (snx_run(rargs, root, NULL, &text) || run(module, image, sizeof(image), iterations, &result, &seconds)) {
                fprintf(stderr, "%s: %s failed\n%s\n", kernels[k].name, backends[b], text ? text : "");
                ++failures;
            } else {
                printf("%-8s %-10s %8.3f s %8.2f ns/iteration\n", kernels[k].name, backends[b], seconds,
                       seconds * 1e9 / (double)iterations);
                if (!b) {
                    reference = result;
                    have_reference = 1;
                } else if (have_reference &&
                           (memcmp(reference.x, result.x, sizeof(result.x)) ||
                            memcmp(reference.v, result.v, sizeof(result.v)) || reference.fpsr != result.fpsr ||
                            reference.n != result.n || reference.z != result.z || reference.cf != result.cf ||
                            reference.vf != result.vf)) {
                    fprintf(stderr, "%s: %s state differs from %s\n", kernels[k].name, backends[b], backends[0]);
                    ++failures;
                }
            }
            fflush(stdout);
            free(text);
            text = NULL;
            free(out);
            free(module);
        }
        free(source);
        free(assembly);
        free(object);
        free(input);
        free(bytes);
        free(code);
    }
    snx_remove_tree(root);
    return failures != 0;
}
