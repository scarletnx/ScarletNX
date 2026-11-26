/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT loader's code can be unwound through, as a module's can: a function the native back end makes (a guest
   function with a frame of its own: STP of the frame pair, SVC, LDP, RET) calls back into this program at its SVC, which
   longjmps out across it. On x86-64 Windows longjmp unwinds every frame between (RtlUnwindEx), which needs the loaded
   code's function table (the .pdata the back end writes, which code_loader_coff.c registers), and
   RtlLookupFunctionEntry finds the loaded function. Elsewhere it checks the same load and call. A host without a native
   back end has no code to load, which is said */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/runtime/interface.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define CODE  0x1000u
#define STACK 0x8000u
/* stp x29, x30, [sp, #-16]!; svc #0x5; ldp x29, x30, [sp], #16; ret */
static const uint32_t k_words[] = {0xa9bf7bfdu, 0xd40000a1u, 0xa8c17bfdu, 0xd65f03c0u};
static uint8_t g_mem[0x10000];

static jmp_buf target;
static volatile unsigned g_svc = ~0u;

/* the SVC, deep in the loaded function's frame */
static void svc(CPU* c, uint32_t imm) {
    (void)c;
    g_svc = imm;
    longjmp(target, 7);
}
static void undef(CPU* c, uint64_t pc, uint32_t word) {
    (void)c;
    fprintf(stderr, "an undefined instruction %08x at 0x%llx\n", (unsigned)word, (unsigned long long)pc);
    exit(1);
}
static void brk(CPU* c, uint64_t pc, uint32_t imm) {
    (void)c;
    fprintf(stderr, "BRK #%u at 0x%llx\n", (unsigned)imm, (unsigned long long)pc);
    exit(1);
}

int main(void) {
    char error[1024] = "";
    char name[32];
    const char* names[1] = {name};
    SnxU64 address = 0, hint = CODE;
    SnxCodeImage view = {0};
    SnxCodegen codegen;
    SnxNativeOptions options = {0};
    unsigned char made[1] = {0}, *object = NULL;
    size_t size = 0;
    if (!snx_native_available()) {
        printf("code loader unwind: this host has no native back end (%s), so no code to load\n", snx_native_name());
        return 0;
    }
    for (size_t i = 0; i < sizeof k_words / sizeof *k_words; ++i)
        for (unsigned b = 0; b < 4; ++b) g_mem[CODE + 4 * i + b] = (uint8_t)(k_words[i] >> (8 * b));
    view.image = g_mem + CODE;
    view.image_size = view.text_size = sizeof k_words;
    view.base = CODE;
    if (snx_codegen_analyze(&codegen, &view, 1, &hint, 1, 1, SNX_A64_TARGET_SWITCH) || codegen.function_count != 1) {
        fprintf(stderr, "the function: %zu found\n", codegen.function_count);
        return 1;
    }
    if (snx_native_object(&codegen, 0, 1, &options, made, &object, &size, error, sizeof(error)) || !made[0]) {
        fprintf(stderr, "the %s back end: %s\n", snx_native_name(), error[0] ? error : "the function not made");
        return 1;
    }
    snx_codegen_free(&codegen);
    SnxCodeLoader* loader = snx_code_loader_new(0, error, sizeof(error));
    if (!loader) {
        fprintf(stderr, "loader: %s\n", error);
        return 1;
    }
    SnxLinkInput input = {.data = object, .size = size, .name = "unwind"};
    snprintf(name, sizeof name, "F_%llX", (unsigned long long)CODE);
    if (snx_code_load(loader, &input, 1, names, 1, &address, error, sizeof(error))) {
        fprintf(stderr, "load: %s\n", error);
        return 1;
    }
    free(object);
#ifdef _WIN32
    {
        DWORD64 image = 0;
        if (!RtlLookupFunctionEntry((DWORD64)address, &image, NULL)) {
            fprintf(stderr, "RtlLookupFunctionEntry finds no entry for the loaded function at %llx\n",
                    (unsigned long long)address);
            return 1;
        }
    }
#endif
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    rt.svc = svc;
    rt.undef = undef;
    rt.brk = brk;
    static CPU cpu; /* static: the state outlives the longjmp, which leaves it as the SVC saw it */
    cpu.mem = g_mem;
    cpu.rt = &rt;
    cpu.x[29] = 0x2929;
    cpu.x[30] = 0x3030;
    cpu.x[31] = STACK;
    GuestFn run = (GuestFn)(uintptr_t)address;
    volatile int result = setjmp(target);
    if (!result) {
        run(&cpu, CODE);
        fprintf(stderr, "the loaded function returned instead of being unwound\n");
        return 1;
    }
    uint64_t saved = 0;
    memcpy(&saved, g_mem + STACK - 16, sizeof saved);
    if (result != 7 || g_svc != 5 || saved != 0x2929) {
        fprintf(stderr, "longjmp came back with %d, SVC #%u, the frame's x29 %llx\n", result, g_svc,
                (unsigned long long)saved);
        return 1;
    }
    snx_code_loader_free(loader);
    printf("code loader unwind: passed (longjmp across the %s back end's loaded code)\n", snx_native_name());
    return 0;
}
